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
    AVBufferRef *hw_device_ctx;

    Debug(3, "codec: using video codec ID %#06x (%s)", codec_id, avcodec_get_name(codec_id));

    if (decoder->VideoCtx) {
	Error("codec: missing close");
    }

    if (!(video_codec = avcodec_find_decoder(codec_id))) {
	Fatal("codec: codec ID %#06x not found", codec_id);
	// FIXME: none fatal
    }
    decoder->VideoCodec = video_codec;

    if (!(decoder->VideoCtx = avcodec_alloc_context3(video_codec))) {
	Fatal("codec: can't allocate video codec context");
    }

    if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, X11DisplayName, NULL, 0)) {
	Fatal("codec: can't allocate HW video codec context");
    }
    decoder->VideoCtx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    // FIXME: for software decoder use all cpus, otherwise 1
    decoder->VideoCtx->thread_count = 1;
    pthread_mutex_lock(&CodecLockMutex);
    // open codec
    if (video_codec->capabilities & (AV_CODEC_CAP_AUTO_THREADS)) {
	Debug(3, "Auto threads enabled");
	decoder->VideoCtx->thread_count = 0;
    }

    if (avcodec_open2(decoder->VideoCtx, video_codec, NULL) < 0) {
	pthread_mutex_unlock(&CodecLockMutex);
	Fatal("codec: can't open video codec!");
    }
    pthread_mutex_unlock(&CodecLockMutex);

    decoder->VideoCtx->opaque = decoder;    // our structure

    Debug(3, "codec: video '%s'", decoder->VideoCodec->long_name);
    if (video_codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
	Debug(3, "codec: video can use truncated packets");
    }
    // FIXME: own memory management for video frames.
    if (video_codec->capabilities & AV_CODEC_CAP_DR1) {
	Debug(3, "codec: can use own buffer management");
    }
    if (video_codec->capabilities & AV_CODEC_CAP_FRAME_THREADS) {
	Debug(3, "codec: codec supports frame threads");
    }
    decoder->VideoCtx->get_format = Codec_get_format;
    decoder->VideoCtx->get_buffer2 = Codec_get_buffer2;
    decoder->VideoCtx->draw_horiz_band = NULL;

    av_opt_set_int(decoder->VideoCtx, "refcounted_frames", 1, 0);

    //
    //	Prepare frame buffer for decoder
    //
    if (!(decoder->Frame = av_frame_alloc())) {
	Fatal("codec: can't allocate video decoder frame buffer");
    }
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
	avcodec_close(video_decoder->VideoCtx);
	av_freep(&video_decoder->VideoCtx);
	pthread_mutex_unlock(&CodecLockMutex);
    }

    av_buffer_unref(&video_decoder->HwDeviceContext);
}

/**
**	Decode a video packet.
**
**	@param decoder	video decoder data
**	@param avpkt	video packet
*/
void CodecVideoDecode(VideoDecoder * decoder, const AVPacket * avpkt)
{
    AVCodecContext *video_ctx = decoder->VideoCtx;

    if (video_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
	int ret;
	AVPacket pkt[1];
	AVFrame *frame = decoder->Frame;

	*pkt = *avpkt;			// use copy
	ret = avcodec_send_packet(video_ctx, pkt);
	if (ret < 0) {
	    Debug(3, "codec: sending video packet failed");
	    return;
	}
	ret = avcodec_receive_frame(video_ctx, frame);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
	    Debug(3, "codec: receiving video frame failed");
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
    if (!(audio_decoder->Frame = av_frame_alloc())) {
	Fatal("codec: can't allocate audio decoder frame buffer");
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
void CodecAudioOpen(AudioDecoder * audio_decoder, int codec_id)
{
    AVCodec *audio_codec;

    Debug(3, "codec: using audio codec ID %#06x (%s)", codec_id, avcodec_get_name(codec_id));

    if (!(audio_codec = avcodec_find_decoder(codec_id))) {
	Fatal("codec: codec ID %#06x not found", codec_id);
	// FIXME: errors aren't fatal
    }
    audio_decoder->AudioCodec = audio_codec;

    if (!(audio_decoder->AudioCtx = avcodec_alloc_context3(audio_codec))) {
	Fatal("codec: can't allocate audio codec context");
    }

    if (CodecDownmix) {
	audio_decoder->AudioCtx->request_channel_layout = AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    pthread_mutex_lock(&CodecLockMutex);
    // open codec
    {
	AVDictionary *av_dict = NULL;

	// FIXME: import settings
	//av_dict_set(&av_dict, "dmix_mode", "0", 0);
	//av_dict_set(&av_dict, "ltrt_cmixlev", "1.414", 0);
	//av_dict_set(&av_dict, "loro_cmixlev", "1.414", 0);
	if (avcodec_open2(audio_decoder->AudioCtx, audio_codec, &av_dict) < 0) {
	    pthread_mutex_unlock(&CodecLockMutex);
	    Fatal("codec: can't open audio codec");
	}
	av_dict_free(&av_dict);
    }
    pthread_mutex_unlock(&CodecLockMutex);
    Debug(3, "codec: audio '%s'", audio_decoder->AudioCodec->long_name);

    if (audio_codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
	Debug(3, "codec: audio can use truncated packets");
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
    if (audio_decoder->Resample) {
	swr_free(&audio_decoder->Resample);
    }
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
    Debug(3, "codec/audio: format change %s %dHz *%d channels%s%s%s%s%s",
	av_get_sample_fmt_name(audio_ctx->sample_fmt), audio_ctx->sample_rate, audio_ctx->channels,
	(CodecPassthrough & CodecPCM) ? " PCM" : "", (CodecPassthrough & CodecMPA) ? " MPA" : "",
	(CodecPassthrough & CodecAC3) ? " AC-3" : "", (CodecPassthrough & CodecEAC3) ? " E-AC-3" : "",
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

	    Debug(3, "codec/audio: audio setup error");
	    // FIXME: handle errors
	    audio_decoder->HwChannels = 0;
	    audio_decoder->HwSampleRate = 0;
	    return err;
	}
    }

    Debug(3, "codec/audio: resample %s %dHz *%d -> %s %dHz *%d", av_get_sample_fmt_name(audio_ctx->sample_fmt),
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
	Debug(3, "codec/audio: inital drift delay %" PRId64 "ms", delay / 90);
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
	Debug(3, "codec/audio: drift(%6d) %3dms reset", audio_decoder->DriftCorr, drift / 90);
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
	    Debug(3, "codec/audio: swr_set_compensation failed");
	}
    }

    if (!(c++ % 10)) {
	Debug(3, "codec/audio: drift(%6d) %8dus %5d", audio_decoder->DriftCorr, drift * 1000 / 90, corr);
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
	Debug(4, "no resample needed");
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
    AVCodecContext *audio_ctx = audio_decoder->AudioCtx;

    if (audio_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
	int ret;
	AVPacket pkt[1];
	AVFrame *frame = audio_decoder->Frame;

	av_frame_unref(frame);
	*pkt = *avpkt;			// use copy
	ret = avcodec_send_packet(audio_ctx, pkt);
	if (ret < 0) {
	    Debug(3, "codec: sending audio packet failed");
	    return;
	}
	ret = avcodec_receive_frame(audio_ctx, frame);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
	    Debug(3, "codec: receiving audio frame failed");
	    return;
	}
	if (ret >= 0) {
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
		return;			// unsupported sample format
	    }
	    if (CodecAudioPassthroughHelper(audio_decoder, avpkt)) {
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
