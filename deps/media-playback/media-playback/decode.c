/*
 * Copyright (c) 2017 Hugh Bailey <obs.jim@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/hwcontext.h>
#include "decode.h"
#include "media.h"

#define PRINT_DEBUG(fmt, args...) {\
	if(Debug) { \
		printf(fmt "\n", ##args); \
	} \
}

static bool Debug = false;

static enum AVPixelFormat get_hw_format(struct AVCodecContext *s, const enum AVPixelFormat *pix_fmts) {
	PRINT_DEBUG("== get_hw_format ==");
	const enum AVPixelFormat *p;

	for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == AV_PIX_FMT_CUDA) {
			AVHWFramesContext  *frames_ctx;
			int ret;

			// create a pool of surfaces to be used by the decoder
			s->hw_frames_ctx = av_hwframe_ctx_alloc(s->hw_device_ctx);
			if (!s->hw_frames_ctx) {
				PRINT_DEBUG("av_hwframe_ctx_alloc failed");
				return AV_PIX_FMT_NONE;
			}

			frames_ctx   = (AVHWFramesContext*)s->hw_frames_ctx->data;
			frames_ctx->format            = AV_PIX_FMT_CUDA;
			frames_ctx->sw_format         = AV_PIX_FMT_NV12;
			frames_ctx->width             = s->width;
			frames_ctx->height            = s->height;
			frames_ctx->initial_pool_size = 32;

			PRINT_DEBUG("format=%d, sw_format=%d, w=%d, h=%d",
				frames_ctx->format, frames_ctx->sw_format, frames_ctx->width, frames_ctx->height);

			ret = av_hwframe_ctx_init(s->hw_frames_ctx);
			if (ret < 0) {
				PRINT_DEBUG("av_hwframe_ctx_init failed");
				return AV_PIX_FMT_NONE;
			}

			return *p;
		}
	}

	PRINT_DEBUG("Failed to get HW surface format.");
	return AV_PIX_FMT_NONE;
}

static AVCodec *find_hardware_decoder(struct mp_decode* d, AVStream* stream)
{
	int i;
	int nb_supported_device_types = 0;
	enum AVCodecID id;
	AVCodec *decoder = NULL;
	char** supported_types;
	enum AVHWDeviceType type;

	char pix_fmt_name[1024];
	memset(pix_fmt_name, 0, 1024);

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
	id = stream->codecpar->codec_id;
#else
	id = stream->codec->codec_id;
#endif

	char* types[] = {
		"AV_HWDEVICE_TYPE_VDPAU",
		"AV_HWDEVICE_TYPE_CUDA",
		"AV_HWDEVICE_TYPE_VAAPI",
		"AV_HWDEVICE_TYPE_DXVA2",
		"AV_HWDEVICE_TYPE_QSV",
		// "AV_HWDEVICE_TYPE_VIDEOTOOLBOX",
		// "AV_HWDEVICE_TYPE_D3D11VA",
		// "AV_HWDEVICE_TYPE_DRM",
		// "AV_HWDEVICE_TYPE_OPENCL",
		// "AV_HWDEVICE_TYPE_MEDIACODEC"
		"AV_HWDEVICE_TYPE_NONE",
	};

	PRINT_DEBUG("find_hardware_decoder, codec id: %d", id);
	PRINT_DEBUG("Available device types:");
	type = AV_HWDEVICE_TYPE_NONE;
	while((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
		PRINT_DEBUG("    %s", av_hwdevice_get_type_name(type));
		nb_supported_device_types++;
	}

	supported_types = malloc(nb_supported_device_types * sizeof(char*));

	type = AV_HWDEVICE_TYPE_NONE;
	i = 0;
	while((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE && i<nb_supported_device_types) {
		const char* name = av_hwdevice_get_type_name(type);
		int length = strlen(name);
		supported_types[i] = malloc(length+1);
		strcpy(supported_types[i], name);
		i++;
	}

	for(i = 0; nb_supported_device_types; i++) {
		type = av_hwdevice_find_type_by_name(supported_types[i]);
		if (type == AV_HWDEVICE_TYPE_NONE) {
			PRINT_DEBUG("HW device type '%s' = %d not supported", supported_types[i], type);
			if(strcmp(types[i], "AV_HWDEVICE_TYPE_NONE") == 0) {
				break;
			}
		} else {
			PRINT_DEBUG("HW device type '%s' = %d is supported", supported_types[i], type);
			break;
		}
	}

	for(i=0; i<nb_supported_device_types; i++) {
		free(supported_types[i]);
	}
	free(supported_types);

	if (type == AV_HWDEVICE_TYPE_NONE) {
		PRINT_DEBUG("Found no supported HW device type");
		return NULL;
	}

	decoder = avcodec_find_decoder(id);
	if (!decoder) {
		PRINT_DEBUG("avcodec_find_decoder failed");
		return NULL;
	}

	 for (i = 0;; i++) {
		const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
		if (!config) {
			PRINT_DEBUG("No HW decoder available for %s", decoder->name);
			return NULL;
		}

		av_get_pix_fmt_string(pix_fmt_name, 1024, config->pix_fmt);
		PRINT_DEBUG("hw config: pix_fmt: '%d' (%s), methods='%d', type='%d'", config->pix_fmt, pix_fmt_name, config->methods, config->device_type);

		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX /* TODO && config->device_type == type*/) {
			// TODO keep first
			//if(d->m->hwscale.hw_device_type == AV_HWDEVICE_TYPE_NONE)
			{
				d->m->hwscale.hw_device_type = type;
				break;
			}
			PRINT_DEBUG("found codec hw config with type: %d, and pix_fmt: %d", d->m->hwscale.hw_device_type, config->pix_fmt);
		}
	}

	return decoder;
}

