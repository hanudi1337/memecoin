// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Audio: libchiaki's Opus decoder -> SDL2 audio output on a selectable device
// (e.g. "CABLE Input (VB-Audio Virtual Cable)"), so Discord can capture the
// sound without it playing on your own speakers. The volume is never changed:
// samples are passed through unmodified (100 %).

#ifndef PV_AUDIO_H
#define PV_AUDIO_H

#include <chiaki/audio.h>
#include <chiaki/audioreceiver.h>

#include <SDL.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct pv_audio_stats_t
{
	uint64_t frames;        // decoded audio frames received
	uint64_t dropped_bytes; // discarded to keep latency low
	uint64_t clears;        // times the queue was flushed because it ran away
	uint64_t underruns;     // queue was empty when new audio arrived
	unsigned queued_ms;
	unsigned channels;
	unsigned rate;
	bool device_open;
	char device[256];
} PvAudioStats;

typedef struct pv_audio_t PvAudio;

// device: exact SDL device name, or NULL for the system default device.
PvAudio *pv_audio_create(const char *device, unsigned buffer_ms, char *err, size_t err_size);
void pv_audio_destroy(PvAudio *a);
void pv_audio_get_sink(PvAudio *a, ChiakiAudioSink *sink);
// Main thread: handles (re)opening the device and device removal.
bool pv_audio_handle_event(PvAudio *a, const SDL_Event *e);
void pv_audio_tick(PvAudio *a); // main thread, about once per second
void pv_audio_get_stats(PvAudio *a, PvAudioStats *out);

// Device enumeration (SDL audio must be initialized).
size_t pv_audio_list_devices(char ***names_out);
void pv_audio_free_list(char **names, size_t n);

typedef enum
{
	PV_AUDIO_MATCH_EXACT,
	PV_AUDIO_MATCH_UNIQUE,    // case-insensitive substring, exactly one device
	PV_AUDIO_MATCH_NONE,
	PV_AUDIO_MATCH_AMBIGUOUS
} PvAudioMatch;

PvAudioMatch pv_audio_match_device(const char *query, char *const *names, size_t n, size_t *index);

#endif
