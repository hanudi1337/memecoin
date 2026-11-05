// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Switch added to libchiaki by ps5view/cmake/PatchLibchiaki.cmake.

#ifndef PV_CHIAKI_PATCH_H
#define PV_CHIAKI_PATCH_H

#include <stdbool.h>

// true (default): libchiaki announces one controller and keeps sending its
// (idle) state, as every Remote Play client does.
// false: no controller announcement and no controller data at all
// (ps5view stream --controller none). Only change it while no session runs.
extern bool ps5view_announce_controller;

#endif