static int mp_open_codec(struct mp_decode *d)
{
	AVCodecContext *c;
	int ret;

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
	c = avcodec_alloc_context3(d->codec);
	if (!c) {
		blog(LOG_WARNING, "MP: Failed to allocate context");
		return -1;
	}

	ret = avcodec_parameters_to_context(c, d->stream->codecpar);
	if (ret < 0)
		goto fail;
#else
	c = d->stream->codec;
#endif

	if (c->thread_count == 1 &&
	    c->codec_id != AV_CODEC_ID_PNG &&
	    c->codec_id != AV_CODEC_ID_TIFF &&
	    c->codec_id != AV_CODEC_ID_JPEG2000 &&
	    c->codec_id != AV_CODEC_ID_MPEG4 &&
	    c->codec_id != AV_CODEC_ID_WEBP)
		c->thread_count = 0;

	if(d->m->hwscale.hw_device_type != AV_HWDEVICE_TYPE_NONE) {
		ret = av_hwdevice_ctx_create(&c->hw_device_ctx, d->m->hwscale.hw_device_type, NULL, NULL, 0);
		if(ret < 0) {
			PRINT_DEBUG("av_hwdevice_ctx_create failed");
			goto fail;
		}
		c->get_format = get_hw_format;
	}

	ret = avcodec_open2(c, d->codec, NULL);
	if (ret < 0)
		goto fail;

	d->decoder = c;
	return ret;

fail:
	avcodec_close(c);
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
	av_free(d->decoder);
#endif
	return ret;
}

bool mp_decode_init(mp_media_t *m, enum AVMediaType type, bool hw)
{
	struct mp_decode *d = type == AVMEDIA_TYPE_VIDEO ? &m->v : &m->a;
	enum AVCodecID id;
	AVStream *stream;
	int ret;

	memset(d, 0, sizeof(*d));
	d->m = m;
	d->audio = type == AVMEDIA_TYPE_AUDIO;

	ret = av_find_best_stream(m->fmt, type, -1, -1, NULL, 0);
	if (ret < 0)
		return false;
	stream = d->stream = m->fmt->streams[ret];

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
	id = stream->codecpar->codec_id;
#else
	id = stream->codec->codec_id;
#endif

	// TODO don't restrict to video only
	if (hw && type == AVMEDIA_TYPE_VIDEO) {
		d->codec = find_hardware_decoder(d, stream);
		if (d->codec) {
			PRINT_DEBUG("mp_decode_init: found HW decoder name='%s' long_name='%s'", d->codec->name, d->codec->long_name);
			ret = mp_open_codec(d);
			if (ret < 0) {
				PRINT_DEBUG("mp_decode_init: could not open codec :(");
				d->codec = NULL;
			}
		} else {
			PRINT_DEBUG("mp_decode_init: HW decoder not found");
		}
	}

	if (!d->codec) {
		if (id == AV_CODEC_ID_VP8)
			d->codec = avcodec_find_decoder_by_name("libvpx");
		else if (id == AV_CODEC_ID_VP9)
			d->codec = avcodec_find_decoder_by_name("libvpx-vp9");

		if (!d->codec)
			d->codec = avcodec_find_decoder(id);
		if (!d->codec) {
			blog(LOG_WARNING, "MP: Failed to find %s codec",
					av_get_media_type_string(type));
			PRINT_DEBUG("mp_decode_init: SW decoder not found");
			return false;
		}
		PRINT_DEBUG("mp_decode_init: found SW decoder name='%s' long_name='%s'", d->codec->name, d->codec->long_name);

		ret = mp_open_codec(d);
		if (ret < 0) {
			blog(LOG_WARNING, "MP: Failed to open %s decoder: %s",
					av_get_media_type_string(type),
					av_err2str(ret));
			PRINT_DEBUG("mp_decode_init: could not open codec :(");
			return false;
		}
	}

	d->frame = av_frame_alloc();
	if (!d->frame) {
		blog(LOG_WARNING, "MP: Failed to allocate %s frame",
				av_get_media_type_string(type));
		return false;
	}

	if (d->codec->capabilities & CODEC_CAP_TRUNC)
		d->decoder->flags |= CODEC_FLAG_TRUNC;
	return true;
}

