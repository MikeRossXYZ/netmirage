#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <argp.h>
#include <strings.h>

#include "log.h"
#include "version.h"

// Argp version and help configuration
void argpVersion(FILE* stream, struct argp_state* state) {
	fprintf(stream, "%s\n", getVersionString());
}
void (*argp_program_version_hook)(FILE*, struct argp_state*) = &argpVersion;

// Compares an argument to a list of possibilities and returns the matching index. Errors if unmatched.
long matchArg(const char* arg, const char* options[], struct argp_state* state) {
	if (arg[0]) {
		// First try converting arg to an index
		char* endptr;
		long userIndex = strtol(arg, &endptr, 10);
		if (endptr[0]) userIndex = -1; // String is not an index

		// Scan the options until we find a match
		long index = -1;
		const char* option;
		while ((option = options[++index]) != NULL) {
			if (index == userIndex || strcasecmp(arg, option) == 0) {
				return index;
			}
		}
	}
	argp_usage(state);
	return -1; // Unreachable
}

// Argument data recovered by argp
static struct {
	const char* topoFile;
	LogLevel verbosity;
	const char* logFile;
	float bandwidthDivisor;
	const char* weightKey;
	const char* clientType;
} args;

static const float ShadowDivisor = 125.f;    // KiB/s
static const float ModelNetDivisor = 1000.f; // Kb/s

// Argument parsing hook for argp
error_t parseArg(int key, char* arg, struct argp_state* state) {
	switch (key) {
	case 'f': args.topoFile = arg; break;
	case 'l': args.logFile = arg; break;
	case 'w': args.weightKey = arg; break;
	case 'c': args.clientType = arg; break;

	case 'v': {
		const char* options[] = {"debug", "info", "warning", "error", NULL};
		args.verbosity = matchArg(arg, options, state);
		break;
	}

	case 'u': {
		const char* options[] = {"shadow", "modelnet", "KiB", "Kb", NULL};
		float divisors[] = {ShadowDivisor, ModelNetDivisor, ShadowDivisor, ModelNetDivisor};
		long index = matchArg(arg, options, state);
		args.bandwidthDivisor = divisors[index];
		break;
	}

	default: return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

int main(int argc, char** argv) {
	struct argp_option options[] = {
			{ "file",        'f', "FILE",                       0, "The GraphML file containing the network topology. If omitted, the topology is read from stdin.", 0 },

			{ "verbosity",   'v', "{debug,info,warning,error}", 0, "Verbosity of log output.", 1 },
			{ "log-file",    'l', "FILE",                       0, "Log output to FILE instead of stdout.", 1 },

			{ "units",       'u', "{shadow,modelnet,KiB,Kb}",   0, "Specifies the bandwidth units used in the input file. Shadow uses KiB/s (the default), whereas ModelNet uses Kbit/s.", 2 },
			{ "weight",      'w', "KEY",                        0, "Edge parameter to use for computing shortest paths for static routes. Must be a key used in the GraphML file (default: \"latency\").", 2},
			{ "client-node", 'c', "TYPE",                       0, "Type of client nodes. Nodes in the GraphML file whose \"type\" attribute matches this value will be clients. If omitted, all nodes are clients.", 2},

			{ NULL },
	};
	struct argp argp = { options, &parseArg, NULL, "Sets up virtual networking infrastructure for a SNEAC core node." };

	// Defaults
	args.verbosity = LogError;
	args.bandwidthDivisor = ShadowDivisor;
	args.weightKey = "latency";

	argp_parse (&argp, argc, argv, 0, NULL, NULL);

	return 0;
}
