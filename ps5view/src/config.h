// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Configuration file (JSON) with any number of registrations ("profiles"),
// e.g. one per PSN account. Default location:
//   Windows: %APPDATA%\ps5view\config.json
//   Linux:   ~/.config/ps5view/config.json

#ifndef PV_CONFIG_H
#define PV_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PV_CONFIG_FORMAT_VERSION 1
#define PV_PROFILE_NAME_MAX 64

typedef struct pv_stream_settings_t
{
	char resolution[8]; // "720p" / "1080p" / ... ; "" = not set
	unsigned fps;       // 30 / 60 ; 0 = not set
	unsigned bitrate;   // kbit/s ; 0 = not set
	char codec[8];      // "h264" / "h265" ; "" = not set
	char *audio_device; // exact SDL device name ; NULL = system default
	int wake;           // -1 = not set, 0 = no, 1 = yes
	char hwdec[24];     // "auto" / "none" / "d3d11va" / ... ; "" = not set
	int display;        // monitor index for the window ; -1 = not set
} PvStreamSettings;

typedef struct pv_profile_t
{
	char name[PV_PROFILE_NAME_MAX];
	char host[256];
	bool ps5;
	int target;           // ChiakiTarget of the registration
	char nickname[0x21];  // console name reported at registration
	bool has_mac;
	uint8_t mac[6];
	uint8_t account_id[8];
	uint8_t regist_key[16]; // ChiakiRegisteredHost.rp_regist_key (raw bytes)
	uint8_t rp_key[16];     // ChiakiRegisteredHost.rp_key ("morning")
	uint32_t rp_key_type;
	char registered_at[32];
	PvStreamSettings stream;
} PvProfile;

typedef struct pv_config_t
{
	char *path;
	PvProfile *profiles;
	size_t count;
	char default_profile[PV_PROFILE_NAME_MAX];
} PvConfig;

typedef enum
{
	PV_CONFIG_OK,
	PV_CONFIG_NOT_FOUND, // no file yet: cfg is valid and empty
	PV_CONFIG_ERROR
} PvConfigResult;

char *pv_config_default_path(void);
// path == NULL: default location. cfg is always initialized (also on error).
PvConfigResult pv_config_load(PvConfig *cfg, const char *path, char *err, size_t err_size);
bool pv_config_save(PvConfig *cfg, char *err, size_t err_size);
void pv_config_free(PvConfig *cfg);

PvProfile *pv_config_find(PvConfig *cfg, const char *name);
PvProfile *pv_config_add(PvConfig *cfg); // appends an empty profile
bool pv_config_remove(PvConfig *cfg, const char *name);
// Explicit name, else default_profile, else the only profile.
PvProfile *pv_config_select(PvConfig *cfg, const char *name, char *err, size_t err_size);

bool pv_profile_name_valid(const char *name);
void pv_profile_sanitize_name(const char *in, char *out, size_t out_size);
void pv_stream_settings_init(PvStreamSettings *s);
void pv_stream_settings_free(PvStreamSettings *s);

// JSON (de)serialization, exposed for tests.
char *pv_config_to_json(const PvConfig *cfg);
bool pv_config_from_json(PvConfig *cfg, const char *json, size_t len, char *err, size_t err_size);

#endif
