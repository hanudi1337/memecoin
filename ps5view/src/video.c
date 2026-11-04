// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "video.h"

#include "log.h"
#include "util.h"

#include <chiaki/ffmpegdecoder.h>
#include <chiaki/thread.h>

#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// Hardware decoders tried for "auto", in this order; software is the fallback.
#ifdef _WIN32
static const char *const auto_hwdec[] = { "d3d11va", "dxva2" };
#elif defined(__APPLE__)
static const char *const auto_hwdec[] = { "videotoolbox" };
#else
static const char *const auto_hwdec[] = { "vaapi", "vdpau" };
#endif

// If a hardware decoder produced no picture after this many samples, or fails
// to hand over frames repeatedly, switch to software decoding.
#define HW_NO_FRAME_SAMPLES 180
#define HW_MAX_CONSECUTIVE_TRANSFER_ERRORS 15

struct pv_video_t
{
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *texture;
	Uint32 tex_format;
	int tex_w, tex_h;
	SDL_YUV_CONVERSION_MODE tex_mode;
	Uint32 frame_event;
	atomic_bool frame_event_pending;

	ChiakiMutex dec_mutex; // guards decoder swap vs. feeding/pulling
	ChiakiFfmpegDecoder decoder;
	bool decoder_active;
	bool decoder_is_hw;
	ChiakiCodec codec;
	unsigned max_fps;
	char decoder_name[24];
	char hwdec[24];
	uint64_t samples_at_start;
	uint64_t shown_at_start;
	unsigned consecutive_transfer_errors;

	AVFrame *sw_frame;
	AVFrame *conv_frame;
	struct SwsContext *sws;

	atomic_ullong samples;
	atomic_ullong frames_lost;
	atomic_ullong decode_errors;
	uint64_t frames_shown;
	uint64_t transfer_errors;
	uint64_t last_frame_ms;
	int video_w, video_h;
	bool hw_frames;
	bool logged_sw_frames_from_hw;
	enum AVPixelFormat logged_format;

	char title[128];
	char status[192];

	void (*request_idr)(void *user);
	void *request_idr_user;
};

// ---------------------------------------------------------------------------
// FFmpeg log -> ps5view log
// ---------------------------------------------------------------------------

#define FFMPEG_LOG_WINDOW_MS 10000
#define FFMPEG_LOG_MAX_PER_WINDOW 40

static atomic_ullong ffmpeg_log_window_start;
static atomic_uint ffmpeg_log_count;
static atomic_uint ffmpeg_log_suppressed;

static void ffmpeg_log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
	if(level > AV_LOG_WARNING)
		return;
	uint64_t now = pv_now_ms();
	uint64_t start = atomic_load(&ffmpeg_log_window_start);
	if(now - start > FFMPEG_LOG_WINDOW_MS && atomic_compare_exchange_strong(&ffmpeg_log_window_start, &start, now))
	{
		unsigned suppressed = atomic_exchange(&ffmpeg_log_suppressed, 0);
		atomic_store(&ffmpeg_log_count, 0);
		if(suppressed)
			LOGD("ffmpeg", "(%u weitere FFmpeg-Meldungen unterdrückt)", suppressed);
	}
	if(atomic_fetch_add(&ffmpeg_log_count, 1) >= FFMPEG_LOG_MAX_PER_WINDOW)
	{
		atomic_fetch_add(&ffmpeg_log_suppressed, 1);
		return;
	}
	char line[1024];
	int print_prefix = 1;
	va_list copy;
	va_copy(copy, vl);
	av_log_format_line2(avcl, level, fmt, copy, line, sizeof(line), &print_prefix);
	va_end(copy);
	size_t n = strlen(line);
	while(n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		line[--n] = '\0';
	if(n)
		pv_log(level <= AV_LOG_FATAL ? PV_LOG_ERROR : PV_LOG_DEBUG, "ffmpeg", "%s", line);
}

void pv_video_route_ffmpeg_log(void)
{
	av_log_set_callback(ffmpeg_log_cb);
}

