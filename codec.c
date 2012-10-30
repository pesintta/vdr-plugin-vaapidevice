///
///	@file codec.c	@brief Codec functions
///
///	Copyright (c) 2009 - 2012 by Johns.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
///	$Id$
//////////////////////////////////////////////////////////////////////////////

///
///	@defgroup Codec The codec module.
///
///		This module contains all decoder and codec functions.
///		It is uses ffmpeg (http://ffmpeg.org) as backend.
///
///		It may work with libav (http://libav.org), but the tests show
///		many bugs and incompatiblity in it.  Don't use this shit.
///

    /// compile with passthrough support (stable, ac3 only)
#define USE_PASSTHROUGH
    /// compile audio drift correction support (experimental)
#define USE_AUDIO_DRIFT_CORRECTION
    /// compile AC3 audio drift correction support (experimental)
#define USE_AC3_DRIFT_CORRECTION

#include <stdio.h>
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/endian.h>
#else
#include <endian.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#include <alsa/iatomic.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/vaapi.h>
#ifdef USE_VDPAU
#include <libavcodec/vdpau.h>
#endif

#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>

#ifdef MAIN_H
#include MAIN_H
#endif
#include "misc.h"
#include "video.h"
#include "audio.h"
#include "codec.h"

//----------------------------------------------------------------------------
//	Global
//----------------------------------------------------------------------------

      ///
      ///	ffmpeg lock mutex
      ///
      ///	new ffmpeg dislikes simultanous open/close
      ///	this breaks our code, until this is fixed use lock.
      ///
static pthread_mutex_t CodecLockMutex;

//----------------------------------------------------------------------------
//	Video
//----------------------------------------------------------------------------

#if 0
///
///	Video decoder typedef.
///
//typedef struct _video_decoder_ Decoder;
#endif

///
///	Video decoder structure.
///
struct _video_decoder_
{
    VideoHwDecoder *HwDecoder;		///< video hardware decoder

    int GetFormatDone;			///< flag get format called!
    AVCodec *VideoCodec;		///< video codec
    AVCodecContext *VideoCtx;		///< video codec context
    AVFrame *Frame;			///< decoded video frame
};

//----------------------------------------------------------------------------
//	Call-backs
//----------------------------------------------------------------------------

/**
**	Callback to negotiate the PixelFormat.
**
**	@param fmt	is the list of formats which are supported by the codec,
**			it is terminated by -1 as 0 is a valid format, the
**			formats are ordered by quality.
*/
static enum PixelFormat Codec_get_format(AVCodecContext * video_ctx,
    const enum PixelFormat *fmt)
{
    VideoDecoder *decoder;

    decoder = video_ctx->opaque;
    //Debug(3, "codec: %s: %18p\n", __FUNCTION__, decoder);
    decoder->GetFormatDone = 1;
    return Video_get_format(decoder->HwDecoder, video_ctx, fmt);
}

/**
**	Video buffer management, get buffer for frame.
**
**	Called at the beginning of each frame to get a buffer for it.
**
**	@param video_ctx	Codec context
**	@param frame		Get buffer for this frame
*/
static int Codec_get_buffer(AVCodecContext * video_ctx, AVFrame * frame)
{
    VideoDecoder *decoder;

    decoder = video_ctx->opaque;
    if (!decoder->GetFormatDone) {	// get_format missing
	enum PixelFormat fmts[2];

	fprintf(stderr, "codec: buggy ffmpeg/libav\n");
	Warning(_("codec: buggy ffmpeg/libav\n"));
	fmts[0] = video_ctx->pix_fmt;
	fmts[1] = PIX_FMT_NONE;
	Codec_get_format(video_ctx, fmts);
    }
#ifdef USE_VDPAU
    // VDPAU: PIX_FMT_VDPAU_H264 .. PIX_FMT_VDPAU_VC1 PIX_FMT_VDPAU_MPEG4
    if ((PIX_FMT_VDPAU_H264 <= video_ctx->pix_fmt
	    && video_ctx->pix_fmt <= PIX_FMT_VDPAU_VC1)
	|| video_ctx->pix_fmt == PIX_FMT_VDPAU_MPEG4) {
	unsigned surface;
	struct vdpau_render_state *vrs;

	surface = VideoGetSurface(decoder->HwDecoder);
	vrs = av_mallocz(sizeof(struct vdpau_render_state));
	vrs->surface = surface;

	//Debug(3, "codec: use surface %#010x\n", surface);

	frame->type = FF_BUFFER_TYPE_USER;
#if LIBAVCODEC_VERSION_INT <= AV_VERSION_INT(53,46,0)
	frame->age = 256 * 256 * 256 * 64;
#endif
	// render
	frame->data[0] = (void *)vrs;
	frame->data[1] = NULL;
	frame->data[2] = NULL;
	frame->data[3] = NULL;

	// reordered frames
	if (video_ctx->pkt) {
	    frame->pkt_pts = video_ctx->pkt->pts;
	} else {
	    frame->pkt_pts = AV_NOPTS_VALUE;
	}
	return 0;
    }
#endif
    // VA-API:
    if (video_ctx->hwaccel_context) {
	unsigned surface;

	surface = VideoGetSurface(decoder->HwDecoder);

	//Debug(3, "codec: use surface %#010x\n", surface);

	frame->type = FF_BUFFER_TYPE_USER;
#if LIBAVCODEC_VERSION_INT <= AV_VERSION_INT(53,46,0)
	frame->age = 256 * 256 * 256 * 64;
#endif
	// vaapi needs both fields set
	frame->data[0] = (void *)(size_t) surface;
	frame->data[3] = (void *)(size_t) surface;

	// reordered frames
	if (video_ctx->pkt) {
	    frame->pkt_pts = video_ctx->pkt->pts;
	} else {
	    frame->pkt_pts = AV_NOPTS_VALUE;
	}
	return 0;
    }
    //Debug(3, "codec: fallback to default get_buffer\n");
    return avcodec_default_get_buffer(video_ctx, frame);
}

