// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// ps5view register --host <ip> --account-id <base64> --pin <8 digits> [--name <profile>]

#include "register.h"

#include "app.h"
#include "args.h"
#include "config.h"
#include "diag.h"
#include "discover.h"
#include "errors.h"
#include "log.h"
#include "util.h"

#include <chiaki/regist.h>
#include <chiaki/thread.h>

#include <stdlib.h>
#include <string.h>

#define REGIST_TIMEOUT_MS 30000

typedef struct
{
	ChiakiMutex mutex;
	ChiakiCond cond;
	bool done;
	bool success;
	bool canceled;
	ChiakiRegisteredHost host;
} RegistCtx;

static void regist_cb(ChiakiRegistEvent *event, void *user)
{
	RegistCtx *ctx = user;
	chiaki_mutex_lock(&ctx->mutex);
	switch(event->type)
	{
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS:
			ctx->success = true;
			if(event->registered_host)
				ctx->host = *event->registered_host;
			break;
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED:
			ctx->canceled = true;
			break;
		default:
			break;
	}
	ctx->done = true;
	chiaki_mutex_unlock(&ctx->mutex);
	chiaki_cond_signal(&ctx->cond);
}

bool pv_register_parse_pin(const char *s, uint32_t *pin)
{
	char digits[16];
	size_t n = 0;
	for(const char *p = s; *p; p++)
	{
		if(*p == ' ' || *p == '-')
			continue;
		if(*p < '0' || *p > '9' || n >= 8)
			return false;
		digits[n++] = *p;
	}
	if(n != 8)
		return false;
	digits[n] = '\0';
	*pin = (uint32_t)strtoul(digits, NULL, 10);
	return true;
}

void pv_register_choose_profile_name(PvConfig *cfg, const char *nickname, const char *host, const uint8_t account_id[8],
		char *out, size_t out_size)
{
	// same console + same PSN account: update the existing profile
	for(size_t i = 0; i < cfg->count; i++)
	{
		PvProfile *p = &cfg->profiles[i];
		if(pv_strcasecmp(p->host, host) == 0 && memcmp(p->account_id, account_id, 8) == 0)
		{
			pv_strlcpy(out, p->name, out_size);
			return;
		}
	}
	char base[PV_PROFILE_NAME_MAX - 4];
	pv_profile_sanitize_name(nickname && *nickname ? nickname : "ps5", base, sizeof(base));
	if(!pv_config_find(cfg, base))
	{
		pv_strlcpy(out, base, out_size);
		return;
	}
	for(int i = 2; i < 100; i++)
	{
		snprintf(out, out_size, "%s-%d", base, i);
		if(!pv_config_find(cfg, out))
			return;
	}
	snprintf(out, out_size, "%s-%lu", base, (unsigned long)pv_now_ms());
}

