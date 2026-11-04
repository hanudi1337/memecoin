// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "config.h"

#include "log.h"
#include "util.h"

#include <json-c/json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_MAX_SIZE (4u * 1024u * 1024u)

void pv_stream_settings_init(PvStreamSettings *s)
{
	memset(s, 0, sizeof(*s));
	s->wake = -1;
	s->display = -1;
}

void pv_stream_settings_free(PvStreamSettings *s)
{
	free(s->audio_device);
	s->audio_device = NULL;
}

bool pv_profile_name_valid(const char *name)
{
	if(!name || !*name || strlen(name) >= PV_PROFILE_NAME_MAX)
		return false;
	for(const char *p = name; *p; p++)
	{
		char c = *p;
		if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
			return false;
	}
	return true;
}

void pv_profile_sanitize_name(const char *in, char *out, size_t out_size)
{
	size_t o = 0;
	for(const char *p = in ? in : ""; *p && o + 1 < out_size; p++)
	{
		char c = *p;
		if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
			out[o++] = c;
		else if(c == ' ' && o > 0 && out[o - 1] != '-')
			out[o++] = '-';
	}
	while(o > 0 && out[o - 1] == '-')
		o--;
	out[o] = '\0';
	if(!o)
		pv_strlcpy(out, "ps5", out_size);
}

char *pv_config_default_path(void)
{
	char *dir = pv_default_config_dir();
	if(!dir)
		return NULL;
	char *p = pv_path_join(dir, "config.json");
	free(dir);
	return p;
}

static void config_init_empty(PvConfig *cfg, const char *path)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->path = path ? pv_strdup(path) : pv_config_default_path();
}

void pv_config_free(PvConfig *cfg)
{
	if(!cfg)
		return;
	for(size_t i = 0; i < cfg->count; i++)
		pv_stream_settings_free(&cfg->profiles[i].stream);
	free(cfg->profiles);
	free(cfg->path);
	memset(cfg, 0, sizeof(*cfg));
}

PvProfile *pv_config_find(PvConfig *cfg, const char *name)
{
	if(!name)
		return NULL;
	for(size_t i = 0; i < cfg->count; i++)
		if(pv_strcasecmp(cfg->profiles[i].name, name) == 0)
			return &cfg->profiles[i];
	return NULL;
}

PvProfile *pv_config_add(PvConfig *cfg)
{
	PvProfile *n = realloc(cfg->profiles, (cfg->count + 1) * sizeof(PvProfile));
	if(!n)
		return NULL;
	cfg->profiles = n;
	PvProfile *p = &cfg->profiles[cfg->count++];
	memset(p, 0, sizeof(*p));
	p->ps5 = true;
	pv_stream_settings_init(&p->stream);
	return p;
}

bool pv_config_remove(PvConfig *cfg, const char *name)
{
	PvProfile *p = pv_config_find(cfg, name);
	if(!p)
		return false;
	size_t idx = (size_t)(p - cfg->profiles);
	if(pv_strcasecmp(cfg->default_profile, p->name) == 0)
		cfg->default_profile[0] = '\0';
	pv_stream_settings_free(&p->stream);
	memmove(&cfg->profiles[idx], &cfg->profiles[idx + 1], (cfg->count - idx - 1) * sizeof(PvProfile));
	cfg->count--;
	return true;
}

