// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Registration helpers (exposed for unit tests).

#ifndef PV_REGISTER_H
#define PV_REGISTER_H

#include "config.h"

#include <chiaki/regist.h>

#include <stdbool.h>
#include <stdint.h>

bool pv_register_parse_pin(const char *s, uint32_t *pin);
void pv_register_choose_profile_name(PvConfig *cfg, const char *nickname, const char *host, const uint8_t account_id[8],
		char *out, size_t out_size);
// Creates or updates the profile for a successful registration. Keeps the
// stream settings of an updated profile. Returns NULL on allocation failure.
PvProfile *pv_register_store(PvConfig *cfg, const ChiakiRegisteredHost *rh, const char *name, const char *host, bool ps5,
		ChiakiTarget target, const uint8_t account_id[8], const char *fallback_nickname, bool make_default);

#endif