/**
**	Video buffer management, release buffer for frame.
**	Called to release buffers which were allocated with get_buffer.
**
**	@param video_ctx	Codec context
**	@param frame		Release buffer for this frame
*/
static void Codec_release_buffer(AVCodecContext * video_ctx, AVFrame * frame)
{
#ifdef USE_VDPAU
    // VDPAU: PIX_FMT_VDPAU_H264 .. PIX_FMT_VDPAU_VC1 PIX_FMT_VDPAU_MPEG4
    if ((PIX_FMT_VDPAU_H264 <= video_ctx->pix_fmt
	    && video_ctx->pix_fmt <= PIX_FMT_VDPAU_VC1)
	|| video_ctx->pix_fmt == PIX_FMT_VDPAU_MPEG4) {
	VideoDecoder *decoder;
	struct vdpau_render_state *vrs;
	unsigned surface;

	decoder = video_ctx->opaque;
	vrs = (struct vdpau_render_state *)frame->data[0];
	surface = vrs->surface;

	//Debug(3, "codec: release surface %#010x\n", surface);
	VideoReleaseSurface(decoder->HwDecoder, surface);

	av_freep(&vrs->bitstream_buffers);
	vrs->bitstream_buffers_allocated = 0;
	av_freep(&frame->data[0]);

	return;
    }
#endif
    // VA-API
    if (video_ctx->hwaccel_context) {
	VideoDecoder *decoder;
	unsigned surface;

	decoder = video_ctx->opaque;
	surface = (unsigned)(size_t) frame->data[3];

	//Debug(3, "codec: release surface %#010x\n", surface);
	VideoReleaseSurface(decoder->HwDecoder, surface);

	frame->data[0] = NULL;
	frame->data[3] = NULL;

	return;
    }
    //Debug(3, "codec: fallback to default release_buffer\n");
    return avcodec_default_release_buffer(video_ctx, frame);
}

/// libav: compatibility hack
#ifndef AV_NUM_DATA_POINTERS
#define AV_NUM_DATA_POINTERS	4
#endif

/**
**	Draw a horizontal band.
**
**	@param video_ctx	Codec context
**	@param frame		draw this frame
**	@param y		y position of slice
**	@param type		1->top field, 2->bottom field, 3->frame
**	@param offset		offset into AVFrame.data from which slice
**				should be read
**	@param height		height of slice
*/
static void Codec_draw_horiz_band(AVCodecContext * video_ctx,
    const AVFrame * frame, __attribute__ ((unused))
    int offset[AV_NUM_DATA_POINTERS], __attribute__ ((unused))
    int y, __attribute__ ((unused))
    int type, __attribute__ ((unused))
    int height)
{
#ifdef USE_VDPAU
    // VDPAU: PIX_FMT_VDPAU_H264 .. PIX_FMT_VDPAU_VC1 PIX_FMT_VDPAU_MPEG4
    if ((PIX_FMT_VDPAU_H264 <= video_ctx->pix_fmt
	    && video_ctx->pix_fmt <= PIX_FMT_VDPAU_VC1)
	|| video_ctx->pix_fmt == PIX_FMT_VDPAU_MPEG4) {
	VideoDecoder *decoder;
	struct vdpau_render_state *vrs;

	//unsigned surface;

	decoder = video_ctx->opaque;
	vrs = (struct vdpau_render_state *)frame->data[0];
	//surface = vrs->surface;

	//Debug(3, "codec: draw slice surface %#010x\n", surface);
	//Debug(3, "codec: %d references\n", vrs->info.h264.num_ref_frames);

	VideoDrawRenderState(decoder->HwDecoder, vrs);
	return;
    }
#else
    (void)video_ctx;
    (void)frame;
#endif
}

//----------------------------------------------------------------------------
//	Test
//----------------------------------------------------------------------------

/**
**	Allocate a new video decoder context.
**
**	@param hw_decoder	video hardware decoder
**
**	@returns private decoder pointer for video decoder.
*/
VideoDecoder *CodecVideoNewDecoder(VideoHwDecoder * hw_decoder)
{
    VideoDecoder *decoder;

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
	Fatal(_("codec: can't allocate vodeo decoder\n"));
    }
    decoder->HwDecoder = hw_decoder;

    return decoder;
}

/**
**	Deallocate a video decoder context.
**
**	@param decoder	private video decoder
*/
void CodecVideoDelDecoder(VideoDecoder * decoder)
{
    free(decoder);
}