PvProfile *pv_config_select(PvConfig *cfg, const char *name, char *err, size_t err_size)
{
	if(name && *name)
	{
		PvProfile *p = pv_config_find(cfg, name);
		if(!p)
		{
			if(cfg->count)
			{
				char list[512] = "";
				for(size_t i = 0; i < cfg->count; i++)
				{
					if(i)
						strncat(list, ", ", sizeof(list) - strlen(list) - 1);
					strncat(list, cfg->profiles[i].name, sizeof(list) - strlen(list) - 1);
				}
				snprintf(err, err_size, "Profil \"%s\" gibt es nicht. Vorhanden: %s", name, list);
			}
			else
				snprintf(err, err_size, "Profil \"%s\" gibt es nicht - es ist noch keine Konsole registriert (ps5view register ...).", name);
		}
		return p;
	}
	if(cfg->default_profile[0])
	{
		PvProfile *p = pv_config_find(cfg, cfg->default_profile);
		if(p)
			return p;
	}
	if(cfg->count == 1)
		return &cfg->profiles[0];
	if(cfg->count == 0)
		snprintf(err, err_size, "Es ist noch keine Konsole registriert. Zuerst: ps5view register --host <ip> --account-id <id> --pin <pin>");
	else
		snprintf(err, err_size, "Mehrere Profile vorhanden - bitte mit --profile <name> auswählen (siehe: ps5view profiles).");
	return NULL;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

static void put_hex(json_object *o, const char *key, const uint8_t *buf, size_t len)
{
	char hex[128];
	pv_hex_encode(buf, len, hex, sizeof(hex));
	json_object_object_add(o, key, json_object_new_string(hex));
}

char *pv_config_to_json(const PvConfig *cfg)
{
	json_object *root = json_object_new_object();
	json_object_object_add(root, "format_version", json_object_new_int(PV_CONFIG_FORMAT_VERSION));
	json_object_object_add(root, "comment", json_object_new_string(
			"ps5view configuration. Contains Remote Play keys - do not share this file."));
	if(cfg->default_profile[0])
		json_object_object_add(root, "default_profile", json_object_new_string(cfg->default_profile));
	json_object *arr = json_object_new_array();
	for(size_t i = 0; i < cfg->count; i++)
	{
		const PvProfile *p = &cfg->profiles[i];
		json_object *o = json_object_new_object();
		json_object_object_add(o, "name", json_object_new_string(p->name));
		json_object_object_add(o, "host", json_object_new_string(p->host));
		json_object_object_add(o, "console", json_object_new_string(p->ps5 ? "ps5" : "ps4"));
		json_object_object_add(o, "target", json_object_new_int(p->target));
		json_object_object_add(o, "nickname", json_object_new_string(p->nickname));
		if(p->has_mac)
		{
			char mac[32];
			snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
					p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5]);
			json_object_object_add(o, "mac", json_object_new_string(mac));
		}
		char b64[32];
		pv_base64_encode(p->account_id, sizeof(p->account_id), b64, sizeof(b64));
		json_object_object_add(o, "account_id", json_object_new_string(b64));
		put_hex(o, "regist_key", p->regist_key, sizeof(p->regist_key));
		put_hex(o, "rp_key", p->rp_key, sizeof(p->rp_key));
		json_object_object_add(o, "rp_key_type", json_object_new_int64(p->rp_key_type));
		if(p->registered_at[0])
			json_object_object_add(o, "registered_at", json_object_new_string(p->registered_at));

		json_object *s = json_object_new_object();
		const PvStreamSettings *st = &p->stream;
		if(st->resolution[0])
			json_object_object_add(s, "resolution", json_object_new_string(st->resolution));
		if(st->fps)
			json_object_object_add(s, "fps", json_object_new_int((int)st->fps));
		if(st->bitrate)
			json_object_object_add(s, "bitrate_kbps", json_object_new_int((int)st->bitrate));
		if(st->codec[0])
			json_object_object_add(s, "codec", json_object_new_string(st->codec));
		if(st->audio_device)
			json_object_object_add(s, "audio_device", json_object_new_string(st->audio_device));
		if(st->wake >= 0)
			json_object_object_add(s, "wake", json_object_new_boolean(st->wake ? 1 : 0));
		if(st->hwdec[0])
			json_object_object_add(s, "hwdec", json_object_new_string(st->hwdec));
		if(st->display >= 0)
			json_object_object_add(s, "display", json_object_new_int(st->display));
		if(st->controller[0])
			json_object_object_add(s, "controller", json_object_new_string(st->controller));
		json_object_object_add(o, "stream", s);

		json_object_array_add(arr, o);
	}
	json_object_object_add(root, "profiles", arr);
	const char *str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
	char *r = pv_strdup(str);
	json_object_put(root);
	return r;
}

static const char *get_str(json_object *o, const char *key)
{
	json_object *v;
	if(!json_object_object_get_ex(o, key, &v) || !json_object_is_type(v, json_type_string))
		return NULL;
	return json_object_get_string(v);
}

static bool get_int(json_object *o, const char *key, int64_t *out)
{
	json_object *v;
	if(!json_object_object_get_ex(o, key, &v) || !json_object_is_type(v, json_type_int))
		return false;
	*out = json_object_get_int64(v);
	return true;
}

