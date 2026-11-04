// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Translates libchiaki failures into understandable (German) messages with a
// concrete hint, an exit code and whether retrying makes sense.

#ifndef PV_ERRORS_H
#define PV_ERRORS_H

#include "diag.h"

#include <chiaki/session.h>

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
	PV_RETRY_NO,   // retrying cannot help (wrong PIN, Remote Play disabled, ...)
	PV_RETRY_FAST, // transient network problem: retry with the normal backoff
	PV_RETRY_SLOW  // console busy (other session, crashed): retry with long waits
} PvRetryClass;

typedef struct pv_failure_t
{
	bool failed;
	int exit_code;
	PvRetryClass retry;
	char message[512]; // one line
	char hint[2048];   // what the user can do; lines start with "- "
	char detail[320];  // technical detail (last libchiaki error etc.)
} PvFailure;

typedef struct pv_app_reason_t
{
	uint32_t code;
	const char *message;
	const char *hint;
	int exit_code;
	PvRetryClass retry;
} PvAppReason;

extern const PvAppReason pv_app_reasons[];
extern const size_t pv_app_reasons_count;

const PvAppReason *pv_app_reason_lookup(uint32_t code);

void pv_failure_clear(PvFailure *f);
void pv_failure_set(PvFailure *f, int exit_code, PvRetryClass retry, const char *hint, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 5, 6)))
#endif
	;
void pv_failure_from_app_reason(PvFailure *f, uint32_t code);
// Session ended with `reason`. Returns with f->failed == false for a regular stop.
void pv_failure_from_session(PvFailure *f, ChiakiQuitReason reason, const char *reason_str,
		const PvDiagInfo *diag, const char *host);
void pv_failure_from_regist(PvFailure *f, const PvDiagInfo *diag, const char *host);
// Logs message (ERROR), hint and detail.
void pv_failure_print(const PvFailure *f);
const char *pv_retry_class_name(PvRetryClass r);

#endif
