// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// ps5view account-id [--no-browser] [--redirect-url <url>]
//
// Determines the PSN account id needed for registration:
// browser login -> redirect URL with ?code=... -> token -> user_id ->
// 8 byte little endian -> base64.

#include "app.h"
#include "args.h"
#include "log.h"
#include "oauth.h"
#include "pv_version.h"
#include "util.h"

#include <curl/curl.h>
#include <json-c/json.h>

#include <stdlib.h>
#include <string.h>

#define HTTP_MAX_BODY (1024 * 1024)

typedef struct
{
	char *data;
	size_t len;
	bool overflow;
} Buffer;

typedef struct
{
	long status;
	char *body;
	CURLcode curl_err;
	char errbuf[CURL_ERROR_SIZE];
} HttpResult;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *user)
{
	Buffer *b = user;
	size_t n = size * nmemb;
	if(b->len + n > HTTP_MAX_BODY)
	{
		b->overflow = true;
		return 0;
	}
	char *nd = realloc(b->data, b->len + n + 1);
	if(!nd)
		return 0;
	b->data = nd;
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	b->data[b->len] = '\0';
	return n;
}

static bool http_request(const char *url, const char *post_body, long timeout_s, HttpResult *res)
{
	memset(res, 0, sizeof(*res));
	CURL *c = curl_easy_init();
	if(!c)
	{
		res->curl_err = CURLE_FAILED_INIT;
		return false;
	}
	Buffer buf = { 0 };
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Accept: application/json");
	if(post_body)
		headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
	curl_easy_setopt(c, CURLOPT_USERNAME, PV_PSN_CLIENT_ID);
	curl_easy_setopt(c, CURLOPT_PASSWORD, PV_PSN_CLIENT_SECRET);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "ps5view/" PV_VERSION);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, res->errbuf);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, pv_str_starts_with(url, "http://") ? "http" : "https");
	const char *ca = getenv("CURL_CA_BUNDLE");
	if(ca && *ca)
		curl_easy_setopt(c, CURLOPT_CAINFO, ca);
	if(post_body)
	{
		curl_easy_setopt(c, CURLOPT_POST, 1L);
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body);
	}
	res->curl_err = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &res->status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(c);
	if(buf.overflow)
	{
		free(buf.data);
		res->curl_err = CURLE_FILESIZE_EXCEEDED;
		return false;
	}
	res->body = buf.data ? buf.data : pv_strdup("");
	return res->curl_err == CURLE_OK;
}

static void report_curl_error(const HttpResult *r, const char *url)
{
	char host[256] = "";
	const char *p = strstr(url, "://");
	if(p)
	{
		p += 3;
		size_t n = strcspn(p, "/:?");
		pv_strlcpy(host, p, n + 1 < sizeof(host) ? n + 1 : sizeof(host));
	}
	const char *detail = r->errbuf[0] ? r->errbuf : curl_easy_strerror(r->curl_err);
	switch(r->curl_err)
	{
		case CURLE_COULDNT_RESOLVE_HOST:
		case CURLE_COULDNT_RESOLVE_PROXY:
			LOGE("account", "Der Server %s kann nicht gefunden werden (DNS). Besteht eine Internetverbindung?", host);
			break;
		case CURLE_COULDNT_CONNECT:
			LOGE("account", "Keine Verbindung zu %s. Internetverbindung, Firewall oder Proxy prüfen.", host);
			break;
		case CURLE_OPERATION_TIMEDOUT:
			LOGE("account", "Zeitüberschreitung bei der Verbindung zu %s.", host);
			break;
		case CURLE_PEER_FAILED_VERIFICATION:
		case CURLE_SSL_CONNECT_ERROR:
		case CURLE_SSL_CACERT_BADFILE:
		case CURLE_SSL_CERTPROBLEM:
			LOGE("account", "Sichere Verbindung (TLS) zu %s fehlgeschlagen: %s", host, detail);
			LOGI("hint", "Was du tun kannst:\n- Stimmen Datum und Uhrzeit des PCs?\n- Prüft ein Virenscanner HTTPS-Verbindungen? Testweise deaktivieren.");
			return;
		default:
			LOGE("account", "Netzwerkfehler bei %s: %s", host, detail);
			if(strstr(detail, "CONNECT tunnel failed") || strstr(detail, "proxy") || strstr(detail, "Proxy"))
				LOGI("hint", "Was du tun kannst:\n- Ein Proxy (Umgebungsvariable HTTPS_PROXY/https_proxy) blockiert die Verbindung zu %s.\n"
						"  Proxy-Einstellung prüfen oder ohne Proxy versuchen.", host);
			break;
	}
	LOGD("account", "curl-Fehler %d: %s", (int)r->curl_err, detail);
}

