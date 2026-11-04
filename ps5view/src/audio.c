// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "audio.h"

#include "log.h"
#include "util.h"

#include <chiaki/opusdecoder.h>
#include <chiaki/thread.h>

#include <stdlib.h>
#include <string.h>

#define REOPEN_INTERVAL_MS 5000

struct pv_audio_t
{
	char *device_name; // NULL = default device
	unsigned buffer_ms;
	ChiakiOpusDecoder opus;
	ChiakiMutex mutex;  // guards everything below

	SDL_AudioDeviceID dev;
	unsigned channels, rate;           // announced by the console
	unsigned open_channels, open_rate; // what the device was opened with
	bool want_open;
	bool open_failed;
	bool removed;
	uint64_t last_open_attempt_ms;
	bool prebuffering;
	Uint32 ev_open;

	uint64_t frames;
	uint64_t dropped_bytes;
	uint64_t clears;
	uint64_t underruns;
	uint64_t frames_without_device;
	bool logged_no_device;
};

static size_t bytes_for_ms(unsigned rate, unsigned channels, unsigned ms)
{
	return (size_t)rate * channels * sizeof(int16_t) * ms / 1000;
}

// --- libchiaki audio thread ---

static void settings_cb(uint32_t channels, uint32_t rate, void *user)
{
	PvAudio *a = user;
	chiaki_mutex_lock(&a->mutex);
	bool changed = channels != a->channels || rate != a->rate || !a->dev;
	a->channels = channels;
	a->rate = rate;
	if(changed)
		a->want_open = true;
	chiaki_mutex_unlock(&a->mutex);
	LOGD("audio", "Audioformat der Konsole: %u Kanäle, %u Hz", (unsigned)channels, (unsigned)rate);
	if(changed)
	{
		SDL_Event ev;
		memset(&ev, 0, sizeof(ev));
		ev.type = a->ev_open;
		SDL_PushEvent(&ev);
	}
}

static void frame_cb(int16_t *buf, size_t samples_count, void *user)
{
	PvAudio *a = user;
	chiaki_mutex_lock(&a->mutex);
	a->frames++;
	if(!a->dev || a->open_channels != a->channels || a->open_rate != a->rate)
	{
		a->frames_without_device++;
		chiaki_mutex_unlock(&a->mutex);
		return;
	}
	Uint32 bytes = (Uint32)(samples_count * a->channels * sizeof(int16_t));
	Uint32 queued = SDL_GetQueuedAudioSize(a->dev);
	size_t max_queue = bytes_for_ms(a->rate, a->channels, a->buffer_ms * 3);
	if(queued == 0 && !a->prebuffering && a->frames > 50)
		a->underruns++;
	if(queued > max_queue)
	{
		// The queue runs away (device slower than the stream): drop the backlog
		// instead of letting the delay grow.
		SDL_ClearQueuedAudio(a->dev);
		a->clears++;
		a->dropped_bytes += queued;
	}
	// Samples are passed through unmodified: no volume change anywhere.
	if(SDL_QueueAudio(a->dev, buf, bytes) != 0)
		LOGD("audio", "SDL_QueueAudio: %s", SDL_GetError());
	if(a->prebuffering && SDL_GetQueuedAudioSize(a->dev) >= bytes_for_ms(a->rate, a->channels, a->buffer_ms))
	{
		SDL_PauseAudioDevice(a->dev, 0);
		a->prebuffering = false;
	}
	chiaki_mutex_unlock(&a->mutex);
}

// --- main thread ---