static void frame_available_cb(ChiakiFfmpegDecoder *decoder, void *user)
{
	(void)decoder;
	PvVideo *v = user;
	if(atomic_exchange(&v->frame_event_pending, true))
		return; // main thread has not processed the previous wake-up yet
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = v->frame_event;
	if(SDL_PushEvent(&ev) != 1)
		atomic_store(&v->frame_event_pending, false);
}

PvVideo *pv_video_create(const PvVideoOptions *opt, char *err, size_t err_size)
{
	PvVideo *v = calloc(1, sizeof(*v));
	if(!v)
	{
		snprintf(err, err_size, "Speicherfehler");
		return NULL;
	}
	if(chiaki_mutex_init(&v->dec_mutex, false) != CHIAKI_ERR_SUCCESS)
	{
		free(v);
		snprintf(err, err_size, "Interner Fehler (Mutex)");
		return NULL;
	}
	atomic_init(&v->frame_event_pending, false);
	atomic_init(&v->samples, 0);
	atomic_init(&v->frames_lost, 0);
	atomic_init(&v->decode_errors, 0);
	v->logged_format = AV_PIX_FMT_NONE;
	pv_strlcpy(v->title, opt->title ? opt->title : "ps5view", sizeof(v->title));
	pv_strlcpy(v->hwdec, opt->hwdec ? opt->hwdec : "auto", sizeof(v->hwdec));

	v->frame_event = SDL_RegisterEvents(1);
	if(v->frame_event == (Uint32)-1)
	{
		snprintf(err, err_size, "SDL_RegisterEvents fehlgeschlagen");
		pv_video_destroy(v);
		return NULL;
	}

	int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED;
	if(opt->display >= 0)
	{
		int n = SDL_GetNumVideoDisplays();
		if(opt->display < n)
			x = y = SDL_WINDOWPOS_CENTERED_DISPLAY(opt->display);
		else
			LOGW("video", "Bildschirm %d gibt es nicht (vorhanden: 0 bis %d) - Fenster öffnet auf dem Standardbildschirm.", opt->display, n - 1);
	}
	Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
	if(opt->fullscreen)
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	v->window = SDL_CreateWindow(v->title, x, y, opt->width > 0 ? opt->width : 1280, opt->height > 0 ? opt->height : 720, flags);
	if(!v->window)
	{
		snprintf(err, err_size, "Fenster kann nicht erstellt werden: %s", SDL_GetError());
		pv_video_destroy(v);
		return NULL;
	}
	SDL_SetWindowMinimumSize(v->window, 320, 180);

	v->renderer = SDL_CreateRenderer(v->window, -1, SDL_RENDERER_ACCELERATED);
	if(!v->renderer)
	{
		LOGW("video", "Kein beschleunigter Renderer verfügbar (%s) - nutze Software-Rendering.", SDL_GetError());
		v->renderer = SDL_CreateRenderer(v->window, -1, SDL_RENDERER_SOFTWARE);
	}
	if(!v->renderer)
	{
		snprintf(err, err_size, "Renderer kann nicht erstellt werden: %s", SDL_GetError());
		pv_video_destroy(v);
		return NULL;
	}
	SDL_RendererInfo info;
	if(SDL_GetRendererInfo(v->renderer, &info) == 0)
		LOGI("video", "Fenster bereit (Renderer: %s).", info.name);

	v->sw_frame = av_frame_alloc();
	v->conv_frame = av_frame_alloc();
	if(!v->sw_frame || !v->conv_frame)
	{
		snprintf(err, err_size, "Speicherfehler");
		pv_video_destroy(v);
		return NULL;
	}
	pv_video_render(v);
	return v;
}

void pv_video_destroy(PvVideo *v)
{
	if(!v)
		return;
	pv_video_stop_decoder(v);
	if(v->texture)
		SDL_DestroyTexture(v->texture);
	if(v->renderer)
		SDL_DestroyRenderer(v->renderer);
	if(v->window)
		SDL_DestroyWindow(v->window);
	av_frame_free(&v->sw_frame);
	av_frame_free(&v->conv_frame);
	sws_freeContext(v->sws);
	chiaki_mutex_fini(&v->dec_mutex);
	free(v);
}

