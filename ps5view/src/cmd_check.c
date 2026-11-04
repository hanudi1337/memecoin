// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// ps5view check [--profile <name>] [--host <ip>]
//
// Diagnoses the typical problems step by step without starting a stream.

#include "app.h"
#include "args.h"
#include "audio.h"
#include "config.h"
#include "discover.h"
#include "log.h"
#include "netif.h"
#include "pv_version.h"
#include "util.h"
#include "video.h"

#include <chiaki/common.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <opus/opus.h>
#include <SDL.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close close
#endif

typedef struct
{
	unsigned ok, warn, fail;
	int first_fail_code;
} Summary;

static void report(Summary *s, int level, int fail_code, const char *fmt, ...) PV_PRINTF(4, 5);

// level: 0 = OK, 1 = WARN, 2 = FAIL, 3 = INFO
static void report(Summary *s, int level, int fail_code, const char *fmt, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	static const char *const tags[] = { "[ OK ]", "[WARN]", "[FAIL]", "[INFO]" };
	pv_out("%s %s", tags[level], msg);
	if(level == 0)
		s->ok++;
	else if(level == 1)
		s->warn++;
	else if(level == 2)
	{
		s->fail++;
		if(!s->first_fail_code)
			s->first_fail_code = fail_code;
	}
}

typedef enum
{
	TCP_OPEN,
	TCP_REFUSED,
	TCP_TIMEOUT,
	TCP_ERROR
} TcpResult;

static TcpResult tcp_probe(const char *host, unsigned short port, unsigned timeout_ms)
{
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char port_s[8];
	snprintf(port_s, sizeof(port_s), "%u", port);
	if(getaddrinfo(host, port_s, &hints, &res) != 0 || !res)
		return TCP_ERROR;
	sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if(s == SOCK_INVALID)
	{
		freeaddrinfo(res);
		return TCP_ERROR;
	}
#ifdef _WIN32
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);
#else
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
	TcpResult r = TCP_ERROR;
	int c = connect(s, res->ai_addr, (int)res->ai_addrlen);
	if(c == 0)
		r = TCP_OPEN;
	else
	{
#ifdef _WIN32
		bool in_progress = WSAGetLastError() == WSAEWOULDBLOCK;
#else
		bool in_progress = errno == EINPROGRESS;
#endif
		if(in_progress)
		{
			fd_set wfds, efds;
			FD_ZERO(&wfds);
			FD_ZERO(&efds);
			FD_SET(s, &wfds);
			FD_SET(s, &efds);
			struct timeval tv = { (long)(timeout_ms / 1000), (long)(timeout_ms % 1000) * 1000 };
			int n = select((int)s + 1, NULL, &wfds, &efds, &tv);
			if(n == 0)
				r = TCP_TIMEOUT;
			else if(n > 0)
			{
				int soerr = 0;
				socklen_t len = sizeof(soerr);
				getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len);
#ifdef _WIN32
				r = soerr == 0 ? TCP_OPEN : (soerr == WSAECONNREFUSED ? TCP_REFUSED : TCP_ERROR);
#else
				r = soerr == 0 ? TCP_OPEN : (soerr == ECONNREFUSED ? TCP_REFUSED : TCP_ERROR);
#endif
			}
		}
#ifndef _WIN32
		else if(errno == ECONNREFUSED)
			r = TCP_REFUSED;
#endif
	}
	sock_close(s);
	freeaddrinfo(res);
	return r;
}

static bool host_in_subnet(const char *host, const PvNetIf *ifs, size_t n, const PvNetIf **match)
{
	struct in_addr a;
	if(inet_pton(AF_INET, host, &a) != 1)
		return true; // hostname: cannot tell
	if((ntohl(a.s_addr) >> 24) == 127)
		return true; // loopback (tests)
	for(size_t i = 0; i < n; i++)
	{
		uint32_t mask = ifs[i].prefix_len ? htonl(0xffffffffu << (32 - ifs[i].prefix_len)) : 0;
		if((a.s_addr & mask) == (ifs[i].addr_be & mask))
		{
			*match = &ifs[i];
			return true;
		}
	}
	return false;
}

