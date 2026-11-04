// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "oauth.h"

#include "util.h"

#include <json-c/json.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *pv_oauth_login_url(const char *authorize_url)
{
	// Same parameters as chiaki-ng's scripts/psn-account-id.py (scope psn:clientapp
	// is enough to read the account id).
	return pv_asprintf("%s?service_entity=urn:service-entity:psn&response_type=code&client_id=%s"
			"&redirect_uri=%s&scope=psn:clientapp&request_locale=de_DE&ui=pr&service_logo=ps"
			"&layout_type=popup&smcid=remoteplay&prompt=always&PlatformPrivacyWs1=minimal&",
			authorize_url ? authorize_url : PV_PSN_AUTHORIZE_URL, PV_PSN_CLIENT_ID, PV_PSN_REDIRECT_URI);
}

static int hexval(char c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void url_decode(const char *in, size_t len, char *out, size_t out_size)
{
	size_t o = 0;
	for(size_t i = 0; i < len && o + 1 < out_size; i++)
	{
		char c = in[i];
		if(c == '%' && i + 2 < len && hexval(in[i + 1]) >= 0 && hexval(in[i + 2]) >= 0)
		{
			out[o++] = (char)(hexval(in[i + 1]) * 16 + hexval(in[i + 2]));
			i += 2;
		}
		else if(c == '+')
			out[o++] = ' ';
		else
			out[o++] = c;
	}
	out[o] = '\0';
}

// Finds "key=value" in a query string (between '?' and '#') and decodes value.
static bool query_param(const char *query, const char *key, char *out, size_t out_size)
{
	size_t klen = strlen(key);
	const char *p = query;
	while(p && *p && *p != '#')
	{
		const char *end = p;
		while(*end && *end != '&' && *end != '#')
			end++;
		if((size_t)(end - p) > klen && strncmp(p, key, klen) == 0 && p[klen] == '=')
		{
			url_decode(p + klen + 1, (size_t)(end - p - (ptrdiff_t)klen - 1), out, out_size);
			return true;
		}
		p = *end == '&' ? end + 1 : NULL;
	}
	return false;
}

PvRedirectResult pv_oauth_extract_code(const char *input, char *code, size_t code_size, char *error, size_t error_size)
{
	code[0] = '\0';
	if(error_size)
		error[0] = '\0';
	if(!input)
		return PV_REDIRECT_EMPTY;
	char *copy = pv_strdup(input);
	if(!copy)
		return PV_REDIRECT_EMPTY;
	char *s = pv_str_trim(copy);
	// people sometimes paste with surrounding quotes or <>
	size_t n = strlen(s);
	if(n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '<' && s[n - 1] == '>') || (s[0] == '\'' && s[n - 1] == '\'')))
	{
		s[n - 1] = '\0';
		s++;
	}
	PvRedirectResult r;
	if(!*s)
		r = PV_REDIRECT_EMPTY;
	else
	{
		const char *q = strchr(s, '?');
		if(q)
		{
			char tmp[512];
			if(query_param(q + 1, "code", tmp, sizeof(tmp)) && tmp[0])
			{
				pv_strlcpy(code, tmp, code_size);
				r = PV_REDIRECT_OK;
			}
			else if(query_param(q + 1, "error", tmp, sizeof(tmp)))
			{
				char desc[256] = "";
				query_param(q + 1, "error_description", desc, sizeof(desc));
				snprintf(error, error_size, "%s%s%s", tmp, desc[0] ? ": " : "", desc);
				r = PV_REDIRECT_ERROR_PARAM;
			}
			else
				r = PV_REDIRECT_NO_CODE;
		}
		else if(strstr(s, "://") || strchr(s, '/') || strchr(s, ' '))
			r = PV_REDIRECT_NO_CODE; // an URL without query, e.g. the login page itself
		else
		{
			// just the code itself
			bool plausible = strlen(s) >= 4;
			for(const char *p = s; *p && plausible; p++)
				if(!(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_'))
					plausible = false;
			if(plausible)
			{
				pv_strlcpy(code, s, code_size);
				r = PV_REDIRECT_OK;
			}
			else
				r = PV_REDIRECT_NO_CODE;
		}
	}
	free(copy);
	return r;
}

bool pv_oauth_user_id_to_account_id(const char *user_id, uint8_t raw[8], char *b64, size_t b64_size)
{
	if(!user_id || !*user_id)
		return false;
	for(const char *p = user_id; *p; p++)
		if(*p < '0' || *p > '9')
			return false;
	errno = 0;
	char *end = NULL;
	unsigned long long v = strtoull(user_id, &end, 10);
	if(errno || !end || *end)
		return false;
	for(int i = 0; i < 8; i++)
		raw[i] = (uint8_t)((v >> (8 * i)) & 0xff);
	return pv_base64_encode(raw, 8, b64, b64_size);
}

void pv_oauth_classify_error(long http_status, const char *body, PvOAuthError *out)
{
	memset(out, 0, sizeof(*out));
	out->http_status = http_status;
	out->kind = PV_OAUTH_ERR_OTHER;

	json_object *root = body ? json_tokener_parse(body) : NULL;
	if(root && json_object_is_type(root, json_type_object))
	{
		json_object *v;
		if(json_object_object_get_ex(root, "error", &v) && json_object_is_type(v, json_type_string))
			pv_strlcpy(out->error, json_object_get_string(v), sizeof(out->error));
		if(json_object_object_get_ex(root, "error_code", &v) && (json_object_is_type(v, json_type_int) || json_object_is_type(v, json_type_string)))
			out->error_code = json_object_is_type(v, json_type_int) ? (long)json_object_get_int64(v) : strtol(json_object_get_string(v), NULL, 10);
		if(json_object_object_get_ex(root, "error_description", &v) && json_object_is_type(v, json_type_string))
			pv_strlcpy(out->description, json_object_get_string(v), sizeof(out->description));
	}
	else if(http_status >= 200 && http_status < 300)
	{
		if(root)
			json_object_put(root);
		out->kind = PV_OAUTH_ERR_NOT_JSON;
		return;
	}
	bool is_json = root != NULL;
	if(root)
		json_object_put(root);

	if(out->error_code == 4177 || pv_strcasecmp(out->error, "too_many_requests") == 0 || http_status == 429)
		out->kind = PV_OAUTH_ERR_TOO_MANY_REQUESTS;
	else if(pv_strcasecmp(out->error, "invalid_grant") == 0)
		out->kind = PV_OAUTH_ERR_CODE_INVALID;
	else if(pv_strcasecmp(out->error, "invalid_client") == 0 || pv_strcasecmp(out->error, "unauthorized_client") == 0)
		out->kind = PV_OAUTH_ERR_CLIENT;
	else if(pv_strcasecmp(out->error, "invalid_request") == 0)
		out->kind = PV_OAUTH_ERR_REQUEST;
	else if(http_status >= 500)
		out->kind = PV_OAUTH_ERR_SERVER;
	else if(!is_json)
		out->kind = PV_OAUTH_ERR_NOT_JSON;
	else if(http_status >= 200 && http_status < 300)
		out->kind = PV_OAUTH_ERR_NONE;
}
