/// Copyright (C) 2009 - 2015 by Johns. All Rights Reserved.
/// Copyright (C) 2018 by pesintta, rofafor.
///
/// SPDX-License-Identifier: AGPL-3.0-only

///
/// This module contains all decoder and codec functions.
/// It is uses ffmpeg (http://ffmpeg.org) as backend.
///
/// It may work with libav (http://libav.org), but the tests show
/// many bugs and incompatiblity in it.	 Don't use this shit.
///

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
// support old ffmpeg versions <1.0
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,18,102)
#define AVCodecID CodecID
#define AV_CODEC_ID_AC3 CODEC_ID_AC3
#define AV_CODEC_ID_EAC3 CODEC_ID_EAC3
#define AV_CODEC_ID_MPEG2VIDEO CODEC_ID_MPEG2VIDEO
#define AV_CODEC_ID_H264 CODEC_ID_H264
#endif
#include <libavcodec/vaapi.h>
#ifdef USE_SWRESAMPLE
#include <libswresample/swresample.h>
#endif
#ifdef USE_AVRESAMPLE
#include <libavresample/avresample.h>
#include <libavutil/opt.h>
#endif

#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>

#ifdef MAIN_H
#include MAIN_H
#endif
#include "iatomic.h"
#include "misc.h"
#include "video.h"
#include "audio.h"
#include "codec.h"

//----------------------------------------------------------------------------

    // correct is AV_VERSION_INT(56,35,101) but some gentoo i* think
    // they must change it.
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56,26,100)
    /// ffmpeg 2.6 started to show artifacts after channel switch
    /// to SDTV channels
#define FFMPEG_WORKAROUND_ARTIFACTS	1
#endif

//----------------------------------------------------------------------------
//  Global
//----------------------------------------------------------------------------

      ///
      ///   ffmpeg lock mutex
      ///
      ///   new ffmpeg dislikes simultanous open/close
      ///   this breaks our code, until this is fixed use lock.
      ///
static pthread_mutex_t CodecLockMutex;

    /// Flag prefer fast channel switch
char CodecUsePossibleDefectFrames;

//----------------------------------------------------------------------------
//  Video
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//  Call-backs
//----------------------------------------------------------------------------

/**
**	Callback to negotiate the PixelFormat.
**
**	@param video_ctx	codec context
**	@param fmt		is the list of formats which are supported by
**				the codec, it is terminated by -1 as 0 is a
**				valid format, the formats are ordered by
**				quality.
*/
static enum AVPixelFormat Codec_get_format(AVCodecContext * video_ctx, const enum AVPixelFormat *fmt)
{
    VideoDecoder *decoder;

    decoder = video_ctx->opaque;
#if LIBAVCODEC_VERSION_INT == AV_VERSION_INT(54,86,100)
    // this begins to stink, 1.1.2 calls get_format for each frame
    // 1.1.3 has the same version, but works again
    if (decoder->GetFormatDone) {
	if (decoder->GetFormatDone < 10) {
	    ++decoder->GetFormatDone;
	    Error("codec/video: ffmpeg/libav buggy: get_format called again\n");
	}
	return *fmt;			// FIXME: this is hack
    }
#endif

    // bug in ffmpeg 1.1.1, called with zero width or height
    if (!video_ctx->width || !video_ctx->height) {
	Error("codec/video: ffmpeg/libav buggy: width or height zero\n");
    }

    decoder->GetFormatDone = 1;
    return Video_get_format(decoder->HwDecoder, video_ctx, fmt);
}

static void Codec_free_buffer(void *opaque, uint8_t * data);

/**
**	Video buffer management, get buffer for frame.
**
**	Called at the beginning of each frame to get a buffer for it.
**
**	@param video_ctx	Codec context
**	@param frame		Get buffer for this frame
*/
static int Codec_get_buffer2(AVCodecContext * video_ctx, AVFrame * frame, int flags)
{
    VideoDecoder *decoder;

    decoder = video_ctx->opaque;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(54,86,100)
    // ffmpeg has this already fixed
    // libav 0.8.5 53.35.0 still needs this
#endif
    if (!decoder->GetFormatDone) {	// get_format missing
	enum AVPixelFormat fmts[2];

	fprintf(stderr, "codec: buggy libav, use ffmpeg\n");
	Warning(_("codec: buggy libav, use ffmpeg\n"));
	fmts[0] = video_ctx->pix_fmt;
	fmts[1] = AV_PIX_FMT_NONE;
	Codec_get_format(video_ctx, fmts);
    }
    // VA-API:
    if (video_ctx->hwaccel_context) {
	unsigned surface;

	surface = VideoGetSurface(decoder->HwDecoder, video_ctx);

	//Debug(3, "codec: use surface %#010x\n", surface);

#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(52,48,101)
	frame->type = FF_BUFFER_TYPE_USER;
#endif
#if LIBAVCODEC_VERSION_INT <= AV_VERSION_INT(53,46,0)
	frame->age = 256 * 256 * 256 * 64;
#endif
	// vaapi needs both fields set
	frame->buf[0] = av_buffer_create((uint8_t *) (size_t) surface, 0, Codec_free_buffer, video_ctx, 0);
	frame->data[0] = frame->buf[0]->data;
	frame->data[3] = frame->data[0];

#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(52,66,100)
	// reordered frames
	if (video_ctx->pkt) {
	    frame->pkt_pts = video_ctx->pkt->pts;
	} else {
	    frame->pkt_pts = AV_NOPTS_VALUE;
	}
#endif
	return 0;
    }
    //Debug(3, "codec: fallback to default get_buffer\n");
    return avcodec_default_get_buffer2(video_ctx, frame, flags);
}