static void check_decoders(Summary *s)
{
	const AVCodec *h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
	const AVCodec *hevc = avcodec_find_decoder(AV_CODEC_ID_HEVC);
	if(h264 && hevc)
		report(s, 0, 0, "FFmpeg %s: Decoder H.264 und H.265 vorhanden", av_version_info());
	else
		report(s, h264 ? 1 : 2, PV_EXIT_DEVICE, "FFmpeg %s: H.264 %s, H.265 %s", av_version_info(), h264 ? "ja" : "FEHLT", hevc ? "ja" : "fehlt");
#ifdef _WIN32
	static const char *const hw[] = { "d3d11va", "dxva2" };
#elif defined(__APPLE__)
	static const char *const hw[] = { "videotoolbox" };
#else
	static const char *const hw[] = { "vaapi", "vdpau", "cuda" };
#endif
	bool any = false;
	char list[256] = "";
	for(size_t i = 0; i < PV_ARRAY_SIZE(hw); i++)
	{
		enum AVHWDeviceType t = av_hwdevice_find_type_by_name(hw[i]);
		bool ok = false;
		if(t != AV_HWDEVICE_TYPE_NONE)
		{
			AVBufferRef *ctx = NULL;
			ok = av_hwdevice_ctx_create(&ctx, t, NULL, NULL, 0) >= 0;
			av_buffer_unref(&ctx);
		}
		size_t len = strlen(list);
		snprintf(list + len, sizeof(list) - len, "%s%s %s", len ? ", " : "", hw[i], ok ? "ok" : "nicht verfügbar");
		any = any || ok;
	}
	if(any)
		report(s, 0, 0, "Hardware-Decoding: %s", list);
	else
		report(s, 1, 0, "Kein Hardware-Decoder nutzbar (%s) - Software-Decoding funktioniert, braucht aber mehr CPU", list);
}

