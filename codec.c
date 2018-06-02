/// Copyright (C) 2009 - 2015 by Johns. All Rights Reserved.
/// Copyright (C) 2018 by pesintta, rofafor.
///
/// SPDX-License-Identifier: AGPL-3.0-only

///
/// This module contains all decoder and codec functions.
/// It is uses ffmpeg (http://ffmpeg.org) as backend.
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

#include <libavcodec/avcodec.h>
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,64,100)
#error "libavcodec is too old - please, upgrade!"
#endif
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavcodec/vaapi.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libswresample/swresample.h>
#if LIBSWRESAMPLE_VERSION_INT < AV_VERSION_INT(0,15,100)
#error "libswresample is too old - please, upgrade!"
#endif

#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>

#include "iatomic.h"
#include "misc.h"
#include "video.h"
#include "audio.h"
#include "codec.h"

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
    VideoDecoder *decoder = video_ctx->opaque;

    return Video_get_format(decoder->HwDecoder, video_ctx, fmt);
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

}

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
    unsigned surface;
    VideoDecoder *decoder = video_ctx->opaque;

    if (frame->format != AV_PIX_FMT_VAAPI || !video_ctx->hw_frames_ctx
	|| !(decoder->VideoCodec->capabilities & AV_CODEC_CAP_DR1))
	return avcodec_default_get_buffer2(video_ctx, frame, flags);

    pthread_mutex_lock(&CodecLockMutex);

    surface = VideoGetSurface(decoder->HwDecoder, video_ctx);

    // vaapi needs both fields set
    frame->buf[0] = av_buffer_create((uint8_t *) (size_t) surface, 0, Codec_free_buffer, video_ctx, 0);
    frame->data[0] = frame->buf[0]->data;
    frame->data[3] = frame->data[0];

    pthread_mutex_unlock(&CodecLockMutex);

    return 0;
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
	Fatal("codec: can't allocate vodeo decoder");
    }

    av_register_all();

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
    if (decoder->VideoFmtCtx) {
	if (decoder->VideoFmtCtx->pb) {
	    av_freep(&decoder->VideoFmtCtx->pb);
	}
	avformat_free_context(decoder->VideoFmtCtx);
    }
    free(decoder);
}

