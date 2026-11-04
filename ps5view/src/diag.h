// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Extracts error details from libchiaki's log messages.
//
// libchiaki reports many failure details only through its log, e.g. the
// "RP-Application-Reason" code of a rejected session request (0x80108b12, ...)
// or that the registration search timed out. To turn those into precise
// messages without forking libchiaki, every libchiaki log line is fed into
// pv_diag_feed(). The matched substrings are listed in pv_diag_patterns and a
// unit test checks that they still exist in the pinned chiaki-ng sources.

#ifndef PV_DIAG_H
#define PV_DIAG_H

#include <chiaki/log.h>
#include <chiaki/thread.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct pv_diag_info_t
{
	bool has_app_reason;
	uint32_t app_reason;        // RP-Application-Reason (session request or registration)
	int regist_http_code;       // HTTP code of the registration response (0 = none)
	int ctrl_http_code;         // HTTP code of the ctrl request (0 = none)

	bool regist_search_timeout; // console did not answer the registration search (UDP 9295)
	bool regist_search_refused; // ICMP port unreachable: Link Device screen not open
	bool regist_connect_failed; // TCP 9295 connect for the registration request failed
	bool regist_resolve_failed; // host name could not be resolved

	bool session_connect_failed;   // TCP 9295 for session request failed
	bool session_connect_refused;  //   ... with "connection refused"
	bool session_connect_timeout;  //   ... with a timeout
	bool session_no_response;      // connected but no answer to the session request

	bool ctrl_connect_failed;      // TCP 9295 ctrl connection failed
	bool login_pin_requested;      // console asked for the user's login passcode
	bool login_pin_incorrect;

	bool senkusha_failed;          // UDP 9297 network test failed (non-fatal)
	bool stream_takion_failed;     // UDP 9296 stream connection could not be established
	bool stream_bang_timeout;      // stream handshake timed out
	bool stream_remote_disconnect; // console ended the stream

	unsigned error_count;
	char last_error[256];          // last libchiaki ERROR message
} PvDiagInfo;

typedef struct pv_diag_t
{
	ChiakiMutex mutex;
	PvDiagInfo info;
} PvDiag;

typedef enum
{
	PV_DIAG_P_APP_REASON,
	PV_DIAG_P_REGIST_HTTP,
	PV_DIAG_P_REGIST_SEARCH_TIMEOUT,
	PV_DIAG_P_REGIST_SEARCH_REFUSED,
	PV_DIAG_P_REGIST_CONNECT_FAILED,
	PV_DIAG_P_REGIST_RESOLVE_FAILED,
	PV_DIAG_P_SESSION_CONNECT_FAILED,
	PV_DIAG_P_SESSION_NO_RESPONSE,
	PV_DIAG_P_CTRL_CONNECT_FAILED,
	PV_DIAG_P_CTRL_HTTP,
	PV_DIAG_P_LOGIN_PIN_REQUESTED,
	PV_DIAG_P_LOGIN_PIN_INCORRECT,
	PV_DIAG_P_SENKUSHA_TAKION_FAILED,
	PV_DIAG_P_SENKUSHA_TIMEOUT,
	PV_DIAG_P_STREAM_TAKION_FAILED,
	PV_DIAG_P_STREAM_BANG_TIMEOUT,
	PV_DIAG_P_STREAM_REMOTE_DISCONNECT,
	PV_DIAG_P_COUNT
} PvDiagPatternId;

typedef struct pv_diag_pattern_t
{
	PvDiagPatternId id;
	const char *text;   // substring searched in the log message
	const char *source; // chiaki-ng source file containing it (for the drift test)
} PvDiagPattern;

extern const PvDiagPattern pv_diag_patterns[];
extern const size_t pv_diag_patterns_count;

bool pv_diag_init(PvDiag *d);
void pv_diag_fini(PvDiag *d);
void pv_diag_reset(PvDiag *d);
void pv_diag_feed(PvDiag *d, ChiakiLogLevel level, const char *msg);
// Consistent copy of the collected facts.
void pv_diag_snapshot(PvDiag *d, PvDiagInfo *out);
// Parses one message into info (no locking; used by pv_diag_feed and tests).
void pv_diag_parse(PvDiagInfo *info, ChiakiLogLevel level, const char *msg);

// Hook for pv_log_set_chiaki_hook(): user = PvDiag *
void pv_diag_log_hook(ChiakiLogLevel level, const char *msg, void *user);

#endif