PvProfile *pv_register_store(PvConfig *cfg, const ChiakiRegisteredHost *rh, const char *name, const char *host, bool ps5,
		ChiakiTarget target, const uint8_t account_id[8], const char *fallback_nickname, bool make_default)
{
	char nickname[sizeof(rh->server_nickname) + 1];
	memcpy(nickname, rh->server_nickname, sizeof(rh->server_nickname));
	nickname[sizeof(rh->server_nickname)] = '\0';
	if(!nickname[0] && fallback_nickname)
		pv_strlcpy(nickname, fallback_nickname, sizeof(nickname));

	char profile_name[PV_PROFILE_NAME_MAX];
	if(name)
		pv_strlcpy(profile_name, name, sizeof(profile_name));
	else
		pv_register_choose_profile_name(cfg, nickname, host, account_id, profile_name, sizeof(profile_name));

	PvProfile *p = pv_config_find(cfg, profile_name);
	PvStreamSettings keep;
	pv_stream_settings_init(&keep);
	if(p)
	{
		LOGI("register", "Profil \"%s\" wird aktualisiert (Stream-Einstellungen bleiben erhalten).", p->name);
		keep = p->stream; // take over the old settings
		pv_stream_settings_init(&p->stream);
	}
	else
		p = pv_config_add(cfg);
	if(!p)
		return NULL;
	memset(p, 0, sizeof(*p));
	pv_strlcpy(p->name, profile_name, sizeof(p->name));
	pv_strlcpy(p->host, host, sizeof(p->host));
	p->ps5 = ps5;
	p->target = (int)(rh->target ? rh->target : target);
	pv_strlcpy(p->nickname, nickname, sizeof(p->nickname));
	memcpy(p->mac, rh->server_mac, sizeof(p->mac));
	p->has_mac = memcmp(p->mac, "\0\0\0\0\0\0", 6) != 0;
	memcpy(p->account_id, account_id, sizeof(p->account_id));
	memcpy(p->regist_key, rh->rp_regist_key, sizeof(p->regist_key));
	memcpy(p->rp_key, rh->rp_key, sizeof(p->rp_key));
	p->rp_key_type = rh->rp_key_type;
	pv_format_time(p->registered_at, sizeof(p->registered_at), PV_TIME_ISO_UTC);
	p->stream = keep;
	if(make_default || cfg->count == 1 || !cfg->default_profile[0])
		pv_strlcpy(cfg->default_profile, p->name, sizeof(cfg->default_profile));
	return p;
}

static void usage(void)
{
	pv_out("Aufruf: ps5view register --host <ip> --account-id <base64> --pin <8 Ziffern> [--name <profil>]");
	pv_out("%s", "");
	pv_out("Registriert diesen PC an der PS5 und speichert die Schlüssel als Profil.");
	pv_out("Vorher an der PS5: Einstellungen > System > Remote Play > Gerät verknüpfen (zeigt die PIN).");
	pv_out("Für mehrere PSN-Konten einfach mehrmals registrieren (je ein Profil).");
	pv_out("%s", "");
	pv_out("  --host <ip>          IP-Adresse der PS5 (siehe ps5view discover)");
	pv_out("  --account-id <id>    PSN-Account-ID (base64, siehe ps5view account-id)");
	pv_out("  --pin <ziffern>      8-stellige PIN vom Bildschirm \"Gerät verknüpfen\"");
	pv_out("  --name <profil>      Profilname (Standard: Name der Konsole); vorhandenes Profil wird ersetzt");
	pv_out("  --default            dieses Profil als Standard für \"stream\" verwenden");
	pv_out("  --ps4                PS4 statt PS5 registrieren (sonst automatisch erkannt)");
}

