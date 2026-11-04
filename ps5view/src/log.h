// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Logging to console and file.
//
// Console: our own messages from INFO up (DEBUG with --verbose). libchiaki's
// messages only with --verbose, because they are technical and English; the
// important ones are translated by diag.c/errors.c instead.
// File: everything from DEBUG up (TRACE = libchiaki hexdumps only with --trace).

#ifndef PV_LOG_H
#define PV_LOG_H

#include "util.h"

#include <chiaki/log.h>

#include <stdarg.h>
#include <stdbool.h>

typedef enum
{
	PV_LOG_TRACE = 0,
	PV_LOG_DEBUG,
	PV_LOG_INFO,
	PV_LOG_WARN,
	PV_LOG_ERROR,
	PV_LOG_NONE
} PvLogLevel;

// Opens the log file (path may be NULL for console-only) and prunes old logs
// in the same directory (keeps the newest `keep_files`).
bool pv_log_init(const char *file_path, unsigned keep_files);
void pv_log_close(void);
void pv_log_set_console_level(PvLogLevel level);
void pv_log_set_chiaki_console(bool show);
void pv_log_set_trace(bool trace);
const char *pv_log_path(void);

void pv_log(PvLogLevel level, const char *tag, const char *fmt, ...) PV_PRINTF(3, 4);
void pv_logv(PvLogLevel level, const char *tag, const char *fmt, va_list ap);

#define LOGT(tag, ...) pv_log(PV_LOG_TRACE, tag, __VA_ARGS__)
#define LOGD(tag, ...) pv_log(PV_LOG_DEBUG, tag, __VA_ARGS__)
#define LOGI(tag, ...) pv_log(PV_LOG_INFO, tag, __VA_ARGS__)
#define LOGW(tag, ...) pv_log(PV_LOG_WARN, tag, __VA_ARGS__)
#define LOGE(tag, ...) pv_log(PV_LOG_ERROR, tag, __VA_ARGS__)

// Result output for the user: stdout, without timestamp, mirrored into the log file.
void pv_out(const char *fmt, ...) PV_PRINTF(1, 2);

// libchiaki bridge. All libchiaki objects log into this ChiakiLog.
ChiakiLog *pv_chiaki_log(void);
typedef void (*PvChiakiLogHook)(ChiakiLogLevel level, const char *msg, void *user);
// The hook sees every libchiaki message (used to extract error details).
void pv_log_set_chiaki_hook(PvChiakiLogHook hook, void *user);

#endif