uint32_t pv_video_window_id(PvVideo *v)
{
	return v && v->window ? SDL_GetWindowID(v->window) : 0;
}

// ---------------------------------------------------------------------------
// decoder
// ---------------------------------------------------------------------------

static bool init_decoder_locked(PvVideo *v, const char *hw)
{
	ChiakiErrorCode e = chiaki_ffmpeg_decoder_init(&v->decoder, pv_chiaki_log(), v->codec, v->max_fps, hw, NULL,
			frame_available_cb, v);
	if(e != CHIAKI_ERR_SUCCESS)
		return false;
	v->decoder_active = true;
	v->decoder_is_hw = hw != NULL;
	pv_strlcpy(v->decoder_name, hw ? hw : "software", sizeof(v->decoder_name));
	v->samples_at_start = atomic_load(&v->samples);
	v->shown_at_start = v->frames_shown;
	v->consecutive_transfer_errors = 0;
	return true;
}

bool pv_video_start_decoder(PvVideo *v, ChiakiCodec codec, unsigned max_fps, char *err, size_t err_size)
{
	pv_video_stop_decoder(v);
	chiaki_mutex_lock(&v->dec_mutex);
	v->codec = codec;
	v->max_fps = max_fps;
	bool ok = false;
	if(pv_strcasecmp(v->hwdec, "none") != 0 && pv_strcasecmp(v->hwdec, "software") != 0)
	{
		if(pv_strcasecmp(v->hwdec, "auto") == 0)
		{
			for(size_t i = 0; i < PV_ARRAY_SIZE(auto_hwdec) && !ok; i++)
			{
				ok = init_decoder_locked(v, auto_hwdec[i]);
				if(!ok)
					LOGD("video", "Hardware-Decoder %s nicht verfügbar.", auto_hwdec[i]);
			}
		}
		else
		{
			ok = init_decoder_locked(v, v->hwdec);
			if(!ok)
				LOGW("video", "Hardware-Decoder \"%s\" nicht verfügbar - nutze Software-Decoding.", v->hwdec);
		}
	}
	if(!ok)
		ok = init_decoder_locked(v, NULL);
	chiaki_mutex_unlock(&v->dec_mutex);
	if(!ok)
	{
		snprintf(err, err_size, "Kein Videodecoder für %s verfügbar (FFmpeg ohne passenden Decoder?).", chiaki_codec_name(codec));
		return false;
	}
	LOGI("video", "Videodecoder: %s (%s%s)", chiaki_codec_name(codec), v->decoder_name, v->decoder_is_hw ? ", Hardware" : "");
	return true;
}

void pv_video_stop_decoder(PvVideo *v)
{
	if(!v)
		return;
	chiaki_mutex_lock(&v->dec_mutex);
	if(v->decoder_active)
	{
		chiaki_ffmpeg_decoder_fini(&v->decoder);
		v->decoder_active = false;
	}
	chiaki_mutex_unlock(&v->dec_mutex);
}

static void switch_to_software(PvVideo *v, const char *why)
{
	LOGW("video", "Hardware-Decoder %s funktioniert nicht (%s) - wechsle auf Software-Decoding.", v->decoder_name, why);
	chiaki_mutex_lock(&v->dec_mutex);
	if(v->decoder_active)
	{
		chiaki_ffmpeg_decoder_fini(&v->decoder);
		v->decoder_active = false;
	}
	pv_strlcpy(v->hwdec, "none", sizeof(v->hwdec)); // also for later reconnects
	if(!init_decoder_locked(v, NULL))
		LOGE("video", "Software-Decoder kann nicht gestartet werden.");
	chiaki_mutex_unlock(&v->dec_mutex);
	// The new decoder can only start with a key frame.
	if(v->request_idr)
		v->request_idr(v->request_idr_user);
}

