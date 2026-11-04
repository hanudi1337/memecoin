// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Console discovery (UDP 9302 for PS5, 987 for PS4) and wake-up.

#ifndef PV_DISCOVER_H
#define PV_DISCOVER_H

#include <chiaki/discovery.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct pv_host_info_t
{
	char addr[64];
	char name[64];
	char host_id[32];
	char host_type[16];
	char system_version[32];
	char protocol_version[16];
	char running_app_name[128];
	char running_app_titleid[32];
	ChiakiDiscoveryHostState state;
	bool ps5;
	uint16_t request_port;
	uint64_t rtt_ms;
} PvHostInfo;

typedef struct pv_discover_options_t
{
	const char *const *hosts; // unicast targets; NULL/0 = broadcast on all interfaces
	size_t host_count;
	uint32_t timeout_ms;
	bool stop_when_all_answered; // only with unicast targets
	bool ps4_too;                // also query PS4 (port 987)
} PvDiscoverOptions;

// Returns the number of consoles found, or -1 on a local error (err is set).
int pv_discover(const PvDiscoverOptions *opt, PvHostInfo *out, size_t max_out, char *err, size_t err_size);

typedef enum
{
	PV_HOST_NO_RESPONSE,
	PV_HOST_READY,
	PV_HOST_STANDBY,
	PV_HOST_UNKNOWN_STATE,
	PV_HOST_ERROR
} PvHostState;

PvHostState pv_discover_host(const char *host, bool ps5, uint32_t timeout_ms, PvHostInfo *info);

typedef enum
{
	PV_WAKE_READY,
	PV_WAKE_TIMEOUT,      // console answered but did not become ready in time
	PV_WAKE_NO_RESPONSE,  // console never answered discovery
	PV_WAKE_INTERRUPTED,
	PV_WAKE_ERROR
} PvWakeResult;

// The wake-up "user credential" is the registration key interpreted as hex.
bool pv_regist_key_credential(const uint8_t regist_key[16], uint64_t *credential);
// tick (may be NULL) is called regularly; returning false aborts (PV_WAKE_INTERRUPTED).
typedef bool (*PvWakeTick)(void *user);
PvWakeResult pv_wake_and_wait(const char *host, const uint8_t regist_key[16], bool ps5, uint32_t timeout_ms,
		PvHostInfo *info, PvWakeTick tick, void *tick_user);

const char *pv_host_state_text(ChiakiDiscoveryHostState s);

#endif