/**
**	Open video decoder.
**
**	@param decoder	private video decoder
*/
void CodecVideoOpen(VideoDecoder * decoder)
{
    int ret, codec_id = AV_CODEC_ID_NONE;
    AVCodec *video_codec;
    AVIOContext *avio_ctx = NULL;
    AVDictionary *options = NULL;
    AVInputFormat *input_format = NULL;
    const int alloc_size = 1024 * 1024 * 1;
    uint8_t *avio_ctx_buffer = av_malloc(alloc_size);

    if (!avio_ctx_buffer)
	return;

    pthread_mutex_lock(&CodecLockMutex);

    switch (device_get_vtype()) {
	case -1:		       // Playback stopped or not reasonable
	    goto error_avformat_alloc_context;
	    break;

	case 0x0:		       // Recording is playing. Probe format with ffmpeg
	    break;

	case 0x1:		       /* FALLTHRU */
	case 0x2:
	    codec_id = AV_CODEC_ID_MPEG2VIDEO;
	    break;

	case 0x1b:		       /* FALLTHRU */
	case 0x20:
	    codec_id = AV_CODEC_ID_H264;
	    break;

	case 0x24:
	    codec_id = AV_CODEC_ID_HEVC;
	    break;

	default:
	    Fatal("codec: unknown vtype: 0x%x", device_get_vtype());
	    break;
    }

    device_set_mode(0);

    if (!(decoder->VideoFmtCtx = avformat_alloc_context())) {
	Error("codec: can't allocate AV Format Context");
	goto error_avformat_alloc_context;
    }

    avio_ctx = avio_alloc_context(avio_ctx_buffer, alloc_size, 0, decoder, &device_read_video_data, NULL, NULL);
    if (!avio_ctx) {
	Error("codec: can't allocate AV IO Context");
	goto error_avio_alloc_context;
    }
    // From now on the ctx_buffer is controlled (and freed) by avio_ctx
    avio_ctx_buffer = NULL;
    avio_ctx->max_packet_size = alloc_size;

    decoder->VideoFmtCtx->pb = avio_ctx;
    if (codec_id != AV_CODEC_ID_NONE)
	decoder->VideoFmtCtx->video_codec_id = codec_id;

    av_dict_set_int(&options, "analyzeduration", 750, 0);
    av_dict_set_int(&options, "probesize", alloc_size / 2, 0);
    av_dict_set_int(&options, "max_streams", 1, 0);

    input_format = av_find_input_format("mpeg");
    if (!input_format) {
	Error("codec: could not find input format. Trying to probe it");
    }
    // This probes things and allocates buffers which cannot block on mutex
    ret = avformat_open_input(&decoder->VideoFmtCtx, NULL, input_format, &options);
    if (ret < 0) {
	Error("codec: can't open input: %s", av_err2str(ret));
	goto error_avformat_open_input;
    }

    ret = avformat_find_stream_info(decoder->VideoFmtCtx, NULL);
    if (ret < 0) {
	Error("codec: can't find stream info: %s", av_err2str(ret));
	goto error_avformat_find_stream_info;
    }

    av_dump_format(decoder->VideoFmtCtx, 0, "vaapidevice video", 0);

    ret = av_find_best_stream(decoder->VideoFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &video_codec, 0);
    if (ret < 0) {
	Error("codec: can't find best stream: %s", av_err2str(ret));
	goto error_avformat_find_best_stream;
    }
    decoder->VideoCodec = video_codec;

    if (!(decoder->VideoCtx = avcodec_alloc_context3(video_codec))) {
	Error("codec: can't allocate video codec context");
	goto error_avcodec_alloc_context3;
    }

    if (!HwDeviceContext) {
	Error("codec: no hw device context to be used");
	goto error_no_hwdevicecontext;
    }
    decoder->VideoCtx->hw_device_ctx = av_buffer_ref(HwDeviceContext);

    // FIXME: for software decoder use all cpus, otherwise 1
    decoder->VideoCtx->thread_count = 1;
    // open codec
    if (video_codec->capabilities & (AV_CODEC_CAP_AUTO_THREADS)) {
	Debug4("codec: auto threads enabled");
	decoder->VideoCtx->thread_count = 0;
    }

    decoder->VideoCtx->opaque = decoder;    // our structure

    Debug4("codec: video '%s'", decoder->VideoCodec->long_name);
    if (video_codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
	Debug4("codec: supports truncated packets");
	//decoder->VideoCtx->flags |= CODEC_FLAG_TRUNCATED;
    }
    // FIXME: own memory management for video frames.
    if (video_codec->capabilities & AV_CODEC_CAP_DR1) {
	Debug4("codec: can use own buffer management");
    }
    if (video_codec->capabilities & AV_CODEC_CAP_FRAME_THREADS) {
	Debug4("codec: supports frame threads");
	decoder->VideoCtx->thread_count = 0;
	decoder->VideoCtx->thread_type |= FF_THREAD_FRAME;
    }
    if (video_codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
	Debug4("codec: supports slice threads");
	decoder->VideoCtx->thread_count = 0;
	decoder->VideoCtx->thread_type |= FF_THREAD_SLICE;
    }
    decoder->VideoCtx->thread_safe_callbacks = 0;
    decoder->VideoCtx->get_format = Codec_get_format;
    decoder->VideoCtx->get_buffer2 = Codec_get_buffer2;
    decoder->VideoCtx->draw_horiz_band = NULL;

    av_opt_set_int(decoder->VideoCtx, "refcounted_frames", 1, 0);

    if (avcodec_open2(decoder->VideoCtx, video_codec, NULL) < 0) {
	Error("codec: can't open video codec!");
	goto error_avcodec_open;
    }
    //
    //	Prepare frame buffer for decoder
    //
    if (!(decoder->Frame = av_frame_alloc())) {
	Error("codec: can't allocate video decoder frame buffer");
	goto error_av_frame_alloc;
    }

    device_set_mode(1);
    av_dict_free(&options);

    pthread_mutex_unlock(&CodecLockMutex);
    return;

  error_av_frame_alloc:
  error_avcodec_open:
    av_buffer_unref(&decoder->VideoCtx->hw_device_ctx);
  error_no_hwdevicecontext:
    avcodec_free_context(&decoder->VideoCtx);
  error_avcodec_alloc_context3:
  error_avformat_find_best_stream:
  error_avformat_find_stream_info:
  error_avformat_open_input:
    av_freep(avio_ctx);
    av_dict_free(&options);
  error_avio_alloc_context:
    avformat_free_context(decoder->VideoFmtCtx);
    decoder->VideoFmtCtx = NULL;
  error_avformat_alloc_context:
    if (avio_ctx_buffer)
	av_freep(&avio_ctx_buffer);
    device_set_mode(1);
    pthread_mutex_unlock(&CodecLockMutex);
}