void pv_video_set_idr_callback(PvVideo *v, void (*cb)(void *user), void *user)
{
	v->request_idr = cb;
	v->request_idr_user = user;
}

bool pv_video_sample_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	PvVideo *v = user;
	atomic_fetch_add(&v->samples, 1);
	if(frames_lost > 0)
		atomic_fetch_add(&v->frames_lost, (unsigned long long)frames_lost);
	chiaki_mutex_lock(&v->dec_mutex);
	bool ok = false;
	if(v->decoder_active)
		ok = chiaki_ffmpeg_decoder_video_sample_cb(buf, buf_size, frames_lost, frame_recovered, &v->decoder);
	chiaki_mutex_unlock(&v->dec_mutex);
	if(!ok)
		atomic_fetch_add(&v->decode_errors, 1);
	return ok; // false makes libchiaki request a new key frame
}

// ---------------------------------------------------------------------------
// presentation
// ---------------------------------------------------------------------------

static SDL_YUV_CONVERSION_MODE conversion_mode(const AVFrame *f)
{
	if(f->color_range == AVCOL_RANGE_JPEG)
		return SDL_YUV_CONVERSION_JPEG;
	switch(f->colorspace)
	{
		case AVCOL_SPC_BT709:
			return SDL_YUV_CONVERSION_BT709;
		case AVCOL_SPC_BT470BG:
		case AVCOL_SPC_SMPTE170M:
			return SDL_YUV_CONVERSION_BT601;
		default:
			return f->height > 576 ? SDL_YUV_CONVERSION_BT709 : SDL_YUV_CONVERSION_BT601;
	}
}

static bool ensure_texture(PvVideo *v, Uint32 fmt, int w, int h, SDL_YUV_CONVERSION_MODE mode)
{
	if(v->texture && v->tex_format == fmt && v->tex_w == w && v->tex_h == h && v->tex_mode == mode)
		return true;
	if(v->texture)
		SDL_DestroyTexture(v->texture);
	SDL_SetYUVConversionMode(mode);
	v->texture = SDL_CreateTexture(v->renderer, fmt, SDL_TEXTUREACCESS_STREAMING, w, h);
	if(!v->texture)
	{
		LOGE("video", "Textur %dx%d kann nicht erstellt werden: %s", w, h, SDL_GetError());
		return false;
	}
	v->tex_format = fmt;
	v->tex_w = w;
	v->tex_h = h;
	v->tex_mode = mode;
	LOGI("video", "Videobild: %dx%d (%s)", w, h, SDL_GetPixelFormatName(fmt));
	return true;
}

