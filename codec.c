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

    /**
    **	use av_parser to support insane dvb audio streams.
    */
#define USE_AVPARSER

    /// compile with passthrough support (experimental)
#define USE_PASSTHROUGH

#include <stdio.h>
#include <unistd.h>
#include <endian.h>

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

    Debug(3, "codec: using codec %s or ID %#04x\n", name, codec_id);

    if (decoder->VideoCtx) {
	Error(_("codec: missing close\n"));
    }
    //
    //	ffmpeg compatibility hack
    //
#if 1 || (LIBAVCODEC_VERSION_INT <= AV_VERSION_INT(52,96,0))
    if (name) {
	if (!strcmp(name, "h264video_vdpau")) {
	    name = "h264_vdpau";
	} else if (!strcmp(name, "mpeg4video_vdpau")) {
	    name = "mpeg4_vdpau";
	} else if (!strcmp(name, "vc1video_vdpau")) {
	    name = "vc1_vdpau";
	} else if (!strcmp(name, "wmv3video_vdpau")) {
	    name = "wmv3_vdpau";
	}
    }
#endif

    if (name && (video_codec = avcodec_find_decoder_by_name(name))) {
	Debug(3, "codec: vdpau decoder found\n");
    } else if (!(video_codec = avcodec_find_decoder(codec_id))) {
	Fatal(_("codec: codec ID %#04x not found\n"), codec_id);
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
	// we do not send complete frames
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

    if (got_frame) {			// frame completed
	//DisplayPts(video_ctx, frame);
	VideoRenderFrame(decoder->HwDecoder, video_ctx, frame);
    } else {
	// some frames are needed for references, interlaced frames ...
	// could happen with h264 dvb streams, just drop data.

	Debug(4, "codec: %8d incomplete interlaced frame %d bytes used\n",
	    video_ctx->frame_number, used);
    }
    if (used != pkt->size) {
	if (used >= 0 && used < pkt->size) {
	    // some tv channels, produce this
	    Debug(4,
		"codec: ooops didn't use complete video packet used %d of %d\n",
		used, pkt->size);
	    pkt->size -= used;
	    pkt->data += used;
	    goto next_part;
	}
	Debug(3, "codec: bad frame %d\n", used);
    }
}

/**
**	Flush the video decoder.
**
**	@param decoder	video decoder data
*/
void CodecVideoFlushBuffers(VideoDecoder * decoder)
{
    avcodec_flush_buffers(decoder->VideoCtx);
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

    /// audio parser to support insane dvb streaks
    AVCodecParserContext *AudioParser;
    int SampleRate;			///< current stream sample rate
    int Channels;			///< current stream channels

    int HwSampleRate;			///< hw sample rate
    int HwChannels;			///< hw channels

    ReSampleContext *ReSample;		///< audio resampling context

};

#ifdef USE_PASSTHROUGH
//static char CodecPassthroughPCM;	///< pass pcm through (unsupported)
static char CodecPassthroughAC3;	///< pass ac3 through

//static char CodecPassthroughDTS;	///< pass dts through (unsupported)
//static char CodecPassthroughMPA;	///< pass mpa through (unsupported)
#endif

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

    if (name && (audio_codec = avcodec_find_decoder_by_name(name))) {
	Debug(3, "codec: audio decoder '%s' found\n", name);
    } else if (!(audio_codec = avcodec_find_decoder(codec_id))) {
	Fatal(_("codec: codec ID %#04x not found\n"), codec_id);
	// FIXME: errors aren't fatal
    }
    audio_decoder->AudioCodec = audio_codec;

    if (!(audio_decoder->AudioCtx = avcodec_alloc_context3(audio_codec))) {
	Fatal(_("codec: can't allocate audio codec context\n"));
    }
    pthread_mutex_lock(&CodecLockMutex);
    // open codec
#if LIBAVCODEC_VERSION_INT <= AV_VERSION_INT(53,5,0)
    if (avcodec_open(audio_decoder->AudioCtx, audio_codec) < 0) {
	pthread_mutex_unlock(&CodecLockMutex);
	Fatal(_("codec: can't open audio codec\n"));
    }
#else
    if (avcodec_open2(audio_decoder->AudioCtx, audio_codec, NULL) < 0) {
	pthread_mutex_unlock(&CodecLockMutex);
	Fatal(_("codec: can't open audio codec\n"));
    }
