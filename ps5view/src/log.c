// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "log.h"

#include <chiaki/thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_LINE_MAX 4096

static struct
{
	bool initialized;
	ChiakiMutex mutex;
	FILE *file;
	char *path;
	PvLogLevel console_level;
	bool chiaki_console;
	bool trace;
	ChiakiLog chiaki_log;
	PvChiakiLogHook hook;
	void *hook_user;
} g_log = { .console_level = PV_LOG_INFO };

static atomic_int thread_counter = 0;
static _Thread_local int thread_id = 0;

static int current_thread_id(void)
{
	if(!thread_id)
		thread_id = atomic_fetch_add(&thread_counter, 1) + 1;
	return thread_id;
}

static char level_char(PvLogLevel level)
{
	switch(level)
	{
		case PV_LOG_TRACE: return 'T';
		case PV_LOG_DEBUG: return 'D';
		case PV_LOG_INFO: return 'I';
		case PV_LOG_WARN: return 'W';
		case PV_LOG_ERROR: return 'E';
		default: return '?';
	}
}

static void chiaki_log_cb(ChiakiLogLevel level, const char *msg, void *user);

// ---------------------------------------------------------------------------

typedef struct
{
	char **names;
	size_t count;
	size_t cap;
} NameList;

static void collect_log_name(const char *name, void *user)
{
	NameList *l = user;
	size_t n = strlen(name);
	if(!pv_str_starts_with(name, "ps5view-") || n < 5 || strcmp(name + n - 4, ".log") != 0)
		return;
	if(l->count == l->cap)
	{
		size_t cap = l->cap ? l->cap * 2 : 32;
		char **nn = realloc(l->names, cap * sizeof(char *));
		if(!nn)
			return;
		l->names = nn;
		l->cap = cap;
	}
	l->names[l->count] = pv_strdup(name);
	if(l->names[l->count])
		l->count++;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void prune_logs(const char *file_path, unsigned keep)
{
	char *dir = pv_path_dirname(file_path);
	if(!dir)
		return;
	NameList l = { 0 };
	pv_list_dir(dir, collect_log_name, &l);
	if(l.count > keep)
	{
		qsort(l.names, l.count, sizeof(char *), cmp_str);
		size_t remove = l.count - keep;
		for(size_t i = 0; i < remove; i++)
		{
			char *p = pv_path_join(dir, l.names[i]);
			if(p && strcmp(p, file_path) != 0)
				pv_remove_file(p);
			free(p);
		}
	}
	for(size_t i = 0; i < l.count; i++)
		free(l.names[i]);
	free(l.names);
	free(dir);
}

bool pv_log_init(const char *file_path, unsigned keep_files)
{
	if(!g_log.initialized)
	{
		if(chiaki_mutex_init(&g_log.mutex, true) != CHIAKI_ERR_SUCCESS)
			return false;
		uint32_t mask = CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE;
		chiaki_log_init(&g_log.chiaki_log, mask, chiaki_log_cb, NULL);
		g_log.initialized = true;
	}
	if(!file_path)
		return true;

	char *dir = pv_path_dirname(file_path);
	if(dir)
		pv_mkdir_p(dir);
	free(dir);

	FILE *f = pv_fopen(file_path, "ab");
	if(!f)
	{
		char err[256];
		fprintf(stderr, "WARNUNG: Logdatei %s kann nicht geöffnet werden: %s\n", file_path, pv_os_error_string(err, sizeof(err)));
		fflush(stderr);
		return false;
	}
	setvbuf(f, NULL, _IOFBF, 16384);
	chiaki_mutex_lock(&g_log.mutex);
	g_log.file = f;
	free(g_log.path);
	g_log.path = pv_strdup(file_path);
	chiaki_mutex_unlock(&g_log.mutex);
	if(keep_files)
		prune_logs(file_path, keep_files);
	return true;
}

void pv_log_close(void)
{
	if(!g_log.initialized)
		return;
	chiaki_mutex_lock(&g_log.mutex);
	if(g_log.file)
	{
		fflush(g_log.file);
		fclose(g_log.file);
		g_log.file = NULL;
	}
	g_log.hook = NULL;
	chiaki_mutex_unlock(&g_log.mutex);
}

void pv_log_set_console_level(PvLogLevel level)
{
	g_log.console_level = level;
}

void pv_log_set_chiaki_console(bool show)
{
	g_log.chiaki_console = show;
}

void pv_log_set_trace(bool trace)
{
	g_log.trace = trace;
	uint32_t mask = CHIAKI_LOG_ALL;
	if(!trace)
		mask &= ~CHIAKI_LOG_VERBOSE;
	chiaki_log_set_level(&g_log.chiaki_log, mask);
}

const char *pv_log_path(void)
{
	return g_log.path;
}

ChiakiLog *pv_chiaki_log(void)
{
	if(!g_log.initialized)
		pv_log_init(NULL, 0);
	return &g_log.chiaki_log;
}

void pv_log_set_chiaki_hook(PvChiakiLogHook hook, void *user)
{
	if(!g_log.initialized)
		pv_log_init(NULL, 0);
	chiaki_mutex_lock(&g_log.mutex);
	g_log.hook = hook;
	g_log.hook_user = user;
	chiaki_mutex_unlock(&g_log.mutex);
}

// ---------------------------------------------------------------------------

static void write_file_line(const char *ts, PvLogLevel level, const char *tag, const char *msg)
{
	if(!g_log.file)
		return;
	// keep one record per line: indent continuation lines
	fprintf(g_log.file, "%s %c [%02d] [%s] ", ts, level_char(level), current_thread_id(), tag ? tag : "-");
	for(const char *p = msg; *p; p++)
	{
		if(*p == '\r')
			continue;
		fputc(*p, g_log.file);
		if(*p == '\n' && p[1])
			fputs("        ", g_log.file);
	}
	fputc('\n', g_log.file);
	if(level >= PV_LOG_WARN)
		fflush(g_log.file);
}

static void write_console_line(PvLogLevel level, const char *tag, bool from_chiaki, const char *msg)
{
	char ts[16];
	pv_format_time(ts, sizeof(ts), PV_TIME_CONSOLE);
	const char *prefix = "";
	switch(level)
	{
		case PV_LOG_WARN: prefix = "WARNUNG: "; break;
		case PV_LOG_ERROR: prefix = "FEHLER: "; break;
		default: break;
	}
	if(from_chiaki)
		fprintf(stderr, "[%s] (libchiaki) %s%s\n", ts, prefix, msg);
	else if(level <= PV_LOG_DEBUG)
		fprintf(stderr, "[%s] (%s) %s%s\n", ts, tag ? tag : "debug", prefix, msg);
	else
		fprintf(stderr, "[%s] %s%s\n", ts, prefix, msg);
	fflush(stderr);
}

static void log_emit(PvLogLevel level, const char *tag, bool from_chiaki, const char *msg)
{
	if(level == PV_LOG_TRACE && !g_log.trace)
		return;
	if(!g_log.initialized)
	{
		write_console_line(level, tag, from_chiaki, msg);
		return;
	}
	char ts[32];
	pv_format_time(ts, sizeof(ts), PV_TIME_LOG);
	chiaki_mutex_lock(&g_log.mutex);
	write_file_line(ts, level, tag, msg);
	bool console;
	if(from_chiaki)
		console = g_log.chiaki_console && level >= (g_log.trace ? PV_LOG_TRACE : PV_LOG_INFO);
	else
		console = level >= g_log.console_level;
	if(console)
		write_console_line(level, tag, from_chiaki, msg);
	chiaki_mutex_unlock(&g_log.mutex);
}

void pv_logv(PvLogLevel level, const char *tag, const char *fmt, va_list ap)
{
	char buf[LOG_LINE_MAX];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	log_emit(level, tag, false, buf);
}

void pv_log(PvLogLevel level, const char *tag, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	pv_logv(level, tag, fmt, ap);
	va_end(ap);
}

void pv_out(const char *fmt, ...)
{
	char buf[LOG_LINE_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if(g_log.initialized)
	{
		char ts[32];
		pv_format_time(ts, sizeof(ts), PV_TIME_LOG);
		chiaki_mutex_lock(&g_log.mutex);
		write_file_line(ts, PV_LOG_INFO, "out", buf);
		fputs(buf, stdout);
		fputc('\n', stdout);
		fflush(stdout);
		chiaki_mutex_unlock(&g_log.mutex);
	}
	else
	{
		fputs(buf, stdout);
		fputc('\n', stdout);
		fflush(stdout);
	}
}

void pv_out_private(const char *fmt, ...)
{
	char buf[LOG_LINE_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if(g_log.initialized)
		chiaki_mutex_lock(&g_log.mutex);
	fputs(buf, stdout);
	fputc('\n', stdout);
	fflush(stdout);
	if(g_log.initialized)
	{
		char ts[32];
		pv_format_time(ts, sizeof(ts), PV_TIME_LOG);
		write_file_line(ts, PV_LOG_INFO, "out", "(Ausgabe mit Kontodaten - nicht protokolliert)");
		chiaki_mutex_unlock(&g_log.mutex);
	}
}

static void chiaki_log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
	(void)user;
	PvLogLevel l;
	switch(level)
	{
		case CHIAKI_LOG_ERROR: l = PV_LOG_ERROR; break;
		case CHIAKI_LOG_WARNING: l = PV_LOG_WARN; break;
		case CHIAKI_LOG_INFO: l = PV_LOG_INFO; break;
		case CHIAKI_LOG_DEBUG: l = PV_LOG_DEBUG; break;
		default: l = PV_LOG_TRACE; break;
	}
	chiaki_mutex_lock(&g_log.mutex);
	if(g_log.hook)
		g_log.hook(level, msg, g_log.hook_user);
	chiaki_mutex_unlock(&g_log.mutex);
	log_emit(l, "chiaki", true, msg);
}