/**
**	Video buffer management, release buffer for frame.
**	Called to release buffers which were allocated with get_buffer.
**
**	@param opaque	opaque data
**	@param data		buffer data
*/
static void Codec_free_buffer(void *opaque, uint8_t * data)
{
    AVCodecContext *video_ctx = (AVCodecContext *) opaque;

    // VA-API
    if (video_ctx->hwaccel_context) {
	VideoDecoder *decoder;
	unsigned surface;

	decoder = video_ctx->opaque;
	surface = (unsigned)(size_t) data;

	//Debug(3, "codec: release surface %#010x\n", surface);
	VideoReleaseSurface(decoder->HwDecoder, surface);

	return;
    }
}

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
**	@param codec_id	video codec id
*/
void CodecVideoOpen(VideoDecoder * decoder, int codec_id)
{
    AVCodec *video_codec;

    Debug(3, "codec: using video codec ID %#06x (%s)\n", codec_id, avcodec_get_name(codec_id));

    if (decoder->VideoCtx) {
	Error(_("codec: missing close\n"));
    }

    if (!(video_codec = avcodec_find_decoder(codec_id))) {
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
    if (video_codec->capabilities & (CODEC_CAP_HWACCEL)) {
	Debug(3, "codec: video mpeg hack active\n");
	// HACK around badly placed checks in mpeg_mc_decode_init
	// taken from mplayer vd_ffmpeg.c
	decoder->VideoCtx->slice_flags = SLICE_FLAG_CODED_ORDER | SLICE_FLAG_ALLOW_FIELD;
	decoder->VideoCtx->thread_count = 1;
	decoder->VideoCtx->active_thread_type = 0;
    }

    if (avcodec_open2(decoder->VideoCtx, video_codec, NULL) < 0) {
	pthread_mutex_unlock(&CodecLockMutex);
	Fatal(_("codec: can't open video codec!\n"));
    }
#endif
    pthread_mutex_unlock(&CodecLockMutex);

    decoder->VideoCtx->opaque = decoder;    // our structure

    Debug(3, "codec: video '%s'\n", decoder->VideoCodec->long_name);
    if (video_codec->capabilities & CODEC_CAP_TRUNCATED) {
	Debug(3, "codec: video can use truncated packets\n");
    }
    // FIXME: own memory management for video frames.
    if (video_codec->capabilities & CODEC_CAP_DR1) {
	Debug(3, "codec: can use own buffer management\n");
    }
#ifdef CODEC_CAP_FRAME_THREADS
    if (video_codec->capabilities & CODEC_CAP_FRAME_THREADS) {
	Debug(3, "codec: codec supports frame threads\n");
    }
#endif
    decoder->VideoCtx->get_format = Codec_get_format;
    decoder->VideoCtx->get_buffer2 = Codec_get_buffer2;
    decoder->VideoCtx->thread_count = 1;
    decoder->VideoCtx->active_thread_type = 0;
    decoder->VideoCtx->draw_horiz_band = NULL;
    decoder->VideoCtx->hwaccel_context = VideoGetHwAccelContext(decoder->HwDecoder);

    //
    //	Prepare frame buffer for decoder
    //
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56,28,1)
    if (!(decoder->Frame = av_frame_alloc())) {
	Fatal(_("codec: can't allocate video decoder frame buffer\n"));
    }
#else
    if (!(decoder->Frame = avcodec_alloc_frame())) {
	Fatal(_("codec: can't allocate video decoder frame buffer\n"));
    }
#endif
    // reset buggy ffmpeg/libav flag
    decoder->GetFormatDone = 0;
#ifdef FFMPEG_WORKAROUND_ARTIFACTS
    decoder->FirstKeyFrame = 1;
#endif
}

/**
**	Close video decoder.
**
**	@param video_decoder	private video decoder
*/
void CodecVideoClose(VideoDecoder * video_decoder)
{
    // FIXME: play buffered data
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56,28,1)
    av_frame_free(&video_decoder->Frame);   // callee does checks
#else
    av_freep(&video_decoder->Frame);
#endif

    if (video_decoder->VideoCtx) {
	pthread_mutex_lock(&CodecLockMutex);
	avcodec_close(video_decoder->VideoCtx);
	av_freep(&video_decoder->VideoCtx);
	pthread_mutex_unlock(&CodecLockMutex);
    }
}

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
    Debug(4, "%s: %p %d -> %d %d\n", __FUNCTION__, pkt->data, pkt->size, used, got_frame);

    if (used < 0) {
	Debug(3, "codec: bad video frame\n");
	return;
    }

    if (got_frame) {			// frame completed
#ifdef FFMPEG_WORKAROUND_ARTIFACTS
	if (!CodecUsePossibleDefectFrames && decoder->FirstKeyFrame) {
	    decoder->FirstKeyFrame++;
	    if (frame->key_frame) {
		Debug(3, "codec: key frame after %d frames\n", decoder->FirstKeyFrame);
		decoder->FirstKeyFrame = 0;
	    }
	} else {
	    //DisplayPts(video_ctx, frame);
	    VideoRenderFrame(decoder->HwDecoder, video_ctx, frame);
	}
#else
	//DisplayPts(video_ctx, frame);
	VideoRenderFrame(decoder->HwDecoder, video_ctx, frame);
#endif
    } else {
	// some frames are needed for references, interlaced frames ...
	// could happen with h264 dvb streams, just drop data.

	Debug(4, "codec: %8d incomplete interlaced frame %d bytes used\n", video_ctx->frame_number, used);
    }

    // old code to support truncated or multi frame packets
    if (used != pkt->size) {
	// ffmpeg 0.8.7 dislikes our seq_end_h264 and enters endless loop here
	if (used == 0 && pkt->size == 5 && pkt->data[4] == 0x0A) {
	    Warning("codec: ffmpeg 0.8.x workaround used\n");
	    return;
	}
	if (used >= 0 && used < pkt->size) {
	    // some tv channels, produce this
	    Debug(4, "codec: ooops didn't use complete video packet used %d of %d\n", used, pkt->size);
	    pkt->size -= used;
	    pkt->data += used;
	    // FIXME: align problem?
	    goto next_part;
	}
    }
    // new AVFrame API
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56,28,1)
    av_frame_unref(frame);
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
//  Audio
//----------------------------------------------------------------------------

