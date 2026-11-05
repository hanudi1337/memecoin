// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// ps5view stream [--profile <name>] [options]
//
// Connects to the console and shows its picture (and sound) in a window.
// View-only: no controller, keyboard or touch input is ever sent. By default
// Remote Play still announces one (idle) virtual controller to the console, as every
// Remote Play client does.

#include "app.h"
#include "args.h"
#include "audio.h"
#include "chiaki_patch.h"
#include "config.h"
#include "diag.h"
#include "discover.h"
#include "errors.h"
#include "log.h"
#include "util.h"
#include "video.h"

#include <chiaki/session.h>
#include <chiaki/thread.h>

#include <SDL.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_RETRIES 5
#define DEFAULT_WAKE_TIMEOUT_S 60
#define STABLE_STREAM_MS 60000      // a stream that ran this long resets the retry counter
#define NO_VIDEO_WARN_MS 10000
#define STATS_INTERVAL_MS 30000
#define STOP_TIMEOUT_MS 8000
#define AUDIO_BUFFER_MS 50

enum
{
	SEV_CONNECTED = 1,
	SEV_LOGIN_PIN,
	SEV_LOGIN_PIN_INCORRECT,
	SEV_QUIT
};

typedef struct
{
	ChiakiVideoResolutionPreset res;
	const char *res_name;
	unsigned fps;
	unsigned bitrate; // 0 = preset default
	ChiakiCodec codec;
	const char *audio_query; // NULL = system default
	bool no_audio;
	bool wake;
	unsigned wake_timeout_s;
	unsigned retries;
	const char *hwdec;
	const char *renderer;
	int win_w, win_h;
	int display;
	bool fullscreen;
	const char *login_pin;
	bool skip_check;
	bool dualsense;           // controller type announced to the console
	bool announce_controller; // false = --controller none (experimental, no controller data at all)
} StreamOpts;

typedef struct
{
	PvProfile *profile;
	StreamOpts opt;
	PvVideo *video;
	PvAudio *audio;
	char *audio_device; // resolved exact name, NULL = default
	PvDiag diag;
	Uint32 ev_session;
	bool quit_requested;

	// current attempt
	uintptr_t attempt_id;
	ChiakiSession *session; // non-NULL while a session runs
	bool stop_sent;
	uint64_t stop_sent_ms;
	bool connected;
	uint64_t connected_ms;
	bool quit_received;
	atomic_bool quit_cb_seen; // set by the callback too, in case the SDL event got lost
	ChiakiQuitReason quit_reason;
	char quit_reason_str[256];
	PvFailure own_failure; // failures detected by us (login PIN, ...)
	uint64_t last_stats_ms;
	bool warned_no_video;
	uint64_t frames_at_last_check;
	uint64_t last_frame_seen_ms;
} Stream;

// ---------------------------------------------------------------------------
// option parsing
// ---------------------------------------------------------------------------

static bool parse_resolution(const char *s, ChiakiVideoResolutionPreset *res, const char **name)
{
	static const struct { const char *name; ChiakiVideoResolutionPreset res; } map[] = {
		{ "360p", CHIAKI_VIDEO_RESOLUTION_PRESET_360p },
		{ "540p", CHIAKI_VIDEO_RESOLUTION_PRESET_540p },
		{ "720p", CHIAKI_VIDEO_RESOLUTION_PRESET_720p },
		{ "1080p", CHIAKI_VIDEO_RESOLUTION_PRESET_1080p },
	};
	for(size_t i = 0; i < PV_ARRAY_SIZE(map); i++)
	{
		if(pv_strcasecmp(s, map[i].name) == 0)
		{
			*res = map[i].res;
			*name = map[i].name;
			return true;
		}
	}
	return false;
}

static bool parse_codec(const char *s, ChiakiCodec *codec)
{
	if(pv_strcasecmp(s, "h264") == 0 || pv_strcasecmp(s, "avc") == 0)
		*codec = CHIAKI_CODEC_H264;
	else if(pv_strcasecmp(s, "h265") == 0 || pv_strcasecmp(s, "hevc") == 0)
		*codec = CHIAKI_CODEC_H265;
	else
		return false;
	return true;
}

static bool parse_size(const char *s, int *w, int *h)
{
	unsigned a, b;
	char tail;
	if(sscanf(s, "%ux%u%c", &a, &b, &tail) != 2 && sscanf(s, "%uX%u%c", &a, &b, &tail) != 2)
		return false;
	if(a < 320 || b < 180 || a > 16384 || b > 16384)
		return false;
	*w = (int)a;
	*h = (int)b;
	return true;
}

static bool parse_digits(const char *s, size_t min_len, size_t max_len)
{
	size_t n = strlen(s);
	if(n < min_len || n > max_len)
		return false;
	for(const char *p = s; *p; p++)
		if(*p < '0' || *p > '9')
			return false;
	return true;
}

