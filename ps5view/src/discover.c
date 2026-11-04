// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "discover.h"

#include "log.h"
#include "netif.h"
#include "util.h"

#include <chiaki/thread.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#define DISCOVER_RESEND_MS 1000
#define MAX_TARGETS 64

typedef struct
{
	ChiakiMutex mutex;
	PvHostInfo *out;
	size_t max_out;
	size_t count;
	uint64_t start_ms;
} Collector;

static void copy_str(char *dst, size_t size, const char *src)
{
	pv_strlcpy(dst, src ? src : "", size);
}

static void discovery_cb(ChiakiDiscoveryHost *host, void *user)
{
	Collector *c = user;
	if(!host || !host->host_addr)
		return;
	chiaki_mutex_lock(&c->mutex);
	PvHostInfo *slot = NULL;
	for(size_t i = 0; i < c->count; i++)
		if(strcmp(c->out[i].addr, host->host_addr) == 0)
			slot = &c->out[i];
	if(!slot && c->count < c->max_out)
	{
		slot = &c->out[c->count++];
		memset(slot, 0, sizeof(*slot));
		slot->rtt_ms = pv_now_ms() - c->start_ms;
	}
	if(slot)
	{
		copy_str(slot->addr, sizeof(slot->addr), host->host_addr);
		copy_str(slot->name, sizeof(slot->name), host->host_name);
		copy_str(slot->host_id, sizeof(slot->host_id), host->host_id);
		copy_str(slot->host_type, sizeof(slot->host_type), host->host_type);
		copy_str(slot->system_version, sizeof(slot->system_version), host->system_version);
		copy_str(slot->protocol_version, sizeof(slot->protocol_version), host->device_discovery_protocol_version);
		copy_str(slot->running_app_name, sizeof(slot->running_app_name), host->running_app_name);
		copy_str(slot->running_app_titleid, sizeof(slot->running_app_titleid), host->running_app_titleid);
		slot->state = host->state;
		slot->ps5 = chiaki_discovery_host_is_ps5(host);
		slot->request_port = host->host_request_port;
	}
	chiaki_mutex_unlock(&c->mutex);
	LOGD("discover", "Antwort von %s: %s, Status %s", host->host_addr, host->host_name ? host->host_name : "?",
			chiaki_discovery_host_state_string(host->state));
}

