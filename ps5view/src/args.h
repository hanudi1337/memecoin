// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Minimal long-option parser: "--name value", "--name=value", "--flag", "-h".
//
//   PvArgs a; pv_args_init(&a, "register", argc, argv);
//   while(pv_args_next(&a)) {
//       if(pv_args_value(&a, "--host", &host)) continue;
//       if(pv_args_flag(&a, "--ps4")) { ps4 = true; continue; }
//       if(pv_args_is_help(&a)) { usage(); return 0; }
//       return pv_args_unknown(&a);
//   }
//   if(a.failed) return PV_EXIT_USAGE;

#ifndef PV_ARGS_H
#define PV_ARGS_H

#include <stdbool.h>

typedef struct pv_args_t
{
	const char *cmd;
	int argc;
	char **argv;
	int idx;
	const char *cur;
	bool failed;
} PvArgs;

// argv[0] must be the first token after the command name.
void pv_args_init(PvArgs *a, const char *cmd, int argc, char **argv);
bool pv_args_next(PvArgs *a);
bool pv_args_flag(PvArgs *a, const char *name);
bool pv_args_value(PvArgs *a, const char *name, const char **value);
bool pv_args_is_help(PvArgs *a);
int pv_args_unknown(PvArgs *a);

#endif