///
/// Audio decoder structure.
///
struct _audio_decoder_
{
    AVCodec *AudioCodec;		///< audio codec
    AVCodecContext *AudioCtx;		///< audio codec context

    char Passthrough;			///< current pass-through flags
    int SampleRate;			///< current stream sample rate
    int Channels;			///< current stream channels

    int HwSampleRate;			///< hw sample rate
    int HwChannels;			///< hw channels

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56,28,1)
    AVFrame *Frame;			///< decoded audio frame buffer
#endif

#if !defined(USE_SWRESAMPLE) && !defined(USE_AVRESAMPLE)
    ReSampleContext *ReSample;		///< old resampling context
#endif
#ifdef USE_SWRESAMPLE
#if LIBSWRESAMPLE_VERSION_INT < AV_VERSION_INT(0, 15, 100)
    struct SwrContext *Resample;	///< ffmpeg software resample context
#else
    SwrContext *Resample;		///< ffmpeg software resample context
#endif
#endif
#ifdef USE_AVRESAMPLE
    AVAudioResampleContext *Resample;	///< libav software resample context
#endif

    uint16_t Spdif[24576 / 2];		///< SPDIF output buffer
    int SpdifIndex;			///< index into SPDIF output buffer
    int SpdifCount;			///< SPDIF repeat counter

    int64_t LastDelay;			///< last delay
    struct timespec LastTime;		///< last time
    int64_t LastPTS;			///< last PTS

    int Drift;				///< accumulated audio drift
    int DriftCorr;			///< audio drift correction value
    int DriftFrac;			///< audio drift fraction for ac3

#if !defined(USE_SWRESAMPLE) && !defined(USE_AVRESAMPLE)
    struct AVResampleContext *AvResample;   ///< second audio resample context
#define MAX_CHANNELS 8			///< max number of channels supported
    int16_t *Buffer[MAX_CHANNELS];	///< deinterleave sample buffers
    int BufferSize;			///< size of sample buffer
    int16_t *Remain[MAX_CHANNELS];	///< filter remaining samples
    int RemainSize;			///< size of remain buffer
    int RemainCount;			///< number of remaining samples
#endif
};

///
/// IEC Data type enumeration.
///
enum IEC61937
{
    IEC61937_AC3 = 0x01,		///< AC-3 data
    // FIXME: more data types
    IEC61937_EAC3 = 0x15,		///< E-AC-3 data
};

#define CORRECT_PCM	1		    ///< do PCM audio-drift correction
#define CORRECT_AC3	2		    ///< do AC-3 audio-drift correction
static char CodecAudioDrift;		///< flag: enable audio-drift correction

    ///
    /// Pass-through flags: CodecPCM, CodecAC3, CodecEAC3, ...
    ///
static char CodecPassthrough;
static char CodecDownmix;		///< enable AC-3 decoder downmix

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
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56,28,1)
    if (!(audio_decoder->Frame = av_frame_alloc())) {
	Fatal(_("codec: can't allocate audio decoder frame buffer\n"));
    }
#endif

    return audio_decoder;
}

/**
**	Deallocate an audio decoder context.
**
**	@param decoder	private audio decoder
*/
void CodecAudioDelDecoder(AudioDecoder * decoder)
{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56,28,1)
    av_frame_free(&decoder->Frame);	// callee does checks
#endif
    free(decoder);
}