static bool resolve_ipv4(const char *host, struct sockaddr_in *out)
{
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if(getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
		return false;
	bool ok = false;
	for(struct addrinfo *ai = res; ai; ai = ai->ai_next)
	{
		if(ai->ai_family == AF_INET && ai->ai_addrlen >= sizeof(struct sockaddr_in))
		{
			memcpy(out, ai->ai_addr, sizeof(*out));
			ok = true;
			break;
		}
	}
	freeaddrinfo(res);
	return ok;
}

int pv_discover(const PvDiscoverOptions *opt, PvHostInfo *out, size_t max_out, char *err, size_t err_size)
{
	ChiakiLog *log = pv_chiaki_log();
	struct sockaddr_in targets[MAX_TARGETS];
	size_t ntargets = 0;
	bool unicast = opt->hosts && opt->host_count;

	if(unicast)
	{
		for(size_t i = 0; i < opt->host_count && ntargets < MAX_TARGETS; i++)
		{
			if(!resolve_ipv4(opt->hosts[i], &targets[ntargets]))
			{
				snprintf(err, err_size, "Adresse \"%s\" ist ungültig oder kann nicht aufgelöst werden.", opt->hosts[i]);
				return -1;
			}
			ntargets++;
		}
	}
	else
	{
		PvNetIf ifs[32];
		size_t n = pv_netif_list(ifs, PV_ARRAY_SIZE(ifs));
		for(size_t i = 0; i < n && ntargets < MAX_TARGETS - 1; i++)
		{
			bool dup = false;
			for(size_t j = 0; j < ntargets; j++)
				if(targets[j].sin_addr.s_addr == ifs[i].broadcast_be)
					dup = true;
			if(dup)
				continue;
			LOGD("discover", "Interface \"%s\": %s/%u, Broadcast %s", ifs[i].name, ifs[i].addr, ifs[i].prefix_len, ifs[i].broadcast);
			memset(&targets[ntargets], 0, sizeof(targets[ntargets]));
			targets[ntargets].sin_family = AF_INET;
			targets[ntargets].sin_addr.s_addr = ifs[i].broadcast_be;
			ntargets++;
		}
		if(!n)
			LOGW("discover", "Keine aktive Netzwerkschnittstelle mit IPv4-Adresse gefunden.");
		memset(&targets[ntargets], 0, sizeof(targets[ntargets]));
		targets[ntargets].sin_family = AF_INET;
		targets[ntargets].sin_addr.s_addr = htonl(INADDR_BROADCAST);
		ntargets++;
	}

	Collector c;
	memset(&c, 0, sizeof(c));
	c.out = out;
	c.max_out = max_out;
	if(chiaki_mutex_init(&c.mutex, false) != CHIAKI_ERR_SUCCESS)
	{
		snprintf(err, err_size, "Interner Fehler (Mutex)");
		return -1;
	}

	ChiakiDiscovery discovery;
	ChiakiErrorCode cerr = chiaki_discovery_init(&discovery, log, AF_INET);
	if(cerr != CHIAKI_ERR_SUCCESS)
	{
		snprintf(err, err_size, "UDP-Socket für die Suche kann nicht geöffnet werden (%s). Blockiert eine Sicherheitssoftware ps5view?",
				chiaki_error_string(cerr));
		chiaki_mutex_fini(&c.mutex);
		return -1;
	}
	ChiakiDiscoveryThread thread;
	cerr = chiaki_discovery_thread_start(&thread, &discovery, discovery_cb, &c);
	if(cerr != CHIAKI_ERR_SUCCESS)
	{
		snprintf(err, err_size, "Discovery-Thread kann nicht gestartet werden (%s)", chiaki_error_string(cerr));
		chiaki_discovery_fini(&discovery);
		chiaki_mutex_fini(&c.mutex);
		return -1;
	}

	c.start_ms = pv_now_ms();
	uint64_t deadline = c.start_ms + opt->timeout_ms;
	uint64_t next_send = 0;
	unsigned rounds = 0;
	while(!pv_interrupted())
	{
		uint64_t now = pv_now_ms();
		if(now >= deadline)
			break;
		if(now >= next_send)
		{
			for(size_t i = 0; i < ntargets; i++)
			{
				char addr[INET_ADDRSTRLEN];
				inet_ntop(AF_INET, &targets[i].sin_addr, addr, sizeof(addr));
				ChiakiDiscoveryPacket packet;
				memset(&packet, 0, sizeof(packet));
				packet.cmd = CHIAKI_DISCOVERY_CMD_SRCH;

				struct sockaddr_in t = targets[i];
				t.sin_port = htons(CHIAKI_DISCOVERY_PORT_PS5);
				packet.protocol_version = CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5;
				if(chiaki_discovery_send(&discovery, &packet, (struct sockaddr *)&t, sizeof(t)) != CHIAKI_ERR_SUCCESS)
					LOGD("discover", "Senden an %s:%u fehlgeschlagen", addr, CHIAKI_DISCOVERY_PORT_PS5);
				if(opt->ps4_too)
				{
					t.sin_port = htons(CHIAKI_DISCOVERY_PORT_PS4);
					packet.protocol_version = CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS4;
					if(chiaki_discovery_send(&discovery, &packet, (struct sockaddr *)&t, sizeof(t)) != CHIAKI_ERR_SUCCESS)
						LOGD("discover", "Senden an %s:%u fehlgeschlagen", addr, CHIAKI_DISCOVERY_PORT_PS4);
				}
				if(rounds == 0)
					LOGD("discover", "Suchanfrage an %s gesendet", addr);
			}
			rounds++;
			next_send = now + DISCOVER_RESEND_MS;
		}
		if(unicast && opt->stop_when_all_answered)
		{
			chiaki_mutex_lock(&c.mutex);
			size_t found = c.count;
			chiaki_mutex_unlock(&c.mutex);
			if(found >= opt->host_count)
				break;
		}
		pv_sleep_ms(20);
	}

	chiaki_discovery_thread_stop(&thread);
	chiaki_discovery_fini(&discovery);
	size_t count = c.count;
	chiaki_mutex_fini(&c.mutex);
	return (int)count;
}

PvHostState pv_discover_host(const char *host, bool ps5, uint32_t timeout_ms, PvHostInfo *info)
{
	PvHostInfo found[4];
	const char *hosts[1] = { host };
	PvDiscoverOptions opt = { 0 };
	opt.hosts = hosts;
	opt.host_count = 1;
	opt.timeout_ms = timeout_ms;
	opt.stop_when_all_answered = true;
	opt.ps4_too = !ps5;
	char err[256];
	int n = pv_discover(&opt, found, PV_ARRAY_SIZE(found), err, sizeof(err));
	if(n < 0)
	{
		LOGE("discover", "%s", err);
		return PV_HOST_ERROR;
	}
	if(n == 0)
		return PV_HOST_NO_RESPONSE;
	if(info)
		*info = found[0];
	switch(found[0].state)
	{
		case CHIAKI_DISCOVERY_HOST_STATE_READY: return PV_HOST_READY;
		case CHIAKI_DISCOVERY_HOST_STATE_STANDBY: return PV_HOST_STANDBY;
		default: return PV_HOST_UNKNOWN_STATE;
	}
}

bool pv_regist_key_credential(const uint8_t regist_key[16], uint64_t *credential)
{
	char hex[17];
	size_t n = 0;
	while(n < 16 && regist_key[n])
	{
		char ch = (char)regist_key[n];
		if(!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
			return false;
		hex[n++] = ch;
	}
	hex[n] = '\0';
	if(n == 0)
		return false;
	*credential = strtoull(hex, NULL, 16);
	return true;
}

PvWakeResult pv_wake_and_wait(const char *host, const uint8_t regist_key[16], bool ps5, uint32_t timeout_ms, PvHostInfo *info)
{
	uint64_t credential;
	if(!pv_regist_key_credential(regist_key, &credential))
	{
		LOGE("wake", "Der gespeicherte Registrierungsschlüssel eignet sich nicht zum Aufwecken. Bitte neu registrieren.");
		return PV_WAKE_ERROR;
	}
	uint64_t start = pv_now_ms();
	uint64_t last_wake = 0;
	uint64_t last_progress = start;
	bool ever_answered = false;
	unsigned wakes = 0;
	while(!pv_interrupted())
	{
		uint64_t now = pv_now_ms();
		if(now - start >= timeout_ms)
			break;
		if(!last_wake || now - last_wake >= 10000)
		{
			ChiakiErrorCode e = chiaki_discovery_wakeup(pv_chiaki_log(), NULL, host, credential, ps5);
			wakes++;
			if(e != CHIAKI_ERR_SUCCESS)
				LOGW("wake", "Weckpaket an %s konnte nicht gesendet werden: %s", host, chiaki_error_string(e));
			else
				LOGI("wake", "Weckpaket an %s gesendet%s.", host, wakes > 1 ? " (erneut)" : "");
			last_wake = now;
		}
		PvHostInfo hi;
		PvHostState st = pv_discover_host(host, ps5, 1500, &hi);
		if(st == PV_HOST_ERROR)
			return PV_WAKE_ERROR;
		if(st != PV_HOST_NO_RESPONSE)
			ever_answered = true;
		if(st == PV_HOST_READY)
		{
			if(info)
				*info = hi;
			LOGI("wake", "PS5 ist bereit (nach %llu s).", (unsigned long long)((pv_now_ms() - start) / 1000));
			return PV_WAKE_READY;
		}
		now = pv_now_ms();
		if(now - last_progress >= 5000)
		{
			LOGI("wake", "Warte, bis die Konsole hochgefahren ist ... %llu s (Status: %s)",
					(unsigned long long)((now - start) / 1000),
					st == PV_HOST_STANDBY ? "Ruhemodus" : st == PV_HOST_NO_RESPONSE ? "keine Antwort" : "unbekannt");
			last_progress = now;
		}
		if(!pv_sleep_interruptible(500))
			break;
	}
	if(pv_interrupted())
		return PV_WAKE_INTERRUPTED;
	return ever_answered ? PV_WAKE_TIMEOUT : PV_WAKE_NO_RESPONSE;
}

const char *pv_host_state_text(ChiakiDiscoveryHostState s)
{
	switch(s)
	{
		case CHIAKI_DISCOVERY_HOST_STATE_READY: return "bereit (ready)";
		case CHIAKI_DISCOVERY_HOST_STATE_STANDBY: return "Ruhemodus (standby)";
		default: return "unbekannt";
	}
}
