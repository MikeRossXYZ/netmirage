#include "netlink.h"

#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>

#include <unistd.h>
#include <asm/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "log.h"

// Struct definition
#include "netlink.inl"


// Raw buffer used to hold the message being constructed or received. We share a
// buffer for all contexts. Even if we did not do this, the module is already
// not thread-safe because of namespaces being bound to the process. This way,
// we don't need large buffers for each context.
static union {
	void* data;
	struct nlmsghdr* nlmsg;
} msgBuffer;
static size_t msgBufferCap;
static size_t msgBufferLen;

void nlInit(void) {}

void nlCleanup(void) {
	if (msgBuffer.data != NULL) free(msgBuffer.data);
}

nlContext* nlNewContext(int* err) {
	nlContext* ctx = malloc(sizeof(nlContext));

	int res = nlNewContextInPlace(ctx);
	if (res == 0) return 0;

	if (err) *err = res;
	free(ctx);
	return NULL;
}

int nlNewContextInPlace(nlContext* ctx) {
	lprintln(LogDebug, "Opening rtnetlink socket");

	errno = 0;
	ctx->sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (ctx->sock == -1) {
		lprintf(LogError, "Failed to open netlink socket: %s\n", strerror(errno));
		goto abort;
	}

	// We let the kernel assign us an address to support multiple threads
	ctx->localAddr.nl_family = AF_NETLINK;
	ctx->localAddr.nl_groups = 0;
	ctx->localAddr.nl_pad = 0;
	ctx->localAddr.nl_pid = 0;
	errno = 0;
	if (bind(ctx->sock, (struct sockaddr*)&ctx->localAddr, sizeof(ctx->localAddr)) != 0) {
		lprintf(LogError, "Failed to bind netlink socket: %s\n", strerror(errno));
		goto abort;
	}
	socklen_t myAddrLen = sizeof(ctx->localAddr);
	if (getsockname(ctx->sock, (struct sockaddr*)&ctx->localAddr, &myAddrLen)) {
		lprintf(LogError, "Failed to retrieve kernel-assigned netlink address: %s\n", strerror(errno));
		goto abort;
	}

	ctx->nextSeq = 0;

	return 0;
abort:
	if (ctx->sock != -1) close(ctx->sock);
	return errno;
}

void nlFreeContext(nlContext* ctx, bool inPlace) {
	if (!inPlace) free(ctx);
}

void nlInvalidateContext(nlContext* ctx) {
	lprintln(LogDebug, "Closing rtnetlink socket");
	close(ctx->sock);
}

static struct sockaddr_nl kernelAddr = { AF_NETLINK, 0, 0, 0 };

static void nlReserveSpace(nlContext* ctx, size_t amount) {
	size_t capacity = msgBufferLen + amount;
	if (msgBufferCap < capacity) {
		size_t newCapacity = capacity * 2;
		msgBuffer.data = realloc(msgBuffer.data, newCapacity);
		#ifdef DEBUG
			// Initialize memory for tools like valgrind, even though we
			// carefully control the used portion of the buffer
			memset((char*)msgBuffer.data + msgBufferCap, 0xCE, newCapacity - msgBufferCap);
		#endif
		msgBufferCap = newCapacity;
	}
}

static void nlCommitSpace(nlContext* ctx, size_t amount) {
	msgBufferLen += amount;
}

static void nlResetSpace(nlContext* ctx, size_t initialCapacity) {
	msgBufferLen = 0;
	nlReserveSpace(ctx, initialCapacity);
}

static void* nlBufferTail(nlContext* ctx) {
	return (char*)msgBuffer.data + msgBufferLen;
}

void nlInitMessage(nlContext* ctx, uint16_t msgType, uint16_t msgFlags) {
	nlResetSpace(ctx, NLMSG_SPACE(0));

	ctx->attrDepth = 0;

	ctx->msg.msg_name = &kernelAddr;
	ctx->msg.msg_namelen = sizeof(kernelAddr);
	ctx->msg.msg_control = NULL;
	ctx->msg.msg_controllen = 0;
	ctx->msg.msg_flags = 0;

	// nlmsg length and other lengths are set just before sending, since they
	// are unknown at this point

	msgBuffer.nlmsg->nlmsg_type = msgType;
	msgBuffer.nlmsg->nlmsg_flags = NLM_F_REQUEST | msgFlags;
	msgBuffer.nlmsg->nlmsg_seq = ctx->nextSeq++;
	msgBuffer.nlmsg->nlmsg_pid = ctx->localAddr.nl_pid;

	// We don't link iov to nlmsg yet because the buffer may be reallocated
	ctx->msg.msg_iov = &ctx->iov;
	ctx->msg.msg_iovlen = 1;

	nlCommitSpace(ctx, (char*)NLMSG_DATA(msgBuffer.nlmsg) - (char*)msgBuffer.nlmsg);
}

void nlBufferAppend(nlContext* ctx, const void* buffer, size_t len) {
	nlReserveSpace(ctx, len);
	memcpy(nlBufferTail(ctx), buffer, len);
	nlCommitSpace(ctx, len);
}