static AVFrame *convert_to_yuv420p(PvVideo *v, const AVFrame *src)
{
	if(v->conv_frame->width != src->width || v->conv_frame->height != src->height || v->conv_frame->format != AV_PIX_FMT_YUV420P)
	{
		av_frame_unref(v->conv_frame);
		v->conv_frame->format = AV_PIX_FMT_YUV420P;
		v->conv_frame->width = src->width;
		v->conv_frame->height = src->height;
		if(av_frame_get_buffer(v->conv_frame, 32) < 0)
			return NULL;
	}
	v->sws = sws_getCachedContext(v->sws, src->width, src->height, (enum AVPixelFormat)src->format,
			src->width, src->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
	if(!v->sws)
		return NULL;
	sws_scale(v->sws, (const uint8_t *const *)src->data, src->linesize, 0, src->height, v->conv_frame->data, v->conv_frame->linesize);
	v->conv_frame->color_range = src->color_range;
	v->conv_frame->colorspace = src->colorspace;
	return v->conv_frame;
}

static bool upload_frame(PvVideo *v, AVFrame *f)
{
	Uint32 fmt;
	AVFrame *src = f;
	switch(f->format)
	{
		case AV_PIX_FMT_NV12:
			fmt = SDL_PIXELFORMAT_NV12;
			break;
		case AV_PIX_FMT_YUV420P:
		case AV_PIX_FMT_YUVJ420P:
			fmt = SDL_PIXELFORMAT_IYUV;
			break;
		default:
			if(v->logged_format != (enum AVPixelFormat)f->format)
			{
				const char *name = av_get_pix_fmt_name((enum AVPixelFormat)f->format);
				LOGI("video", "Pixelformat %s wird nach YUV420P umgerechnet.", name ? name : "?");
				v->logged_format = (enum AVPixelFormat)f->format;
			}
			src = convert_to_yuv420p(v, f);
			if(!src)
			{
				LOGE("video", "Umrechnung des Pixelformats fehlgeschlagen.");
				return false;
			}
			fmt = SDL_PIXELFORMAT_IYUV;
			break;
	}
	if(!ensure_texture(v, fmt, src->width, src->height, conversion_mode(src)))
		return false;
	int r;
	if(fmt == SDL_PIXELFORMAT_NV12)
		r = SDL_UpdateNVTexture(v->texture, NULL, src->data[0], src->linesize[0], src->data[1], src->linesize[1]);
	else
		r = SDL_UpdateYUVTexture(v->texture, NULL, src->data[0], src->linesize[0], src->data[1], src->linesize[1],
				src->data[2], src->linesize[2]);
	if(r != 0)
	{
		LOGE("video", "Textur-Update fehlgeschlagen: %s", SDL_GetError());
		return false;
	}
	v->video_w = src->width;
	v->video_h = src->height;
	return true;
}

void pv_video_render(PvVideo *v)
{
	SDL_SetRenderDrawColor(v->renderer, 0, 0, 0, 255);
	SDL_RenderClear(v->renderer);
	if(v->texture && v->tex_w > 0 && v->tex_h > 0)
	{
		int ow = 0, oh = 0;
		SDL_GetRendererOutputSize(v->renderer, &ow, &oh);
		if(ow > 0 && oh > 0)
		{
			double s = (double)ow / v->tex_w;
			if((double)oh / v->tex_h < s)
				s = (double)oh / v->tex_h;
			SDL_Rect dst;
			dst.w = (int)(v->tex_w * s + 0.5);
			dst.h = (int)(v->tex_h * s + 0.5);
			dst.x = (ow - dst.w) / 2;
			dst.y = (oh - dst.h) / 2;
			SDL_RenderCopy(v->renderer, v->texture, NULL, &dst);
		}
	}
	SDL_RenderPresent(v->renderer);
}

static void handle_new_frame(PvVideo *v)
{
	atomic_store(&v->frame_event_pending, false);
	chiaki_mutex_lock(&v->dec_mutex);
	if(!v->decoder_active)
	{
		chiaki_mutex_unlock(&v->dec_mutex);
		return;
	}
	int32_t lost = 0;
	ChiakiFfmpegFrame cf = chiaki_ffmpeg_decoder_pull_frame(&v->decoder, &lost);
	bool decoder_is_hw = v->decoder_is_hw;
	chiaki_mutex_unlock(&v->dec_mutex);
	AVFrame *frame = cf.frame;
	if(!frame)
		return;

	AVFrame *show = frame;
	if(frame->hw_frames_ctx)
	{
		av_frame_unref(v->sw_frame);
		int r = av_hwframe_transfer_data(v->sw_frame, frame, 0);
		if(r < 0)
		{
			v->transfer_errors++;
			v->consecutive_transfer_errors++;
			if(v->transfer_errors <= 3)
			{
				char e[128];
				av_strerror(r, e, sizeof(e));
				LOGE("video", "Bild aus dem Hardware-Decoder kann nicht übernommen werden: %s", e);
			}
			av_frame_free(&frame);
			if(v->consecutive_transfer_errors >= HW_MAX_CONSECUTIVE_TRANSFER_ERRORS)
				switch_to_software(v, "Bildübergabe schlägt fehl");
			return;
		}
		av_frame_copy_props(v->sw_frame, frame);
		show = v->sw_frame;
		v->hw_frames = true;
	}
	else
	{
		v->hw_frames = false;
		if(decoder_is_hw && !v->logged_sw_frames_from_hw)
		{
			LOGW("video", "Der Hardware-Decoder %s unterstützt diesen Stream nicht - FFmpeg dekodiert in Software.", v->decoder_name);
			v->logged_sw_frames_from_hw = true;
		}
	}
	v->consecutive_transfer_errors = 0;
	if(upload_frame(v, show))
	{
		v->frames_shown++;
		v->last_frame_ms = pv_now_ms();
		pv_video_render(v);
	}
	av_frame_free(&frame);
}

static void toggle_fullscreen(PvVideo *v)
{
	Uint32 flags = SDL_GetWindowFlags(v->window);
	bool fs = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
	SDL_SetWindowFullscreen(v->window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

bool pv_video_handle_event(PvVideo *v, const SDL_Event *e)
{
	if(e->type == v->frame_event)
	{
		handle_new_frame(v);
		// Hardware decoder sanity check: samples arrive, but never a picture.
		if(v->decoder_is_hw)
		{
			uint64_t samples = atomic_load(&v->samples) - v->samples_at_start;
			if(samples > HW_NO_FRAME_SAMPLES && v->frames_shown == v->shown_at_start)
				switch_to_software(v, "kein einziges Bild");
		}
		return true;
	}
	switch(e->type)
	{
		case SDL_WINDOWEVENT:
			if(e->window.windowID != SDL_GetWindowID(v->window))
				return false;
			if(e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e->window.event == SDL_WINDOWEVENT_EXPOSED
					|| e->window.event == SDL_WINDOWEVENT_RESTORED)
				pv_video_render(v);
			return true;
		case SDL_RENDER_TARGETS_RESET:
		case SDL_RENDER_DEVICE_RESET:
			LOGW("video", "Grafikgerät wurde zurückgesetzt - Textur wird neu angelegt.");
			if(v->texture)
				SDL_DestroyTexture(v->texture);
			v->texture = NULL;
			pv_video_render(v);
			return true;
		case SDL_KEYDOWN:
			// Only window controls. Nothing is ever forwarded to the console.
			if(e->key.keysym.sym == SDLK_F11 || (e->key.keysym.sym == SDLK_RETURN && (e->key.keysym.mod & KMOD_ALT)))
				toggle_fullscreen(v);
			else if(e->key.keysym.sym == SDLK_ESCAPE && (SDL_GetWindowFlags(v->window) & SDL_WINDOW_FULLSCREEN_DESKTOP))
				SDL_SetWindowFullscreen(v->window, 0);
			return true;
		case SDL_MOUSEBUTTONDOWN:
			if(e->button.button == SDL_BUTTON_LEFT && e->button.clicks == 2)
				toggle_fullscreen(v);
			return true;
		default:
			return false;
	}
}

void pv_video_set_status(PvVideo *v, const char *status)
{
	if(status && strcmp(status, v->status) == 0)
		return;
	pv_strlcpy(v->status, status ? status : "", sizeof(v->status));
	char t[384];
	if(v->status[0])
		snprintf(t, sizeof(t), "%s - %s", v->title, v->status);
	else
		snprintf(t, sizeof(t), "%s", v->title);
	SDL_SetWindowTitle(v->window, t);
}

void pv_video_get_stats(PvVideo *v, PvVideoStats *out)
{
	memset(out, 0, sizeof(*out));
	out->samples = atomic_load(&v->samples);
	out->frames_lost = atomic_load(&v->frames_lost);
	out->decode_errors = atomic_load(&v->decode_errors);
	out->frames_shown = v->frames_shown;
	out->transfer_errors = v->transfer_errors;
	out->last_frame_ms = v->last_frame_ms;
	out->width = v->video_w;
	out->height = v->video_h;
	out->hw_frames = v->hw_frames;
	pv_strlcpy(out->decoder, v->decoder_active ? v->decoder_name : "-", sizeof(out->decoder));
}

void pv_video_clear(PvVideo *v)
{
	if(v->texture)
		SDL_DestroyTexture(v->texture);
	v->texture = NULL;
	v->tex_w = v->tex_h = 0;
	pv_video_render(v);
}
