// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// ps5view list-audio [--test-tone <device>] [--seconds <n>]

#include "app.h"
#include "args.h"
#include "audio.h"
#include "log.h"
#include "util.h"

#include <SDL.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void usage(void)
{
	pv_out("Aufruf: ps5view list-audio [--test-tone <gerät>] [--seconds <n>]");
	pv_out("%s", "");
	pv_out("Listet die Audio-Ausgabegeräte. Den angezeigten Namen (oder einen eindeutigen Teil davon)");
	pv_out("bei \"ps5view stream --audio-device\" angeben.");
	pv_out("%s", "");
	pv_out("  --test-tone <gerät>  spielt einen Testton auf dem Gerät ab, z. B. um zu prüfen,");
	pv_out("                       ob Discord den Ton von \"CABLE Input\" mitüberträgt");
	pv_out("  --seconds <n>        Dauer des Testtons (Standard 3, max. 30)");
}

static int play_test_tone(const char *device, unsigned seconds)
{
	SDL_AudioSpec want, have;
	memset(&want, 0, sizeof(want));
	want.freq = 48000;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	SDL_AudioDeviceID dev = SDL_OpenAudioDevice(device, 0, &want, &have, 0);
	if(!dev)
	{
		LOGE("audio", "Gerät \"%s\" kann nicht geöffnet werden: %s", device, SDL_GetError());
		return PV_EXIT_DEVICE;
	}
	size_t frames = (size_t)want.freq * seconds;
	int16_t *pcm = malloc(frames * 2 * sizeof(int16_t));
	if(!pcm)
	{
		SDL_CloseAudioDevice(dev);
		return PV_EXIT_INTERNAL;
	}
	for(size_t i = 0; i < frames; i++)
	{
		// 440 Hz at about -12 dBFS, short fade in/out to avoid clicks
		double t = (double)i / want.freq;
		double env = 1.0;
		double fade = 0.05 * want.freq;
		if(i < fade)
			env = i / fade;
		else if(frames - i < fade)
			env = (frames - i) / fade;
		int16_t s = (int16_t)(8000.0 * env * sin(2.0 * M_PI * 440.0 * t));
		pcm[i * 2] = s;
		pcm[i * 2 + 1] = s;
	}
	SDL_QueueAudio(dev, pcm, (Uint32)(frames * 2 * sizeof(int16_t)));
	free(pcm);
	SDL_PauseAudioDevice(dev, 0);
	LOGI("audio", "Spiele %u s Testton (440 Hz) auf \"%s\" ...", seconds, device);
	uint64_t deadline = pv_now_ms() + seconds * 1000ull + 2000;
	while(SDL_GetQueuedAudioSize(dev) > 0 && pv_now_ms() < deadline && !pv_interrupted())
		pv_sleep_ms(50);
	pv_sleep_ms(200);
	SDL_CloseAudioDevice(dev);
	LOGI("audio", "Testton beendet.");
	return pv_interrupted() ? PV_EXIT_INTERRUPTED : PV_EXIT_OK;
}

int pv_cmd_list_audio(int argc, char **argv)
{
	const char *test = NULL, *seconds_s = NULL;
	PvArgs a;
	pv_args_init(&a, "list-audio", argc, argv);
	while(pv_args_next(&a))
	{
		if(pv_args_value(&a, "--test-tone", &test) || pv_args_value(&a, "--seconds", &seconds_s))
			continue;
		if(pv_args_is_help(&a))
		{
			usage();
			return PV_EXIT_OK;
		}
		return pv_args_unknown(&a);
	}
	if(a.failed)
		return PV_EXIT_USAGE;
	unsigned long seconds = 3;
	if(seconds_s && !pv_parse_ulong(seconds_s, 1, 30, &seconds))
	{
		LOGE("audio", "--seconds erwartet 1 bis 30.");
		return PV_EXIT_USAGE;
	}

	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "ps5view");
	SDL_SetMainReady();
	if(SDL_Init(SDL_INIT_AUDIO) != 0)
	{
		LOGE("audio", "Audiosystem kann nicht initialisiert werden: %s", SDL_GetError());
		return PV_EXIT_DEVICE;
	}
	char **names = NULL;
	size_t n = pv_audio_list_devices(&names);
	int rc = PV_EXIT_OK;

	char *default_name = NULL;
#if SDL_VERSION_ATLEAST(2, 24, 0)
	SDL_AudioSpec spec;
	if(SDL_GetDefaultAudioInfo(&default_name, &spec, 0) != 0)
		default_name = NULL;
#endif

	if(!test)
	{
		pv_out("Audio-Ausgabegeräte (SDL-Audiotreiber: %s):", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
		if(!n)
			pv_out("  (keine gefunden)");
		for(size_t i = 0; i < n; i++)
			pv_out("  \"%s\"%s", names[i], default_name && strcmp(default_name, names[i]) == 0 ? "   [Standardgerät]" : "");
		pv_out("%s", "");
		pv_out("Beispiel: ps5view stream --audio-device \"CABLE Input\"");
		pv_out("Mit --save wird das Gerät im Profil gespeichert. Testton: ps5view list-audio --test-tone \"CABLE Input\"");
	}
	else
	{
		size_t idx = 0;
		switch(pv_audio_match_device(test, names, n, &idx))
		{
			case PV_AUDIO_MATCH_EXACT:
			case PV_AUDIO_MATCH_UNIQUE:
				rc = play_test_tone(names[idx], (unsigned)seconds);
				break;
			case PV_AUDIO_MATCH_AMBIGUOUS:
				LOGE("audio", "\"%s\" passt auf mehrere Geräte - bitte genauer angeben:", test);
				for(size_t i = 0; i < n; i++)
					if(pv_str_icontains(names[i], test))
						LOGI("audio", "  \"%s\"", names[i]);
				rc = PV_EXIT_DEVICE;
				break;
			default:
				LOGE("audio", "Kein Audiogerät passt zu \"%s\" (Liste: ps5view list-audio).", test);
				rc = PV_EXIT_DEVICE;
				break;
		}
	}
	SDL_free(default_name);
	pv_audio_free_list(names, n);
	SDL_Quit();
	return rc;
}