/**
**	Open audio decoder.
**
**	@param audio_decoder	private audio decoder
**	@param codec_id	audio	codec id
*/
void CodecAudioOpen(AudioDecoder * audio_decoder, int codec_id)
{
    AVCodec *audio_codec;

    Debug(3, "codec: using audio codec ID %#06x (%s)\n", codec_id, avcodec_get_name(codec_id));

    if (!(audio_codec = avcodec_find_decoder(codec_id))) {
	Fatal(_("codec: codec ID %#06x not found\n"), codec_id);
	// FIXME: errors aren't fatal
    }
    audio_decoder->AudioCodec = audio_codec;

    if (!(audio_decoder->AudioCtx = avcodec_alloc_context3(audio_codec))) {
	Fatal(_("codec: can't allocate audio codec context\n"));
    }

    if (CodecDownmix) {
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(53,61,100)
	audio_decoder->AudioCtx->request_channels = 2;
#endif
	audio_decoder->AudioCtx->request_channel_layout = AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(53,61,100)
    // this has no effect (with ffmpeg and libav)
    // audio_decoder->AudioCtx->request_sample_fmt = AV_SAMPLE_FMT_S16;
#endif
    pthread_mutex_lock(&CodecLockMutex);
    // open codec
#if LIBAVCODEC_VERSION_INT <= AV_VERSION_INT(53,5,0)
    if (avcodec_open(audio_decoder->AudioCtx, audio_codec) < 0) {
	pthread_mutex_unlock(&CodecLockMutex);
	Fatal(_("codec: can't open audio codec\n"));
    }
#else
    {
	AVDictionary *av_dict = NULL;

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
    Debug(3, "codec: audio '%s'\n", audio_decoder->AudioCodec->long_name);

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
#if !defined(USE_SWRESAMPLE) && !defined(USE_AVRESAMPLE)
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
#endif
#ifdef USE_SWRESAMPLE
    if (audio_decoder->Resample) {
	swr_free(&audio_decoder->Resample);
    }
#endif
#ifdef USE_AVRESAMPLE
    if (audio_decoder->Resample) {
	avresample_free(&audio_decoder->Resample);
    }
#endif
    if (audio_decoder->AudioCtx) {
	pthread_mutex_lock(&CodecLockMutex);
	avcodec_close(audio_decoder->AudioCtx);
	av_freep(&audio_decoder->AudioCtx);
	pthread_mutex_unlock(&CodecLockMutex);
    }
}

/**
**	Set audio drift correction.
**
**	@param mask	enable mask (PCM, AC-3)
*/
void CodecSetAudioDrift(int mask)
{
    CodecAudioDrift = mask & (CORRECT_PCM | CORRECT_AC3);
}

/**
**	Set audio pass-through.
**
**	@param mask	enable mask (PCM, AC-3, E-AC-3)
*/
void CodecSetAudioPassthrough(int mask)
{
    CodecPassthrough = mask & (CodecPCM | CodecAC3 | CodecEAC3);
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
**	Handle audio format changes helper.
**
**	@param audio_decoder	audio decoder data
**	@param[out] passthrough	pass-through output
*/
static int CodecAudioUpdateHelper(AudioDecoder * audio_decoder, int *passthrough)
{
    const AVCodecContext *audio_ctx;
    int err;

    audio_ctx = audio_decoder->AudioCtx;
    Debug(3, "codec/audio: format change %s %dHz *%d channels%s%s%s%s%s\n",
	av_get_sample_fmt_name(audio_ctx->sample_fmt), audio_ctx->sample_rate, audio_ctx->channels,
	CodecPassthrough & CodecPCM ? " PCM" : "", CodecPassthrough & CodecMPA ? " MPA" : "",
	CodecPassthrough & CodecAC3 ? " AC-3" : "", CodecPassthrough & CodecEAC3 ? " E-AC-3" : "",
	CodecPassthrough ? " pass-through" : "");

    *passthrough = 0;
    audio_decoder->SampleRate = audio_ctx->sample_rate;
    audio_decoder->HwSampleRate = audio_ctx->sample_rate;
    audio_decoder->Channels = audio_ctx->channels;
    audio_decoder->HwChannels = audio_ctx->channels;
    audio_decoder->Passthrough = CodecPassthrough;

    // SPDIF/HDMI pass-through
    if ((CodecPassthrough & CodecAC3 && audio_ctx->codec_id == AV_CODEC_ID_AC3)
	|| (CodecPassthrough & CodecEAC3 && audio_ctx->codec_id == AV_CODEC_ID_EAC3)) {
	if (audio_ctx->codec_id == AV_CODEC_ID_EAC3) {
	    // E-AC-3 over HDMI some receivers need HBR
	    audio_decoder->HwSampleRate *= 4;
	}
	audio_decoder->HwChannels = 2;
	audio_decoder->SpdifIndex = 0;	// reset buffer
	audio_decoder->SpdifCount = 0;
	*passthrough = 1;
    }
    // channels/sample-rate not support?
    if ((err = AudioSetup(&audio_decoder->HwSampleRate, &audio_decoder->HwChannels, *passthrough))) {

	// try E-AC-3 none HBR
	audio_decoder->HwSampleRate /= 4;
	if (audio_ctx->codec_id != AV_CODEC_ID_EAC3
	    || (err = AudioSetup(&audio_decoder->HwSampleRate, &audio_decoder->HwChannels, *passthrough))) {

	    Debug(3, "codec/audio: audio setup error\n");
	    // FIXME: handle errors
	    audio_decoder->HwChannels = 0;
	    audio_decoder->HwSampleRate = 0;
	    return err;
	}
    }

    Debug(3, "codec/audio: resample %s %dHz *%d -> %s %dHz *%d\n", av_get_sample_fmt_name(audio_ctx->sample_fmt),
	audio_ctx->sample_rate, audio_ctx->channels, av_get_sample_fmt_name(AV_SAMPLE_FMT_S16),
	audio_decoder->HwSampleRate, audio_decoder->HwChannels);

    return 0;
}

/**
**	Audio pass-through decoder helper.
**
**	@param audio_decoder	audio decoder data
**	@param avpkt		undecoded audio packet
*/
static int CodecAudioPassthroughHelper(AudioDecoder * audio_decoder, const AVPacket * avpkt)
{
    const AVCodecContext *audio_ctx;

    audio_ctx = audio_decoder->AudioCtx;
    // SPDIF/HDMI passthrough
    if (CodecPassthrough & CodecAC3 && audio_ctx->codec_id == AV_CODEC_ID_AC3) {
	uint16_t *spdif;
	int spdif_sz;

	spdif = audio_decoder->Spdif;
	spdif_sz = 6144;

	// FIXME: this works with some TVs/AVReceivers
	// FIXME: write burst size drift correction, which should work with all
	if (CodecAudioDrift & CORRECT_AC3) {
	    int x;

	    x = (audio_decoder->DriftFrac +
		(audio_decoder->DriftCorr * spdif_sz)) / (10 * audio_decoder->HwSampleRate * 100);
	    audio_decoder->DriftFrac =
		(audio_decoder->DriftFrac +
		(audio_decoder->DriftCorr * spdif_sz)) % (10 * audio_decoder->HwSampleRate * 100);
	    // round to word border
	    x *= audio_decoder->HwChannels * 4;
	    if (x < -64) {		// limit correction
		x = -64;
	    } else if (x > 64) {
		x = 64;
	    }
	    spdif_sz += x;
	}
	// build SPDIF header and append A52 audio to it
	// avpkt is the original data
	if (spdif_sz < avpkt->size + 8) {
	    Error(_("codec/audio: decoded data smaller than encoded\n"));
	    return -1;
	}
	spdif[0] = htole16(0xF872);	// iec 61937 sync word
	spdif[1] = htole16(0x4E1F);
	spdif[2] = htole16(IEC61937_AC3 | (avpkt->data[5] & 0x07) << 8);
	spdif[3] = htole16(avpkt->size * 8);
	// copy original data for output
	// FIXME: not 100% sure, if endian is correct on not intel hardware
	swab(avpkt->data, spdif + 4, avpkt->size);
	// FIXME: don't need to clear always
	memset(spdif + 4 + avpkt->size / 2, 0, spdif_sz - 8 - avpkt->size);
	// don't play with the ac-3 samples
	AudioEnqueue(spdif, spdif_sz);
	return 1;
    }
    if (CodecPassthrough & CodecEAC3 && audio_ctx->codec_id == AV_CODEC_ID_EAC3) {
	uint16_t *spdif;
	int spdif_sz;
	int repeat;

	// build SPDIF header and append A52 audio to it
	// avpkt is the original data
	spdif = audio_decoder->Spdif;
	spdif_sz = 24576;		// 4 * 6144
	if (audio_decoder->HwSampleRate == 48000) {
	    spdif_sz = 6144;
	}
	if (spdif_sz < audio_decoder->SpdifIndex + avpkt->size + 8) {
	    Error(_("codec/audio: decoded data smaller than encoded\n"));
	    return -1;
	}
	// check if we must pack multiple packets
	repeat = 1;
	if ((avpkt->data[4] & 0xc0) != 0xc0) {	// fscod
	    static const uint8_t eac3_repeat[4] = { 6, 3, 2, 1 };

	    // fscod2
	    repeat = eac3_repeat[(avpkt->data[4] & 0x30) >> 4];
	}
	// fprintf(stderr, "repeat %d %d\n", repeat, avpkt->size);

	// copy original data for output
	// pack upto repeat EAC-3 pakets into one IEC 61937 burst
	// FIXME: not 100% sure, if endian is correct on not intel hardware
	swab(avpkt->data, spdif + 4 + audio_decoder->SpdifIndex, avpkt->size);
	audio_decoder->SpdifIndex += avpkt->size;
	if (++audio_decoder->SpdifCount < repeat) {
	    return 1;
	}

	spdif[0] = htole16(0xF872);	// iec 61937 sync word
	spdif[1] = htole16(0x4E1F);
	spdif[2] = htole16(IEC61937_EAC3);
	spdif[3] = htole16(audio_decoder->SpdifIndex * 8);
	memset(spdif + 4 + audio_decoder->SpdifIndex / 2, 0, spdif_sz - 8 - audio_decoder->SpdifIndex);

	// don't play with the eac-3 samples
	AudioEnqueue(spdif, spdif_sz);

	audio_decoder->SpdifIndex = 0;
	audio_decoder->SpdifCount = 0;
	return 1;
    }
    return 0;
}

#if !defined(USE_SWRESAMPLE) && !defined(USE_AVRESAMPLE)

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
    static int c;

    AudioSetClock(pts);

    delay = AudioGetDelay();
    if (!delay) {
	return;
    }
    clock_gettime(CLOCK_MONOTONIC, &nowtime);
    if (!audio_decoder->LastDelay) {
	audio_decoder->LastTime = nowtime;
	audio_decoder->LastPTS = pts;
	audio_decoder->LastDelay = delay;
	audio_decoder->Drift = 0;
	audio_decoder->DriftFrac = 0;
	Debug(3, "codec/audio: inital drift delay %" PRId64 "ms\n", delay / 90);
	return;
    }
    // collect over some time
    pts_diff = pts - audio_decoder->LastPTS;
    if (pts_diff < 10 * 1000 * 90) {
	return;
    }

    tim_diff = (nowtime.tv_sec - audio_decoder->LastTime.tv_sec)
	* 1000 * 1000 * 1000 + (nowtime.tv_nsec - audio_decoder->LastTime.tv_nsec);

    drift = (tim_diff * 90) / (1000 * 1000) - pts_diff + delay - audio_decoder->LastDelay;

    // adjust rounding error
    nowtime.tv_nsec -= nowtime.tv_nsec % (1000 * 1000 / 90);
    audio_decoder->LastTime = nowtime;
    audio_decoder->LastPTS = pts;
    audio_decoder->LastDelay = delay;

    // underruns and av_resample have the same time :(((
    if (abs(drift) > 10 * 90) {
	// drift too big, pts changed?
	Debug(3, "codec/audio: drift(%6d) %3dms reset\n", audio_decoder->DriftCorr, drift / 90);
	audio_decoder->LastDelay = 0;
#ifdef DEBUG
	corr = 0;			// keep gcc happy
#endif
    } else {

	drift += audio_decoder->Drift;
	audio_decoder->Drift = drift;
	corr = (10 * audio_decoder->HwSampleRate * drift) / (90 * 1000);
	// SPDIF/HDMI passthrough
	if ((CodecAudioDrift & CORRECT_AC3) && (!(CodecPassthrough & CodecAC3)
		|| audio_decoder->AudioCtx->codec_id != AV_CODEC_ID_AC3)
	    && (!(CodecPassthrough & CodecEAC3)
		|| audio_decoder->AudioCtx->codec_id != AV_CODEC_ID_EAC3)) {
	    audio_decoder->DriftCorr = -corr;
	}

	if (audio_decoder->DriftCorr < -20000) {    // limit correction
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
	av_resample_compensate(audio_decoder->AvResample, audio_decoder->DriftCorr / 10, distance);
    }

    if (!(c++ % 10)) {
	Debug(3, "codec/audio: drift(%6d) %8dus %5d\n", audio_decoder->DriftCorr, drift * 1000 / 90, corr);
    }
}

/**
**	Handle audio format changes.
**
**	@param audio_decoder	audio decoder data
**
**	@note this is the old not good supported version
*/
static void CodecAudioUpdateFormat(AudioDecoder * audio_decoder)
{
    int passthrough;
    const AVCodecContext *audio_ctx;
    int err;

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
    if ((err = CodecAudioUpdateHelper(audio_decoder, &passthrough))) {

	Debug(3, "codec/audio: resample %dHz *%d -> %dHz *%d\n", audio_ctx->sample_rate, audio_ctx->channels,
	    audio_decoder->HwSampleRate, audio_decoder->HwChannels);

	if (err == 1) {
	    audio_decoder->ReSample =
		av_audio_resample_init(audio_decoder->HwChannels, audio_ctx->channels, audio_decoder->HwSampleRate,
		audio_ctx->sample_rate, audio_ctx->sample_fmt, audio_ctx->sample_fmt, 16, 10, 0, 0.8);
	    // libav-0.8_pre didn't support 6 -> 2 channels
	    if (!audio_decoder->ReSample) {
		Error(_("codec/audio: resample setup error\n"));
		audio_decoder->HwChannels = 0;
		audio_decoder->HwSampleRate = 0;
	    }
	    return;
	}
	Debug(3, "codec/audio: audio setup error\n");
	// FIXME: handle errors
	audio_decoder->HwChannels = 0;
	audio_decoder->HwSampleRate = 0;
	return;
    }
    if (passthrough) {			// pass-through no conversion allowed
	return;
    }
    // prepare audio drift resample
    if (CodecAudioDrift & CORRECT_PCM) {
	if (audio_decoder->AvResample) {
	    Error(_("codec/audio: overwrite resample\n"));
	}
	audio_decoder->AvResample =
	    av_resample_init(audio_decoder->HwSampleRate, audio_decoder->HwSampleRate, 16, 10, 0, 0.8);
	if (!audio_decoder->AvResample) {
	    Error(_("codec/audio: AvResample setup error\n"));
	} else {
	    // reset drift to some default value
	    audio_decoder->DriftCorr /= 2;
	    audio_decoder->DriftFrac = 0;
	    av_resample_compensate(audio_decoder->AvResample, audio_decoder->DriftCorr / 10,
		10 * audio_decoder->HwSampleRate);
	}
    }
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
    if ((CodecAudioDrift & CORRECT_PCM) && audio_decoder->AvResample) {
	int16_t buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 4 + FF_INPUT_BUFFER_PADDING_SIZE]
	    __attribute__ ((aligned(16)));
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
		audio_decoder->Buffer[ch] = realloc(audio_decoder->Buffer[ch], audio_decoder->BufferSize);
	    }
	}
	// copy remaining bytes into sample buffer
	for (ch = 0; ch < audio_decoder->HwChannels; ++ch) {
	    memcpy(audio_decoder->Buffer[ch], audio_decoder->Remain[ch], audio_decoder->RemainCount);
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
	    n = av_resample(audio_decoder->AvResample, buftmp[ch], audio_decoder->Buffer[ch], &consumed, bytes_n / 2,
		sizeof(buftmp[ch]) / 2, ch == audio_decoder->HwChannels - 1);
	    // fixme remaining channels
	    if (bytes_n - consumed * 2 > audio_decoder->RemainSize) {
		audio_decoder->RemainSize = bytes_n - consumed * 2;
	    }
	    audio_decoder->Remain[ch] = realloc(audio_decoder->Remain[ch], audio_decoder->RemainSize);
	    memcpy(audio_decoder->Remain[ch], audio_decoder->Buffer[ch] + consumed, audio_decoder->RemainSize);
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
	if (!(audio_decoder->Passthrough & CodecPCM)) {
	    CodecReorderAudioFrame(buf, n, audio_decoder->HwChannels);
	}
	AudioEnqueue(buf, n);
	return;
    }
    if (!(audio_decoder->Passthrough & CodecPCM)) {
	CodecReorderAudioFrame(data, count, audio_decoder->HwChannels);
    }
    AudioEnqueue(data, count);
}

int myavcodec_decode_audio3(AVCodecContext * avctx, int16_t * samples, int *frame_size_ptr, AVPacket * avpkt)
{
    AVFrame *frame = av_frame_alloc();
    int ret, got_frame = 0;

    if (!frame)
	return AVERROR(ENOMEM);
    ret = avcodec_decode_audio4(avctx, frame, &got_frame, avpkt);
    if (ret >= 0 && got_frame) {
	int i, ch;
	int planar = av_sample_fmt_is_planar(avctx->sample_fmt);
	int data_size = av_get_bytes_per_sample(avctx->sample_fmt);

	if (data_size < 0) {
	    /* This should not occur, checking just for paranoia */
	    fprintf(stderr, "Failed to calculate data size\n");
	    exit(1);
	}
	for (i = 0; i < frame->nb_samples; i++)
	    for (ch = 0; ch < avctx->channels; ch++) {
		memcpy(samples, frame->extended_data[ch] + data_size * i, data_size);
		samples = (char *)samples + data_size;
	    }
	*frame_size_ptr = data_size * avctx->channels * frame->nb_samples;
    } else {
	*frame_size_ptr = 0;
    }
    av_frame_free(&frame);
    return ret;
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
    int16_t buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 4 + FF_INPUT_BUFFER_PADDING_SIZE] __attribute__ ((aligned(16)));
    int buf_sz;
    int l;
    AVCodecContext *audio_ctx;

    audio_ctx = audio_decoder->AudioCtx;

    // FIXME: don't need to decode pass-through codecs
    buf_sz = sizeof(buf);
    l = myavcodec_decode_audio3(audio_ctx, buf, &buf_sz, (AVPacket *) avpkt);
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
    // update audio clock
    if (avpkt->pts != (int64_t) AV_NOPTS_VALUE) {
	CodecAudioSetClock(audio_decoder, avpkt->pts);
    }
    // FIXME: must first play remainings bytes, than change and play new.
    if (audio_decoder->Passthrough != CodecPassthrough || audio_decoder->SampleRate != audio_ctx->sample_rate
	|| audio_decoder->Channels != audio_ctx->channels) {
	CodecAudioUpdateFormat(audio_decoder);
    }

    if (audio_decoder->HwSampleRate && audio_decoder->HwChannels) {
	// need to resample audio
	if (audio_decoder->ReSample) {
	    int16_t outbuf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 4 + FF_INPUT_BUFFER_PADDING_SIZE]
		__attribute__ ((aligned(16)));
	    int outlen;

	    // FIXME: libav-0.7.2 crash here
	    outlen = audio_resample(audio_decoder->ReSample, outbuf, buf, buf_sz);
#ifdef DEBUG
	    if (outlen != buf_sz) {
		Debug(3, "codec/audio: possible fixed ffmpeg\n");
	    }
#endif
	    if (outlen) {
		// outlen seems to be wrong in ffmpeg-0.9
		outlen /= audio_decoder->Channels * av_get_bytes_per_sample(audio_ctx->sample_fmt);
		outlen *= audio_decoder->HwChannels * av_get_bytes_per_sample(audio_ctx->sample_fmt);
		Debug(4, "codec/audio: %d -> %d\n", buf_sz, outlen);
		CodecAudioEnqueue(audio_decoder, outbuf, outlen);
	    }
	} else {
	    if (CodecAudioPassthroughHelper(audio_decoder, avpkt)) {
		return;
	    }
	    CodecAudioEnqueue(audio_decoder, buf, buf_sz);
	}
    }
}