int pv_cmd_register(int argc, char **argv)
{
	const char *host = NULL, *account_str = NULL, *pin_str = NULL, *name = NULL;
	bool make_default = false, force_ps4 = false;
	PvArgs a;
	pv_args_init(&a, "register", argc, argv);
	while(pv_args_next(&a))
	{
		if(pv_args_value(&a, "--host", &host) || pv_args_value(&a, "--account-id", &account_str)
				|| pv_args_value(&a, "--pin", &pin_str) || pv_args_value(&a, "--name", &name))
			continue;
		if(pv_args_flag(&a, "--default"))
		{
			make_default = true;
			continue;
		}
		if(pv_args_flag(&a, "--ps4"))
		{
			force_ps4 = true;
			continue;
		}
		if(pv_args_is_help(&a))
		{
			usage();
			return PV_EXIT_OK;
		}
		return pv_args_unknown(&a);
	}
	if(a.failed)
		return PV_EXIT_USAGE;

	bool bad = false;
	if(!host || !*host)
	{
		LOGE("register", "--host <ip> fehlt (IP-Adresse der PS5, siehe ps5view discover).");
		bad = true;
	}
	uint8_t account_id[8];
	if(!account_str)
	{
		LOGE("register", "--account-id <base64> fehlt (ermitteln mit: ps5view account-id).");
		bad = true;
	}
	else if(!pv_base64_decode_exact(account_str, account_id, sizeof(account_id)))
	{
		LOGE("register", "Die Account-ID \"%s\" ist ungültig: erwartet wird ein base64-Wert mit 8 Byte (12 Zeichen, z. B. \"ypOR0bknzlo=\").", account_str);
		LOGI("hint", "Was du tun kannst:\n- Account-ID mit \"ps5view account-id\" ermitteln und vollständig kopieren (inkl. \"=\" am Ende).\n"
				"- Nicht verwechseln mit der Online-ID (dem sichtbaren PSN-Namen).");
		bad = true;
	}
	uint32_t pin = 0;
	if(!pin_str)
	{
		LOGE("register", "--pin <8 Ziffern> fehlt (PS5: Einstellungen > System > Remote Play > Gerät verknüpfen).");
		bad = true;
	}
	else if(!pv_register_parse_pin(pin_str, &pin))
	{
		LOGE("register", "Die PIN muss genau 8 Ziffern haben (gefunden: \"%s\").", pin_str);
		bad = true;
	}
	if(name && !pv_profile_name_valid(name))
	{
		LOGE("register", "Ungültiger Profilname \"%s\" (erlaubt: Buchstaben, Ziffern, . _ -, max. %d Zeichen).", name, PV_PROFILE_NAME_MAX - 1);
		bad = true;
	}
	if(bad)
		return PV_EXIT_USAGE;

	PvConfig cfg;
	char err[1024];
	if(pv_config_load(&cfg, pv_global.config_path, err, sizeof(err)) == PV_CONFIG_ERROR)
	{
		LOGE("register", "%s", err);
		pv_config_free(&cfg);
		return PV_EXIT_CONFIG;
	}

	// Pre-check: find out PS4/PS5 and whether the console is on at all.
	bool ps5 = !force_ps4;
	ChiakiTarget target = ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
	PvHostInfo hi;
	PvHostState st = pv_discover_host(host, ps5, 2500, &hi);
	if(st == PV_HOST_ERROR)
	{
		pv_config_free(&cfg);
		return PV_EXIT_NETWORK;
	}
	if(st == PV_HOST_NO_RESPONSE)
		LOGW("register", "%s antwortet nicht auf die Konsolensuche (UDP 9302). Versuche die Registrierung trotzdem ...", host);
	else
	{
		LOGI("register", "Konsole gefunden: %s (%s), Status: %s", hi.name, hi.ps5 ? "PS5" : "PS4", pv_host_state_text(hi.state));
		if(st == PV_HOST_STANDBY)
		{
			LOGE("register", "Die Konsole ist im Ruhemodus. Für die Registrierung muss sie eingeschaltet sein.");
			LOGI("hint", "Was du tun kannst:\n- PS5 einschalten, dann Einstellungen > System > Remote Play > Gerät verknüpfen öffnen.");
			pv_config_free(&cfg);
			return PV_EXIT_CONSOLE;
		}
		if(!hi.ps5 && !force_ps4)
		{
			LOGI("register", "Die Konsole ist eine PS4 - registriere als PS4.");
			ps5 = false;
		}
		if(!ps5)
		{
			PvHostInfo tmp = hi;
			ChiakiDiscoveryHost dh;
			memset(&dh, 0, sizeof(dh));
			dh.system_version = tmp.system_version;
			dh.device_discovery_protocol_version = tmp.protocol_version;
			target = chiaki_discovery_host_system_version_target(&dh);
			if(chiaki_target_is_unknown(target))
				target = CHIAKI_TARGET_PS4_10;
		}
	}

	PvDiag diag;
	RegistCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	if(!pv_diag_init(&diag) || chiaki_mutex_init(&ctx.mutex, false) != CHIAKI_ERR_SUCCESS || chiaki_cond_init(&ctx.cond) != CHIAKI_ERR_SUCCESS)
	{
		LOGE("register", "Interner Fehler (Synchronisation).");
		pv_config_free(&cfg);
		return PV_EXIT_INTERNAL;
	}
	pv_log_set_chiaki_hook(pv_diag_log_hook, &diag);

	ChiakiRegistInfo info;
	memset(&info, 0, sizeof(info));
	info.target = target;
	info.host = host;
	info.broadcast = false;
	info.psn_online_id = NULL;
	memcpy(info.psn_account_id, account_id, sizeof(info.psn_account_id));
	info.pin = pin;
	info.console_pin = 0;
	info.holepunch_info = NULL;
	info.rudp = NULL;

	LOGI("register", "Registriere bei %s ... (der Bildschirm \"Gerät verknüpfen\" muss an der PS5 offen bleiben)", host);
	ChiakiRegist regist;
	ChiakiErrorCode cerr = chiaki_regist_start(&regist, pv_chiaki_log(), &info, regist_cb, &ctx);
	if(cerr != CHIAKI_ERR_SUCCESS)
	{
		LOGE("register", "Registrierung konnte nicht gestartet werden: %s", chiaki_error_string(cerr));
		pv_log_set_chiaki_hook(NULL, NULL);
		pv_config_free(&cfg);
		return PV_EXIT_INTERNAL;
	}

	uint64_t start = pv_now_ms();
	bool timed_out = false;
	chiaki_mutex_lock(&ctx.mutex);
	while(!ctx.done)
	{
		if(pv_interrupted())
			break;
		if(pv_now_ms() - start > REGIST_TIMEOUT_MS)
		{
			timed_out = true;
			break;
		}
		chiaki_cond_timedwait(&ctx.cond, &ctx.mutex, 100);
	}
	bool done = ctx.done;
	chiaki_mutex_unlock(&ctx.mutex);
	if(!done)
		chiaki_regist_stop(&regist);
	chiaki_regist_fini(&regist);
	pv_log_set_chiaki_hook(NULL, NULL);

	PvDiagInfo d;
	pv_diag_snapshot(&diag, &d);
	pv_diag_fini(&diag);
	chiaki_cond_fini(&ctx.cond);
	chiaki_mutex_fini(&ctx.mutex);

	if(!ctx.success)
	{
		pv_config_free(&cfg);
		if(pv_interrupted())
			return PV_EXIT_INTERRUPTED;
		if(timed_out)
			LOGW("register", "Keine Rückmeldung nach %d s - Registrierung abgebrochen.", REGIST_TIMEOUT_MS / 1000);
		PvFailure f;
		pv_failure_from_regist(&f, &d, host);
		pv_failure_print(&f);
		return f.exit_code;
	}

	// ---- success: store profile ----
	const char *fallback_nick = st != PV_HOST_NO_RESPONSE ? hi.name : NULL;
	PvProfile *p = pv_register_store(&cfg, &ctx.host, name, host, ps5, target, account_id, fallback_nick, make_default);
	if(!p)
	{
		LOGE("register", "Speicherfehler.");
		pv_config_free(&cfg);
		return PV_EXIT_INTERNAL;
	}

	uint64_t cred;
	if(!pv_regist_key_credential(p->regist_key, &cred))
		LOGW("register", "Der Registrierungsschlüssel hat ein unerwartetes Format - Aufwecken (--wake) funktioniert eventuell nicht.");

	if(!pv_config_save(&cfg, err, sizeof(err)))
	{
		LOGE("register", "Registrierung war erfolgreich, aber das Speichern ist fehlgeschlagen: %s", err);
		pv_config_free(&cfg);
		return PV_EXIT_CONFIG;
	}

	char mac[32] = "?";
	if(p->has_mac)
		snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5]);
	pv_out("Registrierung erfolgreich.");
	pv_out("  Profil:   %s%s", p->name, pv_strcasecmp(cfg.default_profile, p->name) == 0 ? " (Standard)" : "");
	pv_out("  Konsole:  %s (%s, MAC %s)", p->nickname[0] ? p->nickname : "?", p->ps5 ? "PS5" : "PS4", mac);
	pv_out("  Adresse:  %s", p->host);
	pv_out("  Gespeichert in: %s", cfg.path);
	pv_out("%s", "");
	pv_out("Starten mit:  ps5view stream --profile %s", p->name);
	pv_config_free(&cfg);
	return PV_EXIT_OK;
}