// Opening a device can take a while (WASAPI): never do it under the mutex,
// otherwise libchiaki's receive thread would stall in frame_cb().
static void reopen_device(PvAudio *a)
{
	chiaki_mutex_lock(&a->mutex);
	unsigned channels = a->channels, rate = a->rate;
	bool was_failed = a->open_failed;
	a->want_open = false;
	a->last_open_attempt_ms = pv_now_ms();
	SDL_AudioDeviceID old = a->dev;
	a->dev = 0;
	chiaki_mutex_unlock(&a->mutex);
	if(old)
		SDL_CloseAudioDevice(old);
	if(!channels || !rate)
		return;

	SDL_AudioSpec want, have;
	memset(&want, 0, sizeof(want));
	memset(&have, 0, sizeof(have));
	want.freq = (int)rate;
	want.format = AUDIO_S16SYS;
	want.channels = (Uint8)channels;
	want.samples = 512;
	want.callback = NULL; // queue API
	// allowed_changes = 0: SDL converts to whatever the device needs.
	SDL_AudioDeviceID dev = SDL_OpenAudioDevice(a->device_name, 0, &want, &have, 0);
	if(!dev)
	{
		if(!was_failed)
		{
			LOGE("audio", "Audiogerät \"%s\" kann nicht geöffnet werden: %s", a->device_name ? a->device_name : "(Standard)", SDL_GetError());
			LOGW("audio", "Es läuft KEIN Ton. ps5view weicht absichtlich nicht auf ein anderes Gerät aus; neuer Versuch alle %d s.",
					REOPEN_INTERVAL_MS / 1000);
		}
		chiaki_mutex_lock(&a->mutex);
		a->open_failed = true;
		chiaki_mutex_unlock(&a->mutex);
		return;
	}
	if(was_failed)
		LOGI("audio", "Audiogerät ist wieder verfügbar.");
	LOGI("audio", "Ton läuft über \"%s\" (%u Kanäle, %d Hz, Lautstärke unverändert 100 %%).",
			a->device_name ? a->device_name : "Standardgerät", (unsigned)have.channels, have.freq);
	chiaki_mutex_lock(&a->mutex);
	a->dev = dev;
	a->open_channels = channels;
	a->open_rate = rate;
	a->prebuffering = true;
	a->removed = false;
	a->open_failed = false;
	chiaki_mutex_unlock(&a->mutex);
}

PvAudio *pv_audio_create(const char *device, unsigned buffer_ms, char *err, size_t err_size)
{
	PvAudio *a = calloc(1, sizeof(*a));
	if(!a)
	{
		snprintf(err, err_size, "Speicherfehler");
		return NULL;
	}
	if(chiaki_mutex_init(&a->mutex, false) != CHIAKI_ERR_SUCCESS)
	{
		free(a);
		snprintf(err, err_size, "Interner Fehler (Mutex)");
		return NULL;
	}
	a->device_name = device ? pv_strdup(device) : NULL;
	a->buffer_ms = buffer_ms ? buffer_ms : 50;
	a->ev_open = SDL_RegisterEvents(1);
	if(a->ev_open == (Uint32)-1)
	{
		snprintf(err, err_size, "SDL_RegisterEvents fehlgeschlagen");
		chiaki_mutex_fini(&a->mutex);
		free(a->device_name);
		free(a);
		return NULL;
	}
	chiaki_opus_decoder_init(&a->opus, pv_chiaki_log());
	chiaki_opus_decoder_set_cb(&a->opus, settings_cb, frame_cb, a);
	return a;
}

void pv_audio_destroy(PvAudio *a)
{
	if(!a)
		return;
	chiaki_mutex_lock(&a->mutex);
	SDL_AudioDeviceID dev = a->dev;
	a->dev = 0;
	chiaki_mutex_unlock(&a->mutex);
	if(dev)
		SDL_CloseAudioDevice(dev);
	chiaki_opus_decoder_fini(&a->opus);
	chiaki_mutex_fini(&a->mutex);
	free(a->device_name);
	free(a);
}

void pv_audio_get_sink(PvAudio *a, ChiakiAudioSink *sink)
{
	chiaki_opus_decoder_get_sink(&a->opus, sink);
}