/**
**	Open video decoder.
**
**	@param decoder	private video decoder
**	@param name	video codec name
**	@param codec_id	video codec id, used if name == NULL
*/
void CodecVideoOpen(VideoDecoder * decoder, const char *name, int codec_id)
{
    AVCodec *video_codec;

    Debug(3, "codec: using video codec %s or ID %#06x\n", name, codec_id);

    if (decoder->VideoCtx) {
	Error(_("codec: missing close\n"));
    }

    if (name && (video_codec = avcodec_find_decoder_by_name(name))) {
	Debug(3, "codec: vdpau decoder found\n");
    } else if (!(video_codec = avcodec_find_decoder(codec_id))) {
	Fatal(_("codec: codec ID %#06x not found\n"), codec_id);
	// FIXME: none fatal
    }
    decoder->VideoCodec = video_codec;

    if (!(decoder->VideoCtx = avcodec_alloc_context3(video_codec))) {
	Fatal(_("codec: can't allocate video codec context\n"));
    }
    // FIXME: for software decoder use all cpus, otherwise 1
    decoder->VideoCtx->thread_count = 1;
    pthread_mutex_lock(&CodecLockMutex);
    // open codec
#if LIBAVCODEC_VERSION_INT <= AV_VERSION_INT(53,5,0)
    if (avcodec_open(decoder->VideoCtx, video_codec) < 0) {
	pthread_mutex_unlock(&CodecLockMutex);
	Fatal(_("codec: can't open video codec!\n"));
    }
#else
    if (video_codec->capabilities & (CODEC_CAP_HWACCEL_VDPAU |
	    CODEC_CAP_HWACCEL)) {
	Debug(3, "codec: video mpeg hack active\n");
	// HACK around badly placed checks in mpeg_mc_decode_init
	// taken from mplayer vd_ffmpeg.c
	decoder->VideoCtx->slice_flags =
	    SLICE_FLAG_CODED_ORDER | SLICE_FLAG_ALLOW_FIELD;
	decoder->VideoCtx->thread_count = 1;
	decoder->VideoCtx->active_thread_type = 0;
    }

    if (avcodec_open2(decoder->VideoCtx, video_codec, NULL) < 0) {
	pthread_mutex_unlock(&CodecLockMutex);
	Fatal(_("codec: can't open video codec!\n"));
    }
#endif
    pthread_mutex_unlock(&CodecLockMutex);

    decoder->VideoCtx->opaque = decoder;	// our structure

    Debug(3, "codec: video '%s'\n", decoder->VideoCtx->codec_name);
    if (codec_id == CODEC_ID_H264) {
	// 2.53 Ghz CPU is too slow for this codec at 1080i
	//decoder->VideoCtx->skip_loop_filter = AVDISCARD_ALL;
	//decoder->VideoCtx->skip_loop_filter = AVDISCARD_BIDIR;
    }
    if (video_codec->capabilities & CODEC_CAP_TRUNCATED) {
	Debug(3, "codec: video can use truncated packets\n");
	// we send incomplete frames, for old PES recordings
	decoder->VideoCtx->flags |= CODEC_FLAG_TRUNCATED;
    }
    // FIXME: own memory management for video frames.
    if (video_codec->capabilities & CODEC_CAP_DR1) {
	Debug(3, "codec: can use own buffer management\n");
    }
    if (video_codec->capabilities & CODEC_CAP_HWACCEL_VDPAU) {
	Debug(3, "codec: can export data for HW decoding (VDPAU)\n");
    }
#ifdef CODEC_CAP_FRAME_THREADS
    if (video_codec->capabilities & CODEC_CAP_FRAME_THREADS) {
	Debug(3, "codec: codec supports frame threads\n");
    }
#endif
    //decoder->VideoCtx->debug = FF_DEBUG_STARTCODE;
    //decoder->VideoCtx->err_recognition |= AV_EF_EXPLODE;

    if (video_codec->capabilities & CODEC_CAP_HWACCEL_VDPAU) {
	// FIXME: get_format never called.
	decoder->VideoCtx->get_format = Codec_get_format;
	decoder->VideoCtx->get_buffer = Codec_get_buffer;
	decoder->VideoCtx->release_buffer = Codec_release_buffer;
	decoder->VideoCtx->reget_buffer = Codec_get_buffer;
	decoder->VideoCtx->draw_horiz_band = Codec_draw_horiz_band;
	decoder->VideoCtx->slice_flags =
	    SLICE_FLAG_CODED_ORDER | SLICE_FLAG_ALLOW_FIELD;
	decoder->VideoCtx->thread_count = 1;
	decoder->VideoCtx->active_thread_type = 0;
    } else {
	decoder->VideoCtx->get_format = Codec_get_format;
	decoder->VideoCtx->hwaccel_context =
	    VideoGetVaapiContext(decoder->HwDecoder);
    }

    // our pixel format video hardware decoder hook
    if (decoder->VideoCtx->hwaccel_context) {
	decoder->VideoCtx->get_format = Codec_get_format;
	decoder->VideoCtx->get_buffer = Codec_get_buffer;
	decoder->VideoCtx->release_buffer = Codec_release_buffer;
	decoder->VideoCtx->reget_buffer = Codec_get_buffer;
#if 0
	decoder->VideoCtx->thread_count = 1;
	decoder->VideoCtx->draw_horiz_band = NULL;
	decoder->VideoCtx->slice_flags =
	    SLICE_FLAG_CODED_ORDER | SLICE_FLAG_ALLOW_FIELD;
	//decoder->VideoCtx->flags |= CODEC_FLAG_EMU_EDGE;
#endif
    }
    //
    //	Prepare frame buffer for decoder
    //
    if (!(decoder->Frame = avcodec_alloc_frame())) {
	Fatal(_("codec: can't allocate decoder frame\n"));
    }
    // reset buggy ffmpeg/libav flag
    decoder->GetFormatDone = 0;
}

/**
**	Close video decoder.
**
**	@param video_decoder	private video decoder
*/
void CodecVideoClose(VideoDecoder * video_decoder)
{
    // FIXME: play buffered data
    av_freep(&video_decoder->Frame);
    if (video_decoder->VideoCtx) {
	pthread_mutex_lock(&CodecLockMutex);
	avcodec_close(video_decoder->VideoCtx);
	pthread_mutex_unlock(&CodecLockMutex);
	av_freep(&video_decoder->VideoCtx);
    }
}

#if 0

/**
**	Display pts...
**
**	ffmpeg-0.9 pts always AV_NOPTS_VALUE
**	ffmpeg-0.9 pkt_pts nice monotonic (only with HD)
**	ffmpeg-0.9 pkt_dts wild jumping -160 - 340 ms
**
**	libav 0.8_pre20111116 pts always AV_NOPTS_VALUE
**	libav 0.8_pre20111116 pkt_pts always 0 (could be fixed?)
**	libav 0.8_pre20111116 pkt_dts wild jumping -160 - 340 ms
*/
void DisplayPts(AVCodecContext * video_ctx, AVFrame * frame)
{
    int ms_delay;
    int64_t pts;
    static int64_t last_pts;

    pts = frame->pkt_pts;
    if (pts == (int64_t) AV_NOPTS_VALUE) {
	printf("*");
    }
    ms_delay = (1000 * video_ctx->time_base.num) / video_ctx->time_base.den;
    ms_delay += frame->repeat_pict * ms_delay / 2;
    printf("codec: PTS %s%s %" PRId64 " %d %d/%d %dms\n",
	frame->repeat_pict ? "r" : " ", frame->interlaced_frame ? "I" : " ",
	pts, (int)(pts - last_pts) / 90, video_ctx->time_base.num,
	video_ctx->time_base.den, ms_delay);

    if (pts != (int64_t) AV_NOPTS_VALUE) {
	last_pts = pts;
    }
}

