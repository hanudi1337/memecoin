// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Video: FFmpeg decoding (hardware if available, software fallback) and
// presentation in a resizable SDL2 window without any overlay.
//
// Threading: pv_video_sample_cb() runs on a libchiaki thread and only feeds
// the decoder; it wakes the main thread with an SDL user event. Everything
// else (pulling frames, uploading textures, rendering) runs on the main thread.

#ifndef PV_VIDEO_H
#define PV_VIDEO_H

#include <chiaki/common.h>

#include <SDL.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct pv_video_options_t
{
	int width, height;    // initial window size
	int display;          // -1 = let the OS decide
	bool fullscreen;
	const char *hwdec;    // "auto", "none" or an FFmpeg hwdevice name
	const char *title;    // window title prefix
} PvVideoOptions;

typedef struct pv_video_stats_t
{
	uint64_t samples;        // compressed frames received
	uint64_t frames_shown;   // frames presented
	uint64_t frames_lost;    // reported lost by libchiaki
	uint64_t decode_errors;  // decoder rejected a sample
	uint64_t transfer_errors;
	uint64_t last_frame_ms;  // pv_now_ms() of the last presented frame (0 = none)
	int width, height;       // current video size
	bool hw_frames;          // last frame came from the hardware decoder
	char decoder[24];        // "d3d11va", "software", ...
} PvVideoStats;

typedef struct pv_video_t PvVideo;

PvVideo *pv_video_create(const PvVideoOptions *opt, char *err, size_t err_size);
void pv_video_destroy(PvVideo *v);

bool pv_video_start_decoder(PvVideo *v, ChiakiCodec codec, unsigned max_fps, char *err, size_t err_size);
void pv_video_stop_decoder(PvVideo *v);

// ChiakiVideoSampleCallback, user = PvVideo *
bool pv_video_sample_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user);

// Main thread: returns true if the event was handled by the video module.
bool pv_video_handle_event(PvVideo *v, const SDL_Event *e);
void pv_video_render(PvVideo *v);
void pv_video_set_status(PvVideo *v, const char *status);
void pv_video_get_stats(PvVideo *v, PvVideoStats *out);
void pv_video_clear(PvVideo *v); // forget the last picture (shown black)
uint32_t pv_video_window_id(PvVideo *v);
// Sends FFmpeg's own messages into the ps5view log (rate limited) instead of stderr.
void pv_video_route_ffmpeg_log(void);
// Called (main thread) when the decoder was replaced and needs a key frame.
void pv_video_set_idr_callback(PvVideo *v, void (*cb)(void *user), void *user);

#endif