static int report_oauth_error(const HttpResult *r, const char *step)
{
	PvOAuthError e;
	pv_oauth_classify_error(r->status, r->body, &e);
	LOGD("account", "%s: HTTP %ld, error=\"%s\", error_code=%ld, description=\"%s\"", step, r->status, e.error, e.error_code, e.description);
	switch(e.kind)
	{
		case PV_OAUTH_ERR_TOO_MANY_REQUESTS:
			LOGE("account", "Sony meldet \"too_many_requests\"%s: zu viele Anmeldeversuche in kurzer Zeit.",
					e.error_code ? " (Fehler 4177)" : "");
			LOGI("hint", "Was du tun kannst:\n"
					"- Mindestens 15-60 Minuten warten, dann genau einmal neu versuchen. Jeder weitere Versuch während\n"
					"  der Sperre verlängert sie eventuell.\n"
					"- Nicht parallel in anderen Programmen (chiaki-ng, Skripte) die Account-ID abfragen.\n"
					"- Alternativ ein anderes Netz (z. B. Handy-Hotspot) verwenden.");
			return PV_EXIT_AUTH;
		case PV_OAUTH_ERR_CODE_INVALID:
			LOGE("account", "Der Anmelde-Code ist abgelaufen oder wurde schon benutzt (%s%s%s).", e.error,
					e.description[0] ? ": " : "", e.description);
			LOGI("hint", "Was du tun kannst:\n"
					"- Den Code gibt es nur einmal und nur für kurze Zeit. ps5view account-id erneut starten,\n"
					"  neu anmelden und die Redirect-URL sofort (innerhalb von 1-2 Minuten) einfügen.\n"
					"- Die Seite nach der Anmeldung nicht neu laden, die URL nicht zweimal verwenden.");
			return PV_EXIT_AUTH;
		case PV_OAUTH_ERR_REQUEST:
			LOGE("account", "Sony lehnt die Anfrage ab (%s%s%s) - vermutlich wurde der Code unvollständig kopiert.",
					e.error, e.description[0] ? ": " : "", e.description);
			LOGI("hint", "Was du tun kannst:\n- Die komplette URL aus der Adresszeile kopieren (beginnt mit https://remoteplay.dl.playstation.net/...).");
			return PV_EXIT_AUTH;
		case PV_OAUTH_ERR_CLIENT:
			LOGE("account", "Sony akzeptiert die Remote-Play-Client-Kennung nicht mehr (%s). Eventuell hat Sony sie geändert.", e.error);
			LOGI("hint", "Was du tun kannst:\n- Im chiaki-ng-Projekt nachsehen, ob es eine neue Client-ID gibt: " PV_CHIAKI_URL);
			return PV_EXIT_AUTH;
		case PV_OAUTH_ERR_SERVER:
			LOGE("account", "Sony-Server-Fehler (HTTP %ld). Später erneut versuchen.", r->status);
			return PV_EXIT_NETWORK;
		case PV_OAUTH_ERR_NOT_JSON:
			LOGE("account", "Unerwartete Antwort von Sony (HTTP %ld, kein JSON).", r->status);
			LOGD("account", "Antwort (gekürzt): %.300s", r->body ? r->body : "");
			return PV_EXIT_NETWORK;
		default:
			LOGE("account", "Sony meldet einen Fehler bei \"%s\": HTTP %ld %s%s%s", step, r->status, e.error,
					e.description[0] ? " - " : "", e.description);
			return PV_EXIT_AUTH;
	}
}

static void usage(void)
{
	pv_out("Aufruf: ps5view account-id [--no-browser] [--redirect-url <url>]");
	pv_out("%s", "");
	pv_out("Ermittelt die PSN-Account-ID (base64), die für \"ps5view register\" gebraucht wird.");
	pv_out("Ablauf: Im Browser bei Sony anmelden (mit dem PSN-Konto, das an der PS5 angemeldet ist),");
	pv_out("danach die URL aus der Adresszeile in ps5view einfügen.");
	pv_out("%s", "");
	pv_out("  --no-browser           Browser nicht automatisch öffnen, nur die Login-URL anzeigen");
	pv_out("  --redirect-url <url>   Redirect-URL (oder nur den Code) direkt übergeben statt einzufügen");
}