#endif

/**
**	Decode a video packet.
**
**	@param decoder	video decoder data
**	@param avpkt	video packet
*/
void CodecVideoDecode(VideoDecoder * decoder, const AVPacket * avpkt)
{
    AVCodecContext *video_ctx;
    AVFrame *frame;
    int used;
    int got_frame;
    AVPacket pkt[1];

    video_ctx = decoder->VideoCtx;
    frame = decoder->Frame;
    *pkt = *avpkt;			// use copy

  next_part:
    // FIXME: this function can crash with bad packets
    used = avcodec_decode_video2(video_ctx, frame, &got_frame, pkt);
    Debug(4, "%s: %p %d -> %d %d\n", __FUNCTION__, pkt->data, pkt->size, used,
	got_frame);

    if (used < 0) {
	Debug(3, "codec: bad video frame\n");
	return;
    }

    if (got_frame) {			// frame completed
	//DisplayPts(video_ctx, frame);
	VideoRenderFrame(decoder->HwDecoder, video_ctx, frame);
    } else {
	// some frames are needed for references, interlaced frames ...
	// could happen with h264 dvb streams, just drop data.

	Debug(4, "codec: %8d incomplete interlaced frame %d bytes used\n",
	    video_ctx->frame_number, used);
    }

#if 1
    // old code to support truncated or multi frame packets
    if (used != pkt->size) {
	// ffmpeg 0.8.7 dislikes our seq_end_h264 and enters endless loop here
	if (used == 0 && pkt->size == 5 && pkt->data[4] == 0x0A) {
	    Warning("codec: ffmpeg 0.8.x workaround used\n");
	    return;
	}
	if (used >= 0 && used < pkt->size) {
	    // some tv channels, produce this
	    Debug(4,
		"codec: ooops didn't use complete video packet used %d of %d\n",
		used, pkt->size);
	    pkt->size -= used;
	    pkt->data += used;
	    // FIXME: align problem?
	    goto next_part;
	}
    }
#endif
}

/**
**	Flush the video decoder.
**
**	@param decoder	video decoder data
*/
void CodecVideoFlushBuffers(VideoDecoder * decoder)
{
    if (decoder->VideoCtx) {
	avcodec_flush_buffers(decoder->VideoCtx);
    }
}

//----------------------------------------------------------------------------
//	Audio
//----------------------------------------------------------------------------

#if 0
///
///	Audio decoder typedef.
///
typedef struct _audio_decoder_ AudioDecoder;
#endif

///
///	Audio decoder structure.
///
struct _audio_decoder_
{
    AVCodec *AudioCodec;		///< audio codec
    AVCodecContext *AudioCtx;		///< audio codec context

    int PassthroughAC3;			///< current ac-3 pass-through
    int SampleRate;			///< current stream sample rate
    int Channels;			///< current stream channels

    int HwSampleRate;			///< hw sample rate
    int HwChannels;			///< hw channels

    ReSampleContext *ReSample;		///< audio resampling context

    int64_t LastDelay;			///< last delay
    struct timespec LastTime;		///< last time
    int64_t LastPTS;			///< last PTS

    int Drift;				///< accumulated audio drift
    int DriftCorr;			///< audio drift correction value
    int DriftFrac;			///< audio drift fraction for ac3

    struct AVResampleContext *AvResample;	///< second audio resample context
#define MAX_CHANNELS 8			///< max number of channels supported
    int16_t *Buffer[MAX_CHANNELS];	///< deinterleave sample buffers
    int BufferSize;			///< size of sample buffer
    int16_t *Remain[MAX_CHANNELS];	///< filter remaining samples
    int RemainSize;			///< size of remain buffer
    int RemainCount;			///< number of remaining samples
};

#ifdef USE_AUDIO_DRIFT_CORRECTION
static char CodecAudioDrift;		///< flag: enable audio-drift correction
#else
static const int CodecAudioDrift = 0;
#endif
#ifdef USE_PASSTHROUGH
//static char CodecPassthroughPCM;	///< pass pcm through (unsupported)
static char CodecPassthroughAC3;	///< pass ac3 through

//static char CodecPassthroughDTS;	///< pass dts through (unsupported)
//static char CodecPassthroughMPA;	///< pass mpa through (unsupported)
#else

static const int CodecPassthroughAC3 = 0;
#endif
static char CodecDownmix;		///< enable ac-3 downmix

/**
**	Allocate a new audio decoder context.
**
**	@returns private decoder pointer for audio decoder.
*/
AudioDecoder *CodecAudioNewDecoder(void)
{
    AudioDecoder *audio_decoder;

    if (!(audio_decoder = calloc(1, sizeof(*audio_decoder)))) {
	Fatal(_("codec: can't allocate audio decoder\n"));
    }

    return audio_decoder;
}

/**
**	Deallocate an audio decoder context.
**
**	@param decoder	private audio decoder
*/
void CodecAudioDelDecoder(AudioDecoder * decoder)
{
    free(decoder);
}

