// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "util.h"

#include <chiaki/time.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <fcntl.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

// ===========================================================================
// strings
// ===========================================================================

char *pv_strdup(const char *s)
{
	if(!s)
		return NULL;
	size_t n = strlen(s);
	char *r = malloc(n + 1);
	if(!r)
		return NULL;
	memcpy(r, s, n + 1);
	return r;
}

char *pv_strndup(const char *s, size_t n)
{
	if(!s)
		return NULL;
	size_t len = 0;
	while(len < n && s[len])
		len++;
	char *r = malloc(len + 1);
	if(!r)
		return NULL;
	memcpy(r, s, len);
	r[len] = '\0';
	return r;
}

size_t pv_strlcpy(char *dst, const char *src, size_t size)
{
	size_t len = src ? strlen(src) : 0;
	if(size)
	{
		size_t n = len < size - 1 ? len : size - 1;
		if(n)
			memcpy(dst, src, n);
		dst[n] = '\0';
	}
	return len;
}

static int ascii_lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

int pv_strcasecmp(const char *a, const char *b)
{
	for(;; a++, b++)
	{
		int ca = ascii_lower((unsigned char)*a);
		int cb = ascii_lower((unsigned char)*b);
		if(ca != cb || !ca)
			return ca - cb;
	}
}

bool pv_str_icontains(const char *haystack, const char *needle)
{
	if(!haystack || !needle)
		return false;
	size_t nlen = strlen(needle);
	if(!nlen)
		return true;
	for(const char *h = haystack; *h; h++)
	{
		size_t i = 0;
		while(i < nlen && h[i] && ascii_lower((unsigned char)h[i]) == ascii_lower((unsigned char)needle[i]))
			i++;
		if(i == nlen)
			return true;
	}
	return false;
}

