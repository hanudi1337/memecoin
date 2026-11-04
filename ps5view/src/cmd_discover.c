// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// ps5view discover [--host <ip>]... [--timeout <s>]

#include "app.h"
#include "args.h"
#include "discover.h"
#include "log.h"
#include "netif.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

static void usage(void)
{
	pv_out("Aufruf: ps5view discover [--host <ip>] [--timeout <sekunden>]");
	pv_out("%s", "");
	pv_out("Sucht PlayStation-Konsolen im lokalen Netz (UDP 9302 für PS5, 987 für PS4)");
	pv_out("und zeigt IP-Adresse, Name, Status (bereit/Ruhemodus) und Systemversion.");
	pv_out("%s", "");
	pv_out("  --host <ip>        nur diese Adresse direkt abfragen (mehrfach möglich);");
	pv_out("                     hilft, wenn Broadcasts im Netz blockiert werden");
	pv_out("  --timeout <s>      Wartezeit in Sekunden (Standard 3, max. 30)");
}

static const char *format_version(const char *v, char *buf, size_t size)
{
	// "08050001..." -> 8.05 (best effort; raw value is printed as well)
	if(!v || strlen(v) < 4)
		return v ? v : "?";
	int major = (v[0] - '0') * 10 + (v[1] - '0');
	int minor = (v[2] - '0') * 10 + (v[3] - '0');
	if(major < 0 || major > 99 || minor < 0 || minor > 99)
		return v;
	snprintf(buf, size, "%d.%02d (%.20s)", major, minor, v);
	return buf;
}

int pv_cmd_discover(int argc, char **argv)
{
	const char *hosts[16];
	size_t nhosts = 0;
	unsigned long timeout_s = 3;
	PvArgs a;
	pv_args_init(&a, "discover", argc, argv);
	while(pv_args_next(&a))
	{
		const char *v;
		if(pv_args_value(&a, "--host", &v))
		{
			if(nhosts >= PV_ARRAY_SIZE(hosts))
			{
				LOGE("discover", "Zu viele --host-Angaben (max. %zu).", PV_ARRAY_SIZE(hosts));
				return PV_EXIT_USAGE;
			}
			hosts[nhosts++] = v;
			continue;
		}
		if(pv_args_value(&a, "--timeout", &v))
		{
			if(!pv_parse_ulong(v, 1, 30, &timeout_s))
			{
				LOGE("discover", "--timeout erwartet 1 bis 30 Sekunden, nicht \"%s\".", v);
				return PV_EXIT_USAGE;
			}
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

	if(!nhosts)
	{
		PvNetIf ifs[32];
		size_t n = pv_netif_list(ifs, PV_ARRAY_SIZE(ifs));
		for(size_t i = 0; i < n; i++)
			LOGD("discover", "Netzwerk: %s (%s/%u)", ifs[i].name, ifs[i].addr, ifs[i].prefix_len);
		LOGI("discover", "Suche Konsolen per Broadcast über %zu Netzwerkschnittstelle(n) (%lu s) ...", n, timeout_s);
	}
	else
		LOGI("discover", "Frage %zu Adresse(n) direkt ab (%lu s) ...", nhosts, timeout_s);

	PvHostInfo found[32];
	PvDiscoverOptions opt = { 0 };
	opt.hosts = nhosts ? hosts : NULL;
	opt.host_count = nhosts;
	opt.timeout_ms = (uint32_t)timeout_s * 1000;
	opt.stop_when_all_answered = nhosts > 0;
	opt.ps4_too = true;
	char err[256];
	int n = pv_discover(&opt, found, PV_ARRAY_SIZE(found), err, sizeof(err));
	if(n < 0)
	{
		LOGE("discover", "%s", err);
		return PV_EXIT_NETWORK;
	}
	if(pv_interrupted())
		return PV_EXIT_INTERRUPTED;

	if(n == 0)
	{
		LOGE("discover", "Keine Konsole hat geantwortet.");
		LOGI("hint", "Was du tun kannst:\n"
				"- Ist die PS5 eingeschaltet? Im Ruhemodus antwortet sie nur, wenn unter Einstellungen > System > Energiesparen >\n"
				"  Im Ruhemodus verfügbare Funktionen \"Mit dem Internet verbunden bleiben\" aktiviert ist. Komplett aus: keine Antwort.\n"
				"- PC und PS5 im selben Netz? (kein Gast-WLAN, keine WLAN-Client-Isolation am Router)\n"
				"- Windows-Firewall: ps5view für \"Private Netzwerke\" zulassen; Netzwerkprofil auf \"Privat\" stellen.\n"
				"- VPN am PC trennen.\n"
				"- Wenn du die IP kennst (Router oder PS5: Einstellungen > Netzwerk > Verbindungsstatus anzeigen):\n"
				"  ps5view discover --host <ip>");
		return PV_EXIT_NETWORK;
	}

	pv_out("%d Konsole(n) gefunden:", n);
	for(int i = 0; i < n; i++)
	{
		PvHostInfo *h = &found[i];
		char vbuf[64];
		pv_out("%s", "");
		pv_out("  Adresse:       %s", h->addr);
		pv_out("  Name:          %s", h->name[0] ? h->name : "?");
		pv_out("  Typ:           %s", h->ps5 ? "PS5" : (h->host_type[0] ? h->host_type : "PS4"));
		pv_out("  Status:        %s", pv_host_state_text(h->state));
		pv_out("  Systemversion: %s", format_version(h->system_version, vbuf, sizeof(vbuf)));
		if(h->host_id[0])
			pv_out("  Host-ID:       %s", h->host_id);
		if(h->running_app_name[0])
			pv_out("  Läuft gerade:  %s%s%s%s", h->running_app_name, h->running_app_titleid[0] ? " (" : "",
					h->running_app_titleid, h->running_app_titleid[0] ? ")" : "");
		pv_out("  Antwortzeit:   %llu ms", (unsigned long long)h->rtt_ms);
	}
	for(size_t i = 0; i < nhosts; i++)
	{
		bool answered = false;
		for(int j = 0; j < n; j++)
			if(strcmp(found[j].addr, hosts[i]) == 0)
				answered = true;
		if(!answered)
			LOGW("discover", "%s hat nicht geantwortet.", hosts[i]);
	}
	return PV_EXIT_OK;
}