static bool has_key(json_object *o, const char *key)
{
	json_object *v;
	return json_object_object_get_ex(o, key, &v);
}

static bool parse_mac(const char *s, uint8_t mac[6])
{
	unsigned v[6];
	char tail;
	if(sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6)
		return false;
	for(int i = 0; i < 6; i++)
		mac[i] = (uint8_t)v[i];
	return true;
}

static bool parse_profile(json_object *o, PvProfile *p, size_t index, char *err, size_t err_size)
{
	const char *name = get_str(o, "name");
	if(!name || !pv_profile_name_valid(name))
	{
		snprintf(err, err_size, "Profil Nr. %zu: \"name\" fehlt oder ist ungültig (erlaubt: A-Z a-z 0-9 . _ -)", index + 1);
		return false;
	}
	pv_strlcpy(p->name, name, sizeof(p->name));

#define FAIL(...) do { char _m[256]; snprintf(_m, sizeof(_m), __VA_ARGS__); \
		snprintf(err, err_size, "Profil \"%s\": %s", p->name, _m); return false; } while(0)

	const char *host = get_str(o, "host");
	if(!host || !*host || strlen(host) >= sizeof(p->host))
		FAIL("\"host\" fehlt oder ist ungültig");
	pv_strlcpy(p->host, host, sizeof(p->host));

	const char *console = get_str(o, "console");
	if(console && strcmp(console, "ps4") == 0)
		p->ps5 = false;
	else if(!console || strcmp(console, "ps5") == 0)
		p->ps5 = true;
	else
		FAIL("\"console\" muss \"ps5\" oder \"ps4\" sein");

	int64_t iv;
	p->target = get_int(o, "target", &iv) ? (int)iv : (p->ps5 ? 1000100 : 1000);

	const char *nick = get_str(o, "nickname");
	pv_strlcpy(p->nickname, nick ? nick : "", sizeof(p->nickname));

	const char *mac = get_str(o, "mac");
	if(mac && *mac)
	{
		if(!parse_mac(mac, p->mac))
			FAIL("\"mac\" ist ungültig");
		p->has_mac = true;
	}

	const char *acc = get_str(o, "account_id");
	if(!acc || !pv_base64_decode_exact(acc, p->account_id, sizeof(p->account_id)))
		FAIL("\"account_id\" fehlt oder ist kein base64-Wert mit 8 Byte");

	const char *rk = get_str(o, "regist_key");
	if(!rk || !pv_hex_decode(rk, p->regist_key, sizeof(p->regist_key)))
		FAIL("\"regist_key\" fehlt oder ist ungültig (erwartet 32 Hex-Zeichen)");

	const char *rpk = get_str(o, "rp_key");
	if(!rpk || !pv_hex_decode(rpk, p->rp_key, sizeof(p->rp_key)))
		FAIL("\"rp_key\" fehlt oder ist ungültig (erwartet 32 Hex-Zeichen)");

	p->rp_key_type = get_int(o, "rp_key_type", &iv) ? (uint32_t)iv : 0;

	const char *ra = get_str(o, "registered_at");
	pv_strlcpy(p->registered_at, ra ? ra : "", sizeof(p->registered_at));

	pv_stream_settings_init(&p->stream);
	json_object *s;
	if(json_object_object_get_ex(o, "stream", &s))
	{
		if(!json_object_is_type(s, json_type_object))
			FAIL("\"stream\" muss ein Objekt sein");
		PvStreamSettings *st = &p->stream;
		const char *v;
		if((v = get_str(s, "resolution")))
			pv_strlcpy(st->resolution, v, sizeof(st->resolution));
		if(get_int(s, "fps", &iv))
			st->fps = iv > 0 ? (unsigned)iv : 0;
		if(get_int(s, "bitrate_kbps", &iv))
			st->bitrate = iv > 0 ? (unsigned)iv : 0;
		if((v = get_str(s, "codec")))
			pv_strlcpy(st->codec, v, sizeof(st->codec));
		if((v = get_str(s, "audio_device")) && *v)
			st->audio_device = pv_strdup(v);
		json_object *w;
		if(json_object_object_get_ex(s, "wake", &w) && json_object_is_type(w, json_type_boolean))
			st->wake = json_object_get_boolean(w) ? 1 : 0;
		if((v = get_str(s, "hwdec")))
			pv_strlcpy(st->hwdec, v, sizeof(st->hwdec));
		if(get_int(s, "display", &iv) && iv >= 0 && iv < 16)
			st->display = (int)iv;
		if((v = get_str(s, "controller")))
			pv_strlcpy(st->controller, v, sizeof(st->controller));
	}