/**
**	Open audio decoder.
**
**	@param audio_decoder	private audio decoder
**	@param name	audio codec name
**	@param codec_id	audio codec id, used if name == NULL
*/
void CodecAudioOpen(AudioDecoder * audio_decoder, const char *name,
    int codec_id)
{
    AVCodec *audio_codec;

    Debug(3, "codec: using audio codec %s or ID %#06x\n", name, codec_id);

    if (name && (audio_codec = avcodec_find_decoder_by_name(name))) {
	Debug(3, "codec: audio decoder '%s' found\n", name);
    } else if (!(audio_codec = avcodec_find_decoder(codec_id))) {
	Fatal(_("codec: codec ID %#06x not found\n"), codec_id);
	// FIXME: errors aren't fatal
    }
    audio_decoder->AudioCodec = audio_codec;

    if (!(audio_decoder->AudioCtx = avcodec_alloc_context3(audio_codec))) {
	Fatal(_("codec: can't allocate audio codec context\n"));
    }

    if (CodecDownmix) {
	audio_decoder->AudioCtx->request_channels = 2;
	audio_decoder->AudioCtx->request_channel_layout =
	    AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    pthread_mutex_lock(&CodecLockMutex);
    // open codec
#if LIBAVCODEC_VERSION_INT <= AV_VERSION_INT(53,5,0)
    if (avcodec_open(audio_decoder->AudioCtx, audio_codec) < 0) {
	pthread_mutex_unlock(&CodecLockMutex);
	Fatal(_("codec: can't open audio codec\n"));
    }
#else
    if (1) {
	AVDictionary *av_dict;

	av_dict = NULL;
	// FIXME: import settings
	//av_dict_set(&av_dict, "dmix_mode", "0", 0);
	//av_dict_set(&av_dict, "ltrt_cmixlev", "1.414", 0);
	//av_dict_set(&av_dict, "loro_cmixlev", "1.414", 0);
	if (avcodec_open2(audio_decoder->AudioCtx, audio_codec, &av_dict) < 0) {
	    pthread_mutex_unlock(&CodecLockMutex);
	    Fatal(_("codec: can't open audio codec\n"));
	}
	av_dict_free(&av_dict);
    }
#endif
    pthread_mutex_unlock(&CodecLockMutex);
    Debug(3, "codec: audio '%s'\n", audio_decoder->AudioCtx->codec_name);

    if (audio_codec->capabilities & CODEC_CAP_TRUNCATED) {
	Debug(3, "codec: audio can use truncated packets\n");
	// we send only complete frames
	// audio_decoder->AudioCtx->flags |= CODEC_FLAG_TRUNCATED;
    }
    audio_decoder->SampleRate = 0;
    audio_decoder->Channels = 0;
    audio_decoder->HwSampleRate = 0;
    audio_decoder->HwChannels = 0;
    audio_decoder->LastDelay = 0;
}

/**
**	Close audio decoder.
**
**	@param audio_decoder	private audio decoder
*/
void CodecAudioClose(AudioDecoder * audio_decoder)
{
    // FIXME: output any buffered data
    if (audio_decoder->AvResample) {
	int ch;

	av_resample_close(audio_decoder->AvResample);
	audio_decoder->AvResample = NULL;
	audio_decoder->RemainCount = 0;
	audio_decoder->BufferSize = 0;
	audio_decoder->RemainSize = 0;
	for (ch = 0; ch < MAX_CHANNELS; ++ch) {
	    free(audio_decoder->Buffer[ch]);
	    audio_decoder->Buffer[ch] = NULL;
	    free(audio_decoder->Remain[ch]);
	    audio_decoder->Remain[ch] = NULL;
	}
    }
    if (audio_decoder->ReSample) {
	audio_resample_close(audio_decoder->ReSample);
	audio_decoder->ReSample = NULL;
    }
    if (audio_decoder->AudioCtx) {
	pthread_mutex_lock(&CodecLockMutex);
	avcodec_close(audio_decoder->AudioCtx);
	pthread_mutex_unlock(&CodecLockMutex);
	av_freep(&audio_decoder->AudioCtx);
    }
}

/**
**	Set audio drift correction.
**
**	@param mask	enable mask (PCM, AC3)
*/
void CodecSetAudioDrift(int mask)
{
#ifdef USE_AUDIO_DRIFT_CORRECTION
    CodecAudioDrift = mask & 3;
#endif
    (void)mask;
}

/**
**	Set audio pass-through.
**
**	@param mask	enable mask (PCM, AC3)
*/
void CodecSetAudioPassthrough(int mask)
{
#ifdef USE_PASSTHROUGH
    CodecPassthroughAC3 = mask & 1 ? 1 : 0;
#endif
    (void)mask;
}

/**
**	Set audio downmix.
**
**	@param onoff	enable/disable downmix.
*/
void CodecSetAudioDownmix(int onoff)
{
    if (onoff == -1) {
	CodecDownmix ^= 1;
	return;
    }
    CodecDownmix = onoff;
}

/**
**	Reorder audio frame.
**
**	ffmpeg L  R  C	Ls Rs		-> alsa L R  Ls Rs C
**	ffmpeg L  R  C	LFE Ls Rs	-> alsa L R  Ls Rs C  LFE
**	ffmpeg L  R  C	LFE Ls Rs Rl Rr	-> alsa L R  Ls Rs C  LFE Rl Rr
**
**	@param buf[IN,OUT]	sample buffer
**	@param size		size of sample buffer in bytes
**	@param channels		number of channels interleaved in sample buffer
*/
static void CodecReorderAudioFrame(int16_t * buf, int size, int channels)
{
    int i;
    int c;
    int ls;
    int rs;
    int lfe;

    switch (channels) {
	case 5:
	    size /= 2;
	    for (i = 0; i < size; i += 5) {
		c = buf[i + 2];
		ls = buf[i + 3];
		rs = buf[i + 4];
		buf[i + 2] = ls;
		buf[i + 3] = rs;
		buf[i + 4] = c;
	    }
	    break;
	case 6:
	    size /= 2;
	    for (i = 0; i < size; i += 6) {
		c = buf[i + 2];
		lfe = buf[i + 3];
		ls = buf[i + 4];
		rs = buf[i + 5];
		buf[i + 2] = ls;
		buf[i + 3] = rs;
		buf[i + 4] = c;
		buf[i + 5] = lfe;
	    }
	    break;
	case 8:
	    size /= 2;
	    for (i = 0; i < size; i += 8) {
		c = buf[i + 2];
		lfe = buf[i + 3];
		ls = buf[i + 4];
		rs = buf[i + 5];
		buf[i + 2] = ls;
		buf[i + 3] = rs;
		buf[i + 4] = c;
		buf[i + 5] = lfe;
	    }
	    break;
    }
}

/**
**	Set/update audio pts clock.
**
**	@param audio_decoder	audio decoder data
**	@param pts		presentation timestamp
*/
static void CodecAudioSetClock(AudioDecoder * audio_decoder, int64_t pts)
{
    struct timespec nowtime;
    int64_t delay;
    int64_t tim_diff;
    int64_t pts_diff;
    int drift;
    int corr;

    AudioSetClock(pts);

    delay = AudioGetDelay();
    if (!delay) {
	return;
    }
    clock_gettime(CLOCK_REALTIME, &nowtime);
    if (!audio_decoder->LastDelay) {
	audio_decoder->LastTime = nowtime;
	audio_decoder->LastPTS = pts;
	audio_decoder->LastDelay = delay;
	audio_decoder->Drift = 0;
	audio_decoder->DriftFrac = 0;
	Debug(3, "codec/audio: inital delay %" PRId64 "ms\n", delay / 90);
	return;
    }
    // collect over some time
    pts_diff = pts - audio_decoder->LastPTS;
    if (pts_diff < 10 * 1000 * 90) {
	return;
    }

    tim_diff = (nowtime.tv_sec - audio_decoder->LastTime.tv_sec)
	* 1000 * 1000 * 1000 + (nowtime.tv_nsec -
	audio_decoder->LastTime.tv_nsec);

    drift =
	(tim_diff * 90) / (1000 * 1000) - pts_diff + delay -
	audio_decoder->LastDelay;

    // adjust rounding error
    nowtime.tv_nsec -= nowtime.tv_nsec % (1000 * 1000 / 90);
    audio_decoder->LastTime = nowtime;
    audio_decoder->LastPTS = pts;
    audio_decoder->LastDelay = delay;

    if (0) {
	Debug(3,
	    "codec/audio: interval P:%5" PRId64 "ms T:%5" PRId64 "ms D:%4"
	    PRId64 "ms %f %d\n", pts_diff / 90, tim_diff / (1000 * 1000),
	    delay / 90, drift / 90.0, audio_decoder->DriftCorr);
    }
    // underruns and av_resample have the same time :(((
    if (abs(drift) > 10 * 90) {
	// drift too big, pts changed?
	Debug(3, "codec/audio: drift(%6d) %3dms reset\n",
	    audio_decoder->DriftCorr, drift / 90);
	audio_decoder->LastDelay = 0;
#ifdef DEBUG
	corr = 0;			// keep gcc happy
#endif
    } else {

	drift += audio_decoder->Drift;
	audio_decoder->Drift = drift;
	corr = (10 * audio_decoder->HwSampleRate * drift) / (90 * 1000);
	// SPDIF/HDMI passthrough
	if ((CodecAudioDrift & 2) && (!CodecPassthroughAC3
		|| audio_decoder->AudioCtx->codec_id != CODEC_ID_AC3)) {
	    audio_decoder->DriftCorr = -corr;
	}

	if (audio_decoder->DriftCorr < -20000) {	// limit correction
	    audio_decoder->DriftCorr = -20000;
	} else if (audio_decoder->DriftCorr > 20000) {
	    audio_decoder->DriftCorr = 20000;
	}
    }
    // FIXME: this works with libav 0.8, and only with >10ms with ffmpeg 0.10
    if (audio_decoder->AvResample && audio_decoder->DriftCorr) {
	int distance;

	// try workaround for buggy ffmpeg 0.10
	if (abs(audio_decoder->DriftCorr) < 2000) {
	    distance = (pts_diff * audio_decoder->HwSampleRate) / (900 * 1000);
	} else {
	    distance = (pts_diff * audio_decoder->HwSampleRate) / (90 * 1000);
	}
	av_resample_compensate(audio_decoder->AvResample,
	    audio_decoder->DriftCorr / 10, distance);
    }
    if (1) {
	static int c;

	if (!(c++ % 10)) {
	    Debug(3, "codec/audio: drift(%6d) %8dus %5d\n",
		audio_decoder->DriftCorr, drift * 1000 / 90, corr);
	}
    }
}

/**
**	Handle audio format changes.
**
**	@param audio_decoder	audio decoder data
*/
static void CodecAudioUpdateFormat(AudioDecoder * audio_decoder)
{
    const AVCodecContext *audio_ctx;
    int err;
    int isAC3;

    // FIXME: use swr_convert from swresample (only in ffmpeg!)
    if (audio_decoder->ReSample) {
	audio_resample_close(audio_decoder->ReSample);
	audio_decoder->ReSample = NULL;
    }
    if (audio_decoder->AvResample) {
	av_resample_close(audio_decoder->AvResample);
	audio_decoder->AvResample = NULL;
	audio_decoder->RemainCount = 0;
    }

    audio_ctx = audio_decoder->AudioCtx;
    Debug(3, "codec/audio: format change %dHz %d channels %s\n",
	audio_ctx->sample_rate, audio_ctx->channels,
	CodecPassthroughAC3 ? "pass-through" : "");

    audio_decoder->SampleRate = audio_ctx->sample_rate;
    audio_decoder->HwSampleRate = audio_ctx->sample_rate;
    audio_decoder->Channels = audio_ctx->channels;
    audio_decoder->PassthroughAC3 = CodecPassthroughAC3;

    // SPDIF/HDMI passthrough
    if (CodecPassthroughAC3 && audio_ctx->codec_id == CODEC_ID_AC3) {
	audio_decoder->HwChannels = 2;
	isAC3 = 1;
    } else {
	audio_decoder->HwChannels = audio_ctx->channels;
	isAC3 = 0;
    }

    // channels not support?
    if ((err =
	    AudioSetup(&audio_decoder->HwSampleRate,
		&audio_decoder->HwChannels, isAC3))) {
	Debug(3, "codec/audio: resample %dHz *%d -> %dHz *%d\n",
	    audio_ctx->sample_rate, audio_ctx->channels,
	    audio_decoder->HwSampleRate, audio_decoder->HwChannels);

	if (err == 1) {
	    audio_decoder->ReSample =
		av_audio_resample_init(audio_decoder->HwChannels,
		audio_ctx->channels, audio_decoder->HwSampleRate,
		audio_ctx->sample_rate, audio_ctx->sample_fmt,
		audio_ctx->sample_fmt, 16, 10, 0, 0.8);
	    // libav-0.8_pre didn't support 6 -> 2 channels
	    if (!audio_decoder->ReSample) {
		Error(_("codec/audio: resample setup error\n"));
		audio_decoder->HwChannels = 0;
		audio_decoder->HwSampleRate = 0;
		return;
	    }
	} else {
	    Debug(3, "codec/audio: audio setup error\n");
	    // FIXME: handle errors
	    audio_decoder->HwChannels = 0;
	    audio_decoder->HwSampleRate = 0;
	    return;
	}
    }
    // prepare audio drift resample
#ifdef USE_AUDIO_DRIFT_CORRECTION
    if ((CodecAudioDrift & 1) && !isAC3) {
	if (audio_decoder->AvResample) {
	    Error(_("codec/audio: overwrite resample\n"));
	}
	audio_decoder->AvResample =
	    av_resample_init(audio_decoder->HwSampleRate,
	    audio_decoder->HwSampleRate, 16, 10, 0, 0.8);
	if (!audio_decoder->AvResample) {
	    Error(_("codec/audio: AvResample setup error\n"));
	} else {
	    // reset drift to some default value
	    audio_decoder->DriftCorr /= 2;
	    audio_decoder->DriftFrac = 0;
	    av_resample_compensate(audio_decoder->AvResample,
		audio_decoder->DriftCorr / 10,
		10 * audio_decoder->HwSampleRate);
	}
    }
#endif
}

/**
**	Codec enqueue audio samples.
**
**	@param audio_decoder	audio decoder data
**	@param data		samples data
**	@param count		number of bytes in sample data
*/
void CodecAudioEnqueue(AudioDecoder * audio_decoder, int16_t * data, int count)
{
#ifdef USE_AUDIO_DRIFT_CORRECTION
    if ((CodecAudioDrift & 1) && audio_decoder->AvResample) {
	int16_t buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 4 +
	    FF_INPUT_BUFFER_PADDING_SIZE] __attribute__ ((aligned(16)));
	int16_t buftmp[MAX_CHANNELS][(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 4];
	int consumed;
	int i;
	int n;
	int ch;
	int bytes_n;

	bytes_n = count / audio_decoder->HwChannels;
	// resize sample buffer, if needed
	if (audio_decoder->RemainCount + bytes_n > audio_decoder->BufferSize) {
	    audio_decoder->BufferSize = audio_decoder->RemainCount + bytes_n;
	    for (ch = 0; ch < MAX_CHANNELS; ++ch) {
		audio_decoder->Buffer[ch] =
		    realloc(audio_decoder->Buffer[ch],
		    audio_decoder->BufferSize);
	    }
	}
	// copy remaining bytes into sample buffer
	for (ch = 0; ch < audio_decoder->HwChannels; ++ch) {
	    memcpy(audio_decoder->Buffer[ch], audio_decoder->Remain[ch],
		audio_decoder->RemainCount);
	}
	// deinterleave samples into sample buffer
	for (i = 0; i < bytes_n / 2; i++) {
	    for (ch = 0; ch < audio_decoder->HwChannels; ++ch) {
		audio_decoder->Buffer[ch][audio_decoder->RemainCount / 2 + i]
		    = data[i * audio_decoder->HwChannels + ch];
	    }
	}

	bytes_n += audio_decoder->RemainSize;
	n = 0;				// keep gcc lucky
	// resample the sample buffer into tmp buffer
	for (ch = 0; ch < audio_decoder->HwChannels; ++ch) {
	    n = av_resample(audio_decoder->AvResample, buftmp[ch],
		audio_decoder->Buffer[ch], &consumed, bytes_n / 2,
		sizeof(buftmp[ch]) / 2, ch == audio_decoder->HwChannels - 1);
	    // fixme remaining channels
	    if (bytes_n - consumed * 2 > audio_decoder->RemainSize) {
		audio_decoder->RemainSize = bytes_n - consumed * 2;
	    }
	    audio_decoder->Remain[ch] =
		realloc(audio_decoder->Remain[ch], audio_decoder->RemainSize);
	    memcpy(audio_decoder->Remain[ch],
		audio_decoder->Buffer[ch] + consumed,
		audio_decoder->RemainSize);
	    audio_decoder->RemainCount = audio_decoder->RemainSize;
	}

	// interleave samples from sample buffer
	for (i = 0; i < n; i++) {
	    for (ch = 0; ch < audio_decoder->HwChannels; ++ch) {
		buf[i * audio_decoder->HwChannels + ch] = buftmp[ch][i];
	    }
	}
	n *= 2;

	n *= audio_decoder->HwChannels;
	CodecReorderAudioFrame(buf, n, audio_decoder->HwChannels);
	AudioEnqueue(buf, n);
	return;
    }
#endif
    CodecReorderAudioFrame(data, count, audio_decoder->HwChannels);
    AudioEnqueue(data, count);
}

/**
**	Decode an audio packet.
**
**	PTS must be handled self.
**
**	@param audio_decoder	audio decoder data
**	@param avpkt		audio packet
*/
void CodecAudioDecode(AudioDecoder * audio_decoder, const AVPacket * avpkt)
{
    int16_t buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 4 +
	FF_INPUT_BUFFER_PADDING_SIZE] __attribute__ ((aligned(16)));
    int buf_sz;
    int l;
    AVCodecContext *audio_ctx;

    audio_ctx = audio_decoder->AudioCtx;

    buf_sz = sizeof(buf);
    l = avcodec_decode_audio3(audio_ctx, buf, &buf_sz, (AVPacket *) avpkt);
    if (avpkt->size != l) {
	if (l == AVERROR(EAGAIN)) {
	    Error(_("codec: latm\n"));
	    return;
	}
	if (l < 0) {			// no audio frame could be decompressed
	    Error(_("codec: error audio data\n"));
	    return;
	}
	Error(_("codec: error more than one frame data\n"));
    }
#ifdef notyetFF_API_OLD_DECODE_AUDIO
    // FIXME: ffmpeg git comeing
    int got_frame;

    avcodec_decode_audio4(audio_ctx, frame, &got_frame, avpkt);
#else
#endif

    // update audio clock
    if (avpkt->pts != (int64_t) AV_NOPTS_VALUE) {
	CodecAudioSetClock(audio_decoder, avpkt->pts);
    }
    // FIXME: must first play remainings bytes, than change and play new.
    if (audio_decoder->PassthroughAC3 != CodecPassthroughAC3
	|| audio_decoder->SampleRate != audio_ctx->sample_rate
	|| audio_decoder->Channels != audio_ctx->channels) {
	CodecAudioUpdateFormat(audio_decoder);
    }

    if (audio_decoder->HwSampleRate && audio_decoder->HwChannels) {
	// need to resample audio
	if (audio_decoder->ReSample) {
	    int16_t outbuf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 4 +
		FF_INPUT_BUFFER_PADDING_SIZE]
		__attribute__ ((aligned(16)));
	    int outlen;

	    // FIXME: libav-0.7.2 crash here
	    outlen =
		audio_resample(audio_decoder->ReSample, outbuf, buf, buf_sz);
#ifdef DEBUG
	    if (outlen != buf_sz) {
		Debug(3, "codec/audio: possible fixed ffmpeg\n");
	    }
#endif
	    if (outlen) {
		// outlen seems to be wrong in ffmpeg-0.9
		outlen /= audio_decoder->Channels *
		    av_get_bytes_per_sample(audio_ctx->sample_fmt);
		outlen *=
		    audio_decoder->HwChannels *
		    av_get_bytes_per_sample(audio_ctx->sample_fmt);
		Debug(4, "codec/audio: %d -> %d\n", buf_sz, outlen);
		CodecAudioEnqueue(audio_decoder, outbuf, outlen);
	    }
	} else {
#ifdef USE_PASSTHROUGH
	    // SPDIF/HDMI passthrough
	    if (CodecPassthroughAC3 && audio_ctx->codec_id == CODEC_ID_AC3) {
		// build SPDIF header and append A52 audio to it
		// avpkt is the original data
		buf_sz = 6144;

#ifdef USE_AC3_DRIFT_CORRECTION
		if (CodecAudioDrift & 2) {
		    int x;

		    x = (audio_decoder->DriftFrac +
			(audio_decoder->DriftCorr * buf_sz)) / (10 *
			audio_decoder->HwSampleRate * 100);
		    audio_decoder->DriftFrac =
			(audio_decoder->DriftFrac +
			(audio_decoder->DriftCorr * buf_sz)) % (10 *
			audio_decoder->HwSampleRate * 100);
		    x *= audio_decoder->HwChannels * 4;
		    if (x < -64) {	// limit correction
			x = -64;
		    } else if (x > 64) {
			x = 64;
		    }
		    buf_sz += x;
		}
#endif
		if (buf_sz < avpkt->size + 8) {
		    Error(_
			("codec/audio: decoded data smaller than encoded\n"));
		    return;
		}
		// copy original data for output
		// FIXME: not 100% sure, if endian is correct
		buf[0] = htole16(0xF872);	// iec 61937 sync word
		buf[1] = htole16(0x4E1F);
		buf[2] = htole16(0x01 | (avpkt->data[5] & 0x07) << 8);
		buf[3] = htole16(avpkt->size * 8);
		swab(avpkt->data, buf + 4, avpkt->size);
		memset(buf + 4 + avpkt->size / 2, 0, buf_sz - 8 - avpkt->size);
		// don't play with the ac-3 samples
		AudioEnqueue(buf, buf_sz);
		return;
	    }
#if 0
	    //
	    //	old experimental code
	    //
	    if (1) {
		// FIXME: need to detect dts
		// copy original data for output
		// FIXME: buf is sint
		buf[0] = 0x72;
		buf[1] = 0xF8;
		buf[2] = 0x1F;
		buf[3] = 0x4E;
		buf[4] = 0x00;
		switch (avpkt->size) {
		    case 512:
			buf[5] = 0x0B;
			break;
		    case 1024:
			buf[5] = 0x0C;
			break;
		    case 2048:
			buf[5] = 0x0D;
			break;
		    default:
			Debug(3,
			    "codec/audio: dts sample burst not supported\n");
			buf[5] = 0x00;
			break;
		}
		buf[6] = (avpkt->size * 8);
		buf[7] = (avpkt->size * 8) >> 8;
		//buf[8] = 0x0B;
		//buf[9] = 0x77;
		//printf("%x %x\n", avpkt->data[0],avpkt->data[1]);
		// swab?
		memcpy(buf + 8, avpkt->data, avpkt->size);
		memset(buf + 8 + avpkt->size, 0, buf_sz - 8 - avpkt->size);
	    } else if (1) {
		// FIXME: need to detect mp2
		// FIXME: mp2 passthrough
		// see softhddev.c version/layer
		// 0x04 mpeg1 layer1
		// 0x05 mpeg1 layer23
		// 0x06 mpeg2 ext
		// 0x07 mpeg2.5 layer 1
		// 0x08 mpeg2.5 layer 2
		// 0x09 mpeg2.5 layer 3
	    }
	    // DTS HD?
	    // True HD?
#endif
#endif
	    CodecAudioEnqueue(audio_decoder, buf, buf_sz);
	}
    }
}

/**
**	Flush the audio decoder.
**
**	@param decoder	audio decoder data
*/
void CodecAudioFlushBuffers(AudioDecoder * decoder)
{
    avcodec_flush_buffers(decoder->AudioCtx);
}

//----------------------------------------------------------------------------
//	Codec
//----------------------------------------------------------------------------

/**
**	Empty log callback
*/
static void CodecNoopCallback( __attribute__ ((unused))
    void *ptr, __attribute__ ((unused))
    int level, __attribute__ ((unused))
    const char *fmt, __attribute__ ((unused)) va_list vl)
{
}

/**
**	Codec init
*/
void CodecInit(void)
{
    pthread_mutex_init(&CodecLockMutex, NULL);
#ifndef DEBUG
    // disable display ffmpeg error messages
    av_log_set_callback(CodecNoopCallback);
#else
    (void)CodecNoopCallback;
#endif
    avcodec_register_all();		// register all formats and codecs
}

/**
**	Codec exit.
*/
void CodecExit(void)
{
    pthread_mutex_destroy(&CodecLockMutex);
}
