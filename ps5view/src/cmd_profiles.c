// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// ps5view profiles [--default <name>]
// ps5view remove-profile --profile <name>

#include "app.h"
#include "args.h"
#include "config.h"
#include "log.h"
#include "util.h"

#include <string.h>

static void print_profile(const PvConfig *cfg, const PvProfile *p)
{
	char acc[16];
	pv_base64_encode(p->account_id, sizeof(p->account_id), acc, sizeof(acc));
	const PvStreamSettings *s = &p->stream;
	bool is_default = pv_strcasecmp(cfg->default_profile, p->name) == 0;
	pv_out("%s", "");
	pv_out("  %s%s", p->name, is_default ? "   [Standard]" : "");
	pv_out("    Konsole:     %s (%s) unter %s", p->nickname[0] ? p->nickname : "?", p->ps5 ? "PS5" : "PS4", p->host);
	pv_out("    Account-ID:  %.4s...", acc);
	if(p->registered_at[0])
		pv_out("    Registriert: %s", p->registered_at);
	char settings[512] = "";
	size_t off = 0;
#define ADD(...) do { if(off < sizeof(settings)) off += (size_t)snprintf(settings + off, sizeof(settings) - off, __VA_ARGS__); } while(0)
	if(s->resolution[0])
		ADD("%s ", s->resolution);
	if(s->fps)
		ADD("%u fps ", s->fps);
	if(s->bitrate)
		ADD("%u kbit/s ", s->bitrate);
	if(s->codec[0])
		ADD("%s ", s->codec);
	if(s->hwdec[0])
		ADD("hwdec=%s ", s->hwdec);
	if(s->wake >= 0)
		ADD("%s ", s->wake ? "wake" : "no-wake");
#undef ADD
	if(settings[0])
		pv_out("    Stream:      %s", settings);
	pv_out("    Audiogerät:  %s", s->audio_device ? s->audio_device : "(Windows-Standardgerät)");
}

int pv_cmd_profiles(int argc, char **argv)
{
	const char *set_default = NULL;
	PvArgs a;
	pv_args_init(&a, "profiles", argc, argv);
	while(pv_args_next(&a))
	{
		if(pv_args_value(&a, "--default", &set_default))
			continue;
		if(pv_args_is_help(&a))
		{
			pv_out("Aufruf: ps5view profiles [--default <profil>]");
			pv_out("Zeigt die gespeicherten Registrierungen (ohne Schlüssel).");
			pv_out("  --default <profil>   Profil als Standard für \"stream\" festlegen");
			return PV_EXIT_OK;
		}
		return pv_args_unknown(&a);
	}
	if(a.failed)
		return PV_EXIT_USAGE;

	PvConfig cfg;
	char err[1024];
	PvConfigResult r = pv_config_load(&cfg, pv_global.config_path, err, sizeof(err));
	if(r == PV_CONFIG_ERROR)
	{
		LOGE("profiles", "%s", err);
		pv_config_free(&cfg);
		return PV_EXIT_CONFIG;
	}
	if(set_default)
	{
		PvProfile *p = pv_config_select(&cfg, set_default, err, sizeof(err));
		if(!p)
		{
			LOGE("profiles", "%s", err);
			pv_config_free(&cfg);
			return PV_EXIT_CONFIG;
		}
		pv_strlcpy(cfg.default_profile, p->name, sizeof(cfg.default_profile));
		if(!pv_config_save(&cfg, err, sizeof(err)))
		{
			LOGE("profiles", "%s", err);
			pv_config_free(&cfg);
			return PV_EXIT_CONFIG;
		}
		LOGI("profiles", "Standardprofil ist jetzt \"%s\".", p->name);
	}
	pv_out("Konfiguration: %s", cfg.path);
	if(!cfg.count)
	{
		pv_out("Noch keine Konsole registriert. Siehe: ps5view register --help");
		pv_config_free(&cfg);
		return PV_EXIT_OK;
	}
	pv_out("%zu Profil(e):", cfg.count);
	for(size_t i = 0; i < cfg.count; i++)
		print_profile(&cfg, &cfg.profiles[i]);
	pv_config_free(&cfg);
	return PV_EXIT_OK;
}

int pv_cmd_remove_profile(int argc, char **argv)
{
	const char *name = NULL;
	PvArgs a;
	pv_args_init(&a, "remove-profile", argc, argv);
	while(pv_args_next(&a))
	{
		if(pv_args_value(&a, "--profile", &name))
			continue;
		if(pv_args_is_help(&a))
		{
			pv_out("Aufruf: ps5view remove-profile --profile <profil>");
			pv_out("Löscht die gespeicherte Registrierung (an der PS5 bleibt das Gerät verknüpft,");
			pv_out("dort ggf. unter Einstellungen > System > Remote Play entfernen).");
			return PV_EXIT_OK;
		}
		return pv_args_unknown(&a);
	}
	if(a.failed)
		return PV_EXIT_USAGE;
	if(!name)
	{
		LOGE("profiles", "--profile <name> fehlt (siehe ps5view profiles).");
		return PV_EXIT_USAGE;
	}
	PvConfig cfg;
	char err[1024];
	if(pv_config_load(&cfg, pv_global.config_path, err, sizeof(err)) == PV_CONFIG_ERROR)
	{
		LOGE("profiles", "%s", err);
		pv_config_free(&cfg);
		return PV_EXIT_CONFIG;
	}
	PvProfile *p = pv_config_select(&cfg, name, err, sizeof(err));
	if(!p)
	{
		LOGE("profiles", "%s", err);
		pv_config_free(&cfg);
		return PV_EXIT_CONFIG;
	}
	char removed[PV_PROFILE_NAME_MAX];
	pv_strlcpy(removed, p->name, sizeof(removed));
	pv_config_remove(&cfg, removed);
	if(!cfg.default_profile[0] && cfg.count == 1)
		pv_strlcpy(cfg.default_profile, cfg.profiles[0].name, sizeof(cfg.default_profile));
	if(!pv_config_save(&cfg, err, sizeof(err)))
	{
		LOGE("profiles", "%s", err);
		pv_config_free(&cfg);
		return PV_EXIT_CONFIG;
	}
	pv_out("Profil \"%s\" gelöscht (Sicherung der vorherigen Datei: %s.bak).", removed, cfg.path);
	pv_config_free(&cfg);
	return PV_EXIT_OK;
}