#endif

#if defined(USE_SWRESAMPLE) || defined(USE_AVRESAMPLE)

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
    static int c;

    AudioSetClock(pts);

    delay = AudioGetDelay();
    if (!delay) {
	return;
    }
    clock_gettime(CLOCK_MONOTONIC, &nowtime);
    if (!audio_decoder->LastDelay) {
	audio_decoder->LastTime = nowtime;
	audio_decoder->LastPTS = pts;
	audio_decoder->LastDelay = delay;
	audio_decoder->Drift = 0;
	audio_decoder->DriftFrac = 0;
	Debug(3, "codec/audio: inital drift delay %" PRId64 "ms\n", delay / 90);
	return;
    }
    // collect over some time
    pts_diff = pts - audio_decoder->LastPTS;
    if (pts_diff < 10 * 1000 * 90) {
	return;
    }

    tim_diff = (nowtime.tv_sec - audio_decoder->LastTime.tv_sec)
	* 1000 * 1000 * 1000 + (nowtime.tv_nsec - audio_decoder->LastTime.tv_nsec);

    drift = (tim_diff * 90) / (1000 * 1000) - pts_diff + delay - audio_decoder->LastDelay;

    // adjust rounding error
    nowtime.tv_nsec -= nowtime.tv_nsec % (1000 * 1000 / 90);
    audio_decoder->LastTime = nowtime;
    audio_decoder->LastPTS = pts;
    audio_decoder->LastDelay = delay;

    // underruns and av_resample have the same time :(((
    if (abs(drift) > 10 * 90) {
	// drift too big, pts changed?
	Debug(3, "codec/audio: drift(%6d) %3dms reset\n", audio_decoder->DriftCorr, drift / 90);
	audio_decoder->LastDelay = 0;
#ifdef DEBUG
	corr = 0;			// keep gcc happy
#endif
    } else {

	drift += audio_decoder->Drift;
	audio_decoder->Drift = drift;
	corr = (10 * audio_decoder->HwSampleRate * drift) / (90 * 1000);
	// SPDIF/HDMI passthrough
	if ((CodecAudioDrift & CORRECT_AC3) && (!(CodecPassthrough & CodecAC3)
		|| audio_decoder->AudioCtx->codec_id != AV_CODEC_ID_AC3)
	    && (!(CodecPassthrough & CodecEAC3)
		|| audio_decoder->AudioCtx->codec_id != AV_CODEC_ID_EAC3)) {
	    audio_decoder->DriftCorr = -corr;
	}

	if (audio_decoder->DriftCorr < -20000) {    // limit correction
	    audio_decoder->DriftCorr = -20000;
	} else if (audio_decoder->DriftCorr > 20000) {
	    audio_decoder->DriftCorr = 20000;
	}
    }