static void usage(void)
{
	pv_out("Aufruf: ps5view stream [--profile <name>] [optionen]");
	pv_out("%s", "");
	pv_out("Verbindet sich mit der PS5 und zeigt Bild und Ton in einem Fenster (nur Zuschauen:");
	pv_out("es werden keine Controller- oder Tastatureingaben gesendet).");
	pv_out("Fenster: frei skalierbar, F11/Alt+Enter/Doppelklick = Vollbild, Schließen = Verbindung beenden.");
	pv_out("%s", "");
	pv_out("  --profile <name>        Profil (Standard: Standardprofil oder einziges Profil)");
	pv_out("  --resolution <r>        360p, 540p, 720p oder 1080p (Standard PS5: 1080p)");
	pv_out("  --fps <30|60>           Bildrate (Standard 60)");
	pv_out("  --bitrate <kbit/s>      Videobitrate (Standard: 720p 10000, 1080p 15000)");
	pv_out("  --codec <h264|h265>     Videocodec (Standard h264)");
	pv_out("  --audio-device <name>   Audio-Ausgabegerät, exakt oder eindeutiger Teil (z. B. \"CABLE Input\");");
	pv_out("                          \"default\" = Windows-Standardgerät. Liste: ps5view list-audio");
	pv_out("  --no-audio              ohne Ton");
	pv_out("  --wake / --no-wake      PS5 aus dem Ruhemodus aufwecken (Standard: nein)");
	pv_out("  --wake-timeout <s>      so lange auf das Hochfahren warten (Standard %d)", DEFAULT_WAKE_TIMEOUT_S);
	pv_out("  --retries <n>           Verbindungsversuche mit steigender Pause (Standard %d, 1 = keine Wiederholung)", DEFAULT_RETRIES);
	pv_out("  --login-pin <ziffern>   Anmelde-Passcode des PS5-Benutzers, falls einer gesetzt ist");
	pv_out("  --hwdec <name>          auto (Standard), none, d3d11va, dxva2, ...");
	pv_out("  --renderer <name>       SDL-Renderer: direct3d11, direct3d, opengl, software");
	pv_out("  --window-size <BxH>     Startgröße des Fensters, z. B. 1280x720");
	pv_out("  --display <n>           Fenster auf Bildschirm n öffnen (0 = erster)");
	pv_out("  --fullscreen            im Vollbild starten");
	pv_out("  --controller <typ>      ds4 (Standard) oder dualsense: Typ des unbetätigten Remote-Play-Controllers;");
	pv_out("                          none: Experiment - keinen Controller anmelden, keinerlei Controllerdaten senden");
	pv_out("  --skip-check            Konsolensuche vor dem Verbinden überspringen");
	pv_out("  --save                  angegebene Stream-Optionen im Profil speichern");
}

// ---------------------------------------------------------------------------
// libchiaki callbacks (any thread) -> SDL events for the main thread
// ---------------------------------------------------------------------------

static void push_session_event(Stream *s, int code)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = s->ev_session;
	ev.user.code = code;
	ev.user.data1 = (void *)s->attempt_id;
	if(SDL_PushEvent(&ev) != 1)
		LOGW("stream", "SDL-Ereignis konnte nicht zugestellt werden: %s", SDL_GetError());
}

static void session_event_cb(ChiakiEvent *event, void *user)
{
	// Called with libchiaki's internal locks held: never call chiaki_session_*() here.
	Stream *s = user;
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			push_session_event(s, SEV_CONNECTED);
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			push_session_event(s, event->login_pin_request.pin_incorrect ? SEV_LOGIN_PIN_INCORRECT : SEV_LOGIN_PIN);
			break;
		case CHIAKI_EVENT_QUIT:
			s->quit_reason = event->quit.reason;
			pv_strlcpy(s->quit_reason_str, event->quit.reason_str ? event->quit.reason_str : "", sizeof(s->quit_reason_str));
			atomic_store(&s->quit_cb_seen, true);
			push_session_event(s, SEV_QUIT);
			break;
		case CHIAKI_EVENT_NICKNAME_RECEIVED:
			LOGD("stream", "Konsolenname: %.32s", event->server_nickname);
			break;
		case CHIAKI_EVENT_VIDEO_FEC_FAILURE:
			LOGD("stream", "Bild %d konnte nicht rekonstruiert werden%s", event->video_fec_failure.frame_index,
					event->video_fec_failure.idr_request_sent ? " (Keyframe angefordert)" : "");
			break;
		default:
			// rumble, LEDs, haptics, keyboard requests ...: irrelevant for a viewer
			break;
	}
}

static void request_idr_cb(void *user)
{
	Stream *s = user;
	if(s->session)
		chiaki_session_request_idr(s->session);
}

// ---------------------------------------------------------------------------
// main-thread event handling
// ---------------------------------------------------------------------------

static void stop_session(Stream *s)
{
	if(s->session && !s->stop_sent)
	{
		chiaki_session_stop(s->session);
		s->stop_sent = true;
		s->stop_sent_ms = pv_now_ms();
	}
}

static void update_title_connected(Stream *s)
{
	PvVideoStats vs;
	pv_video_get_stats(s->video, &vs);
	char st[192];
	if(vs.width)
		snprintf(st, sizeof(st), "verbunden (%dx%d, %s%s)", vs.width, vs.height, vs.decoder, vs.hw_frames ? " HW" : "");
	else
		snprintf(st, sizeof(st), "verbunden - warte auf Bild ...");
	pv_video_set_status(s->video, st);
}

static void handle_session_event(Stream *s, const SDL_Event *e)
{
	if((uintptr_t)e->user.data1 != s->attempt_id || !s->session)
		return; // stale event of an earlier attempt
	switch(e->user.code)
	{
		case SEV_CONNECTED:
			s->connected = true;
			s->connected_ms = pv_now_ms();
			s->last_frame_seen_ms = s->connected_ms;
			LOGI("stream", "Verbunden mit %s. Warte auf das erste Bild ...", s->profile->host);
			update_title_connected(s);
			break;
		case SEV_LOGIN_PIN:
			if(s->opt.login_pin)
			{
				LOGI("stream", "Die PS5 fragt nach dem Anmelde-Passcode - sende --login-pin.");
				chiaki_session_set_login_pin(s->session, (const uint8_t *)s->opt.login_pin, strlen(s->opt.login_pin));
			}
			else
			{
				pv_failure_set(&s->own_failure, PV_EXIT_AUTH, PV_RETRY_NO,
						"- Den Passcode mit --login-pin <ziffern> angeben (oder an der PS5 unter Einstellungen > Benutzer und Konten >\n"
						"  Anmeldeeinstellungen > Passcode für die Anmeldung an der PS5 entfernen).",
						"Der PS5-Benutzer ist mit einem Anmelde-Passcode geschützt.");
				stop_session(s);
			}
			break;
		case SEV_LOGIN_PIN_INCORRECT:
			pv_failure_set(&s->own_failure, PV_EXIT_AUTH, PV_RETRY_NO,
					"- --login-pin prüfen (Passcode für die Anmeldung an der PS5, nicht die Registrierungs-PIN).",
					"Der Anmelde-Passcode (--login-pin) ist falsch.");
			stop_session(s);
			break;
		case SEV_QUIT:
			s->quit_received = true;
			break;
	}
}