#undef FAIL
	return true;
}

static void offset_to_line(const char *json, size_t offset, int *line, int *col)
{
	*line = 1;
	*col = 1;
	for(size_t i = 0; i < offset && json[i]; i++)
	{
		if(json[i] == '\n')
		{
			(*line)++;
			*col = 1;
		}
		else
			(*col)++;
	}
}

bool pv_config_from_json(PvConfig *cfg, const char *json, size_t len, char *err, size_t err_size)
{
	json_tokener *tok = json_tokener_new();
	if(!tok)
	{
		snprintf(err, err_size, "Speicherfehler");
		return false;
	}
	json_object *root = json_tokener_parse_ex(tok, json, (int)len);
	enum json_tokener_error jerr = json_tokener_get_error(tok);
	if(!root || jerr != json_tokener_success)
	{
		int line, col;
		offset_to_line(json, json_tokener_get_parse_end(tok), &line, &col);
		snprintf(err, err_size, "JSON-Fehler in Zeile %d, Spalte %d: %s", line, col,
				jerr != json_tokener_success ? json_tokener_error_desc(jerr) : "unvollständig");
		if(root)
			json_object_put(root);
		json_tokener_free(tok);
		return false;
	}
	json_tokener_free(tok);

	bool ok = false;
	if(!json_object_is_type(root, json_type_object))
	{
		snprintf(err, err_size, "Wurzelelement muss ein JSON-Objekt sein");
		goto done;
	}
	int64_t version = 0;
	if(has_key(root, "format_version") && (!get_int(root, "format_version", &version) || version < 1))
	{
		snprintf(err, err_size, "\"format_version\" ist ungültig");
		goto done;
	}
	if(version > PV_CONFIG_FORMAT_VERSION)
	{
		snprintf(err, err_size, "Konfiguration hat Format-Version %lld, diese ps5view-Version kennt nur %d. Bitte ps5view aktualisieren.",
				(long long)version, PV_CONFIG_FORMAT_VERSION);
		goto done;
	}
	const char *def = get_str(root, "default_profile");
	pv_strlcpy(cfg->default_profile, def ? def : "", sizeof(cfg->default_profile));

	json_object *arr;
	if(json_object_object_get_ex(root, "profiles", &arr))
	{
		if(!json_object_is_type(arr, json_type_array))
		{
			snprintf(err, err_size, "\"profiles\" muss eine Liste sein");
			goto done;
		}
		size_t n = json_object_array_length(arr);
		for(size_t i = 0; i < n; i++)
		{
			json_object *o = json_object_array_get_idx(arr, i);
			if(!json_object_is_type(o, json_type_object))
			{
				snprintf(err, err_size, "Profil Nr. %zu ist kein JSON-Objekt", i + 1);
				goto done;
			}
			PvProfile tmp;
			memset(&tmp, 0, sizeof(tmp));
			tmp.ps5 = true;
			pv_stream_settings_init(&tmp.stream);
			if(!parse_profile(o, &tmp, i, err, err_size))
			{
				pv_stream_settings_free(&tmp.stream);
				goto done;
			}
			if(pv_config_find(cfg, tmp.name))
			{
				snprintf(err, err_size, "Profilname \"%s\" kommt mehrfach vor", tmp.name);
				pv_stream_settings_free(&tmp.stream);
				goto done;
			}
			PvProfile *p = pv_config_add(cfg);
			if(!p)
			{
				snprintf(err, err_size, "Speicherfehler");
				pv_stream_settings_free(&tmp.stream);
				goto done;
			}
			*p = tmp;
		}
	}
	ok = true;
done:
	json_object_put(root);
	return ok;
}

// ---------------------------------------------------------------------------
// files
// ---------------------------------------------------------------------------

