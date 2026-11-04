// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Small platform helpers. All strings are UTF-8, also on Windows (paths are
// converted to UTF-16 internally, so user names with umlauts work).

#ifndef PV_UTIL_H
#define PV_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__MINGW32__) && defined(__MINGW_PRINTF_FORMAT)
#define PV_PRINTF(fmt_idx, args_idx) __attribute__((format(__MINGW_PRINTF_FORMAT, fmt_idx, args_idx)))
#elif defined(__GNUC__) || defined(__clang__)
#define PV_PRINTF(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define PV_PRINTF(fmt_idx, args_idx)
#endif

#define PV_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

// ---- strings ----
char *pv_strdup(const char *s);
char *pv_strndup(const char *s, size_t n);
size_t pv_strlcpy(char *dst, const char *src, size_t size);
int pv_strcasecmp(const char *a, const char *b);                    // ASCII only
bool pv_str_icontains(const char *haystack, const char *needle);    // ASCII case-insensitive
bool pv_str_starts_with(const char *s, const char *prefix);
char *pv_str_trim(char *s);                                         // trims in place, returns new start
char *pv_asprintf(const char *fmt, ...) PV_PRINTF(1, 2);

// ---- numbers / encodings ----
bool pv_parse_ulong(const char *s, unsigned long min, unsigned long max, unsigned long *out);
bool pv_hex_encode(const uint8_t *in, size_t len, char *out, size_t out_size);
bool pv_hex_decode(const char *hex, uint8_t *out, size_t out_len); // exactly out_len bytes
bool pv_base64_encode(const uint8_t *in, size_t len, char *out, size_t out_size);
bool pv_base64_decode_exact(const char *in, uint8_t *out, size_t out_len);

// ---- time ----
uint64_t pv_now_ms(void); // monotonic
void pv_sleep_ms(uint32_t ms);
typedef enum
{
	PV_TIME_LOG,      // 2026-11-04 17:01:02.123 (local)
	PV_TIME_CONSOLE,  // 17:01:02 (local)
	PV_TIME_FILENAME, // 20261104-170102 (local)
	PV_TIME_ISO_UTC   // 2026-11-04T16:01:02Z
} PvTimeStyle;
void pv_format_time(char *buf, size_t size, PvTimeStyle style);

// ---- Ctrl+C / console close ----
void pv_interrupt_install(void);
bool pv_interrupted(void);
void pv_interrupt_clear(void);
// Tell the console-close handler that cleanup is finished (Windows only).
void pv_shutdown_complete(void);
// Sleeps up to ms, returns false early if interrupted.
bool pv_sleep_interruptible(uint32_t ms);

// ---- files & paths ----
FILE *pv_fopen(const char *path, const char *mode);
bool pv_file_exists(const char *path);
bool pv_mkdir_p(const char *path);
bool pv_rename_replace(const char *from, const char *to);
bool pv_remove_file(const char *path);
// Reads a whole file. Returns NULL on error (*not_found set if it does not exist).
char *pv_read_file(const char *path, size_t max_size, size_t *len_out, bool *not_found);
// Flushes to disk and closes. Returns false if anything failed.
bool pv_fclose_sync(FILE *f);
void pv_chmod_private(const char *path);
char *pv_path_join(const char *a, const char *b);
char *pv_path_dirname(const char *path);
char *pv_default_config_dir(void); // %APPDATA%\ps5view or ~/.config/ps5view
char *pv_default_state_dir(void);  // %LOCALAPPDATA%\ps5view or ~/.local/state/ps5view
typedef void (*PvDirCb)(const char *name, void *user);
bool pv_list_dir(const char *dir, PvDirCb cb, void *user);

// ---- console / process ----
void pv_console_init(void);
bool pv_console_owned_by_us(void); // true if started by double click (own console window)
void pv_console_pause(void);
char *pv_read_line(void);           // one line from stdin without newline, NULL on EOF
bool pv_stdin_is_terminal(void);
bool pv_open_url(const char *url);
char **pv_utf8_argv(int argc, char **argv, int *argc_out);
const char *pv_os_error_string(char *buf, size_t size); // last OS error (errno / GetLastError)
void pv_os_description(char *buf, size_t size);          // e.g. "Windows 10.0.22631 (x64)"
unsigned long pv_process_id(void);

#ifdef _WIN32
#include <wchar.h>
wchar_t *pv_utf8_to_wide(const char *s);
char *pv_wide_to_utf8(const wchar_t *s);
#endif

#endif
