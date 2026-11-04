// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Unit tests for ps5view. Run: ps5view-tests (or ctest).

#include "audio.h"
#include "config.h"
#include "diag.h"
#include "discover.h"
#include "errors.h"
#include "log.h"
#include "oauth.h"
#include "register.h"
#include "util.h"
#include "video.h"

#include <chiaki/common.h>

#include <libavcodec/avcodec.h>
#include <opus/opus.h>

#include <math.h>

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;

#define CHECK(cond) do { checks++; if(!(cond)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while(0)
#define CHECK_STR(a, b) do { checks++; const char *_a = (a), *_b = (b); \
	if(!_a || !_b || strcmp(_a, _b) != 0) { failures++; \
	fprintf(stderr, "%s:%d: CHECK_STR failed: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); } } while(0)

// ---------------------------------------------------------------------------

static void test_util(void)
{
	uint8_t buf[16];
	char hex[64];
	const uint8_t data[4] = { 0x00, 0xab, 0x10, 0xff };
	CHECK(pv_hex_encode(data, 4, hex, sizeof(hex)));
	CHECK_STR(hex, "00ab10ff");
	CHECK(pv_hex_decode("00AB10ff", buf, 4) && memcmp(buf, data, 4) == 0);
	CHECK(!pv_hex_decode("00ab10f", buf, 4));
	CHECK(!pv_hex_decode("00ab10fg", buf, 4));
	CHECK(!pv_hex_decode("00ab10ff00", buf, 4));

	char b64[32];
	const uint8_t acc[8] = { 0xca, 0x93, 0x91, 0xd1, 0xb9, 0x27, 0xce, 0x5a };
	CHECK(pv_base64_encode(acc, 8, b64, sizeof(b64)));
	CHECK_STR(b64, "ypOR0bknzlo=");
	CHECK(pv_base64_decode_exact("ypOR0bknzlo=", buf, 8) && memcmp(buf, acc, 8) == 0);
	CHECK(pv_base64_decode_exact(" ypOR0bknzlo=\n", buf, 8));
	CHECK(pv_base64_decode_exact("ypOR0bknzlo", buf, 8)); // missing padding is tolerated
	CHECK(!pv_base64_decode_exact("ypOR0bknzl", buf, 8));       // too short
	CHECK(!pv_base64_decode_exact("ypOR0bknzloAAA==", buf, 8)); // too long
	CHECK(!pv_base64_decode_exact("ypOR0bkn\xc3\xa4lo=", buf, 8)); // non-ASCII
	CHECK(!pv_base64_decode_exact("ypOR0bknzlo=x", buf, 8));     // data after padding
	CHECK(!pv_base64_decode_exact("", buf, 8));

	unsigned long v;
	CHECK(pv_parse_ulong("60", 30, 60, &v) && v == 60);
	CHECK(!pv_parse_ulong("61", 30, 60, &v));
	CHECK(!pv_parse_ulong("-1", 0, 60, &v));
	CHECK(!pv_parse_ulong("5x", 0, 60, &v));
	CHECK(!pv_parse_ulong("", 0, 60, &v));

	CHECK(pv_str_icontains("CABLE Input (VB-Audio Virtual Cable)", "cable input"));
	CHECK(!pv_str_icontains("Lautsprecher", "cable"));
	CHECK(pv_strcasecmp("AbC", "aBc") == 0);
	char t[] = "  x y \r\n";
	CHECK_STR(pv_str_trim(t), "x y");

	char *j = pv_path_join("a", "b");
#ifdef _WIN32
	CHECK_STR(j, "a\\b");
#else
	CHECK_STR(j, "a/b");
#endif
	free(j);
	char *d = pv_path_dirname("/x/y/config.json");
	CHECK_STR(d, "/x/y");
	free(d);
}

static void test_oauth(void)
{
	char code[128], err[128];
	CHECK(pv_oauth_extract_code("https://remoteplay.dl.playstation.net/remoteplay/redirect?code=v3.AbC123&cid=xyz", code, sizeof(code), err, sizeof(err)) == PV_REDIRECT_OK);
	CHECK_STR(code, "v3.AbC123");
	CHECK(pv_oauth_extract_code("  \"https://x/redirect?cid=1&code=a%2Bb%3D#frag\"  ", code, sizeof(code), err, sizeof(err)) == PV_REDIRECT_OK);
	CHECK_STR(code, "a+b=");
	CHECK(pv_oauth_extract_code("v3.ABCDEF", code, sizeof(code), err, sizeof(err)) == PV_REDIRECT_OK);
	CHECK_STR(code, "v3.ABCDEF");
	CHECK(pv_oauth_extract_code("https://x/redirect?error=access_denied&error_description=User+denied", code, sizeof(code), err, sizeof(err)) == PV_REDIRECT_ERROR_PARAM);
	CHECK_STR(err, "access_denied: User denied");
	CHECK(pv_oauth_extract_code("https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize", code, sizeof(code), err, sizeof(err)) == PV_REDIRECT_NO_CODE);
	CHECK(pv_oauth_extract_code("https://x/redirect?code=", code, sizeof(code), err, sizeof(err)) == PV_REDIRECT_NO_CODE);
	CHECK(pv_oauth_extract_code("   ", code, sizeof(code), err, sizeof(err)) == PV_REDIRECT_EMPTY);
	CHECK(pv_oauth_extract_code("das ist kein code", code, sizeof(code), err, sizeof(err)) == PV_REDIRECT_NO_CODE);

	uint8_t raw[8];
	char b64[32];
	CHECK(pv_oauth_user_id_to_account_id("6543210987654321098", raw, b64, sizeof(b64)));
	CHECK_STR(b64, "ypOR0bknzlo="); // python: base64(int.to_bytes(8, "little"))
	CHECK(pv_oauth_user_id_to_account_id("1", raw, b64, sizeof(b64)));
	CHECK_STR(b64, "AQAAAAAAAAA=");
	CHECK(!pv_oauth_user_id_to_account_id("12a", raw, b64, sizeof(b64)));
	CHECK(!pv_oauth_user_id_to_account_id("99999999999999999999999", raw, b64, sizeof(b64)));
	CHECK(!pv_oauth_user_id_to_account_id("", raw, b64, sizeof(b64)));

	PvOAuthError e;
	pv_oauth_classify_error(400, "{\"error\":\"too_many_requests\",\"error_code\":4177}", &e);
	CHECK(e.kind == PV_OAUTH_ERR_TOO_MANY_REQUESTS && e.error_code == 4177);
	pv_oauth_classify_error(403, "{\"error\":\"access_denied\",\"error_code\":4177}", &e);
	CHECK(e.kind == PV_OAUTH_ERR_TOO_MANY_REQUESTS);
	pv_oauth_classify_error(429, "", &e);
	CHECK(e.kind == PV_OAUTH_ERR_TOO_MANY_REQUESTS);
	pv_oauth_classify_error(400, "{\"error\":\"invalid_grant\",\"error_description\":\"Invalid authorization code\"}", &e);
	CHECK(e.kind == PV_OAUTH_ERR_CODE_INVALID);
	CHECK_STR(e.description, "Invalid authorization code");
	pv_oauth_classify_error(401, "{\"error\":\"invalid_client\"}", &e);
	CHECK(e.kind == PV_OAUTH_ERR_CLIENT);
	pv_oauth_classify_error(400, "{\"error\":\"invalid_request\"}", &e);
	CHECK(e.kind == PV_OAUTH_ERR_REQUEST);
	pv_oauth_classify_error(503, "<html>down</html>", &e);
	CHECK(e.kind == PV_OAUTH_ERR_SERVER);
	pv_oauth_classify_error(403, "<html>blocked</html>", &e);
	CHECK(e.kind == PV_OAUTH_ERR_NOT_JSON);

	char *url = pv_oauth_login_url(NULL);
	CHECK(url && strstr(url, "client_id=" PV_PSN_CLIENT_ID) && strstr(url, "response_type=code") && strstr(url, "redirect_uri=" PV_PSN_REDIRECT_URI));
	free(url);
}

static void test_diag(void)
{
	PvDiagInfo d;
	memset(&d, 0, sizeof(d));
	pv_diag_parse(&d, CHIAKI_LOG_ERROR, "Reported Application Reason: 0x80108b12 (unknown)");
	CHECK(d.has_app_reason && d.app_reason == 0x80108b12);
	CHECK(d.error_count == 1);
	CHECK_STR(d.last_error, "Reported Application Reason: 0x80108b12 (unknown)");

	memset(&d, 0, sizeof(d));
	pv_diag_parse(&d, CHIAKI_LOG_ERROR, "Regist received HTTP code 403");
	pv_diag_parse(&d, CHIAKI_LOG_ERROR, "Reported Application Reason: 0x80108b09 (Regist failed, probably invalid PIN)");
	CHECK(d.regist_http_code == 403 && d.app_reason == 0x80108b09);

	memset(&d, 0, sizeof(d));
	pv_diag_parse(&d, CHIAKI_LOG_ERROR, "Session request connect failed: Connection Refused");
	CHECK(d.session_connect_failed && d.session_connect_refused && !d.session_connect_timeout);
	memset(&d, 0, sizeof(d));
	pv_diag_parse(&d, CHIAKI_LOG_ERROR, "Session request connect failed: Timeout");
	CHECK(d.session_connect_failed && d.session_connect_timeout);

	memset(&d, 0, sizeof(d));
	pv_diag_parse(&d, CHIAKI_LOG_ERROR, "Regist timed out waiting for search response");
	CHECK(d.regist_search_timeout);
	pv_diag_parse(&d, CHIAKI_LOG_ERROR, "The console refused the registration search, keep its Link Device (PS5) or Add Device (PS4) screen with the code open while registering");
	CHECK(d.regist_search_refused);
	pv_diag_parse(&d, CHIAKI_LOG_ERROR, "Ctrl http response was not successful. HTTP code was 403");
	CHECK(d.ctrl_http_code == 403);
	pv_diag_parse(&d, CHIAKI_LOG_ERROR, "StreamConnection Takion connect failed");
	CHECK(d.stream_takion_failed);
	pv_diag_parse(&d, CHIAKI_LOG_ERROR, "Senkusha Takion connect failed");
	CHECK(d.senkusha_failed);
	pv_diag_parse(&d, CHIAKI_LOG_INFO, "Login PIN was incorrect, requested again by Ctrl");
	CHECK(d.login_pin_requested && d.login_pin_incorrect);
	pv_diag_parse(&d, CHIAKI_LOG_INFO, "nothing interesting");
	CHECK(d.error_count == 5);
}

// The diag patterns must still exist in the pinned chiaki-ng sources.
static void test_diag_patterns_in_chiaki_source(void)
{
#ifdef PV_CHIAKI_SOURCE_DIR
	for(size_t i = 0; i < pv_diag_patterns_count; i++)
	{
		char *path = pv_path_join(PV_CHIAKI_SOURCE_DIR, pv_diag_patterns[i].source);
		size_t len;
		char *src = pv_read_file(path, 4u << 20, &len, NULL);
		checks++;
		if(!src)
		{
			failures++;
			fprintf(stderr, "cannot read %s\n", path);
		}
		else if(!strstr(src, pv_diag_patterns[i].text))
		{
			failures++;
			fprintf(stderr, "pattern \"%s\" not found in %s - libchiaki changed, update diag.c\n", pv_diag_patterns[i].text, path);
		}
		free(src);
		free(path);
	}
#endif
}

static void test_errors(void)
{
	static const uint32_t codes[] = { 0x80108b02, 0x80108b09, 0x80108b10, 0x80108b11, 0x80108b12, 0x80108b13, 0x80108b15, 0x80108bff };
	for(size_t i = 0; i < PV_ARRAY_SIZE(codes); i++)
	{
		const PvAppReason *r = pv_app_reason_lookup(codes[i]);
		CHECK(r && r->message && r->hint && r->hint[0]);
	}
	CHECK(!pv_app_reason_lookup(0x80108b99));

	PvFailure f;
	PvDiagInfo d;
	memset(&d, 0, sizeof(d));
	d.has_app_reason = true;
	d.app_reason = 0x80108b12;
	pv_failure_from_session(&f, CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN, NULL, &d, "192.168.178.88");
	CHECK(f.failed && f.retry == PV_RETRY_NO && strstr(f.message, "0x80108b12") && strstr(f.hint, "Remote Play aktivieren"));

	d.app_reason = 0x80108b10;
	pv_failure_from_session(&f, CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE, NULL, &d, "h");
	CHECK(f.failed && f.retry == PV_RETRY_SLOW);

	d.app_reason = 0x80108b77; // unknown code: retried normally
	pv_failure_from_session(&f, CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN, NULL, &d, "h");
	CHECK(f.failed && f.retry == PV_RETRY_FAST && strstr(f.message, "0x80108b77"));

	memset(&d, 0, sizeof(d));
	pv_failure_from_session(&f, CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED, NULL, &d, "h");
	CHECK(f.failed && f.retry == PV_RETRY_FAST && strstr(f.hint, "--wake"));

	d.session_connect_failed = d.session_connect_timeout = true;
	pv_failure_from_session(&f, CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN, NULL, &d, "h");
	CHECK(f.failed && strstr(f.message, "Zeitüberschreitung"));

	memset(&d, 0, sizeof(d));
	d.ctrl_http_code = 403;
	pv_failure_from_session(&f, CHIAKI_QUIT_REASON_CTRL_UNKNOWN, NULL, &d, "h");
	CHECK(f.failed && f.retry == PV_RETRY_NO && f.exit_code == 4);

	memset(&d, 0, sizeof(d));
	d.stream_takion_failed = true;
	d.senkusha_failed = true;
	pv_failure_from_session(&f, CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN, NULL, &d, "h");
	CHECK(f.failed && strstr(f.message, "UDP") && strstr(f.hint, "Firewall") && strstr(f.hint, "9297"));

	pv_failure_from_session(&f, CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED, "Remote Play session ended", &d, "h");
	CHECK(f.failed && strstr(f.message, "Remote Play session ended"));

	pv_failure_from_session(&f, CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN, "Server shutting down", &d, "h");
	CHECK(!f.failed && f.exit_code == 0 && f.message[0]);

	pv_failure_from_session(&f, CHIAKI_QUIT_REASON_STOPPED, NULL, &d, "h");
	CHECK(!f.failed && !f.message[0]);

	memset(&d, 0, sizeof(d));
	d.regist_search_timeout = true;
	pv_failure_from_regist(&f, &d, "h");
	CHECK(f.failed && strstr(f.message, "UDP 9295"));
	memset(&d, 0, sizeof(d));
	d.regist_search_refused = true;
	pv_failure_from_regist(&f, &d, "h");
	CHECK(f.failed && strstr(f.message, "Gerät verknüpfen"));
	memset(&d, 0, sizeof(d));
	d.has_app_reason = true;
	d.app_reason = 0x80108b09;
	pv_failure_from_regist(&f, &d, "h");
	CHECK(f.failed && f.exit_code == 4 && strstr(f.message, "PIN"));
}

static void make_profile(PvProfile *p, const char *name, const char *host)
{
	memset(p, 0, sizeof(*p));
	pv_strlcpy(p->name, name, sizeof(p->name));
	pv_strlcpy(p->host, host, sizeof(p->host));
	p->ps5 = true;
	p->target = 1000100;
	pv_strlcpy(p->nickname, "PS5-123", sizeof(p->nickname));
	for(int i = 0; i < 16; i++)
	{
		p->regist_key[i] = i < 8 ? (uint8_t)"abcd1234"[i] : 0;
		p->rp_key[i] = (uint8_t)(0xf0 + i);
	}
	for(int i = 0; i < 8; i++)
		p->account_id[i] = (uint8_t)(i * 17);
	pv_stream_settings_init(&p->stream);
}

static void test_config(void)
{
	PvConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	PvProfile *a = pv_config_add(&cfg);
	make_profile(a, "main", "192.168.178.88");
	a->has_mac = true;
	memcpy(a->mac, "\x01\x02\x03\x04\x05\x06", 6);
	pv_strlcpy(a->stream.resolution, "1080p", sizeof(a->stream.resolution));
	a->stream.fps = 60;
	a->stream.audio_device = pv_strdup("CABLE Input (VB-Audio Virtual Cable)");
	a->stream.wake = 1;
	a->stream.display = 1;
	pv_strlcpy(a->stream.controller, "dualsense", sizeof(a->stream.controller));
	PvProfile *b = pv_config_add(&cfg);
	make_profile(b, "zweitkonto", "192.168.178.88");
	pv_strlcpy(cfg.default_profile, "main", sizeof(cfg.default_profile));

	char *json = pv_config_to_json(&cfg);
	CHECK(json != NULL);
	PvConfig back;
	memset(&back, 0, sizeof(back));
	char err[512];
	CHECK(pv_config_from_json(&back, json, strlen(json), err, sizeof(err)));
	CHECK(back.count == 2);
	CHECK_STR(back.default_profile, "main");
	PvProfile *m = pv_config_find(&back, "MAIN");
	CHECK(m != NULL);
	if(m)
	{
		CHECK(memcmp(m->regist_key, a->regist_key, 16) == 0 && memcmp(m->rp_key, a->rp_key, 16) == 0);
		CHECK(memcmp(m->account_id, a->account_id, 8) == 0);
		CHECK(m->has_mac && memcmp(m->mac, a->mac, 6) == 0);
		CHECK_STR(m->stream.audio_device, "CABLE Input (VB-Audio Virtual Cable)");
		CHECK(m->stream.wake == 1 && m->stream.fps == 60 && m->stream.display == 1);
		CHECK_STR(m->stream.resolution, "1080p");
		CHECK_STR(m->stream.controller, "dualsense");
	}
	PvProfile *z = pv_config_find(&back, "zweitkonto");
	CHECK(z && z->stream.wake == -1 && z->stream.audio_device == NULL && z->stream.display == -1);

	CHECK(pv_config_select(&back, NULL, err, sizeof(err)) == m);
	CHECK(pv_config_select(&back, "zweitkonto", err, sizeof(err)) == z);
	CHECK(pv_config_select(&back, "nope", err, sizeof(err)) == NULL && strstr(err, "main"));
	CHECK(pv_config_remove(&back, "main"));
	CHECK(back.count == 1 && !back.default_profile[0]);
	CHECK(pv_config_select(&back, NULL, err, sizeof(err)) == &back.profiles[0]); // only profile
	free(json);
	pv_config_free(&back);
	pv_config_free(&cfg);

	// strict validation
	static const char *bad[] = {
		"{",
		"[]",
		"{\"format_version\":99,\"profiles\":[]}",
		"{\"profiles\":[{\"name\":\"a b\",\"host\":\"h\",\"account_id\":\"ypOR0bknzlo=\",\"regist_key\":\"00000000000000000000000000000000\",\"rp_key\":\"00000000000000000000000000000000\"}]}",
		"{\"profiles\":[{\"name\":\"a\",\"host\":\"h\",\"account_id\":\"xx\",\"regist_key\":\"00000000000000000000000000000000\",\"rp_key\":\"00000000000000000000000000000000\"}]}",
		"{\"profiles\":[{\"name\":\"a\",\"host\":\"h\",\"account_id\":\"ypOR0bknzlo=\",\"regist_key\":\"0000\",\"rp_key\":\"00000000000000000000000000000000\"}]}",
		"{\"profiles\":[{\"name\":\"a\",\"host\":\"h\",\"account_id\":\"ypOR0bknzlo=\",\"regist_key\":\"00000000000000000000000000000000\",\"rp_key\":\"00000000000000000000000000000000\"},"
		"{\"name\":\"A\",\"host\":\"h\",\"account_id\":\"ypOR0bknzlo=\",\"regist_key\":\"00000000000000000000000000000000\",\"rp_key\":\"00000000000000000000000000000000\"}]}",
	};
	for(size_t i = 0; i < PV_ARRAY_SIZE(bad); i++)
	{
		PvConfig c;
		memset(&c, 0, sizeof(c));
		err[0] = '\0';
		bool ok = pv_config_from_json(&c, bad[i], strlen(bad[i]), err, sizeof(err));
		CHECK(!ok && err[0]);
		pv_config_free(&c);
	}
	PvConfig c;
	memset(&c, 0, sizeof(c));
	const char *minimal = "{\"profiles\":[{\"name\":\"a\",\"host\":\"h\",\"account_id\":\"ypOR0bknzlo=\",\"regist_key\":\"00000000000000000000000000000000\",\"rp_key\":\"00000000000000000000000000000000\",\"future_field\":1}]}";
	CHECK(pv_config_from_json(&c, minimal, strlen(minimal), err, sizeof(err)) && c.count == 1 && c.profiles[0].ps5);
	pv_config_free(&c);

	char name[64];
	pv_profile_sanitize_name("PS5 Wohnzimmer!", name, sizeof(name));
	CHECK_STR(name, "PS5-Wohnzimmer");
	pv_profile_sanitize_name("", name, sizeof(name));
	CHECK_STR(name, "ps5");
	CHECK(pv_profile_name_valid("PS5-123_a.b"));
	CHECK(!pv_profile_name_valid("a b"));
	CHECK(!pv_profile_name_valid(""));
}

static void test_config_file(void)
{
	char *dir = pv_default_state_dir();
	char *tmpdir = dir ? pv_path_join(dir, "test-tmp") : NULL;
	char *path = tmpdir ? pv_path_join(tmpdir, "config.json") : NULL;
	CHECK(path != NULL);
	if(!path)
		goto out;
	pv_remove_file(path);
	PvConfig cfg;
	char err[512];
	CHECK(pv_config_load(&cfg, path, err, sizeof(err)) == PV_CONFIG_NOT_FOUND);
	make_profile(pv_config_add(&cfg), "x", "10.0.0.2");
	CHECK(pv_config_save(&cfg, err, sizeof(err)));
	CHECK(pv_config_save(&cfg, err, sizeof(err))); // second save creates .bak
	pv_config_free(&cfg);
	CHECK(pv_config_load(&cfg, path, err, sizeof(err)) == PV_CONFIG_OK && cfg.count == 1);
	pv_config_free(&cfg);
	char *bak = pv_asprintf("%s.bak", path);
	CHECK(pv_file_exists(bak));
	pv_remove_file(bak);
	free(bak);
	pv_remove_file(path);
out:
	free(path);
	free(tmpdir);
	free(dir);
}

static void test_register_helpers(void)
{
	uint32_t pin;
	CHECK(pv_register_parse_pin("12345678", &pin) && pin == 12345678);
	CHECK(pv_register_parse_pin("0123 4567", &pin) && pin == 1234567);
	CHECK(pv_register_parse_pin("1234-5678", &pin));
	CHECK(!pv_register_parse_pin("1234567", &pin));
	CHECK(!pv_register_parse_pin("123456789", &pin));
	CHECK(!pv_register_parse_pin("1234567a", &pin));

	PvConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	ChiakiRegisteredHost rh;
	memset(&rh, 0, sizeof(rh));
	rh.target = CHIAKI_TARGET_PS5_1;
	memcpy(rh.server_nickname, "PS5-123", 7);
	memcpy(rh.rp_regist_key, "abcd1234", 8);
	for(int i = 0; i < 16; i++)
		rh.rp_key[i] = (uint8_t)i;
	memcpy(rh.server_mac, "\xaa\xbb\xcc\xdd\xee\xff", 6);
	uint8_t acc1[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, acc2[8] = { 8, 7, 6, 5, 4, 3, 2, 1 };

	PvProfile *p = pv_register_store(&cfg, &rh, NULL, "192.168.178.88", true, CHIAKI_TARGET_PS5_1, acc1, NULL, false);
	CHECK(p && strcmp(p->name, "PS5-123") == 0 && cfg.count == 1);
	CHECK_STR(cfg.default_profile, "PS5-123");
	CHECK(p && p->has_mac && memcmp(p->regist_key, "abcd1234\0\0\0\0\0\0\0\0", 16) == 0);
	if(p)
		p->stream.audio_device = pv_strdup("CABLE Input");

	// second PSN account on the same console -> second profile
	p = pv_register_store(&cfg, &rh, NULL, "192.168.178.88", true, CHIAKI_TARGET_PS5_1, acc2, NULL, false);
	CHECK(p && strcmp(p->name, "PS5-123-2") == 0 && cfg.count == 2);
	CHECK_STR(cfg.default_profile, "PS5-123");

	// re-registering the first account updates it and keeps the stream settings
	rh.rp_key[0] = 0x99;
	p = pv_register_store(&cfg, &rh, NULL, "192.168.178.88", true, CHIAKI_TARGET_PS5_1, acc1, NULL, false);
	CHECK(p && strcmp(p->name, "PS5-123") == 0 && cfg.count == 2);
	CHECK(p && p->rp_key[0] == 0x99 && p->stream.audio_device && strcmp(p->stream.audio_device, "CABLE Input") == 0);
	pv_config_free(&cfg);

	uint64_t cred;
	CHECK(pv_regist_key_credential((const uint8_t *)"abcd1234\0\0\0\0\0\0\0\0", &cred) && cred == 0xabcd1234u);
	CHECK(!pv_regist_key_credential((const uint8_t *)"abcz1234\0\0\0\0\0\0\0\0", &cred));
	CHECK(!pv_regist_key_credential((const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", &cred));
}

static void test_audio_match(void)
{
	char *names[] = {
		(char *)"Lautsprecher (Realtek(R) Audio)",
		(char *)"CABLE Input (VB-Audio Virtual Cable)",
		(char *)"DELL U2720Q (NVIDIA High Definition Audio)",
		(char *)"CABLE-A Input (VB-Audio Cable A)",
	};
	size_t idx = 99;
	CHECK(pv_audio_match_device("CABLE Input (VB-Audio Virtual Cable)", names, 4, &idx) == PV_AUDIO_MATCH_EXACT && idx == 1);
	CHECK(pv_audio_match_device("cable input", names, 4, &idx) == PV_AUDIO_MATCH_UNIQUE && idx == 1);
	CHECK(pv_audio_match_device("DELL", names, 4, &idx) == PV_AUDIO_MATCH_UNIQUE && idx == 2);
	CHECK(pv_audio_match_device("CABLE", names, 4, &idx) == PV_AUDIO_MATCH_AMBIGUOUS);
	CHECK(pv_audio_match_device("Kopfhörer", names, 4, &idx) == PV_AUDIO_MATCH_NONE);
	CHECK(pv_audio_match_device("x", NULL, 0, &idx) == PV_AUDIO_MATCH_NONE);
}

static void pump_audio(PvAudio *a, int ms)
{
	for(int t = 0; t < ms; t += 5)
	{
		SDL_Event e;
		while(SDL_PollEvent(&e))
			pv_audio_handle_event(a, &e);
		SDL_Delay(5);
	}
}

// Opus packets -> libchiaki opus decoder -> our sink -> SDL (dummy driver).
static void test_audio_pipeline(void)
{
#ifdef _WIN32
	_putenv_s("SDL_AUDIODRIVER", "dummy");
#else
	setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
	SDL_SetMainReady();
	if(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0)
	{
		fprintf(stderr, "skipping audio test: %s\n", SDL_GetError());
		return;
	}
	int oerr = 0;
	OpusEncoder *enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &oerr);
	CHECK(enc != NULL);
	for(int round = 0; round < 2 && enc; round++)
	{
		// round 0: default device; round 1: a device that does not exist (must stay silent, no fallback)
		char err[128];
		PvAudio *a = pv_audio_create(round == 0 ? NULL : "Gibt es nicht 123", 50, err, sizeof(err));
		CHECK(a != NULL);
		if(!a)
			break;
		ChiakiAudioSink sink;
		pv_audio_get_sink(a, &sink);
		ChiakiAudioHeader h;
		chiaki_audio_header_set(&h, 2, 16, 48000, 480);
		sink.header_cb(&h, sink.user);
		pump_audio(a, 50);
		int16_t pcm[480 * 2];
		uint8_t pkt[1500];
		for(int f = 0; f < 100; f++)
		{
			for(int i = 0; i < 480; i++)
				pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)(6000 * sin((f * 480 + i) * 2.0 * 3.14159265 * 440.0 / 48000.0));
			opus_int32 n = opus_encode(enc, pcm, 480, pkt, sizeof(pkt));
			CHECK(n > 0);
			if(n > 0)
				sink.frame_cb(pkt, (size_t)n, sink.user);
			if(f % 10 == 0)
				pump_audio(a, 0);
		}
		pump_audio(a, 20);
		PvAudioStats st;
		pv_audio_get_stats(a, &st);
		fprintf(stderr, "audio pipeline (%s): %llu frames, device %s, %u ch, %u Hz, queue %u ms\n",
				round == 0 ? "default" : "missing device", (unsigned long long)st.frames,
				st.device_open ? "open" : "closed", st.channels, st.rate, st.queued_ms);
		CHECK(st.frames == 100);
		if(round == 0)
			CHECK(st.device_open && st.channels == 2 && st.rate == 48000);
		else
			CHECK(!st.device_open); // never falls back to another device
		pv_audio_destroy(a);
	}
	if(enc)
		opus_encoder_destroy(enc);
	SDL_Quit();
}

// Feeds a real H.264 stream through the decoder and the SDL presentation path.
static void test_video_pipeline(void)
{
#ifdef PV_TEST_DATA_DIR
#ifdef _WIN32
	_putenv_s("SDL_VIDEODRIVER", "dummy");
#else
	setenv("SDL_VIDEODRIVER", "dummy", 1);
#endif
	SDL_SetMainReady();
	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
	{
		fprintf(stderr, "skipping video test: SDL_Init failed: %s\n", SDL_GetError());
		return;
	}
	char *path = pv_path_join(PV_TEST_DATA_DIR, "sample_720p.h264");
	size_t len = 0;
	uint8_t *data = (uint8_t *)pv_read_file(path, 8u << 20, &len, NULL);
	CHECK(data != NULL);
	PvVideoOptions vo;
	memset(&vo, 0, sizeof(vo));
	vo.width = 640;
	vo.height = 360;
	vo.display = -1;
	vo.hwdec = "auto"; // no GPU in CI: exercises the software fallback
	vo.title = "ps5view-test";
	char err[256];
	PvVideo *v = data ? pv_video_create(&vo, err, sizeof(err)) : NULL;
	CHECK(v != NULL);
	if(v && pv_video_start_decoder(v, CHIAKI_CODEC_H264, 60, err, sizeof(err)))
	{
		const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264);
		AVCodecContext *pctx = avcodec_alloc_context3(codec);
		const uint8_t *p = data;
		size_t left = len;
		int frames = 0;
		while(parser && pctx)
		{
			uint8_t *out = NULL;
			int out_size = 0;
			int used = av_parser_parse2(parser, pctx, &out, &out_size, p, (int)left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
			p += used;
			left -= (size_t)used;
			if(out_size > 0)
			{
				// libchiaki guarantees padding after the buffer
				uint8_t *buf = calloc(1, (size_t)out_size + AV_INPUT_BUFFER_PADDING_SIZE);
				memcpy(buf, out, (size_t)out_size);
				CHECK(pv_video_sample_cb(buf, (size_t)out_size, 0, false, v));
				free(buf);
				frames++;
				SDL_Event e;
				while(SDL_PollEvent(&e))
					pv_video_handle_event(v, &e);
			}
			if(left == 0 && out_size == 0)
				break;
		}
		av_parser_close(parser);
		avcodec_free_context(&pctx);
		SDL_Event e;
		for(int i = 0; i < 50; i++)
		{
			while(SDL_PollEvent(&e))
				pv_video_handle_event(v, &e);
			SDL_Delay(5);
		}
		PvVideoStats st;
		pv_video_get_stats(v, &st);
		fprintf(stderr, "video pipeline: %d samples fed, %llu frames shown, %dx%d via %s\n", frames,
				(unsigned long long)st.frames_shown, st.width, st.height, st.decoder);
		CHECK(frames >= 20);
		CHECK(st.frames_shown > 0);
		CHECK(st.width == 1280 && st.height == 720);
		CHECK(st.decode_errors == 0);
	}
	else if(v)
	{
		CHECK(!"decoder start failed");
		fprintf(stderr, "%s\n", err);
	}
	pv_video_destroy(v);
	free(data);
	free(path);
	SDL_Quit();
#endif
}

int main(void)
{
	pv_log_init(NULL, 0);
	pv_log_set_console_level(PV_LOG_WARN);
	chiaki_lib_init();
	test_util();
	test_oauth();
	test_diag();
	test_diag_patterns_in_chiaki_source();
	test_errors();
	test_config();
	test_config_file();
	test_register_helpers();
	test_audio_match();
	test_audio_pipeline();
	test_video_pipeline();
	fprintf(stderr, "%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
