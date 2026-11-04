// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Step 5 placeholder: audio output follows in the next step.

#include "audio.h"

#include "util.h"

#include <stdlib.h>
#include <string.h>

PvAudio *pv_audio_create(const char *device, unsigned buffer_ms, char *err, size_t err_size)
{
	(void)device;
	(void)buffer_ms;
	snprintf(err, err_size, "Audioausgabe ist noch nicht implementiert");
	return NULL;
}

void pv_audio_destroy(PvAudio *a) { (void)a; }
void pv_audio_get_sink(PvAudio *a, ChiakiAudioSink *sink) { (void)a; memset(sink, 0, sizeof(*sink)); }
bool pv_audio_handle_event(PvAudio *a, const SDL_Event *e) { (void)a; (void)e; return false; }
void pv_audio_tick(PvAudio *a) { (void)a; }
void pv_audio_get_stats(PvAudio *a, PvAudioStats *out) { (void)a; memset(out, 0, sizeof(*out)); }
size_t pv_audio_list_devices(char ***names_out) { *names_out = NULL; return 0; }
void pv_audio_free_list(char **names, size_t n) { (void)names; (void)n; }
PvAudioMatch pv_audio_match_device(const char *query, char *const *names, size_t n, size_t *index)
{
	(void)query; (void)names; (void)n; (void)index;
	return PV_AUDIO_MATCH_NONE;
}