bool pv_audio_handle_event(PvAudio *a, const SDL_Event *e)
{
	if(e->type == a->ev_open)
	{
		chiaki_mutex_lock(&a->mutex);
		bool want = a->want_open;
		chiaki_mutex_unlock(&a->mutex);
		if(want)
			reopen_device(a);
		return true;
	}
	if(e->type == SDL_AUDIODEVICEREMOVED && !e->adevice.iscapture)
	{
		chiaki_mutex_lock(&a->mutex);
		SDL_AudioDeviceID dev = a->dev;
		bool ours = dev && e->adevice.which == dev;
		if(ours)
		{
			a->dev = 0;
			a->removed = true;
			a->open_failed = true;
			a->last_open_attempt_ms = pv_now_ms();
		}
		chiaki_mutex_unlock(&a->mutex);
		if(ours)
		{
			LOGW("audio", "Das Audiogerät \"%s\" wurde entfernt oder deaktiviert - kein Ton. Neuer Versuch alle %d s.",
					a->device_name ? a->device_name : "Standardgerät", REOPEN_INTERVAL_MS / 1000);
			SDL_CloseAudioDevice(dev);
		}
		return ours;
	}
	if(e->type == SDL_AUDIODEVICEADDED && !e->adevice.iscapture)
	{
		chiaki_mutex_lock(&a->mutex);
		bool retry = !a->dev && a->open_failed && a->channels;
		chiaki_mutex_unlock(&a->mutex);
		if(retry)
			reopen_device(a); // maybe our device came back
		return true;
	}
	return false;
}

void pv_audio_tick(PvAudio *a)
{
	chiaki_mutex_lock(&a->mutex);
	bool retry = !a->dev && a->open_failed && a->channels && pv_now_ms() - a->last_open_attempt_ms >= REOPEN_INTERVAL_MS;
	bool warn = !a->dev && a->frames_without_device > 500 && !a->logged_no_device && !a->open_failed;
	if(warn)
		a->logged_no_device = true;
	chiaki_mutex_unlock(&a->mutex);
	if(retry)
		reopen_device(a);
	if(warn)
		LOGW("audio", "Es kommen Audiodaten an, aber kein Ausgabegerät ist offen.");
}

void pv_audio_get_stats(PvAudio *a, PvAudioStats *out)
{
	memset(out, 0, sizeof(*out));
	chiaki_mutex_lock(&a->mutex);
	out->frames = a->frames;
	out->dropped_bytes = a->dropped_bytes;
	out->clears = a->clears;
	out->underruns = a->underruns;
	out->channels = a->open_channels;
	out->rate = a->open_rate;
	out->device_open = a->dev != 0;
	if(a->dev && a->rate && a->channels)
		out->queued_ms = (unsigned)(SDL_GetQueuedAudioSize(a->dev) * 1000ull / bytes_for_ms(a->rate, a->channels, 1000));
	pv_strlcpy(out->device, a->device_name ? a->device_name : "", sizeof(out->device));
	chiaki_mutex_unlock(&a->mutex);
}

// ---------------------------------------------------------------------------

size_t pv_audio_list_devices(char ***names_out)
{
	*names_out = NULL;
	int n = SDL_GetNumAudioDevices(0);
	if(n <= 0)
		return 0;
	char **names = calloc((size_t)n, sizeof(char *));
	if(!names)
		return 0;
	size_t count = 0;
	for(int i = 0; i < n; i++)
	{
		const char *name = SDL_GetAudioDeviceName(i, 0);
		if(name && *name)
			names[count++] = pv_strdup(name);
	}
	*names_out = names;
	return count;
}

void pv_audio_free_list(char **names, size_t n)
{
	if(!names)
		return;
	for(size_t i = 0; i < n; i++)
		free(names[i]);
	free(names);
}

PvAudioMatch pv_audio_match_device(const char *query, char *const *names, size_t n, size_t *index)
{
	if(!query || !*query || !names)
		return PV_AUDIO_MATCH_NONE;
	for(size_t i = 0; i < n; i++)
	{
		if(strcmp(names[i], query) == 0)
		{
			*index = i;
			return PV_AUDIO_MATCH_EXACT;
		}
	}
	for(size_t i = 0; i < n; i++)
	{
		if(pv_strcasecmp(names[i], query) == 0)
		{
			*index = i;
			return PV_AUDIO_MATCH_EXACT;
		}
	}
	size_t hits = 0;
	for(size_t i = 0; i < n; i++)
	{
		if(pv_str_icontains(names[i], query))
		{
			if(!hits)
				*index = i;
			hits++;
		}
	}
	if(hits == 1)
		return PV_AUDIO_MATCH_UNIQUE;
	return hits ? PV_AUDIO_MATCH_AMBIGUOUS : PV_AUDIO_MATCH_NONE;
}