/**
**	Close video decoder.
**
**	@param video_decoder	private video decoder
*/
void CodecVideoClose(VideoDecoder * video_decoder)
{
    // FIXME: play buffered data
    av_frame_free(&video_decoder->Frame);   // callee does checks

    if (video_decoder->VideoCtx) {
	pthread_mutex_lock(&CodecLockMutex);
	avformat_close_input(&video_decoder->VideoFmtCtx);
	avcodec_free_context(&video_decoder->VideoCtx);
	pthread_mutex_unlock(&CodecLockMutex);
    }
}

/**
**	Decode a video packet.
**
**	@param decoder	video decoder data
**	@param avpkt	video packet
*/
void CodecVideoDecode(VideoDecoder * decoder)
{
    AVCodecContext *video_ctx;

    if (!decoder->VideoCtx)
	CodecVideoOpen(decoder);

    video_ctx = decoder->VideoCtx;
    if (!video_ctx)
	return;

    if (video_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
	int ret;
	AVPacket pkt[1];
	AVPacket *avpacket = pkt;
	AVFrame *frame = decoder->Frame;

	if (decoder->VideoFmtCtx) {

	    ret = av_read_frame(decoder->VideoFmtCtx, avpacket);
	    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
		Error("codec: read frame failed: %s", av_err2str(ret));
		return;
	    } else if (ret == AVERROR_EOF) {
		Debug4("codec: received EOF - draining");

		// Sending null packet enters draining mode
		avpacket = NULL;
	    } else if (ret == AVERROR(EAGAIN)) {
		return;
	    } else if (decoder->VideoFmtCtx->streams[pkt->stream_index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
		return;
	    }
	}

	ret = avcodec_send_packet(video_ctx, avpacket);
	if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
	    Debug4("codec: sending video packet failed: %s", av_err2str(ret));
	    return;
	}
	ret = avcodec_receive_frame(video_ctx, frame);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
	    Debug4("codec: receiving video frame failed: %s", av_err2str(ret));
	    return;
	} else if (ret == AVERROR_EOF) {
	    Debug4("codec: drain completed");
	    CodecVideoClose(decoder);
	    return;
	}
	if (ret >= 0) {
	    VideoRenderFrame(decoder->HwDecoder, video_ctx, frame);
	}
	av_frame_unref(frame);
    }
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
    if (decoder->VideoFmtCtx) {
	avio_flush(decoder->VideoFmtCtx->pb);
	avformat_flush(decoder->VideoFmtCtx);
    }
}

/**
**	Get video codec name for debug OSD purposes.
**
**	@param decoder	video decoder data
*/
const char* CodecVideoGetCodecName(VideoDecoder * decoder)
{
    if (decoder && decoder->VideoCodec)
	return avcodec_get_name(decoder->VideoCodec->id);

    return avcodec_get_name(AV_CODEC_ID_NONE);
}


//----------------------------------------------------------------------------
//  Audio
//----------------------------------------------------------------------------

