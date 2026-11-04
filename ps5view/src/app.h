// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Shared definitions for all ps5view commands.

#ifndef PV_APP_H
#define PV_APP_H

#include <stdbool.h>

// Process exit codes. Documented in README.md, keep in sync.
enum
{
	PV_EXIT_OK = 0,
	PV_EXIT_USAGE = 1,       // invalid command line
	PV_EXIT_CONFIG = 2,      // config file / profile problem
	PV_EXIT_NETWORK = 3,     // console unreachable, firewall, timeouts
	PV_EXIT_AUTH = 4,        // PIN, account id, registration or PSN login rejected
	PV_EXIT_CONSOLE = 5,     // console state: standby, Remote Play disabled/in use, version
	PV_EXIT_DEVICE = 6,      // audio/video device or decoder problem
	PV_EXIT_INTERNAL = 10,   // unexpected internal error
	PV_EXIT_INTERRUPTED = 130 // Ctrl+C
};

typedef struct pv_global_opts_t
{
	const char *config_path; // NULL = default location
	bool verbose;            // libchiaki INFO and our DEBUG messages on the console
	bool trace;              // additionally libchiaki VERBOSE (hexdumps, may contain keys!)
} PvGlobalOpts;

extern PvGlobalOpts pv_global;

int pv_cmd_discover(int argc, char **argv);
int pv_cmd_account_id(int argc, char **argv);
int pv_cmd_register(int argc, char **argv);
int pv_cmd_profiles(int argc, char **argv);
int pv_cmd_remove_profile(int argc, char **argv);
int pv_cmd_stream(int argc, char **argv);
int pv_cmd_list_audio(int argc, char **argv);
int pv_cmd_check(int argc, char **argv);

#endif
