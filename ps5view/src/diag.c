// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "diag.h"

#include "util.h"

#include <stdlib.h>
#include <string.h>

// Substrings of libchiaki log messages (chiaki-ng, see PV_CHIAKI_COMMIT).
// tests/test_main.c verifies each text still occurs in the given source file.
const PvDiagPattern pv_diag_patterns[] = {
	{ PV_DIAG_P_APP_REASON, "Reported Application Reason: ", "lib/src/session.c" },
	{ PV_DIAG_P_REGIST_HTTP, "Regist received HTTP code ", "lib/src/regist.c" },
	{ PV_DIAG_P_REGIST_SEARCH_TIMEOUT, "Regist timed out waiting for search response", "lib/src/regist.c" },
	{ PV_DIAG_P_REGIST_SEARCH_REFUSED, "The console refused the registration search", "lib/src/regist.c" },
	{ PV_DIAG_P_REGIST_CONNECT_FAILED, "Regist connect failed", "lib/src/regist.c" },
	{ PV_DIAG_P_REGIST_RESOLVE_FAILED, "Regist failed to getaddrinfo", "lib/src/regist.c" },
	{ PV_DIAG_P_SESSION_CONNECT_FAILED, "Session request connect failed: ", "lib/src/session.c" },
	{ PV_DIAG_P_SESSION_NO_RESPONSE, "Failed to receive session request response", "lib/src/session.c" },
	{ PV_DIAG_P_CTRL_CONNECT_FAILED, "Ctrl connect failed: ", "lib/src/ctrl.c" },
	{ PV_DIAG_P_CTRL_HTTP, "Ctrl http response was not successful. HTTP code was ", "lib/src/ctrl.c" },
	{ PV_DIAG_P_LOGIN_PIN_REQUESTED, "Ctrl requested Login PIN", "lib/src/session.c" },
	{ PV_DIAG_P_LOGIN_PIN_INCORRECT, "Login PIN was incorrect", "lib/src/session.c" },
	{ PV_DIAG_P_SENKUSHA_TAKION_FAILED, "Senkusha Takion connect failed", "lib/src/senkusha.c" },
	{ PV_DIAG_P_SENKUSHA_TIMEOUT, "Senkusha connect timeout", "lib/src/senkusha.c" },
	{ PV_DIAG_P_STREAM_TAKION_FAILED, "StreamConnection Takion connect failed", "lib/src/streamconnection.c" },
	{ PV_DIAG_P_STREAM_BANG_TIMEOUT, "StreamConnection bang receive timeout", "lib/src/streamconnection.c" },
	{ PV_DIAG_P_STREAM_REMOTE_DISCONNECT, "Remote disconnected from StreamConnection", "lib/src/session.c" },
};
const size_t pv_diag_patterns_count = PV_ARRAY_SIZE(pv_diag_patterns);

static const char *pattern_text(PvDiagPatternId id)
{
	for(size_t i = 0; i < pv_diag_patterns_count; i++)
		if(pv_diag_patterns[i].id == id)
			return pv_diag_patterns[i].text;
	return NULL;
}

static const char *find_after(const char *msg, PvDiagPatternId id)
{
	const char *t = pattern_text(id);
	if(!t)
		return NULL;
	const char *p = strstr(msg, t);
	return p ? p + strlen(t) : NULL;
}

bool pv_diag_init(PvDiag *d)
{
	memset(d, 0, sizeof(*d));
	return chiaki_mutex_init(&d->mutex, false) == CHIAKI_ERR_SUCCESS;
}

void pv_diag_fini(PvDiag *d)
{
	chiaki_mutex_fini(&d->mutex);
}

void pv_diag_reset(PvDiag *d)
{
	chiaki_mutex_lock(&d->mutex);
	memset(&d->info, 0, sizeof(d->info));
	chiaki_mutex_unlock(&d->mutex);
}

void pv_diag_snapshot(PvDiag *d, PvDiagInfo *out)
{
	chiaki_mutex_lock(&d->mutex);
	*out = d->info;
	chiaki_mutex_unlock(&d->mutex);
}

void pv_diag_feed(PvDiag *d, ChiakiLogLevel level, const char *msg)
{
	if(!msg)
		return;
	chiaki_mutex_lock(&d->mutex);
	pv_diag_parse(&d->info, level, msg);
	chiaki_mutex_unlock(&d->mutex);
}

void pv_diag_parse(PvDiagInfo *d, ChiakiLogLevel level, const char *msg)
{
	const char *p;
	if(level == CHIAKI_LOG_ERROR)
	{
		d->error_count++;
		pv_strlcpy(d->last_error, msg, sizeof(d->last_error));
	}

	if((p = find_after(msg, PV_DIAG_P_APP_REASON)))
	{
		char *end = NULL;
		unsigned long v = strtoul(p, &end, 0);
		if(end != p)
		{
			d->has_app_reason = true;
			d->app_reason = (uint32_t)v;
		}
	}
	else if((p = find_after(msg, PV_DIAG_P_REGIST_HTTP)))
		d->regist_http_code = atoi(p);
	else if(find_after(msg, PV_DIAG_P_REGIST_SEARCH_TIMEOUT))
		d->regist_search_timeout = true;
	else if(find_after(msg, PV_DIAG_P_REGIST_SEARCH_REFUSED))
		d->regist_search_refused = true;
	else if(find_after(msg, PV_DIAG_P_REGIST_CONNECT_FAILED))
		d->regist_connect_failed = true;
	else if(find_after(msg, PV_DIAG_P_REGIST_RESOLVE_FAILED))
		d->regist_resolve_failed = true;
	else if((p = find_after(msg, PV_DIAG_P_SESSION_CONNECT_FAILED)))
	{
		d->session_connect_failed = true;
		if(pv_str_icontains(p, "refused"))
			d->session_connect_refused = true;
		else if(pv_str_icontains(p, "timeout"))
			d->session_connect_timeout = true;
	}
	else if(find_after(msg, PV_DIAG_P_SESSION_NO_RESPONSE))
		d->session_no_response = true;
	else if(find_after(msg, PV_DIAG_P_CTRL_CONNECT_FAILED))
		d->ctrl_connect_failed = true;
	else if((p = find_after(msg, PV_DIAG_P_CTRL_HTTP)))
		d->ctrl_http_code = atoi(p);
	else if(find_after(msg, PV_DIAG_P_LOGIN_PIN_REQUESTED))
		d->login_pin_requested = true;
	else if(find_after(msg, PV_DIAG_P_LOGIN_PIN_INCORRECT))
	{
		d->login_pin_requested = true;
		d->login_pin_incorrect = true;
	}
	else if(find_after(msg, PV_DIAG_P_SENKUSHA_TAKION_FAILED) || find_after(msg, PV_DIAG_P_SENKUSHA_TIMEOUT))
		d->senkusha_failed = true;
	else if(find_after(msg, PV_DIAG_P_STREAM_TAKION_FAILED))
		d->stream_takion_failed = true;
	else if(find_after(msg, PV_DIAG_P_STREAM_BANG_TIMEOUT))
		d->stream_bang_timeout = true;
	else if(find_after(msg, PV_DIAG_P_STREAM_REMOTE_DISCONNECT))
		d->stream_remote_disconnect = true;
}

void pv_diag_log_hook(ChiakiLogLevel level, const char *msg, void *user)
{
	pv_diag_feed((PvDiag *)user, level, msg);
}