#endif
    pthread_mutex_unlock(&CodecLockMutex);
    Debug(3, "codec: audio '%s'\n", audio_decoder->AudioCtx->codec_name);

    if (audio_codec->capabilities & CODEC_CAP_TRUNCATED) {
	Debug(3, "codec: audio can use truncated packets\n");
	// we do not send complete frames
	audio_decoder->AudioCtx->flags |= CODEC_FLAG_TRUNCATED;
    }
    if (!(audio_decoder->AudioParser =
	    av_parser_init(audio_decoder->AudioCtx->codec_id))) {
	Fatal(_("codec: can't init audio parser\n"));
    }
    audio_decoder->SampleRate = 0;
    audio_decoder->Channels = 0;
    audio_decoder->HwSampleRate = 0;
    audio_decoder->HwChannels = 0;
}

/**
**	Close audio decoder.
**
**	@param audio_decoder	private audio decoder
*/
void CodecAudioClose(AudioDecoder * audio_decoder)
{
    // FIXME: output any buffered data
    if (audio_decoder->ReSample) {
	audio_resample_close(audio_decoder->ReSample);
	audio_decoder->ReSample = NULL;
    }
    if (audio_decoder->AudioParser) {
	av_parser_close(audio_decoder->AudioParser);
	audio_decoder->AudioParser = NULL;
    }
    if (audio_decoder->AudioCtx) {
	pthread_mutex_lock(&CodecLockMutex);
	avcodec_close(audio_decoder->AudioCtx);
	pthread_mutex_unlock(&CodecLockMutex);
	av_freep(&audio_decoder->AudioCtx);
    }
}

/**
**	Set audio pass-through.
*/
void CodecSetAudioPassthrough(int mask)
{
#ifdef USE_PASSTHROUGH
    CodecPassthroughAC3 = mask & 1 ? 1 : 0;
#endif
    // FIXME: must update audio decoder (nr. of channels wrong)
    (void)mask;
}

#ifdef USE_AVPARSER

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
    AVCodecContext *audio_ctx;
    int index;

//#define spkt avpkt
#if 1					// didn't fix crash in av_parser_parse2
    AVPacket spkt[1];

    // av_new_packet reserves FF_INPUT_BUFFER_PADDING_SIZE and clears it
    if (av_new_packet(spkt, avpkt->size)) {
	Error(_("codec: out of memory\n"));
	return;
    }
    memcpy(spkt->data, avpkt->data, avpkt->size);
    spkt->pts = avpkt->pts;
    spkt->dts = avpkt->dts;
#endif
#ifdef DEBUG
    if (!audio_decoder->AudioParser) {
	Fatal(_("codec: internal error parser freeded while running\n"));
    }