static void check_graphics(Summary *s)
{
	if(SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
	{
		report(s, 2, PV_EXIT_DEVICE, "Grafik: SDL kann nicht initialisiert werden: %s", SDL_GetError());
		return;
	}
	SDL_Window *w = SDL_CreateWindow("ps5view check", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 320, 180, SDL_WINDOW_HIDDEN);
	if(!w)
	{
		report(s, 2, PV_EXIT_DEVICE, "Grafik: Fenster kann nicht erstellt werden: %s", SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
		return;
	}
	SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED);
	bool accel = r != NULL;
	if(!r)
		r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
	SDL_RendererInfo info;
	if(r && SDL_GetRendererInfo(r, &info) == 0)
	{
		bool nv12 = false;
		for(Uint32 i = 0; i < info.num_texture_formats; i++)
			nv12 = nv12 || info.texture_formats[i] == SDL_PIXELFORMAT_NV12;
		report(s, accel ? 0 : 1, 0, "Grafik: SDL %s, Renderer %s%s, %d Bildschirm(e)", SDL_GetCurrentVideoDriver(), info.name,
				accel ? (nv12 ? " (NV12 direkt)" : "") : " (nur Software-Rendering)", SDL_GetNumVideoDisplays());
	}
	else
		report(s, 2, PV_EXIT_DEVICE, "Grafik: kein Renderer verfügbar: %s", SDL_GetError());
	if(r)
		SDL_DestroyRenderer(r);
	SDL_DestroyWindow(w);
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void check_audio(Summary *s, const char *configured)
{
	if(SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
	{
		report(s, 2, PV_EXIT_DEVICE, "Audio: SDL kann nicht initialisiert werden: %s", SDL_GetError());
		return;
	}
	char **names = NULL;
	size_t n = pv_audio_list_devices(&names);
	report(s, n ? 0 : 1, 0, "Audio: Treiber %s, %zu Ausgabegerät(e), Opus %s", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
			n, opus_get_version_string());
	if(configured)
	{
		size_t idx;
		PvAudioMatch m = pv_audio_match_device(configured, names, n, &idx);
		if(m == PV_AUDIO_MATCH_EXACT || m == PV_AUDIO_MATCH_UNIQUE)
			report(s, 0, 0, "Audio: Profil-Gerät \"%s\" ist vorhanden", names[idx]);
		else
			report(s, 2, PV_EXIT_DEVICE, "Audio: Profil-Gerät \"%s\" %s (ps5view list-audio)", configured,
					m == PV_AUDIO_MATCH_AMBIGUOUS ? "ist nicht eindeutig" : "wurde NICHT gefunden");
	}
	else
	{
		bool cable = false;
		for(size_t i = 0; i < n; i++)
			cable = cable || pv_str_icontains(names[i], "CABLE Input");
		report(s, 3, 0, "Audio: im Profil ist kein Gerät gesetzt (Standardgerät)%s", cable ? "; \"CABLE Input\" ist vorhanden" : "");
	}
	pv_audio_free_list(names, n);
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

int pv_cmd_check(int argc, char **argv)
{
	const char *profile_name = NULL, *host_arg = NULL;
	PvArgs a;
	pv_args_init(&a, "check", argc, argv);
	while(pv_args_next(&a))
	{
		if(pv_args_value(&a, "--profile", &profile_name) || pv_args_value(&a, "--host", &host_arg))
			continue;
		if(pv_args_is_help(&a))
		{
			pv_out("Aufruf: ps5view check [--profile <name>] [--host <ip>]");
			pv_out("Prüft Schritt für Schritt Konfiguration, Netzwerk, Konsole, Decoder, Audio und Grafik.");
			return PV_EXIT_OK;
		}
		return pv_args_unknown(&a);
	}
	if(a.failed)
		return PV_EXIT_USAGE;

	pv_video_route_ffmpeg_log();
	Summary s = { 0 };
	char os[128];
	pv_os_description(os, sizeof(os));
	report(&s, 3, 0, "ps5view %s (git %s), libchiaki %.12s, %s", PV_VERSION, PV_GIT_COMMIT, PV_CHIAKI_COMMIT, os);

	// ---- configuration ----
	PvConfig cfg;
	char err[1024];
	PvConfigResult cr = pv_config_load(&cfg, pv_global.config_path, err, sizeof(err));
	PvProfile *p = NULL;
	if(cr == PV_CONFIG_ERROR)
		report(&s, 2, PV_EXIT_CONFIG, "Konfiguration: %s", err);
	else if(cr == PV_CONFIG_NOT_FOUND || !cfg.count)
		report(&s, host_arg ? 3 : 1, 0, "Konfiguration: noch keine Konsole registriert (%s)", cfg.path);
	else
	{
		report(&s, 0, 0, "Konfiguration: %s (%zu Profil(e))", cfg.path, cfg.count);
		p = pv_config_select(&cfg, profile_name, err, sizeof(err));
		if(!p)
			report(&s, 2, PV_EXIT_CONFIG, "Profil: %s", err);
		else
			report(&s, 0, 0, "Profil \"%s\": %s (%s) unter %s", p->name, p->nickname[0] ? p->nickname : "?", p->ps5 ? "PS5" : "PS4", p->host);
	}
	const char *host = host_arg ? host_arg : (p ? p->host : NULL);
	bool ps5 = p ? p->ps5 : true;

	// ---- network ----
	PvNetIf ifs[32];
	size_t nifs = pv_netif_list(ifs, PV_ARRAY_SIZE(ifs));
	if(!nifs)
		report(&s, 2, PV_EXIT_NETWORK, "Netzwerk: keine aktive IPv4-Verbindung gefunden");
	for(size_t i = 0; i < nifs; i++)
		report(&s, 3, 0, "Netzwerk: \"%s\" %s/%u", ifs[i].name, ifs[i].addr, ifs[i].prefix_len);
	if(nifs > 1)
		report(&s, 3, 0, "Mehrere Netzwerkadapter aktiv - ps5view sucht auf allen (VPN-Adapter ggf. trennen)");

	if(host)
	{
		const PvNetIf *match = NULL;
		if(nifs && !host_in_subnet(host, ifs, nifs, &match))
			report(&s, 1, 0, "%s liegt in keinem lokalen Netz dieses PCs - Gast-WLAN, anderes Subnetz oder VPN?", host);
		else if(match)
			report(&s, 0, 0, "%s liegt im lokalen Netz von \"%s\"", host, match->name);

		PvHostInfo hi;
		PvHostState hs = pv_discover_host(host, ps5, 3000, &hi);
		switch(hs)
		{
			case PV_HOST_READY:
				report(&s, 0, 0, "Konsole antwortet (UDP 9302): %s, bereit, System %s, %llu ms", hi.name, hi.system_version,
						(unsigned long long)hi.rtt_ms);
				break;
			case PV_HOST_STANDBY:
				report(&s, 1, 0, "Konsole antwortet (UDP 9302): %s ist im Ruhemodus (stream --wake weckt sie)", hi.name);
				break;
			case PV_HOST_UNKNOWN_STATE:
				report(&s, 1, 0, "Konsole antwortet mit unbekanntem Status");
				break;
			case PV_HOST_NO_RESPONSE:
				report(&s, 2, PV_EXIT_NETWORK, "Keine Antwort von %s auf UDP 9302 (aus? falsche IP? Firewall/VPN? \"ps5view discover\" probieren)", host);
				break;
			default:
				report(&s, 2, PV_EXIT_NETWORK, "Konsolensuche fehlgeschlagen (lokaler Fehler, siehe Log)");
				break;
		}
		if(hs == PV_HOST_READY)
		{
			switch(tcp_probe(host, 9295, 3000))
			{
				case TCP_OPEN:
					report(&s, 0, 0, "Remote-Play-Port TCP 9295 ist erreichbar");
					break;
				case TCP_REFUSED:
					report(&s, 2, PV_EXIT_CONSOLE, "TCP 9295 abgelehnt: Remote Play an der PS5 deaktiviert? (Einstellungen > System > Remote Play)");
					break;
				case TCP_TIMEOUT:
					report(&s, 2, PV_EXIT_NETWORK, "TCP 9295: Zeitüberschreitung - blockiert eine Firewall ausgehende Verbindungen?");
					break;
				default:
					report(&s, 2, PV_EXIT_NETWORK, "TCP 9295: Verbindungstest fehlgeschlagen");
					break;
			}
		}
		else if(hs == PV_HOST_STANDBY)
			report(&s, 3, 0, "Port-Test übersprungen (Konsole im Ruhemodus)");
		if(p)
		{
			uint64_t cred;
			if(pv_regist_key_credential(p->regist_key, &cred))
				report(&s, 0, 0, "Registrierungsschlüssel ist gültig (Aufwecken möglich)");
			else
				report(&s, 1, 0, "Registrierungsschlüssel hat ein unerwartetes Format - Aufwecken eventuell nicht möglich");
		}
	}
	else
		report(&s, 3, 0, "Konsolentest übersprungen (kein Profil, keine --host-Angabe)");

	// ---- decoders, graphics, audio ----
	check_decoders(&s);
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	SDL_SetMainReady();
	if(SDL_Init(SDL_INIT_EVENTS) == 0)
	{
		check_graphics(&s);
		check_audio(&s, p ? p->stream.audio_device : NULL);
		SDL_Quit();
	}
	else
		report(&s, 2, PV_EXIT_DEVICE, "SDL kann nicht initialisiert werden: %s", SDL_GetError());

	pv_out("%s", "");
	if(s.fail)
		pv_out("Ergebnis: %u Fehler, %u Warnung(en). Die Logdatei enthält Details.", s.fail, s.warn);
	else if(s.warn)
		pv_out("Ergebnis: keine Fehler, %u Warnung(en).", s.warn);
	else
		pv_out("Ergebnis: alles in Ordnung.");
	pv_config_free(&cfg);
	return s.fail ? (s.first_fail_code ? s.first_fail_code : PV_EXIT_INTERNAL) : PV_EXIT_OK;
}