static void handle_event(Stream *s, const SDL_Event *e)
{
	if(e->type == s->ev_session)
	{
		handle_session_event(s, e);
		return;
	}
	if(e->type == SDL_QUIT)
	{
		if(!s->quit_requested)
			LOGI("stream", "Fenster geschlossen - beende die Verbindung ...");
		s->quit_requested = true;
		return;
	}
	if(e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_CLOSE && e->window.windowID == pv_video_window_id(s->video))
	{
		if(!s->quit_requested)
			LOGI("stream", "Fenster geschlossen - beende die Verbindung ...");
		s->quit_requested = true;
		return;
	}
	if(s->audio && pv_audio_handle_event(s->audio, e))
		return;
	pv_video_handle_event(s->video, e);
}

// Processes events for up to wait_ms. Returns false if the user wants to quit.
static bool pump(Stream *s, uint32_t wait_ms)
{
	SDL_Event e;
	if(SDL_WaitEventTimeout(&e, (int)wait_ms))
	{
		handle_event(s, &e);
		int n = 0;
		while(n++ < 256 && SDL_PollEvent(&e))
			handle_event(s, &e);
	}
	if(pv_interrupted() && !s->quit_requested)
	{
		LOGI("stream", "Strg+C - beende die Verbindung ...");
		s->quit_requested = true;
	}
	return !s->quit_requested;
}

static bool wake_tick(void *user)
{
	return pump((Stream *)user, 0);
}