#endif

    audio_ctx = audio_decoder->AudioCtx;
    index = 0;
    while (spkt->size > index) {
	int n;
	int l;
	AVPacket dpkt[1];

	av_init_packet(dpkt);
	n = av_parser_parse2(audio_decoder->AudioParser, audio_ctx,
	    &dpkt->data, &dpkt->size, spkt->data + index, spkt->size - index,
	    !index ? (uint64_t) spkt->pts : AV_NOPTS_VALUE,
	    !index ? (uint64_t) spkt->dts : AV_NOPTS_VALUE, -1);

	// FIXME: make this a function for both #ifdef cases
	if (dpkt->size) {
	    int buf_sz;

	    dpkt->pts = audio_decoder->AudioParser->pts;
	    dpkt->dts = audio_decoder->AudioParser->dts;
	    buf_sz = sizeof(buf);
	    l = avcodec_decode_audio3(audio_ctx, buf, &buf_sz, dpkt);
	    if (l < 0) {		// no audio frame could be decompressed
		Error(_("codec: error audio data at %d\n"), index);
		break;
	    }
#ifdef notyetFF_API_OLD_DECODE_AUDIO
	    // FIXME: ffmpeg git comeing
	    int got_frame;

	    avcodec_decode_audio4(audio_ctx, frame, &got_frame, dpkt);
#else
#endif
	    // Update audio clock
	    if ((uint64_t) dpkt->pts != AV_NOPTS_VALUE) {
		AudioSetClock(dpkt->pts);
	    }
	    // FIXME: must first play remainings bytes, than change and play new.
	    if (audio_decoder->SampleRate != audio_ctx->sample_rate
		|| audio_decoder->Channels != audio_ctx->channels) {
		int err;
		int isAC3;

		if (audio_decoder->ReSample) {
		    audio_resample_close(audio_decoder->ReSample);
		    audio_decoder->ReSample = NULL;
		}

		audio_decoder->SampleRate = audio_ctx->sample_rate;
		audio_decoder->HwSampleRate = audio_ctx->sample_rate;
		audio_decoder->Channels = audio_ctx->channels;
#ifdef USE_PASSTHROUGH
		// SPDIF/HDMI passthrough
		if (CodecPassthroughAC3 && audio_ctx->codec_id == CODEC_ID_AC3) {
		    audio_decoder->HwChannels = 2;
		    isAC3 = 1;
		} else
#endif
		{
		    audio_decoder->HwChannels = audio_ctx->channels;
		    isAC3 = 0;
		}

		// channels not support?
		if ((err =
			AudioSetup(&audio_decoder->HwSampleRate,
			    &audio_decoder->HwChannels, isAC3))) {
		    Debug(3, "codec/audio: resample %dHz *%d -> %dHz *%d\n",
			audio_ctx->sample_rate, audio_ctx->channels,
			audio_decoder->HwSampleRate,
			audio_decoder->HwChannels);

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
			}
		    } else {
			Debug(3, "codec/audio: audio setup error\n");
			// FIXME: handle errors
			audio_decoder->HwChannels = 0;
			audio_decoder->HwSampleRate = 0;
			break;
		    }
		}
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
			audio_resample(audio_decoder->ReSample, outbuf, buf,
			buf_sz);
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
			AudioEnqueue(outbuf, outlen);
		    }
		} else {
#ifdef USE_PASSTHROUGH
		    // SPDIF/HDMI passthrough
		    if (CodecPassthroughAC3
			&& audio_ctx->codec_id == CODEC_ID_AC3) {
			// build SPDIF header and append A52 audio to it
			// dpkt is the original data
			buf_sz = 6144;
			if (buf_sz < dpkt->size + 8) {
			    Error(_
				("codec/audio: decoded data smaller than encoded\n"));
			    break;
			}
			// copy original data for output
			// FIXME: not 100% sure, if endian is correct
			buf[0] = htole16(0xF872);	// iec 61937 sync word
			buf[1] = htole16(0x4E1F);
			buf[2] = htole16(0x01 | (dpkt->data[5] & 0x07) << 8);
			buf[3] = htole16(dpkt->size * 8);
			swab(dpkt->data, buf + 4, dpkt->size);
			memset(buf + 4 + dpkt->size / 2, 0,
			    buf_sz - 8 - dpkt->size);
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
			switch (dpkt->size) {
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
			buf[6] = (dpkt->size * 8);
			buf[7] = (dpkt->size * 8) >> 8;
			//buf[8] = 0x0B;
			//buf[9] = 0x77;
			//printf("%x %x\n", dpkt->data[0],dpkt->data[1]);
			// swab?
			memcpy(buf + 8, dpkt->data, dpkt->size);
			memset(buf + 8 + dpkt->size, 0,
			    buf_sz - 8 - dpkt->size);
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
		    AudioEnqueue(buf, buf_sz);
		}
	    }

	    if (dpkt->size > l) {
		Error(_("codec: error more than one frame data\n"));
	    }
	}

	index += n;
    }

#if 1
    // or av_free_packet, make no difference here
    av_destruct_packet(spkt);
#endif
}

#else

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
    AVCodecContext *audio_ctx;
    int index;

//#define spkt avpkt
#if 1
    AVPacket spkt[1];

    // av_new_packet reserves FF_INPUT_BUFFER_PADDING_SIZE and clears it
    if (av_new_packet(spkt, avpkt->size)) {
	Error(_("codec: out of memory\n"));
	return;
    }
    memcpy(spkt->data, avpkt->data, avpkt->size);
    spkt->pts = avpkt->pts;
    spkt->dts = avpkt->dts;
#endif
    audio_ctx = audio_decoder->AudioCtx;
    index = 0;
    while (spkt->size > index) {
	int n;
	int buf_sz;
	AVPacket dpkt[1];

	av_init_packet(dpkt);
	dpkt->data = spkt->data + index;
	dpkt->size = spkt->size - index;

	buf_sz = sizeof(buf);
	n = avcodec_decode_audio3(audio_ctx, buf, &buf_sz, dpkt);
	if (n < 0) {			// no audio frame could be decompressed
	    Error(_("codec: error audio data at %d\n"), index);
	    break;
	}
#ifdef DEBUG
	Debug(4, "codec/audio: -> %d\n", buf_sz);
	if ((unsigned)buf_sz > sizeof(buf)) {
	    abort();
	}
#endif
#ifdef notyetFF_API_OLD_DECODE_AUDIO
	// FIXME: ffmpeg git comeing
	int got_frame;

	avcodec_decode_audio4(audio_ctx, frame, &got_frame, dpkt);
#else
#endif
	// FIXME: see above, old code removed

	index += n;
    }

#if 1
    // or av_free_packet, make no difference here
    av_destruct_packet(spkt);
#endif
}

#endif

/**
**	Flush the audio decoder.
**
**	@param decoder	audio decoder data
*/
void CodecAudioFlushBuffers(AudioDecoder * decoder)
{
    // FIXME: reset audio parser
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