int nlPushAttr(nlContext* ctx, unsigned short type) {
	if (ctx->attrDepth >= MAX_ATTR_NEST) {
		lprintln(LogError, "BUG: rtnetattr exceeded allowed nesting depth!");
		return -1;
	}

	nlReserveSpace(ctx, RTA_SPACE(0));

	struct rtattr* attr = nlBufferTail(ctx);
	attr->rta_type = type;

	ctx->attrNestPos[ctx->attrDepth] = msgBufferLen;
	++ctx->attrDepth;

	nlCommitSpace(ctx, (char*)RTA_DATA(attr) - (char*)attr);

	return 0;
}

int nlPopAttr(nlContext* ctx) {
	if (ctx->attrDepth == 0) {
		lprintln(LogError, "BUG: rtnetattr was finished when the stack was empty!");
		return -1;
	}

	--ctx->attrDepth;
	struct rtattr* attr = (struct rtattr*)((char*)msgBuffer.data + ctx->attrNestPos[ctx->attrDepth]);

	size_t payloadLen = (size_t)((char*)nlBufferTail(ctx) - (char*)RTA_DATA(attr));
	attr->rta_len = (unsigned short)RTA_LENGTH(payloadLen);

	size_t paddingDeficit = RTA_SPACE(payloadLen) - (size_t)((char*)nlBufferTail(ctx) - (char*)attr);
	if (paddingDeficit > 0) {
		nlReserveSpace(ctx, paddingDeficit);
		nlCommitSpace(ctx, paddingDeficit);
	}
	return 0;
}

int nlSendMessage(nlContext* ctx, bool waitResponse, nlResponseHandler handler, void* arg) {
	if (ctx->attrDepth > 0) {
		lprintf(LogError, "BUG: Attempted to send netlink packet with an rtattr depth of %d!\n", ctx->attrDepth);
		return -1;
	}
	msgBuffer.nlmsg->nlmsg_len = (__u32)NLMSG_LENGTH((char*)nlBufferTail(ctx) - (char*)NLMSG_DATA(msgBuffer.nlmsg));
	ctx->iov.iov_base = msgBuffer.nlmsg;
	ctx->iov.iov_len = msgBuffer.nlmsg->nlmsg_len;

	while (true) {
		lprintf(LogDebug, "Sending netlink message %lu\n", msgBuffer.nlmsg->nlmsg_seq);
		errno = 0;
		if (sendmsg(ctx->sock, &ctx->msg, 0) == -1) {
			if (errno == EAGAIN || errno == EINTR) continue;
			lprintf(LogError, "Error when sending netlink request to the kernel: %s\n", strerror(errno));
			return errno;
		} else break;
	}

	if (!waitResponse) return 0;

	// Cache sent information to prevent losing it when reusing the buffer
	__u32 seq = msgBuffer.nlmsg->nlmsg_seq;

	// We reuse the send buffers for receiving to avoid extra allocations
	nlResetSpace(ctx, 4096);
	ctx->iov.iov_base = msgBuffer.data;
	ctx->iov.iov_len = msgBufferCap;
	struct sockaddr_nl fromAddr = { AF_NETLINK, 0, 0, 0 };
	ctx->msg.msg_name = &fromAddr;

	bool foundResponse = false;
	while (!foundResponse) {
		errno = 0;
		ssize_t res = recvmsg(ctx->sock, &ctx->msg, 0);
		if (res < 0) {
			if (res == ENOBUFS) {
				lprintln(LogWarning, "Kernel ran out of memory when sending netlink responses. View of state may be desynchronized, resulting in potential stalls!");
				continue;
			}
			if (res == EAGAIN || errno == EINTR) continue;
			lprintf(LogError, "Netlink socket read error: %s\n", strerror(errno));
			return errno;
		}
		if (res == 0) {
			lprintln(LogError, "Netlink socket was closed by the kernel");
			return -1;
		}
		if (ctx->msg.msg_namelen != sizeof(fromAddr)) {
			lprintln(LogError, "Netlink response used wrong address protocol");
			return -1;
		}

		for (struct nlmsghdr* nlm = msgBuffer.data; NLMSG_OK(nlm, res); nlm = NLMSG_NEXT(nlm, res)) {
			if (nlm->nlmsg_type == NLMSG_DONE) break;
			if (nlm->nlmsg_seq != seq) {
				// We ignore responses to previous messages, since we were not
				// interested in the errors when the calls were made
				continue;
			}
			foundResponse = true;
			if (nlm->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr* nlerr = NLMSG_DATA(nlm);
				if (nlerr->error != 0) {
					// Errors reported by the kernel are negative
					lprintf(LogError, "Netlink-reported error: %s\n", strerror(-nlerr->error));
					return -nlerr->error;
				}
			}
			if (handler != NULL) {
				int userError = handler(ctx, NLMSG_DATA(nlm), NLMSG_PAYLOAD(nlm, 0), nlm->nlmsg_type, nlm->nlmsg_flags, arg);
				if (userError != 0) return userError;
			}
		}
	}
	lprintf(LogDebug, "Kernel acknowledged netlink message %d\n", seq);
	return 0;
}