#ifdef USE_SWRESAMPLE
    if (audio_decoder->Resample && audio_decoder->DriftCorr) {
	int distance;

	// try workaround for buggy ffmpeg 0.10
	if (abs(audio_decoder->DriftCorr) < 2000) {
	    distance = (pts_diff * audio_decoder->HwSampleRate) / (900 * 1000);
	} else {
	    distance = (pts_diff * audio_decoder->HwSampleRate) / (90 * 1000);
	}
	if (swr_set_compensation(audio_decoder->Resample, audio_decoder->DriftCorr / 10, distance)) {
	    Debug(3, "codec/audio: swr_set_compensation failed\n");
	}
    }
#endif
#ifdef USE_AVRESAMPLE
    if (audio_decoder->Resample && audio_decoder->DriftCorr) {
	int distance;

	distance = (pts_diff * audio_decoder->HwSampleRate) / (900 * 1000);
	if (avresample_set_compensation(audio_decoder->Resample, audio_decoder->DriftCorr / 10, distance)) {
	    Debug(3, "codec/audio: swr_set_compensation failed\n");
	}
    }
#endif
    if (!(c++ % 10)) {
	Debug(3, "codec/audio: drift(%6d) %8dus %5d\n", audio_decoder->DriftCorr, drift * 1000 / 90, corr);
    }
}