///
/// Audio decoder structure.
///
struct _audio_decoder_
{
    AVFormatContext *AudioFmtCtx;	///< format context
    AVCodec *AudioCodec;		///< audio codec
    AVCodecContext *AudioCtx;		///< audio codec context

    char Passthrough;			///< current pass-through flags
    int SampleRate;			///< current stream sample rate
    int Channels;			///< current stream channels

    int HwSampleRate;			///< hw sample rate
    int HwChannels;			///< hw channels

    AVFrame *Frame;			///< decoded audio frame buffer

    SwrContext *Resample;		///< ffmpeg software resample context

    uint16_t Spdif[24576 / 2];		///< SPDIF output buffer
    int SpdifIndex;			///< index into SPDIF output buffer
    int SpdifCount;			///< SPDIF repeat counter

    int64_t LastDelay;			///< last delay
    struct timespec LastTime;		///< last time
    int64_t LastPTS;			///< last PTS

    int Drift;				///< accumulated audio drift
    int DriftCorr;			///< audio drift correction value
    int DriftFrac;			///< audio drift fraction for ac3
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
	Fatal("codec: can't allocate audio decoder");
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
    av_frame_free(&decoder->Frame);	// callee does checks
    free(decoder);
}

/**
**	Open audio decoder.
**
**	@param audio_decoder	private audio decoder
**	@param codec_id	audio	codec id
*/
void CodecAudioOpen(AudioDecoder * audio_decoder, int codec_id_old)
{
    int ret, codec_id = AV_CODEC_ID_NONE;
    AVCodec *audio_codec;
    AVIOContext *avio_ctx = NULL;
    AVDictionary *options = NULL;
    AVInputFormat *input_format = NULL;
    const int alloc_size = 1024 * 64 * 1;
    uint8_t *avio_ctx_buffer = av_malloc(alloc_size);

    if (!avio_ctx_buffer)
	return;

    pthread_mutex_lock(&CodecLockMutex);

    switch (device_get_atype()) {
	case 0:
	    break;
	case 0x3:	/* FALLTHRU */
	case 0x4:
	    codec_id = AV_CODEC_ID_MP2;
	    break;

	case 0x6a:
	    codec_id = AV_CODEC_ID_AC3;
	    break;
	default:
	    Fatal("Unknown atype: 0x%x", device_get_atype());
	    break;
    }

    device_set_mode(0);

    if (!(audio_decoder->AudioFmtCtx = avformat_alloc_context())) {
	Error("codec: can't allocate audio AV Format Context");
	goto error_avformat_alloc_context;
    }

    avio_ctx = avio_alloc_context(avio_ctx_buffer, alloc_size, 0, audio_decoder, &device_read_audio_data, NULL, NULL);
    if (!avio_ctx) {
	Error("codec: can't allocate audio AV IO Context");
	goto error_avio_alloc_context;
    }
    // From now on the ctx_buffer is controlled (and freed) by avio_ctx
    avio_ctx_buffer = NULL;

    audio_decoder->AudioFmtCtx->pb = avio_ctx;
    if (codec_id != AV_CODEC_ID_NONE)
	audio_decoder->AudioFmtCtx->audio_codec_id = codec_id;

    av_dict_set_int(&options, "analyzeduration", 500, 0);
    av_dict_set_int(&options, "probesize", alloc_size / 2 , 0);

    input_format = av_find_input_format("mpeg");
    if (!input_format) {
	Error("codec: could not find audio input format. Trying to probe it");
    }
    // This probes things and allocates buffers which cannot block on mutex
    ret = avformat_open_input(&audio_decoder->AudioFmtCtx, NULL, input_format, &options);
    if (ret < 0) {
	Error("codec: can't open audio input: %s", av_err2str(ret));
	goto error_avformat_open_input;
    }

    audio_codec = avcodec_find_decoder(codec_id);
    if (!audio_codec) {
	Error("codec: could not find audio codec (ID: 0x%x)", codec_id);
	goto error_avcodec_find_decoder;
    }
    if (!avformat_new_stream(audio_decoder->AudioFmtCtx, audio_codec)) {
	Error("codec: failed to add audio stream to context");
	goto error_avformat_new_stream;
    }

#if 1
    audio_decoder->AudioFmtCtx->streams[0]->codecpar->codec_id = codec_id;
    audio_decoder->AudioFmtCtx->streams[0]->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
#endif
#if 1
    ret = avformat_find_stream_info(audio_decoder->AudioFmtCtx, NULL);
    if (ret < 0) {
	Error("codec: can't find audio stream info: %s", av_err2str(ret));
	goto error_avformat_find_stream_info;
    }
#endif

    av_dump_format(audio_decoder->AudioFmtCtx, 0, "vaapidevice audio", 0);
#if 0
    ret = av_find_best_stream(audio_decoder->AudioFmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec, 0);
    if (ret < 0) {
	Error("codec: can't find best audio stream: %s", av_err2str(ret));
	goto error_avformat_find_best_stream;
    }
#endif
    audio_decoder->AudioCodec = audio_codec;

    if (!(audio_decoder->AudioCtx = avcodec_alloc_context3(audio_codec))) {
	Error("codec: can't allocate audio codec context");
	goto error_avcodec_alloc_context3;
    }

    Debug4("codec: using audio codec ID %#06x (%s)", codec_id, avcodec_get_name(codec_id));

    // FIXME: for software audio_decoder use all cpus, otherwise 1
    audio_decoder->AudioCtx->thread_count = 1;
    // open codec
    if (audio_codec->capabilities & (AV_CODEC_CAP_AUTO_THREADS)) {
	Debug4("codec: auto threads enabled");
	audio_decoder->AudioCtx->thread_count = 0;
    }
    if (audio_codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
	Debug4("codec: audio can use truncated packets");
	// we send only complete frames
	// audio_decoder->AudioCtx->flags |= CODEC_FLAG_TRUNCATED;
    }

    if (CodecDownmix) {
	audio_decoder->AudioCtx->request_channel_layout = AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    // FIXME: import settings
    //av_dict_set(&av_dict, "dmix_mode", "0", 0);
    //av_dict_set(&av_dict, "ltrt_cmixlev", "1.414", 0);
    //av_dict_set(&av_dict, "loro_cmixlev", "1.414", 0);
    if (avcodec_open2(audio_decoder->AudioCtx, audio_codec, &options) < 0) {
	Error("codec: can't open audio codec");
	goto error_avcodec_open;
    }
    //
    //	Prepare frame buffer for decoder
    //
    if (!(audio_decoder->Frame = av_frame_alloc())) {
	Error("codec: can't allocate audio decoder frame buffer");
	goto error_av_frame_alloc;
    }

    Debug4("codec: audio '%s'", audio_decoder->AudioCodec->long_name);

    audio_decoder->SampleRate = 0;
    audio_decoder->Channels = 0;
    audio_decoder->HwSampleRate = 0;
    audio_decoder->HwChannels = 0;
    audio_decoder->LastDelay = 0;

    device_set_mode(1);
    av_dict_free(&options);

    pthread_mutex_unlock(&CodecLockMutex);
    return;

  error_av_frame_alloc:
  error_avcodec_open:
    avcodec_free_context(&audio_decoder->AudioCtx);
  error_avcodec_alloc_context3:
  error_avcodec_find_decoder:
  error_avformat_new_stream:
  error_avformat_find_best_stream:
  error_avformat_find_stream_info:
  error_avformat_open_input:
    av_freep(avio_ctx);
    av_dict_free(&options);
  error_avio_alloc_context:
    avformat_free_context(audio_decoder->AudioFmtCtx);
    audio_decoder->AudioFmtCtx = NULL;
  error_avformat_alloc_context:
    if (avio_ctx_buffer)
	av_freep(&avio_ctx_buffer);
    device_set_mode(1);
    pthread_mutex_unlock(&CodecLockMutex);
}

/**
**	Close audio decoder.
**
**	@param audio_decoder	private audio decoder
*/
void CodecAudioClose(AudioDecoder * audio_decoder)
{
    // FIXME: output any buffered data
    av_frame_free(&audio_decoder->Frame);   // callee does checks

    if (audio_decoder->Resample) {
	swr_free(&audio_decoder->Resample);
    }
    if (audio_decoder->AudioCtx) {
	pthread_mutex_lock(&CodecLockMutex);
	avformat_close_input(&audio_decoder->AudioFmtCtx);
	avcodec_free_context(&audio_decoder->AudioCtx);
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
    Debug4("codec/audio: format change %s %dHz *%d channels%s%s%s%s%s", av_get_sample_fmt_name(audio_ctx->sample_fmt),
	audio_ctx->sample_rate, audio_ctx->channels, (CodecPassthrough & CodecPCM) ? " PCM" : "",
	(CodecPassthrough & CodecMPA) ? " MPA" : "", (CodecPassthrough & CodecAC3) ? " AC-3" : "",
	(CodecPassthrough & CodecEAC3) ? " E-AC-3" : "", CodecPassthrough ? " pass-through" : "");

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

	    Debug4("codec/audio: audio setup error");
	    // FIXME: handle errors
	    audio_decoder->HwChannels = 0;
	    audio_decoder->HwSampleRate = 0;
	    return err;
	}
    }

    Debug4("codec/audio: resample %s %dHz *%d -> %s %dHz *%d", av_get_sample_fmt_name(audio_ctx->sample_fmt),
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
	    Error("codec/audio: decoded data smaller than encoded");
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
	    Error("codec/audio: decoded data smaller than encoded");
	    return -1;
	}
	// check if we must pack multiple packets
	repeat = 1;
	if ((avpkt->data[4] & 0xc0) != 0xc0) {	// fscod
	    static const uint8_t eac3_repeat[4] = { 6, 3, 2, 1 };

	    // fscod2
	    repeat = eac3_repeat[(avpkt->data[4] & 0x30) >> 4];
	}
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
    int corr = 0;
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
	Debug4("codec/audio: initial drift delay %" PRId64 "ms", delay / 90);
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
	Debug4("codec/audio: drift(%6d) %3dms reset", audio_decoder->DriftCorr, drift / 90);
	audio_decoder->LastDelay = 0;
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

    if (audio_decoder->Resample && audio_decoder->DriftCorr) {
	int distance;

	// try workaround for buggy ffmpeg 0.10
	if (abs(audio_decoder->DriftCorr) < 2000) {
	    distance = (pts_diff * audio_decoder->HwSampleRate) / (900 * 1000);
	} else {
	    distance = (pts_diff * audio_decoder->HwSampleRate) / (90 * 1000);
	}
	if (swr_set_compensation(audio_decoder->Resample, audio_decoder->DriftCorr / 10, distance)) {
	    Debug4("codec/audio: swr_set_compensation failed");
	}
    }

    if (!(c++ % 10)) {
	Debug4("codec/audio: drift(%6d) %8dus %5d", audio_decoder->DriftCorr, drift * 1000 / 90, corr);
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
	Debug4("codec/audio: no resample needed");
    }
#endif

    audio_decoder->Resample =
	swr_alloc_set_opts(audio_decoder->Resample, audio_ctx->channel_layout, AV_SAMPLE_FMT_S16,
	audio_decoder->HwSampleRate, audio_ctx->channel_layout, audio_ctx->sample_fmt, audio_ctx->sample_rate, 0,
	NULL);
    if (audio_decoder->Resample) {
	swr_init(audio_decoder->Resample);
    } else {
	Error("codec/audio: can't setup resample");
    }
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

    if (!audio_decoder->AudioCtx) {
	CodecAudioOpen(audio_decoder, AV_CODEC_ID_NONE);
    }

    audio_ctx = audio_decoder->AudioCtx;
    if (!audio_ctx)
	return;

    if (audio_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
	int ret;
	AVPacket pkt[1];
	AVPacket *avpacket = pkt;
	AVFrame *frame = audio_decoder->Frame;

	if (audio_decoder->AudioFmtCtx) {

	    ret = av_read_frame(audio_decoder->AudioFmtCtx, avpacket);
	    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
		Error("codec: read audio frame failed: %s", av_err2str(ret));
		return;
	    } else if (ret == AVERROR_EOF) {
		Debug4("codec: audio received EOF - draining");
		// Sending null packet enters draining mode
		avpacket = NULL;
	    } else if (ret == AVERROR(EAGAIN)) {
		return;
	    } else if (audio_decoder->AudioFmtCtx->streams[pkt->stream_index]->codecpar->codec_type !=
		AVMEDIA_TYPE_AUDIO) {
		return;
	    }
	}

	ret = avcodec_send_packet(audio_ctx, avpacket);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
	    Debug4("codec: sending audio packet failed");
	    return;
	}
	ret = avcodec_receive_frame(audio_ctx, frame);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
	    Debug4("codec: receiving audio frame failed");
	    return;
	} else if (ret == AVERROR_EOF) {
	    Debug4("codec: audio drain completed");
	    CodecAudioClose(audio_decoder);
	    return;
	}

	if (ret >= 0) {
	    // update audio clock
	    if (avpacket->pts != (int64_t) AV_NOPTS_VALUE) {
		CodecAudioSetClock(audio_decoder, avpacket->pts);
	    }
	    // format change
	    if (audio_decoder->Passthrough != CodecPassthrough || audio_decoder->SampleRate != audio_ctx->sample_rate
		|| audio_decoder->Channels != audio_ctx->channels) {
		CodecAudioUpdateFormat(audio_decoder);
	    }
	    if (!audio_decoder->HwSampleRate || !audio_decoder->HwChannels) {
		return;			// unsupported sample format
	    }
	    if (CodecAudioPassthroughHelper(audio_decoder, avpacket)) {
		return;
	    }
	    if (audio_decoder->Resample) {
		uint8_t outbuf[8192 * 2 * 8];
		uint8_t *out[1];

		out[0] = outbuf;
		ret =
		    swr_convert(audio_decoder->Resample, out, sizeof(outbuf) / (2 * audio_decoder->HwChannels),
		    (const uint8_t **)frame->extended_data, frame->nb_samples);
		if (ret > 0) {
		    if (!(audio_decoder->Passthrough & CodecPCM)) {
			CodecReorderAudioFrame((int16_t *) outbuf, ret * 2 * audio_decoder->HwChannels,
			    audio_decoder->HwChannels);
		    }
		    AudioEnqueue(outbuf, ret * 2 * audio_decoder->HwChannels);
		}
		return;
	    }
	}
	av_frame_unref(frame);
    }
}

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