void mp_decode_clear_packets(struct mp_decode *d)
{
	if (d->packet_pending) {
		av_packet_unref(&d->orig_pkt);
		d->packet_pending = false;
	}

	while (d->packets.size) {
		AVPacket pkt;
		circlebuf_pop_front(&d->packets, &pkt, sizeof(pkt));
		av_packet_unref(&pkt);
	}
}

void mp_decode_free(struct mp_decode *d)
{
	mp_decode_clear_packets(d);
	circlebuf_free(&d->packets);
	AVBufferRef *hw_frames_ctx = NULL;

	if (d->decoder) {
		avcodec_flush_buffers(d->decoder);
		if(d->decoder->hw_frames_ctx) {
			hw_frames_ctx = malloc(sizeof(AVBufferRef));
			hw_frames_ctx->buffer = d->decoder->hw_frames_ctx->buffer;
			hw_frames_ctx->data = d->decoder->hw_frames_ctx->data;
			hw_frames_ctx->size = d->decoder->hw_frames_ctx->size;
		}
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
		avcodec_free_context(&d->decoder);
#else
		avcodec_close(d->decoder);
#endif
	}
	if (d->frame) {
		av_frame_unref(d->frame);
		av_free(d->frame);
	}

	// Force release the hw frames by unref-ing in a loop until the
	// reference reaches 0, which triggers a the hw-specific free() call
	// TODO this is hacky, the unref-ing should be done gracefully but I can't
	// find the places where new references are taken and not released
	if(hw_frames_ctx) {
		AVBufferRef *hw_frames_ctx2 = NULL;
		do {
			hw_frames_ctx2 = malloc(sizeof(AVBufferRef));
			hw_frames_ctx2->buffer = hw_frames_ctx->buffer;
			hw_frames_ctx2->data = hw_frames_ctx->data;
			hw_frames_ctx2->size = hw_frames_ctx->size;
			av_buffer_unref(&hw_frames_ctx2);
		} while(hw_frames_ctx->buffer && av_buffer_get_ref_count(hw_frames_ctx) > 0);
		av_freep(&hw_frames_ctx);
	}

	memset(d, 0, sizeof(*d));
}

void mp_decode_push_packet(struct mp_decode *decode, AVPacket *packet)
{
	circlebuf_push_back(&decode->packets, packet, sizeof(*packet));
}

static inline int64_t get_estimated_duration(struct mp_decode *d,
		int64_t last_pts)
{
	if (last_pts)
		return d->frame_pts - last_pts;

	if (d->audio) {
		return av_rescale_q(d->frame->nb_samples,
				(AVRational){1, d->frame->sample_rate},
				(AVRational){1, 1000000000});
	} else {
		if (d->last_duration)
			return d->last_duration;

		return av_rescale_q(d->decoder->time_base.num,
				d->decoder->time_base,
				(AVRational){1, 1000000000});
	}
}

static int decode_packet(struct mp_decode *d, int *got_frame)
{
	int ret;
	*got_frame = 0;

#ifdef USE_NEW_FFMPEG_DECODE_API
	ret = avcodec_receive_frame(d->decoder, d->frame);
	if (ret != 0 && ret != AVERROR(EAGAIN)) {
		if (ret == AVERROR_EOF)
			ret = 0;
		return ret;
	}

	if (ret != 0) {
		ret = avcodec_send_packet(d->decoder, &d->pkt);
		if (ret != 0 && ret != AVERROR(EAGAIN)) {
			if (ret == AVERROR_EOF)
				ret = 0;
			return ret;
		}

		ret = avcodec_receive_frame(d->decoder, d->frame);
		if (ret != 0 && ret != AVERROR(EAGAIN)) {
			if (ret == AVERROR_EOF)
				ret = 0;
			return ret;
		}

		*got_frame = (ret == 0);
		ret = d->pkt.size;
	} else {
		ret = 0;
		*got_frame = 1;
	}

#else
	if (d->audio) {
		ret = avcodec_decode_audio4(d->decoder,
				d->frame, got_frame, &d->pkt);
	} else {
		ret = avcodec_decode_video2(d->decoder,
				d->frame, got_frame, &d->pkt);
	}
#endif
	return ret;
}

