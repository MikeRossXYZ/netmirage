#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// This module defines functions for sending and receiving rtnetlink messages.

// Individual contexts are not thread-safe, but multiple threads may use their
// own contexts simultaneously.
typedef struct nlContext_s nlContext;

// Creates a new rtnetlink context. Returns NULL on error, in which case *err
// is set to the error code if it is provided. The netlink context implicitly
// operates in the namespaces that were active at the time of creation. This
// cannot be changed.
nlContext* nlNewContext(int* err);

// Frees a context. Calls made after freeing the context yield undefined
// behavior.
int nlFreeContext(nlContext* ctx);

// Initiates the construction of a new request packet. The contents of the
// packet are modified by using the appending functions below. Once the contents
// have been added, the message can be sent to the kernel. Callers should
// specify NLM_F_ACK in the message flags if they intend to wait for an
// acknowledgment. Any response message in the context is discarded by calling
// this function.
void nlInitMessage(nlContext* ctx, uint16_t msgType, uint16_t msgFlags);

void nlBufferAppend(nlContext* ctx, const void* buffer, size_t len);
int nlPushAttr(nlContext* ctx, unsigned short type);
int nlPopAttr(nlContext* ctx);

// Handler function for responses from the kernel. Some requests produce
// multiple responses. If a non-zero result is returned, the error is passed to
// the caller of the send function.
typedef int (*nlResponseHandler)(const nlContext* ctx, const void* data, uint32_t len, uint16_t type, uint16_t flags, void* arg);

// Sends the message under construction to the kernel. If waitResponse is set to
// true, then this function will block until confirmation is received. If no
// acknowledgment is requested, kernel errors are silently dropped. The message
// being constructed is discarded by calling this function, so the caller should
// not attempt to re-send it. If waitResponse is true and handler is non-NULL,
// then the handler is called for each response message from the kernel. The
// handler can use the subsequent functions to process the data in the response
// message. arg is passed to the handler.
int nlSendMessage(nlContext* ctx, bool waitResponse, nlResponseHandler handler, void* arg);