/**
**	Get the audio decoder info.
**
**	@param decoder	audio decoder data
**	@param codec_id audio	codec id
*/
char *CodecAudioGetInfo(AudioDecoder * decoder, int codec_id)
{
    if (decoder) {
	char buffer[255];

	if (snprintf(&buffer[0], sizeof(buffer), " Audio: %s %dHz %d channels", avcodec_get_name(codec_id),
		decoder->SampleRate, decoder->Channels)) {
	    return strdup(buffer);
	}
    }

    return NULL;
}

//----------------------------------------------------------------------------
//  Codec
//----------------------------------------------------------------------------

/**
**	FFMPEG log callback
*/
static void FFmpegLogCallback(void *ptr, int level, const char *fmt, va_list vargs)
{
    if (level >= AV_LOG_VERBOSE)
	Debug9(fmt, vargs);
    else if (level >= AV_LOG_INFO)
	Debug10(fmt, vargs);
    else if (level >= AV_LOG_WARNING)
	Debug11(fmt, vargs);
    else if (level >= AV_LOG_ERROR)
	Debug12(fmt, vargs);
}

/**
**	Codec init
*/
void CodecInit(void)
{
    pthread_mutex_init(&CodecLockMutex, NULL);
    av_log_set_level(AV_LOG_VERBOSE);
    av_log_set_callback(FFmpegLogCallback);
    avcodec_register_all();		// register all formats and codecs
}

/**
**	Codec exit.
*/
void CodecExit(void)
{
    pthread_mutex_destroy(&CodecLockMutex);
}