bool pv_str_starts_with(const char *s, const char *prefix)
{
	return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

char *pv_str_trim(char *s)
{
	if(!s)
		return s;
	while(*s && isspace((unsigned char)*s))
		s++;
	size_t n = strlen(s);
	while(n && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
	return s;
}

char *pv_asprintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	va_list ap2;
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if(n < 0)
	{
		va_end(ap2);
		return NULL;
	}
	char *r = malloc((size_t)n + 1);
	if(r)
		vsnprintf(r, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	return r;
}

// ===========================================================================
// numbers / encodings
// ===========================================================================

bool pv_parse_ulong(const char *s, unsigned long min, unsigned long max, unsigned long *out)
{
	if(!s || !*s)
		return false;
	for(const char *p = s; *p; p++)
		if(*p < '0' || *p > '9')
			return false;
	errno = 0;
	char *end = NULL;
	unsigned long v = strtoul(s, &end, 10);
	if(errno || !end || *end || v < min || v > max)
		return false;
	*out = v;
	return true;
}

bool pv_hex_encode(const uint8_t *in, size_t len, char *out, size_t out_size)
{
	static const char digits[] = "0123456789abcdef";
	if(out_size < len * 2 + 1)
		return false;
	for(size_t i = 0; i < len; i++)
	{
		out[i * 2] = digits[in[i] >> 4];
		out[i * 2 + 1] = digits[in[i] & 0xf];
	}
	out[len * 2] = '\0';
	return true;
}

static int hex_val(char c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

bool pv_hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
	if(!hex || strlen(hex) != out_len * 2)
		return false;
	for(size_t i = 0; i < out_len; i++)
	{
		int hi = hex_val(hex[i * 2]);
		int lo = hex_val(hex[i * 2 + 1]);
		if(hi < 0 || lo < 0)
			return false;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return true;
}

static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool pv_base64_encode(const uint8_t *in, size_t len, char *out, size_t out_size)
{
	size_t need = ((len + 2) / 3) * 4 + 1;
	if(out_size < need)
		return false;
	size_t o = 0;
	for(size_t i = 0; i < len; i += 3)
	{
		uint32_t n = (uint32_t)in[i] << 16;
		if(i + 1 < len)
			n |= (uint32_t)in[i + 1] << 8;
		if(i + 2 < len)
			n |= in[i + 2];
		out[o++] = b64_chars[(n >> 18) & 63];
		out[o++] = b64_chars[(n >> 12) & 63];
		out[o++] = i + 1 < len ? b64_chars[(n >> 6) & 63] : '=';
		out[o++] = i + 2 < len ? b64_chars[n & 63] : '=';
	}
	out[o] = '\0';
	return true;
}

static int b64_val(char c)
{
	if(c >= 'A' && c <= 'Z')
		return c - 'A';
	if(c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if(c >= '0' && c <= '9')
		return c - '0' + 52;
	if(c == '+' || c == '-') // also accept the URL-safe alphabet
		return 62;
	if(c == '/' || c == '_')
		return 63;
	return -1;
}

bool pv_base64_decode_exact(const char *in, uint8_t *out, size_t out_len)
{
	if(!in)
		return false;
	uint32_t acc = 0;
	int bits = 0;
	size_t o = 0;
	bool padding = false;
	for(const char *p = in; *p; p++)
	{
		char c = *p;
		if(c == ' ' || c == '\t' || c == '\r' || c == '\n')
			continue;
		if(c == '=')
		{
			padding = true;
			continue;
		}
		if(padding) // data after padding
			return false;
		int v = b64_val(c);
		if(v < 0)
			return false;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if(bits >= 8)
		{
			bits -= 8;
			if(o >= out_len)
				return false;
			out[o++] = (uint8_t)((acc >> bits) & 0xff);
		}
	}
	// remaining bits must be zero padding bits
	if(bits >= 6 || (acc & ((1u << bits) - 1)) != 0)
		return false;
	return o == out_len;
}

// ===========================================================================
// time
// ===========================================================================

uint64_t pv_now_ms(void)
{
	return chiaki_time_now_monotonic_ms();
}

void pv_sleep_ms(uint32_t ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while(nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
#endif
}

void pv_format_time(char *buf, size_t size, PvTimeStyle style)
{
	int year, mon, day, hour, min, sec, msec;
#ifdef _WIN32
	SYSTEMTIME st;
	if(style == PV_TIME_ISO_UTC)
		GetSystemTime(&st);
	else
		GetLocalTime(&st);
	year = st.wYear; mon = st.wMonth; day = st.wDay;
	hour = st.wHour; min = st.wMinute; sec = st.wSecond; msec = st.wMilliseconds;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	time_t t = tv.tv_sec;
	struct tm tm;
	if(style == PV_TIME_ISO_UTC)
		gmtime_r(&t, &tm);
	else
		localtime_r(&t, &tm);
	year = tm.tm_year + 1900; mon = tm.tm_mon + 1; day = tm.tm_mday;
	hour = tm.tm_hour; min = tm.tm_min; sec = tm.tm_sec; msec = (int)(tv.tv_usec / 1000);
#endif
	switch(style)
	{
		case PV_TIME_LOG:
			snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d.%03d", year, mon, day, hour, min, sec, msec);
			break;
		case PV_TIME_CONSOLE:
			snprintf(buf, size, "%02d:%02d:%02d", hour, min, sec);
			break;
		case PV_TIME_FILENAME:
			snprintf(buf, size, "%04d%02d%02d-%02d%02d%02d", year, mon, day, hour, min, sec);
			break;
		case PV_TIME_ISO_UTC:
			snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, mon, day, hour, min, sec);
			break;
	}
}

// ===========================================================================
// interrupt handling
// ===========================================================================

static volatile sig_atomic_t interrupted_flag = 0;
static volatile sig_atomic_t shutdown_done_flag = 0;

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
	switch(type)
	{
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
			interrupted_flag = 1;
			return TRUE;
		case CTRL_CLOSE_EVENT:
		case CTRL_LOGOFF_EVENT:
		case CTRL_SHUTDOWN_EVENT:
			// Windows kills the process once we return. Give the main thread a
			// moment to end the Remote Play session cleanly, so the console does
			// not keep a stale session around ("already in use" on next start).
			interrupted_flag = 1;
			for(int i = 0; i < 40 && !shutdown_done_flag; i++)
				Sleep(100);
			return TRUE;
		default:
			return FALSE;
	}
}
#else
static void signal_handler(int sig)
{
	(void)sig;
	interrupted_flag = 1;
}
#endif

void pv_interrupt_install(void)
{
#ifdef _WIN32
	SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
#endif
}

bool pv_interrupted(void)
{
	return interrupted_flag != 0;
}

void pv_interrupt_clear(void)
{
	interrupted_flag = 0;
}

void pv_shutdown_complete(void)
{
	shutdown_done_flag = 1;
}

bool pv_sleep_interruptible(uint32_t ms)
{
	uint64_t end = pv_now_ms() + ms;
	while(!pv_interrupted())
	{
		uint64_t now = pv_now_ms();
		if(now >= end)
			return true;
		uint64_t left = end - now;
		pv_sleep_ms(left > 50 ? 50 : (uint32_t)left);
	}
	return false;
}

// ===========================================================================
// files & paths
// ===========================================================================

#ifdef _WIN32
wchar_t *pv_utf8_to_wide(const char *s)
{
	if(!s)
		return NULL;
	int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
	if(n <= 0)
		return NULL;
	wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
	if(!w)
		return NULL;
	if(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0)
	{
		free(w);
		return NULL;
	}
	return w;
}

char *pv_wide_to_utf8(const wchar_t *s)
{
	if(!s)
		return NULL;
	int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
	if(n <= 0)
		return NULL;
	char *r = malloc((size_t)n);
	if(!r)
		return NULL;
	if(WideCharToMultiByte(CP_UTF8, 0, s, -1, r, n, NULL, NULL) <= 0)
	{
		free(r);
		return NULL;
	}
	return r;
}
#endif

FILE *pv_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
	wchar_t *wpath = pv_utf8_to_wide(path);
	wchar_t *wmode = pv_utf8_to_wide(mode);
	FILE *f = NULL;
	if(wpath && wmode)
		f = _wfopen(wpath, wmode);
	free(wpath);
	free(wmode);
	return f;
#else
	return fopen(path, mode);
#endif
}

bool pv_file_exists(const char *path)
{
#ifdef _WIN32
	wchar_t *w = pv_utf8_to_wide(path);
	if(!w)
		return false;
	DWORD attr = GetFileAttributesW(w);
	free(w);
	return attr != INVALID_FILE_ATTRIBUTES;
#else
	struct stat st;
	return stat(path, &st) == 0;
#endif
}

static bool mkdir_one(const char *path)
{
#ifdef _WIN32
	wchar_t *w = pv_utf8_to_wide(path);
	if(!w)
		return false;
	BOOL ok = CreateDirectoryW(w, NULL);
	DWORD err = ok ? 0 : GetLastError();
	free(w);
	return ok || err == ERROR_ALREADY_EXISTS;
#else
	return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

static bool is_sep(char c)
{
#ifdef _WIN32
	return c == '\\' || c == '/';
#else
	return c == '/';
#endif
}

bool pv_mkdir_p(const char *path)
{
	if(!path || !*path)
		return false;
	char *tmp = pv_strdup(path);
	if(!tmp)
		return false;
	size_t len = strlen(tmp);
	while(len > 1 && is_sep(tmp[len - 1]))
		tmp[--len] = '\0';
	size_t start = 1;
#ifdef _WIN32
	// skip drive ("C:\") and UNC ("\\server\share\") prefixes
	if(len >= 2 && tmp[1] == ':')
		start = 3;
	else if(len >= 2 && is_sep(tmp[0]) && is_sep(tmp[1]))
	{
		int seps = 0;
		size_t i = 2;
		for(; i < len; i++)
			if(is_sep(tmp[i]) && ++seps == 2)
				break;
		start = i + 1;
	}
#endif
	bool ok = true;
	for(size_t i = start; i <= len && ok; i++)
	{
		if(i == len || is_sep(tmp[i]))
		{
			char saved = tmp[i];
			tmp[i] = '\0';
			if(*tmp)
				ok = mkdir_one(tmp);
			tmp[i] = saved;
		}
	}
	free(tmp);
	return ok;
}

bool pv_rename_replace(const char *from, const char *to)
{
#ifdef _WIN32
	wchar_t *wf = pv_utf8_to_wide(from);
	wchar_t *wt = pv_utf8_to_wide(to);
	BOOL ok = wf && wt && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	free(wf);
	free(wt);
	return ok;
#else
	return rename(from, to) == 0;
#endif
}

bool pv_remove_file(const char *path)
{
#ifdef _WIN32
	wchar_t *w = pv_utf8_to_wide(path);
	BOOL ok = w && DeleteFileW(w);
	free(w);
	return ok;
#else
	return unlink(path) == 0;
#endif
}

char *pv_read_file(const char *path, size_t max_size, size_t *len_out, bool *not_found)
{
	if(not_found)
		*not_found = false;
	FILE *f = pv_fopen(path, "rb");
	if(!f)
	{
#ifdef _WIN32
		DWORD e = GetLastError();
		if(not_found)
			*not_found = (errno == ENOENT) || e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
#else
		if(not_found)
			*not_found = errno == ENOENT;
#endif
		return NULL;
	}
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap + 1);
	while(buf)
	{
		if(len == cap)
		{
			if(cap >= max_size)
			{
				free(buf);
				buf = NULL;
				errno = EFBIG;
				break;
			}
			cap *= 2;
			char *nb = realloc(buf, cap + 1);
			if(!nb)
			{
				free(buf);
				buf = NULL;
				break;
			}
			buf = nb;
		}
		size_t n = fread(buf + len, 1, cap - len, f);
		len += n;
		if(n == 0)
		{
			if(ferror(f))
			{
				free(buf);
				buf = NULL;
			}
			break;
		}
	}
	fclose(f);
	if(buf)
	{
		buf[len] = '\0';
		if(len_out)
			*len_out = len;
	}
	return buf;
}

bool pv_fclose_sync(FILE *f)
{
	if(!f)
		return false;
	bool ok = fflush(f) == 0;
#ifdef _WIN32
	if(ok)
		ok = _commit(_fileno(f)) == 0;
#else
	if(ok)
		ok = fsync(fileno(f)) == 0;
#endif
	if(fclose(f) != 0)
		ok = false;
	return ok;
}

void pv_chmod_private(const char *path)
{
#ifndef _WIN32
	chmod(path, 0600);
#else
	(void)path; // %APPDATA% is per-user already
#endif
}

char *pv_path_join(const char *a, const char *b)
{
	if(!a || !*a)
		return pv_strdup(b);
	size_t la = strlen(a);
#ifdef _WIN32
	const char *sep = "\\";
#else
	const char *sep = "/";
#endif
	if(is_sep(a[la - 1]))
		sep = "";
	return pv_asprintf("%s%s%s", a, sep, b);
}

char *pv_path_dirname(const char *path)
{
	char *r = pv_strdup(path);
	if(!r)
		return NULL;
	size_t n = strlen(r);
	while(n && !is_sep(r[n - 1]))
		n--;
	while(n > 1 && is_sep(r[n - 1]))
		n--;
	if(n == 0)
	{
		free(r);
		return pv_strdup(".");
	}
	r[n] = '\0';
	return r;
}

#ifdef _WIN32
static char *env_utf8(const wchar_t *name)
{
	const wchar_t *v = _wgetenv(name);
	if(!v || !*v)
		return NULL;
	return pv_wide_to_utf8(v);
}
#endif

static char *base_dir(bool state)
{
#ifdef _WIN32
	char *base = env_utf8(state ? L"LOCALAPPDATA" : L"APPDATA");
	if(base)
		return base;
	char *profile = env_utf8(L"USERPROFILE");
	if(profile)
	{
		char *r = pv_asprintf("%s\\AppData\\%s", profile, state ? "Local" : "Roaming");
		free(profile);
		return r;
	}
	return pv_strdup(".");
#else
	const char *xdg = getenv(state ? "XDG_STATE_HOME" : "XDG_CONFIG_HOME");
	if(xdg && *xdg && xdg[0] == '/')
		return pv_strdup(xdg);
	const char *home = getenv("HOME");
	if(home && *home)
		return pv_asprintf("%s/%s", home, state ? ".local/state" : ".config");
	return pv_strdup(".");
#endif
}

char *pv_default_config_dir(void)
{
	char *base = base_dir(false);
	char *r = base ? pv_path_join(base, "ps5view") : NULL;
	free(base);
	return r;
}

char *pv_default_state_dir(void)
{
	char *base = base_dir(true);
	char *r = base ? pv_path_join(base, "ps5view") : NULL;
	free(base);
	return r;
}

bool pv_list_dir(const char *dir, PvDirCb cb, void *user)
{
#ifdef _WIN32
	char *pattern = pv_path_join(dir, "*");
	wchar_t *w = pv_utf8_to_wide(pattern);
	free(pattern);
	if(!w)
		return false;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(w, &fd);
	free(w);
	if(h == INVALID_HANDLE_VALUE)
		return false;
	do
	{
		if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		char *name = pv_wide_to_utf8(fd.cFileName);
		if(name)
		{
			cb(name, user);
			free(name);
		}
	} while(FindNextFileW(h, &fd));
	FindClose(h);
	return true;
#else
	DIR *d = opendir(dir);
	if(!d)
		return false;
	struct dirent *e;
	while((e = readdir(d)))
	{
		if(e->d_name[0] == '.')
			continue;
		cb(e->d_name, user);
	}
	closedir(d);
	return true;
#endif
}

// ===========================================================================
// console / process
// ===========================================================================

void pv_console_init(void)
{
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
	// Fully buffered + explicit fflush() after every message: the classic
	// console host garbles UTF-8 sequences that are split across writes.
	setvbuf(stdout, NULL, _IOFBF, 8192);
	setvbuf(stderr, NULL, _IOFBF, 8192);
#else
	setvbuf(stdout, NULL, _IOLBF, 0);
#endif
}

bool pv_console_owned_by_us(void)
{
#ifdef _WIN32
	DWORD pids[2];
	DWORD n = GetConsoleProcessList(pids, 2);
	return n == 1;
#else
	return false;
#endif
}

void pv_console_pause(void)
{
	fprintf(stderr, "\nZum Schließen Enter drücken ...");
	fflush(stderr);
	int c;
	while((c = getchar()) != EOF && c != '\n')
		;
}

bool pv_stdin_is_terminal(void)
{
#ifdef _WIN32
	DWORD mode;
	return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
	return isatty(STDIN_FILENO);
#endif
}

char *pv_read_line(void)
{
#ifdef _WIN32
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	DWORD mode;
	if(GetConsoleMode(in, &mode))
	{
		// Console input: read UTF-16 so pasted non-ASCII text survives.
		size_t cap = 8192, len = 0;
		wchar_t *w = malloc(sizeof(wchar_t) * cap);
		if(!w)
			return NULL;
		for(;;)
		{
			DWORD got = 0;
			if(len + 1 >= cap)
				break;
			if(!ReadConsoleW(in, w + len, (DWORD)(cap - len - 1), &got, NULL) || got == 0)
			{
				if(len == 0)
				{
					free(w);
					return NULL;
				}
				break;
			}
			len += got;
			if(len && w[len - 1] == L'\n')
				break;
		}
		w[len] = 0;
		while(len && (w[len - 1] == L'\n' || w[len - 1] == L'\r'))
			w[--len] = 0;
		char *r = pv_wide_to_utf8(w);
		free(w);
		return r;
	}
#endif
	size_t cap = 1024, len = 0;
	char *buf = malloc(cap);
	if(!buf)
		return NULL;
	int c;
	bool any = false;
	while((c = getchar()) != EOF)
	{
		any = true;
		if(c == '\n')
			break;
		if(len + 1 >= cap)
		{
			if(cap >= 65536)
				continue; // ignore absurdly long input
			cap *= 2;
			char *nb = realloc(buf, cap);
			if(!nb)
			{
				free(buf);
				return NULL;
			}
			buf = nb;
		}
		buf[len++] = (char)c;
	}
	if(!any)
	{
		free(buf);
		return NULL;
	}
	while(len && buf[len - 1] == '\r')
		len--;
	buf[len] = '\0';
	return buf;
}

bool pv_open_url(const char *url)
{
#ifdef _WIN32
	wchar_t *w = pv_utf8_to_wide(url);
	if(!w)
		return false;
	HINSTANCE r = ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
	free(w);
	return (INT_PTR)r > 32;
#else
#ifdef __APPLE__
	const char *opener = "open";
#else
	const char *opener = "xdg-open";
#endif
	char *const argv[] = { (char *)opener, (char *)url, NULL };
	pid_t pid;
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	int r = posix_spawnp(&pid, opener, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	if(r != 0)
		return false;
	// xdg-open returns quickly; reap it to see whether it worked.
	int status = 0;
	for(int i = 0; i < 30; i++)
	{
		pid_t w = waitpid(pid, &status, WNOHANG);
		if(w == pid)
			return WIFEXITED(status) && WEXITSTATUS(status) == 0;
		if(w < 0)
			return false;
		pv_sleep_ms(100);
	}
	return true; // still running: assume a browser was started
#endif
}

char **pv_utf8_argv(int argc, char **argv, int *argc_out)
{
#ifdef _WIN32
	(void)argc;
	int n = 0;
	LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &n);
	if(wargv && n > 0)
	{
		char **r = calloc((size_t)n + 1, sizeof(char *));
		if(r)
		{
			bool ok = true;
			for(int i = 0; i < n && ok; i++)
			{
				r[i] = pv_wide_to_utf8(wargv[i]);
				ok = r[i] != NULL;
			}
			LocalFree(wargv);
			if(ok)
			{
				*argc_out = n;
				return r; // intentionally kept for the whole process lifetime
			}
		}
		else
			LocalFree(wargv);
	}
	*argc_out = argc;
	return argv;
#else
	*argc_out = argc;
	return argv;
#endif
}

const char *pv_os_error_string(char *buf, size_t size)
{
#ifdef _WIN32
	DWORD e = GetLastError();
	wchar_t wbuf[512];
	DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e,
			0, wbuf, PV_ARRAY_SIZE(wbuf), NULL);
	if(n == 0)
	{
		snprintf(buf, size, "Windows-Fehler %lu", (unsigned long)e);
		return buf;
	}
	while(n && (wbuf[n - 1] == L'\r' || wbuf[n - 1] == L'\n' || wbuf[n - 1] == L' '))
		wbuf[--n] = 0;
	char *u = pv_wide_to_utf8(wbuf);
	snprintf(buf, size, "%s (%lu)", u ? u : "?", (unsigned long)e);
	free(u);
	return buf;
#else
	snprintf(buf, size, "%s", strerror(errno));
	return buf;
#endif
}

void pv_os_description(char *buf, size_t size)
{
#ifdef _WIN32
	typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW *);
	OSVERSIONINFOEXW vi;
	memset(&vi, 0, sizeof(vi));
	vi.dwOSVersionInfoSize = sizeof(vi);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	RtlGetVersionFn fn = ntdll ? (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
	if(fn && fn(&vi) == 0)
		snprintf(buf, size, "Windows %lu.%lu.%lu%s", (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion,
				(unsigned long)vi.dwBuildNumber, vi.dwBuildNumber >= 22000 ? " (Windows 11)" : "");
	else
		snprintf(buf, size, "Windows (Version unbekannt)");
#else
	struct utsname u;
	if(uname(&u) == 0)
		snprintf(buf, size, "%s %s %s", u.sysname, u.release, u.machine);
	else
		snprintf(buf, size, "POSIX (unbekannt)");
#endif
}

unsigned long pv_process_id(void)
{
#ifdef _WIN32
	return (unsigned long)GetCurrentProcessId();
#else
	return (unsigned long)getpid();
#endif
}