bool mp_decode_next(struct mp_decode *d)
{
	bool eof = d->m->eof;
	int got_frame;
	int ret;

	d->frame_ready = false;

	if (!eof && !d->packets.size)
		return true;

	while (!d->frame_ready) {
		if (!d->packet_pending) {
			if (!d->packets.size) {
				if (eof) {
					d->pkt.data = NULL;
					d->pkt.size = 0;
				} else {
					return true;
				}
			} else {
				circlebuf_pop_front(&d->packets, &d->orig_pkt,
						sizeof(d->orig_pkt));
				d->pkt = d->orig_pkt;
				d->packet_pending = true;
			}
		}

		ret = decode_packet(d, &got_frame);

		if (!got_frame && ret == 0) {
			d->eof = true;
			return true;
		}
		if (ret < 0) {
#ifdef DETAILED_DEBUG_INFO
			blog(LOG_DEBUG, "MP: decode failed: %s",
					av_err2str(ret));
#endif

			if (d->packet_pending) {
				av_packet_unref(&d->orig_pkt);
				av_init_packet(&d->orig_pkt);
				av_init_packet(&d->pkt);
				d->packet_pending = false;
			}
			return true;
		}

		d->frame_ready = !!got_frame;

		if (d->packet_pending) {
			if (d->pkt.size) {
				d->pkt.data += ret;
				d->pkt.size -= ret;
			}

			if (d->pkt.size <= 0) {
				av_packet_unref(&d->orig_pkt);
				av_init_packet(&d->orig_pkt);
				av_init_packet(&d->pkt);
				d->packet_pending = false;
			}
		}
	}

	if (d->frame_ready) {
		bool debug = false;
		int64_t last_pts = d->frame_pts;

		if(debug) {
			PRINT_DEBUG("%s bet=%ld pts=%ld, next=%ld tb=%d/%d dur=%ld\n",
				(d->audio ? "AUDIO" : "VIDEO"),
				d->frame->best_effort_timestamp,
				d->frame_pts,
				d->next_pts,
				d->stream->time_base.num, d->stream->time_base.den,
				d->frame->pkt_duration
			);
		}

		if (d->frame->best_effort_timestamp == AV_NOPTS_VALUE) {
			d->frame_pts = d->next_pts;
			if(debug) {
				PRINT_DEBUG("%s pts <= next\n", (d->audio ? "AUDIO" : "VIDEO"));
			}
		} else {
			d->frame_pts = av_rescale_q(
					d->frame->best_effort_timestamp,
					d->stream->time_base,
					(AVRational){1, 1000000000});
			if(debug) {
				PRINT_DEBUG("%s pts <= av_rescale_q: %ld\n", (d->audio ? "AUDIO" : "VIDEO"), d->frame_pts);
			}
		}

		int64_t duration = d->frame->pkt_duration;
		if (!duration) {
			duration = get_estimated_duration(d, last_pts);
			if(debug) {
				PRINT_DEBUG("%s dur <= get_estimated_duration: %ld\n", (d->audio ? "AUDIO" : "VIDEO"), duration);
			}
		} else {
			duration = av_rescale_q(duration,
					d->stream->time_base,
					(AVRational){1, 1000000000});
			if(debug) {
				PRINT_DEBUG("%s dur <= av_rescale_q: %ld\n", (d->audio ? "AUDIO" : "VIDEO"), duration);
			}
		}

		if (d->m->speed != 100) {
			d->frame_pts = av_rescale_q(d->frame_pts,
					(AVRational){1, d->m->speed},
					(AVRational){1, 100});
			duration = av_rescale_q(duration,
					(AVRational){1, d->m->speed},
					(AVRational){1, 100});
			if(debug) {
				PRINT_DEBUG("%s m->speed != 100. NOT SUPPOSED TO HAPPEN\n", (d->audio ? "AUDIO" : "VIDEO"));
			}
		}

		d->last_duration = duration;
		d->next_pts = d->frame_pts + duration;
		if(debug) {
			PRINT_DEBUG("%s last_duration=%ld next=%ld\n", (d->audio ? "AUDIO" : "VIDEO"), d->last_duration, d->next_pts);
		}
	}

	return true;
}

void mp_decode_flush(struct mp_decode *d)
{
	avcodec_flush_buffers(d->decoder);
	mp_decode_clear_packets(d);
	d->eof = false;
	d->frame_pts = 0;
	d->frame_ready = false;
}