PvConfigResult pv_config_load(PvConfig *cfg, const char *path, char *err, size_t err_size)
{
	config_init_empty(cfg, path);
	if(!cfg->path)
	{
		snprintf(err, err_size, "Konfigurationspfad kann nicht bestimmt werden");
		return PV_CONFIG_ERROR;
	}
	size_t len = 0;
	bool not_found = false;
	char *data = pv_read_file(cfg->path, CONFIG_MAX_SIZE, &len, &not_found);
	if(!data)
	{
		if(not_found)
		{
			LOGD("config", "Noch keine Konfiguration unter %s", cfg->path);
			return PV_CONFIG_NOT_FOUND;
		}
		char os[256];
		snprintf(err, err_size, "Konfiguration %s kann nicht gelesen werden: %s", cfg->path, pv_os_error_string(os, sizeof(os)));
		return PV_CONFIG_ERROR;
	}
	char perr[512];
	// tolerate a UTF-8 BOM (e.g. after editing with Notepad)
	const char *json = data;
	if(len >= 3 && (unsigned char)json[0] == 0xef && (unsigned char)json[1] == 0xbb && (unsigned char)json[2] == 0xbf)
	{
		json += 3;
		len -= 3;
	}
	bool ok = pv_config_from_json(cfg, json, len, perr, sizeof(perr));
	free(data);
	if(!ok)
	{
		snprintf(err, err_size, "Konfiguration %s ist fehlerhaft: %s", cfg->path, perr);
		for(size_t i = 0; i < cfg->count; i++)
			pv_stream_settings_free(&cfg->profiles[i].stream);
		cfg->count = 0;
		return PV_CONFIG_ERROR;
	}
	LOGD("config", "Konfiguration geladen: %s (%zu Profil(e))", cfg->path, cfg->count);
	return PV_CONFIG_OK;
}

bool pv_config_save(PvConfig *cfg, char *err, size_t err_size)
{
	char os[256];
	char *json = pv_config_to_json(cfg);
	if(!json)
	{
		snprintf(err, err_size, "Speicherfehler beim Erzeugen der Konfiguration");
		return false;
	}
	char *dir = pv_path_dirname(cfg->path);
	if(dir && !pv_mkdir_p(dir))
	{
		snprintf(err, err_size, "Verzeichnis %s kann nicht angelegt werden: %s", dir, pv_os_error_string(os, sizeof(os)));
		free(dir);
		free(json);
		return false;
	}
	free(dir);

	char *tmp = pv_asprintf("%s.tmp", cfg->path);
	char *bak = pv_asprintf("%s.bak", cfg->path);
	bool ok = false;
	if(!tmp || !bak)
	{
		snprintf(err, err_size, "Speicherfehler");
		goto out;
	}
	FILE *f = pv_fopen(tmp, "wb");
	if(!f)
	{
		snprintf(err, err_size, "%s kann nicht geschrieben werden: %s", tmp, pv_os_error_string(os, sizeof(os)));
		goto out;
	}
	pv_chmod_private(tmp);
	size_t len = strlen(json);
	bool written = fwrite(json, 1, len, f) == len && fputc('\n', f) != EOF;
	if(!pv_fclose_sync(f) || !written)
	{
		snprintf(err, err_size, "Fehler beim Schreiben von %s (Datenträger voll?)", tmp);
		pv_remove_file(tmp);
		goto out;
	}
	bool had_old = pv_file_exists(cfg->path);
	if(had_old && !pv_rename_replace(cfg->path, bak))
		LOGW("config", "Backup %s konnte nicht angelegt werden: %s", bak, pv_os_error_string(os, sizeof(os)));
	if(!pv_rename_replace(tmp, cfg->path))
	{
		snprintf(err, err_size, "%s kann nicht ersetzt werden: %s", cfg->path, pv_os_error_string(os, sizeof(os)));
		if(had_old && !pv_file_exists(cfg->path))
			pv_rename_replace(bak, cfg->path); // restore
		pv_remove_file(tmp);
		goto out;
	}
	pv_chmod_private(cfg->path);
	LOGD("config", "Konfiguration gespeichert: %s", cfg->path);
	ok = true;
out:
	free(tmp);
	free(bak);
	free(json);
	return ok;
}