int pv_cmd_account_id(int argc, char **argv)
{
	bool no_browser = false;
	const char *redirect = NULL;
	PvArgs a;
	pv_args_init(&a, "account-id", argc, argv);
	while(pv_args_next(&a))
	{
		if(pv_args_flag(&a, "--no-browser"))
		{
			no_browser = true;
			continue;
		}
		if(pv_args_value(&a, "--redirect-url", &redirect))
			continue;
		if(pv_args_is_help(&a))
		{
			usage();
			return PV_EXIT_OK;
		}
		return pv_args_unknown(&a);
	}
	if(a.failed)
		return PV_EXIT_USAGE;

	const char *token_url = getenv(PV_PSN_TOKEN_URL_ENV);
	char *authorize_url = NULL;
	if(token_url && *token_url)
	{
		LOGW("account", "Testmodus: Token-URL aus %s = %s", PV_PSN_TOKEN_URL_ENV, token_url);
		const char *slash = strrchr(token_url, '/');
		authorize_url = slash ? pv_asprintf("%.*s/authorize", (int)(slash - token_url), token_url) : pv_strdup(token_url);
	}
	else
	{
		token_url = PV_PSN_TOKEN_URL;
		authorize_url = pv_strdup(PV_PSN_AUTHORIZE_URL);
	}

	int rc = PV_EXIT_OK;
	char *login_url = pv_oauth_login_url(authorize_url);
	char *line = NULL;
	char *body = NULL;
	char *info_url = NULL;
	char *esc_code = NULL;
	char *esc_redirect = NULL;
	CURL *esc = NULL;
	HttpResult tok = { 0 }, info = { 0 };
	json_object *tok_json = NULL, *info_json = NULL;
	bool curl_inited = false;

	char code[512];
	char perr[300];
	if(!redirect)
	{
		pv_out("1. Diese Adresse im Browser öffnen und mit dem PSN-Konto anmelden, das an der PS5 angemeldet ist:");
		pv_out("%s", "");
		pv_out("%s", login_url);
		pv_out("%s", "");
		if(!no_browser)
		{
			if(pv_open_url(login_url))
				pv_out("   (Der Browser wurde geöffnet.)");
			else
				pv_out("   (Browser konnte nicht automatisch geöffnet werden - Adresse bitte kopieren.)");
		}
		pv_out("2. Nach der Anmeldung landet der Browser auf einer fast leeren Seite (oft nur \"redirect\").");
		pv_out("   Die KOMPLETTE Adresse aus der Adresszeile kopieren. Sie beginnt mit");
		pv_out("   %s?code=...", PV_PSN_REDIRECT_URI);
		pv_out("3. Hier einfügen und Enter drücken (der Code ist nur kurz gültig):");
		fputs("> ", stdout);
		fflush(stdout);
		line = pv_read_line();
		if(!line)
		{
			LOGE("account", "Keine Eingabe erhalten.");
			rc = pv_interrupted() ? PV_EXIT_INTERRUPTED : PV_EXIT_USAGE;
			goto out;
		}
		redirect = line;
	}

	switch(pv_oauth_extract_code(redirect, code, sizeof(code), perr, sizeof(perr)))
	{
		case PV_REDIRECT_OK:
			break;
		case PV_REDIRECT_EMPTY:
			LOGE("account", "Leere Eingabe. Bitte die Redirect-URL aus der Adresszeile einfügen.");
			rc = PV_EXIT_USAGE;
			goto out;
		case PV_REDIRECT_ERROR_PARAM:
			LOGE("account", "Sony hat die Anmeldung nicht abgeschlossen: %s", perr);
			LOGI("hint", "Was du tun kannst:\n- Login erneut durchführen; bei \"access_denied\" wurde die Anmeldung abgebrochen.");
			rc = PV_EXIT_AUTH;
			goto out;
		case PV_REDIRECT_NO_CODE:
			LOGE("account", "In der Eingabe ist kein \"code=\" enthalten.");
			LOGI("hint", "Was du tun kannst:\n"
					"- Erst NACH der Anmeldung die Adresse kopieren; sie beginnt mit " PV_PSN_REDIRECT_URI "?code=...\n"
					"- Die Login-Adresse selbst (auth.api.sonyentertainmentnetwork.com/...) ist nicht gemeint.");
			rc = PV_EXIT_USAGE;
			goto out;
	}
	LOGD("account", "Code erhalten (%zu Zeichen)", strlen(code));

	if(curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
	{
		LOGE("account", "libcurl konnte nicht initialisiert werden.");
		rc = PV_EXIT_INTERNAL;
		goto out;
	}
	curl_inited = true;
	esc = curl_easy_init();
	if(!esc)
	{
		rc = PV_EXIT_INTERNAL;
		goto out;
	}
	esc_code = curl_easy_escape(esc, code, 0);
	esc_redirect = curl_easy_escape(esc, PV_PSN_REDIRECT_URI, 0);
	body = pv_asprintf("grant_type=authorization_code&code=%s&redirect_uri=%s&", esc_code, esc_redirect);

	LOGI("account", "Tausche den Code bei Sony gegen ein Token ...");
	if(!http_request(token_url, body, 30, &tok))
	{
		report_curl_error(&tok, token_url);
		rc = PV_EXIT_NETWORK;
		goto out;
	}
	if(tok.status != 200)
	{
		rc = report_oauth_error(&tok, "Token");
		goto out;
	}
	tok_json = json_tokener_parse(tok.body);
	json_object *v;
	if(!tok_json || !json_object_object_get_ex(tok_json, "access_token", &v) || !json_object_is_type(v, json_type_string))
	{
		LOGE("account", "Sony hat kein Zugriffstoken geliefert (unerwartete Antwort).");
		rc = PV_EXIT_NETWORK;
		goto out;
	}
	char *esc_token = curl_easy_escape(esc, json_object_get_string(v), 0);
	info_url = pv_asprintf("%s/%s", token_url, esc_token ? esc_token : "");
	curl_free(esc_token);

	LOGI("account", "Frage die Kontodaten ab ...");
	if(!http_request(info_url, NULL, 30, &info))
	{
		report_curl_error(&info, token_url);
		rc = PV_EXIT_NETWORK;
		goto out;
	}
	if(info.status != 200)
	{
		rc = report_oauth_error(&info, "Kontodaten");
		goto out;
	}
	info_json = json_tokener_parse(info.body);
	const char *user_id = NULL;
	if(info_json && json_object_object_get_ex(info_json, "user_id", &v))
	{
		if(json_object_is_type(v, json_type_string))
			user_id = json_object_get_string(v);
		else if(json_object_is_type(v, json_type_int))
			user_id = json_object_to_json_string(v);
	}
	uint8_t raw[8];
	char b64[32];
	if(!user_id || !pv_oauth_user_id_to_account_id(user_id, raw, b64, sizeof(b64)))
	{
		LOGE("account", "Die Antwort von Sony enthält keine gültige \"user_id\".");
		rc = PV_EXIT_NETWORK;
		goto out;
	}
	const char *online_id = NULL;
	if(json_object_object_get_ex(info_json, "online_id", &v) && json_object_is_type(v, json_type_string))
		online_id = json_object_get_string(v);

	pv_out("%s", "");
	if(online_id)
		pv_out("PSN-Konto:   %s", online_id);
	pv_out("Account-ID:  %s", b64);
	pv_out("%s", "");
	pv_out("Nächster Schritt: An der PS5 \"Einstellungen > System > Remote Play > Gerät verknüpfen\" öffnen und dann:");
	pv_out("  ps5view register --host <IP der PS5> --account-id %s --pin <8-stellige PIN>", b64);

out:
	if(tok_json)
		json_object_put(tok_json);
	if(info_json)
		json_object_put(info_json);
	free(tok.body);
	free(info.body);
	if(esc_code)
		curl_free(esc_code);
	if(esc_redirect)
		curl_free(esc_redirect);
	if(esc)
		curl_easy_cleanup(esc);
	if(curl_inited)
		curl_global_cleanup();
	free(body);
	free(info_url);
	free(line);
	free(login_url);
	free(authorize_url);
	return rc;
}