/**
**	Handle audio format changes.
**
**	@param audio_decoder	audio decoder data
*/
static void CodecAudioUpdateFormat(AudioDecoder * audio_decoder)
{
    int passthrough;
    const AVCodecContext *audio_ctx;

    if (CodecAudioUpdateHelper(audio_decoder, &passthrough)) {
	// FIXME: handle swresample format conversions.
	return;
    }
    if (passthrough) {			// pass-through no conversion allowed
	return;
    }

    audio_ctx = audio_decoder->AudioCtx;

#ifdef DEBUG
    if (audio_ctx->sample_fmt == AV_SAMPLE_FMT_S16 && audio_ctx->sample_rate == audio_decoder->HwSampleRate
	&& !CodecAudioDrift) {
	// FIXME: use Resample only, when it is needed!
	fprintf(stderr, "no resample needed\n");
    }
#endif

#ifdef USE_SWRESAMPLE
    audio_decoder->Resample =
	swr_alloc_set_opts(audio_decoder->Resample, audio_ctx->channel_layout, AV_SAMPLE_FMT_S16,
	audio_decoder->HwSampleRate, audio_ctx->channel_layout, audio_ctx->sample_fmt, audio_ctx->sample_rate, 0,
	NULL);
    if (audio_decoder->Resample) {
	swr_init(audio_decoder->Resample);
    } else {
	Error(_("codec/audio: can't setup resample\n"));
    }
#endif
#ifdef USE_AVRESAMPLE
    if (!(audio_decoder->Resample = avresample_alloc_context())) {
	Error(_("codec/audio: can't setup resample\n"));
	return;
    }

    av_opt_set_int(audio_decoder->Resample, "in_channel_layout", audio_ctx->channel_layout, 0);
    av_opt_set_int(audio_decoder->Resample, "in_sample_fmt", audio_ctx->sample_fmt, 0);
    av_opt_set_int(audio_decoder->Resample, "in_sample_rate", audio_ctx->sample_rate, 0);
    av_opt_set_int(audio_decoder->Resample, "out_channel_layout", audio_ctx->channel_layout, 0);
    av_opt_set_int(audio_decoder->Resample, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    av_opt_set_int(audio_decoder->Resample, "out_sample_rate", audio_decoder->HwSampleRate, 0);

    if (avresample_open(audio_decoder->Resample)) {
	avresample_free(&audio_decoder->Resample);
	audio_decoder->Resample = NULL;
	Error(_("codec/audio: can't open resample\n"));
	return;
    }
#endif
}

/**
**	Decode an audio packet.
**
**	PTS must be handled self.
**
**	@note the caller has not aligned avpkt and not cleared the end.
**
**	@param audio_decoder	audio decoder data
**	@param avpkt		audio packet
*/
void CodecAudioDecode(AudioDecoder * audio_decoder, const AVPacket * avpkt)
{
    AVCodecContext *audio_ctx;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56,28,1)
    AVFrame frame[1];
#else
    AVFrame *frame;
#endif
    int got_frame;
    int n;

    audio_ctx = audio_decoder->AudioCtx;

    // FIXME: don't need to decode pass-through codecs

    // new AVFrame API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56,28,1)
    avcodec_get_frame_defaults(frame);
#else
    frame = audio_decoder->Frame;
    av_frame_unref(frame);
#endif

    got_frame = 0;
    n = avcodec_decode_audio4(audio_ctx, frame, &got_frame, (AVPacket *) avpkt);

    if (n != avpkt->size) {
	if (n == AVERROR(EAGAIN)) {
	    Error(_("codec/audio: latm\n"));
	    return;
	}
	if (n < 0) {			// no audio frame could be decompressed
	    Error(_("codec/audio: bad audio frame\n"));
	    return;
	}
	Error(_("codec/audio: error more than one frame data\n"));
    }
    if (!got_frame) {
	Error(_("codec/audio: no frame\n"));
	return;
    }
    // update audio clock
    if (avpkt->pts != (int64_t) AV_NOPTS_VALUE) {
	CodecAudioSetClock(audio_decoder, avpkt->pts);
    }
    // format change
    if (audio_decoder->Passthrough != CodecPassthrough || audio_decoder->SampleRate != audio_ctx->sample_rate
	|| audio_decoder->Channels != audio_ctx->channels) {
	CodecAudioUpdateFormat(audio_decoder);
    }

    if (!audio_decoder->HwSampleRate || !audio_decoder->HwChannels) {
	return;				// unsupported sample format
    }

    if (CodecAudioPassthroughHelper(audio_decoder, avpkt)) {
	return;
    }
#ifdef USE_SWRESAMPLE
    if (audio_decoder->Resample) {
	uint8_t outbuf[8192 * 2 * 8];
	uint8_t *out[1];

	out[0] = outbuf;
	n = swr_convert(audio_decoder->Resample, out, sizeof(outbuf) / (2 * audio_decoder->HwChannels),
	    (const uint8_t **)frame->extended_data, frame->nb_samples);
	if (n > 0) {
	    if (!(audio_decoder->Passthrough & CodecPCM)) {
		CodecReorderAudioFrame((int16_t *) outbuf, n * 2 * audio_decoder->HwChannels,
		    audio_decoder->HwChannels);
	    }
	    AudioEnqueue(outbuf, n * 2 * audio_decoder->HwChannels);
	}
	return;
    }
#endif

#ifdef USE_AVRESAMPLE
    if (audio_decoder->Resample) {
	uint8_t outbuf[8192 * 2 * 8];
	uint8_t *out[1];

	out[0] = outbuf;
	n = avresample_convert(audio_decoder->Resample, out, 0, sizeof(outbuf) / (2 * audio_decoder->HwChannels),
	    (uint8_t **) frame->extended_data, 0, frame->nb_samples);
	// FIXME: set out_linesize, in_linesize correct
	if (n > 0) {
	    if (!(audio_decoder->Passthrough & CodecPCM)) {
		CodecReorderAudioFrame((int16_t *) outbuf, n * 2 * audio_decoder->HwChannels,
		    audio_decoder->HwChannels);
	    }
	    AudioEnqueue(outbuf, n * 2 * audio_decoder->HwChannels);
	}
	return;
    }
#endif

#ifdef DEBUG
    // should be never reached
    fprintf(stderr, "oops\n");
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
    if (decoder->AudioCtx)
	avcodec_flush_buffers(decoder->AudioCtx);
}

//----------------------------------------------------------------------------
//  Codec
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