// Waits ms while keeping the window responsive. False if the user quit.
static bool wait_responsive(Stream *s, uint32_t ms)
{
	uint64_t end = pv_now_ms() + ms;
	while(pv_now_ms() < end)
	{
		uint64_t left = end - pv_now_ms();
		if(!pump(s, left > 100 ? 100 : (uint32_t)left))
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// periodic checks while a session runs
// ---------------------------------------------------------------------------

static void periodic(Stream *s)
{
	uint64_t now = pv_now_ms();
	PvVideoStats vs;
	pv_video_get_stats(s->video, &vs);
	if(s->connected)
	{
		if(vs.frames_shown != s->frames_at_last_check)
		{
			if(s->frames_at_last_check == 0 && vs.frames_shown)
				LOGI("stream", "Bild läuft: %dx%d, Decoder %s%s.", vs.width, vs.height, vs.decoder, vs.hw_frames ? " (Hardware)" : "");
			if(s->warned_no_video)
				LOGI("stream", "Das Bild läuft wieder.");
			s->frames_at_last_check = vs.frames_shown;
			s->last_frame_seen_ms = now;
			s->warned_no_video = false;
			update_title_connected(s);
		}
		else if(!s->warned_no_video && now - s->last_frame_seen_ms > NO_VIDEO_WARN_MS)
		{
			s->warned_no_video = true;
			if(vs.samples == 0 && !s->opt.announce_controller && vs.frames_shown == 0)
				LOGW("stream", "Seit %u s kommen keine Videodaten an. Mit --controller none sendet die PS5 eventuell kein Bild -"
						" dann ohne diese Option starten (und ps5view mit einem zweiten PSN-Konto registrieren, siehe README).",
						(unsigned)((now - s->last_frame_seen_ms) / 1000));
			else if(vs.samples == 0)
				LOGW("stream", "Seit %u s kommen keine Videodaten an. Blockiert eine Firewall UDP von der PS5 (Port 9296)?",
						(unsigned)((now - s->last_frame_seen_ms) / 1000));
			else
				LOGW("stream", "Seit %u s kein neues Bild, obwohl Daten ankommen (Decoder %s, %llu Fehler). Ggf. --codec h264 oder --hwdec none versuchen.",
						(unsigned)((now - s->last_frame_seen_ms) / 1000), vs.decoder, (unsigned long long)vs.decode_errors);
			pv_video_set_status(s->video, "verbunden - kein Bild");
		}
	}
	if(s->audio)
		pv_audio_tick(s->audio);
	if(now - s->last_stats_ms >= STATS_INTERVAL_MS)
	{
		s->last_stats_ms = now;
		PvAudioStats as;
		memset(&as, 0, sizeof(as));
		if(s->audio)
			pv_audio_get_stats(s->audio, &as);
		LOGD("stats", "Video: %llu Bilder angezeigt, %llu Pakete, %llu verloren, %llu Decoderfehler, %dx%d, %s%s | "
				"Audio: %s, Puffer %u ms, %llu Leerläufe, %llu Verwerfungen",
				(unsigned long long)vs.frames_shown, (unsigned long long)vs.samples, (unsigned long long)vs.frames_lost,
				(unsigned long long)vs.decode_errors, vs.width, vs.height, vs.decoder, vs.hw_frames ? " HW" : "",
				s->audio ? (as.device_open ? "aktiv" : "Gerät nicht offen") : "aus", as.queued_ms,
				(unsigned long long)as.underruns, (unsigned long long)as.clears);
	}
}

// ---------------------------------------------------------------------------
// one connection attempt
// ---------------------------------------------------------------------------

typedef enum
{
	PRE_OK,
	PRE_FAILED,
	PRE_QUIT
} PrecheckResult;

static PrecheckResult precheck(Stream *s, PvFailure *f)
{
	PvProfile *p = s->profile;
	PvHostInfo hi;
	PvHostState hs = pv_discover_host(p->host, p->ps5, 2500, &hi);
	if(!pump(s, 0))
		return PRE_QUIT;
	switch(hs)
	{
		case PV_HOST_READY:
			LOGI("stream", "Konsole %s (%s) ist bereit.", hi.name[0] ? hi.name : p->host, p->host);
			if(hi.running_app_name[0])
				LOGI("stream", "Läuft gerade: %s", hi.running_app_name);
			return PRE_OK;
		case PV_HOST_STANDBY:
		case PV_HOST_NO_RESPONSE:
			if(hs == PV_HOST_STANDBY && !s->opt.wake)
			{
				pv_failure_set(f, PV_EXIT_CONSOLE, PV_RETRY_NO,
						"- ps5view mit --wake starten (dauerhaft: ps5view stream --wake --save), oder\n"
						"- die PS-Taste am Controller drücken.",
						"Die PS5 ist im Ruhemodus.");
				return PRE_FAILED;
			}
			if(!s->opt.wake)
			{
				LOGW("stream", "%s antwortet nicht auf die Konsolensuche (UDP 9302) - versuche trotzdem zu verbinden ...", p->host);
				return PRE_OK;
			}
			LOGI("stream", "%s - wecke die Konsole auf ...", hs == PV_HOST_STANDBY ? "Die PS5 ist im Ruhemodus" : "Keine Antwort von der PS5");
			pv_video_set_status(s->video, "PS5 wird aufgeweckt ...");
			{
				PvWakeResult w = pv_wake_and_wait(p->host, p->regist_key, p->ps5, s->opt.wake_timeout_s * 1000, &hi, wake_tick, s);
				switch(w)
				{
					case PV_WAKE_READY:
						// Remote Play needs a moment after the console reports "ready"
						if(!wait_responsive(s, 2000))
							return PRE_QUIT;
						return PRE_OK;
					case PV_WAKE_INTERRUPTED:
						return PRE_QUIT;
					case PV_WAKE_TIMEOUT:
						pv_failure_set(f, PV_EXIT_CONSOLE, PV_RETRY_FAST,
								"- Für das Aufwecken über das Netz: Einstellungen > System > Energiesparen > Im Ruhemodus verfügbare\n"
								"  Funktionen > \"Einschalten der PS5 über das Netzwerk ermöglichen\" aktivieren.\n"
								"- Längere Wartezeit: --wake-timeout 120",
								"Die PS5 ist nach %u s noch nicht bereit.", s->opt.wake_timeout_s);
						return PRE_FAILED;
					case PV_WAKE_NO_RESPONSE:
						if(hs == PV_HOST_NO_RESPONSE)
						{
							LOGW("stream", "Die PS5 antwortet weiterhin nicht auf die Suche - versuche trotzdem zu verbinden ...");
							return PRE_OK;
						}
						pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_FAST, "", "Die PS5 antwortet nach dem Aufwecken nicht mehr.");
						return PRE_FAILED;
					case PV_WAKE_ERROR:
					default:
						pv_failure_set(f, PV_EXIT_CONFIG, PV_RETRY_NO,
								"- Konsole neu registrieren (ps5view register ...).",
								"Das Aufwecken ist nicht möglich (Registrierungsschlüssel ungeeignet).");
						return PRE_FAILED;
				}
			}
		case PV_HOST_UNKNOWN_STATE:
			LOGW("stream", "Die Konsole meldet einen unbekannten Status - versuche zu verbinden ...");
			return PRE_OK;
		case PV_HOST_ERROR:
		default:
			return PRE_OK;
	}
}

// Runs one session. Returns false if the user quit (f is then irrelevant).
static bool run_session(Stream *s, PvFailure *f, uint64_t *connected_duration_ms)
{
	PvProfile *p = s->profile;
	*connected_duration_ms = 0;
	pv_failure_clear(f);
	pv_failure_clear(&s->own_failure);
	pv_diag_reset(&s->diag);
	s->connected = false;
	s->quit_received = false;
	atomic_store(&s->quit_cb_seen, false);
	s->stop_sent = false;
	s->warned_no_video = false;
	s->frames_at_last_check = 0;
	s->quit_reason = CHIAKI_QUIT_REASON_NONE;
	s->quit_reason_str[0] = '\0';

	char err[256];
	if(!pv_video_start_decoder(s->video, s->opt.codec, s->opt.fps, err, sizeof(err)))
	{
		pv_failure_set(f, PV_EXIT_DEVICE, PV_RETRY_NO, "- FFmpeg-Installation prüfen; --codec h264 versuchen.", "%s", err);
		return true;
	}
	{
		PvVideoStats vs;
		pv_video_get_stats(s->video, &vs);
		s->frames_at_last_check = vs.frames_shown;
	}

	ChiakiConnectInfo ci;
	memset(&ci, 0, sizeof(ci));
	ci.ps5 = p->ps5;
	ci.host = p->host;
	memcpy(ci.regist_key, p->regist_key, sizeof(ci.regist_key));
	memcpy(ci.morning, p->rp_key, sizeof(ci.morning));
	chiaki_connect_video_profile_preset(&ci.video_profile, s->opt.res, s->opt.fps == 30 ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60);
	if(s->opt.bitrate)
		ci.video_profile.bitrate = s->opt.bitrate;
	ci.video_profile.codec = s->opt.codec;
	ci.video_profile_auto_downgrade = true;
	ci.enable_keyboard = false;   // view-only
	ci.enable_dualsense = s->opt.dualsense && s->opt.announce_controller; // only the type of the (idle) announced controller
	ci.audio_video_disabled = s->audio ? CHIAKI_NONE_DISABLED : CHIAKI_AUDIO_DISABLED;
	ci.auto_regist = false;
	ci.holepunch_session = NULL;
	ci.rudp_sock = NULL;
	memcpy(ci.psn_account_id, p->account_id, sizeof(ci.psn_account_id));
	ci.packet_loss_max = 0.05;
	ci.enable_idr_on_fec_failure = true;

	LOGI("stream", "Verbinde mit %s: %s, %u fps, %u kbit/s, %s%s ...", p->host, s->opt.res_name, s->opt.fps,
			ci.video_profile.bitrate, chiaki_codec_name(ci.video_profile.codec), s->audio ? "" : ", ohne Ton");

	ChiakiSession session;
	ChiakiErrorCode cerr = chiaki_session_init(&session, &ci, pv_chiaki_log());
	if(cerr != CHIAKI_ERR_SUCCESS)
	{
		pv_video_stop_decoder(s->video);
		if(cerr == CHIAKI_ERR_PARSE_ADDR)
			pv_failure_set(f, PV_EXIT_CONFIG, PV_RETRY_NO,
					"- Adresse im Profil prüfen (ps5view profiles) bzw. neu registrieren.",
					"Die Adresse \"%s\" ist ungültig oder kann nicht aufgelöst werden.", p->host);
		else
			pv_failure_set(f, PV_EXIT_INTERNAL, PV_RETRY_NO, "", "Sitzung kann nicht initialisiert werden: %s", chiaki_error_string(cerr));
		return true;
	}
	chiaki_session_set_event_cb(&session, session_event_cb, s);
	chiaki_session_set_video_sample_cb(&session, pv_video_sample_cb, s->video);
	if(s->audio)
	{
		ChiakiAudioSink sink;
		pv_audio_get_sink(s->audio, &sink);
		chiaki_session_set_audio_sink(&session, &sink);
	}
	// Intentionally never calling chiaki_session_set_controller_state(): the
	// console only ever sees the idle state of the announced controller.

	s->attempt_id++;
	s->session = &session;
	ps5view_announce_controller = s->opt.announce_controller; // read by libchiaki's stream thread
	cerr = chiaki_session_start(&session);
	if(cerr != CHIAKI_ERR_SUCCESS)
	{
		s->session = NULL;
		chiaki_session_fini(&session);
		ps5view_announce_controller = true;
		pv_video_stop_decoder(s->video);
		pv_failure_set(f, PV_EXIT_INTERNAL, PV_RETRY_NO, "", "Sitzung kann nicht gestartet werden: %s", chiaki_error_string(cerr));
		return true;
	}

	uint64_t last_periodic = 0;
	while(!s->quit_received && !atomic_load(&s->quit_cb_seen))
	{
		pump(s, 50);
		if(s->quit_requested)
			stop_session(s);
		if(s->stop_sent && pv_now_ms() - s->stop_sent_ms > STOP_TIMEOUT_MS)
		{
			LOGW("stream", "libchiaki reagiert nicht auf das Beenden - warte auf den Sitzungs-Thread ...");
			break;
		}
		uint64_t now = pv_now_ms();
		if(now - last_periodic >= 500)
		{
			last_periodic = now;
			periodic(s);
		}
	}
	if(s->connected)
		*connected_duration_ms = pv_now_ms() - s->connected_ms;
	chiaki_session_join(&session);
	s->session = NULL;
	chiaki_session_fini(&session);
	ps5view_announce_controller = true;
	pv_video_stop_decoder(s->video);

	if(s->quit_requested && (s->quit_reason == CHIAKI_QUIT_REASON_STOPPED || s->quit_reason == CHIAKI_QUIT_REASON_NONE))
		return false;

	PvDiagInfo d;
	pv_diag_snapshot(&s->diag, &d);
	if(s->own_failure.failed)
		*f = s->own_failure;
	else
		pv_failure_from_session(f, s->quit_reason, s->quit_reason_str, &d, p->host);
	if(f->failed && !s->opt.announce_controller && s->connected && *connected_duration_ms < 30000)
		strncat(f->hint, "\n- Mit --controller none hat die PS5 die Sitzung kurz nach dem Verbinden beendet: ohne diese Option starten\n"
				"  und ps5view mit einem zweiten PSN-Konto registrieren (README: \"Mit nur einem Controller an der PS5 spielen\").",
				sizeof(f->hint) - strlen(f->hint) - 1);
	LOGD("stream", "Sitzung beendet: %s%s%s", chiaki_quit_reason_string(s->quit_reason),
			s->quit_reason_str[0] ? " - " : "", s->quit_reason_str);
	return !s->quit_requested;
}

static uint32_t backoff_ms(PvRetryClass r, unsigned attempt)
{
	// attempt = number of the attempt that just failed (1-based)
	static const uint32_t fast[] = { 2000, 4000, 8000, 16000, 30000 };
	static const uint32_t slow[] = { 10000, 20000, 30000, 45000, 60000 };
	unsigned i = attempt ? attempt - 1 : 0;
	if(r == PV_RETRY_SLOW)
		return slow[i < PV_ARRAY_SIZE(slow) ? i : PV_ARRAY_SIZE(slow) - 1];
	return fast[i < PV_ARRAY_SIZE(fast) ? i : PV_ARRAY_SIZE(fast) - 1];
}

// ---------------------------------------------------------------------------

static int resolve_audio_device(Stream *s)
{
	if(s->opt.no_audio)
		return PV_EXIT_OK;
	if(!s->opt.audio_query || pv_strcasecmp(s->opt.audio_query, "default") == 0)
	{
		s->audio_device = NULL;
		return PV_EXIT_OK;
	}
	char **names = NULL;
	size_t n = pv_audio_list_devices(&names);
	size_t idx = 0;
	PvAudioMatch m = pv_audio_match_device(s->opt.audio_query, names, n, &idx);
	int rc = PV_EXIT_OK;
	switch(m)
	{
		case PV_AUDIO_MATCH_EXACT:
		case PV_AUDIO_MATCH_UNIQUE:
			s->audio_device = pv_strdup(names[idx]);
			if(m == PV_AUDIO_MATCH_UNIQUE)
				LOGI("stream", "Audiogerät \"%s\" -> \"%s\"", s->opt.audio_query, names[idx]);
			break;
		case PV_AUDIO_MATCH_AMBIGUOUS:
			LOGE("stream", "Audiogerät \"%s\" ist nicht eindeutig. Passende Geräte:", s->opt.audio_query);
			for(size_t i = 0; i < n; i++)
				if(pv_str_icontains(names[i], s->opt.audio_query))
					LOGI("stream", "  \"%s\"", names[i]);
			LOGI("hint", "Was du tun kannst:\n- Den vollständigen Namen in Anführungszeichen angeben: --audio-device \"<name>\"");
			rc = PV_EXIT_DEVICE;
			break;
		case PV_AUDIO_MATCH_NONE:
		default:
			LOGE("stream", "Audiogerät \"%s\" nicht gefunden. Der Ton wird NICHT auf ein anderes Gerät umgeleitet.", s->opt.audio_query);
			if(n)
			{
				LOGI("stream", "Verfügbare Ausgabegeräte:");
				for(size_t i = 0; i < n; i++)
					LOGI("stream", "  \"%s\"", names[i]);
			}
			else
				LOGI("stream", "Es wurden keine Audio-Ausgabegeräte gefunden.");
			LOGI("hint", "Was du tun kannst:\n"
					"- Ist VB-Audio Virtual Cable installiert und aktiviert? (Windows: Sound-Einstellungen > Ausgabegeräte)\n"
					"- Namen mit \"ps5view list-audio\" prüfen; ohne Ton starten: --no-audio");
			rc = PV_EXIT_DEVICE;
			break;
	}
	pv_audio_free_list(names, n);
	return rc;
}

static int stream_main(Stream *s)
{
	PvProfile *p = s->profile;

	pv_video_route_ffmpeg_log();
	char title[160];
	snprintf(title, sizeof(title), "ps5view - %s", p->name);
	PvVideoOptions vo;
	memset(&vo, 0, sizeof(vo));
	vo.width = s->opt.win_w;
	vo.height = s->opt.win_h;
	vo.display = s->opt.display;
	vo.fullscreen = s->opt.fullscreen;
	vo.hwdec = s->opt.hwdec;
	vo.title = title;
	char err[512];
	s->video = pv_video_create(&vo, err, sizeof(err));
	if(!s->video)
	{
		LOGE("stream", "%s", err);
		LOGI("hint", "Was du tun kannst:\n- Grafiktreiber aktualisieren; anderen Renderer versuchen: --renderer software");
		return PV_EXIT_DEVICE;
	}
	pv_video_set_idr_callback(s->video, request_idr_cb, s);

	if(!s->opt.no_audio)
	{
		s->audio = pv_audio_create(s->audio_device, AUDIO_BUFFER_MS, err, sizeof(err));
		if(!s->audio)
			LOGW("stream", "Ohne Ton: %s", err);
	}

	if(s->opt.announce_controller)
	{
		LOGI("stream", "Nur Zuschauen: ps5view sendet keine Eingaben. Remote Play meldet aber einen (unbetätigten) %s an.",
				s->opt.dualsense ? "DualSense" : "DualShock 4");
		LOGI("stream", "Die PS5 schaltet beim Verbinden deinen Controller aus: PS-Taste drücken und dein Konto wählen."
				" Ist ps5view mit DEMSELBEN Konto registriert, beendet die PS5 dabei Remote Play -"
				" dann ps5view mit einem zweiten PSN-Konto registrieren (README) oder --controller none testen.");
	}
	else
		LOGI("stream", "Experiment --controller none: ps5view meldet keinen Controller an und sendet keinerlei Controllerdaten."
				" Achte beim Verbinden darauf, ob dein Controller an der PS5 an bleibt - und gib mir Bescheid.");

	int rc = PV_EXIT_OK;
	PvFailure f;
	pv_failure_clear(&f);
	unsigned attempt = 1;
	unsigned total_attempts = 0;
	char last_message[sizeof(f.message)] = "";
	bool hint_suppressed = false;
	while(!s->quit_requested)
	{
		total_attempts++;
		char st[128];
		snprintf(st, sizeof(st), "verbinde ... (Versuch %u/%u)", attempt, s->opt.retries);
		pv_video_set_status(s->video, st);
		if(attempt > 1 || total_attempts > 1)
			LOGI("stream", "Verbindungsversuch %u von %u ...", attempt, s->opt.retries);

		pv_failure_clear(&f);
		uint64_t connected_ms = 0;
		bool user_quit = false;
		PrecheckResult pre = s->opt.skip_check ? PRE_OK : precheck(s, &f);
		if(pre == PRE_QUIT)
			user_quit = true;
		else if(pre == PRE_OK)
			user_quit = !run_session(s, &f, &connected_ms);
		if(user_quit || s->quit_requested)
		{
			rc = PV_EXIT_OK;
			break;
		}
		if(!f.failed)
		{
			// regular end (e.g. console went to rest mode)
			pv_failure_print(&f);
			rc = f.exit_code;
			break;
		}

		pv_video_clear(s->video);
		if(strcmp(last_message, f.message) == 0)
		{
			// same problem as last time: keep the console readable
			LOGE("stream", "%s", f.message);
			hint_suppressed = true;
		}
		else
			pv_failure_print(&f);
		pv_strlcpy(last_message, f.message, sizeof(last_message));
		if(connected_ms >= STABLE_STREAM_MS)
		{
			LOGI("stream", "Der Stream lief %llu min - Versuchszähler wird zurückgesetzt.", (unsigned long long)(connected_ms / 60000));
			attempt = 1;
		}
		if(f.retry == PV_RETRY_NO)
		{
			LOGE("stream", "Abbruch: Ein erneuter Versuch kann diesen Fehler nicht beheben.");
			rc = f.exit_code;
			break;
		}
		if(attempt >= s->opt.retries)
		{
			LOGE("stream", "Aufgegeben nach %u Versuch(en). Letzter Fehler: %s", attempt, f.message);
			if(hint_suppressed && f.hint[0])
				LOGI("hint", "Was du tun kannst:\n%s", f.hint);
			rc = f.exit_code;
			break;
		}
		uint32_t wait = backoff_ms(f.retry, attempt);
		LOGI("stream", "Neuer Versuch (%u/%u) in %u s ... (Abbrechen: Fenster schließen oder Strg+C)", attempt + 1, s->opt.retries, wait / 1000);
		snprintf(st, sizeof(st), "getrennt - neuer Versuch in %u s", wait / 1000);
		pv_video_set_status(s->video, st);
		attempt++;
		if(!wait_responsive(s, wait))
			break;
	}
	if(s->quit_requested && rc == PV_EXIT_OK)
		LOGI("stream", "Verbindung beendet.");
	if(rc != PV_EXIT_OK)
		pv_video_set_status(s->video, "Fehler - siehe Konsole");
	return rc;
}

int pv_cmd_stream(int argc, char **argv)
{
	const char *profile_name = NULL;
	const char *res_s = NULL, *fps_s = NULL, *bitrate_s = NULL, *codec_s = NULL, *audio_s = NULL;
	const char *wake_timeout_s = NULL, *retries_s = NULL, *hwdec_s = NULL, *renderer_s = NULL;
	const char *size_s = NULL, *display_s = NULL, *login_pin_s = NULL, *controller_s = NULL;
	int wake = -1;
	bool no_audio = false, fullscreen = false, save = false, skip_check = false;

	PvArgs a;
	pv_args_init(&a, "stream", argc, argv);
	while(pv_args_next(&a))
	{
		if(pv_args_value(&a, "--profile", &profile_name) || pv_args_value(&a, "--resolution", &res_s)
				|| pv_args_value(&a, "--fps", &fps_s) || pv_args_value(&a, "--bitrate", &bitrate_s)
				|| pv_args_value(&a, "--codec", &codec_s) || pv_args_value(&a, "--audio-device", &audio_s)
				|| pv_args_value(&a, "--wake-timeout", &wake_timeout_s) || pv_args_value(&a, "--retries", &retries_s)
				|| pv_args_value(&a, "--hwdec", &hwdec_s) || pv_args_value(&a, "--renderer", &renderer_s)
				|| pv_args_value(&a, "--window-size", &size_s) || pv_args_value(&a, "--display", &display_s)
				|| pv_args_value(&a, "--login-pin", &login_pin_s) || pv_args_value(&a, "--controller", &controller_s))
			continue;
		if(pv_args_flag(&a, "--wake")) { wake = 1; continue; }
		if(pv_args_flag(&a, "--no-wake")) { wake = 0; continue; }
		if(pv_args_flag(&a, "--no-audio")) { no_audio = true; continue; }
		if(pv_args_flag(&a, "--fullscreen")) { fullscreen = true; continue; }
		if(pv_args_flag(&a, "--save")) { save = true; continue; }
		if(pv_args_flag(&a, "--skip-check")) { skip_check = true; continue; }
		if(pv_args_is_help(&a))
		{
			usage();
			return PV_EXIT_OK;
		}
		return pv_args_unknown(&a);
	}
	if(a.failed)
		return PV_EXIT_USAGE;

	PvConfig cfg;
	char err[1024];
	if(pv_config_load(&cfg, pv_global.config_path, err, sizeof(err)) == PV_CONFIG_ERROR)
	{
		LOGE("stream", "%s", err);
		pv_config_free(&cfg);
		return PV_EXIT_CONFIG;
	}
	PvProfile *p = pv_config_select(&cfg, profile_name, err, sizeof(err));
	if(!p)
	{
		LOGE("stream", "%s", err);
		pv_config_free(&cfg);
		return PV_EXIT_CONFIG;
	}
	PvStreamSettings *ps = &p->stream;

	// ---- merge: command line > profile > defaults ----
	Stream s;
	memset(&s, 0, sizeof(s));
	s.profile = p;
	StreamOpts *o = &s.opt;
	o->res = p->ps5 ? CHIAKI_VIDEO_RESOLUTION_PRESET_1080p : CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
	o->res_name = p->ps5 ? "1080p" : "720p";
	o->fps = 60;
	o->codec = CHIAKI_CODEC_H264;
	o->wake_timeout_s = DEFAULT_WAKE_TIMEOUT_S;
	o->retries = DEFAULT_RETRIES;
	o->hwdec = "auto";
	o->display = -1;
	o->win_w = 1280;
	o->win_h = 720;

	bool bad = false;
	const char *v;
	unsigned long ul;
	if((v = res_s ? res_s : (ps->resolution[0] ? ps->resolution : NULL)) && !parse_resolution(v, &o->res, &o->res_name))
	{
		LOGE("stream", "Unbekannte Auflösung \"%s\" (erlaubt: 360p, 540p, 720p, 1080p).", v);
		bad = true;
	}
	if(fps_s)
	{
		if(!pv_parse_ulong(fps_s, 30, 60, &ul) || (ul != 30 && ul != 60))
		{
			LOGE("stream", "--fps muss 30 oder 60 sein.");
			bad = true;
		}
		else
			o->fps = (unsigned)ul;
	}
	else if(ps->fps == 30 || ps->fps == 60)
		o->fps = ps->fps;
	if(bitrate_s)
	{
		if(!pv_parse_ulong(bitrate_s, 1000, 100000, &ul))
		{
			LOGE("stream", "--bitrate erwartet 1000 bis 100000 kbit/s.");
			bad = true;
		}
		else
			o->bitrate = (unsigned)ul;
	}
	else if(ps->bitrate)
		o->bitrate = ps->bitrate;
	if((v = codec_s ? codec_s : (ps->codec[0] ? ps->codec : NULL)) && !parse_codec(v, &o->codec))
	{
		LOGE("stream", "Unbekannter Codec \"%s\" (erlaubt: h264, h265).", v);
		bad = true;
	}
	if(!p->ps5 && o->codec != CHIAKI_CODEC_H264)
	{
		LOGW("stream", "Die PS4 unterstützt nur H.264 - verwende h264.");
		o->codec = CHIAKI_CODEC_H264;
	}
	o->audio_query = audio_s ? audio_s : ps->audio_device;
	o->no_audio = no_audio;
	o->wake = wake >= 0 ? wake == 1 : ps->wake == 1;
	if(wake_timeout_s)
	{
		if(!pv_parse_ulong(wake_timeout_s, 10, 600, &ul))
		{
			LOGE("stream", "--wake-timeout erwartet 10 bis 600 Sekunden.");
			bad = true;
		}
		else
			o->wake_timeout_s = (unsigned)ul;
	}
	if(retries_s)
	{
		if(!pv_parse_ulong(retries_s, 1, 50, &ul))
		{
			LOGE("stream", "--retries erwartet 1 bis 50.");
			bad = true;
		}
		else
			o->retries = (unsigned)ul;
	}
	if(hwdec_s)
		o->hwdec = hwdec_s;
	else if(ps->hwdec[0])
		o->hwdec = ps->hwdec;
	o->renderer = renderer_s;
	if(size_s && !parse_size(size_s, &o->win_w, &o->win_h))
	{
		LOGE("stream", "--window-size erwartet BREITExHÖHE, z. B. 1280x720.");
		bad = true;
	}
	if(display_s)
	{
		if(!pv_parse_ulong(display_s, 0, 15, &ul))
		{
			LOGE("stream", "--display erwartet eine Bildschirmnummer 0 bis 15.");
			bad = true;
		}
		else
			o->display = (int)ul;
	}
	else if(ps->display >= 0)
		o->display = ps->display;
	o->fullscreen = fullscreen;
	if(login_pin_s && !parse_digits(login_pin_s, 4, 8))
	{
		LOGE("stream", "--login-pin erwartet 4 bis 8 Ziffern.");
		bad = true;
	}
	o->login_pin = login_pin_s;
	o->skip_check = skip_check;
	o->announce_controller = true;
	if((v = controller_s ? controller_s : (ps->controller[0] ? ps->controller : NULL)))
	{
		if(pv_strcasecmp(v, "dualsense") == 0 || pv_strcasecmp(v, "ds") == 0)
			o->dualsense = true;
		else if(pv_strcasecmp(v, "ds4") == 0 || pv_strcasecmp(v, "dualshock4") == 0)
			o->dualsense = false;
		else if(pv_strcasecmp(v, "none") == 0)
			o->announce_controller = false;
		else
		{
			LOGE("stream", "--controller erwartet ds4, dualsense oder none.");
			bad = true;
		}
	}
	if(!p->ps5)
		o->dualsense = false;
	if(bad)
	{
		pv_config_free(&cfg);
		return PV_EXIT_USAGE;
	}

	if(save)
	{
		if(res_s)
			pv_strlcpy(ps->resolution, o->res_name, sizeof(ps->resolution));
		if(fps_s)
			ps->fps = o->fps;
		if(bitrate_s)
			ps->bitrate = o->bitrate;
		if(codec_s)
			pv_strlcpy(ps->codec, o->codec == CHIAKI_CODEC_H264 ? "h264" : "h265", sizeof(ps->codec));
		if(wake >= 0)
			ps->wake = wake;
		if(hwdec_s)
			pv_strlcpy(ps->hwdec, hwdec_s, sizeof(ps->hwdec));
		if(display_s)
			ps->display = o->display;
		if(controller_s)
			pv_strlcpy(ps->controller, !o->announce_controller ? "none" : (o->dualsense ? "dualsense" : "ds4"), sizeof(ps->controller));
		// the audio device is stored after it has been resolved to its exact name (below)
	}

	// ---- SDL ----
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
	// Keep a fullscreen window visible when another window gets the focus
	// (otherwise the Discord stream would turn black).
	SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
#ifdef SDL_HINT_APP_NAME
	SDL_SetHint(SDL_HINT_APP_NAME, "ps5view");
#endif
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "ps5view");
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "PS5");
	if(o->renderer)
		SDL_SetHint(SDL_HINT_RENDER_DRIVER, o->renderer);
	SDL_SetMainReady();
	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
	{
		LOGE("stream", "Grafikausgabe kann nicht initialisiert werden: %s", SDL_GetError());
		pv_config_free(&cfg);
		return PV_EXIT_DEVICE;
	}
	if(!o->no_audio && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
	{
		LOGE("stream", "Audioausgabe kann nicht initialisiert werden: %s", SDL_GetError());
		LOGI("hint", "Was du tun kannst:\n- Ohne Ton starten: --no-audio");
		SDL_Quit();
		pv_config_free(&cfg);
		return PV_EXIT_DEVICE;
	}
	LOGD("stream", "SDL-Videotreiber: %s, Audiotreiber: %s", SDL_GetCurrentVideoDriver(),
			o->no_audio ? "-" : (SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?"));

	int rc = resolve_audio_device(&s);
	if(rc == PV_EXIT_OK && save && audio_s)
	{
		free(ps->audio_device);
		ps->audio_device = s.audio_device ? pv_strdup(s.audio_device) : NULL;
	}
	if(rc == PV_EXIT_OK && save)
	{
		if(pv_config_save(&cfg, err, sizeof(err)))
			LOGI("stream", "Stream-Einstellungen im Profil \"%s\" gespeichert.", p->name);
		else
			LOGW("stream", "Einstellungen konnten nicht gespeichert werden: %s", err);
	}
	if(rc == PV_EXIT_OK)
	{
		LOGI("stream", "Profil \"%s\": %s (%s), Ton: %s", p->name, p->nickname[0] ? p->nickname : "?", p->host,
				o->no_audio ? "aus" : (s.audio_device ? s.audio_device : "Standardgerät"));
		s.ev_session = SDL_RegisterEvents(1);
		if(s.ev_session == (Uint32)-1 || !pv_diag_init(&s.diag))
		{
			LOGE("stream", "Interner Fehler (Ereignisse).");
			rc = PV_EXIT_INTERNAL;
		}
		else
		{
			pv_log_set_chiaki_hook(pv_diag_log_hook, &s.diag);
			rc = stream_main(&s);
			pv_log_set_chiaki_hook(NULL, NULL);
			pv_diag_fini(&s.diag);
		}
	}

	if(s.audio)
		pv_audio_destroy(s.audio);
	if(s.video)
		pv_video_destroy(s.video);
	free(s.audio_device);
	SDL_Quit();
	pv_config_free(&cfg);
	if(rc == PV_EXIT_OK && pv_interrupted())
		rc = PV_EXIT_OK; // Ctrl+C is the normal way to end a stream
	return rc;
}
