///
///	@file softhddev.c	@brief A software HD device plugin for VDR.
///
///	Copyright (c) 2011, 2012 by Johns.  All Rights Reserved.
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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#include <libavcodec/avcodec.h>

#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>
#ifdef USE_JPEG
#include <jpeglib.h>
#endif

#include "misc.h"
#include "softhddev.h"

#include "audio.h"
#include "video.h"
#include "codec.h"

//////////////////////////////////////////////////////////////////////////////
//	Variables
//////////////////////////////////////////////////////////////////////////////

#ifdef USE_VDPAU
static char ConfigVdpauDecoder = 1;	///< use vdpau decoder, if possible
#else
#define ConfigVdpauDecoder 0		///< no vdpau decoder configured
#endif

static char ConfigFullscreen;		///< fullscreen modus
static char ConfigStartSuspended;	///< flag to start in suspend mode
static char ConfigStartX11Server;	///< flag start the x11 server

static pthread_mutex_t SuspendLockMutex;	///< suspend lock mutex

static volatile char VideoFreezed;	///< video freezed

//////////////////////////////////////////////////////////////////////////////
//	Audio
//////////////////////////////////////////////////////////////////////////////

static volatile char NewAudioStream;	///< new audio stream
static volatile char SkipAudio;		///< skip audio stream
static AudioDecoder *MyAudioDecoder;	///< audio decoder
static enum CodecID AudioCodecID;	///< current codec id

/**
**	mpeg bitrate table.
**
**	BitRateTable[Version][Layer][Index]
*/
static const uint16_t BitRateTable[2][4][16] = {
    // MPEG Version 1
    {{},
	{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448,
	    0},
	{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
	{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
    // MPEG Version 2 & 2.5
    {{},
	{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
	{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
	{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}
	}
};

/**
**	mpeg samperate table.
*/
static const uint16_t SampleRateTable[4] = {
    44100, 48000, 32000, 0
};

/**
**	Find sync in audio packet.
**
**	@param avpkt	audio packet
**
**	From: http://www.mpgedit.org/mpgedit/mpeg_format/mpeghdr.htm
**
**	AAAAAAAA AAABBCCD EEEEFFGH IIJJKLMM
**
**	o a 11x	frame sync
**	o b 2x	mpeg audio version (2.5, reserved, 2, 1)
**	o c 2x	layer (reserved, III, II, I)
**	o e 2x	BitRate index
**	o f 2x	SampleRate index
**	o g 1x	Paddding bit
**	o ..	doesn't care
**
**	frame length:
**	Layer I:
**		FrameLengthInBytes = (12 * BitRate / SampleRate + Padding) * 4
**	Layer II & III:
**		FrameLengthInBytes = 144 * BitRate / SampleRate + Padding
**
**	@todo sometimes detects wrong position
*/
static int FindAudioSync(const AVPacket * avpkt)
{
    int i;
    const uint8_t *data;

    i = 0;
    data = avpkt->data;
    while (i < avpkt->size - 4) {
	if (data[i] == 0xFF && (data[i + 1] & 0xFC) == 0xFC) {
	    int mpeg2;
	    int mpeg25;
	    int layer;
	    int bit_rate_index;
	    int sample_rate_index;
	    int padding;
	    int bit_rate;
	    int sample_rate;
	    int frame_size;

	    mpeg2 = !(data[i + 1] & 0x08) && (data[i + 1] & 0x10);
	    mpeg25 = !(data[i + 1] & 0x08) && !(data[i + 1] & 0x10);
	    layer = 4 - ((data[i + 1] >> 1) & 0x03);
	    bit_rate_index = (data[i + 2] >> 4) & 0x0F;
	    sample_rate_index = (data[i + 2] >> 2) & 0x03;
	    padding = (data[i + 2] >> 1) & 0x01;

	    sample_rate = SampleRateTable[sample_rate_index];
	    if (!sample_rate) {		// no valid sample rate try next
		i++;
		continue;
	    }
	    sample_rate >>= mpeg2;	// mpeg 2 half rate
	    sample_rate >>= mpeg25;	// mpeg 2.5 quarter rate

	    bit_rate = BitRateTable[mpeg2 | mpeg25][layer][bit_rate_index];
	    bit_rate *= 1000;
	    switch (layer) {
		case 1:
		    frame_size = (12 * bit_rate) / sample_rate;
		    frame_size = (frame_size + padding) * 4;
		    break;
		case 2:
		case 3:
		default:
		    frame_size = (144 * bit_rate) / sample_rate;
		    frame_size = frame_size + padding;
		    break;
	    }
	    Debug(3,
		"audio: mpeg%s layer%d bitrate=%d samplerate=%d %d bytes\n",
		mpeg25 ? "2.5" : mpeg2 ? "2" : "1", layer, bit_rate,
		sample_rate, frame_size);
	    if (i + frame_size < avpkt->size - 4) {
		if (data[i + frame_size] == 0xFF
		    && (data[i + frame_size + 1] & 0xFC) == 0xFC) {
		    Debug(3, "audio: mpeg1/2 found at %d\n", i);
		    return i;
		}
	    }
	    // no valid frame size or no continuation, try next
	}
	i++;
    }
    return -1;
}

/**
**	Play audio packet.
**
**	@param data	data of exactly one complete PES packet
**	@param size	size of PES packet
**	@param id	PES packet type
*/
int PlayAudio(const uint8_t * data, int size,
    __attribute__ ((unused)) uint8_t id)
{
    int n;
    int osize;
    AVPacket avpkt[1];

    // channel switch: SetAudioChannelDevice: SetDigitalAudioDevice:

    if (VideoFreezed) {			// video freezed
	return 0;
    }
    if (SkipAudio || !MyAudioDecoder) {	// skip audio
	return size;
    }
    if (NewAudioStream) {
	// FIXME: does this clear the audio ringbuffer?
	CodecAudioClose(MyAudioDecoder);
	AudioCodecID = CODEC_ID_NONE;
	NewAudioStream = 0;
    }
    // PES header 0x00 0x00 0x01 ID
    // ID 0xBD 0xC0-0xCF

    if (size < 9) {
	Error(_("[softhddev] invalid audio packet\n"));
	return size;
    }
    // Don't overrun audio buffers on replay
    if (AudioFreeBytes() < 3072 * 8 * 8) {	// 8 channels 8 packets
	return 0;
    }

    n = data[8];			// header size

    av_init_packet(avpkt);
    if (data[7] & 0x80 && n >= 5) {
	avpkt->pts =
	    (int64_t) (data[9] & 0x0E) << 29 | data[10] << 22 | (data[11] &
	    0xFE) << 14 | data[12] << 7 | (data[13] & 0xFE) >> 1;
	//Debug(3, "audio: pts %#012" PRIx64 "\n", avpkt->pts);
    }
    if (0) {				// dts is unused
	if (data[7] & 0x40) {
	    avpkt->dts =
		(int64_t) (data[14] & 0x0E) << 29 | data[15] << 22 | (data[16]
		& 0xFE) << 14 | data[17] << 7 | (data[18] & 0xFE) >> 1;
	    Debug(3, "audio: dts %#012" PRIx64 "\n", avpkt->dts);
	}
    }

    osize = size;
    data += 9 + n;
    size -= 9 + n;			// skip pes header
    if (size <= 0) {
	Error(_("[softhddev] invalid audio packet\n"));
	return osize;
    }
    // Detect audio code
    // MPEG-PS mp2 MPEG1, MPEG2, AC3, LPCM, AAC LATM

    // Syncword - 0x0B77
    if (data[0] == 0x0B && data[1] == 0x77) {
	if (AudioCodecID != CODEC_ID_AC3) {
	    Debug(3, "[softhddev]%s: AC-3 %d\n", __FUNCTION__, id);
	    CodecAudioClose(MyAudioDecoder);
	    CodecAudioOpen(MyAudioDecoder, NULL, CODEC_ID_AC3);
	    AudioCodecID = CODEC_ID_AC3;
	}
	// Syncword - 0xFFFC - 0xFFFF
    } else if (data[0] == 0xFF && (data[1] & 0xFC) == 0xFC) {
	if (AudioCodecID != CODEC_ID_MP2) {
	    Debug(3, "[softhddev]%s: MP2 %d\n", __FUNCTION__, id);
	    CodecAudioClose(MyAudioDecoder);
	    CodecAudioOpen(MyAudioDecoder, NULL, CODEC_ID_MP2);
	    AudioCodecID = CODEC_ID_MP2;
	}
	// latm header 0x56E0 11bits: 0x2B7
    } else if (data[0] == 0x56 && (data[1] & 0xE0) == 0xE0) {
	if (AudioCodecID != CODEC_ID_AAC_LATM) {
	    Debug(3, "[softhddev]%s: AAC LATM %d\n", __FUNCTION__, id);
	    CodecAudioClose(MyAudioDecoder);
	    CodecAudioOpen(MyAudioDecoder, NULL, CODEC_ID_AAC_LATM);
	    AudioCodecID = CODEC_ID_AAC_LATM;
	}
	// Private stream + LPCM ID
    } else if (data[-n - 9 + 3] == 0xBD && data[0] == 0xA0) {
	if (AudioCodecID != CODEC_ID_PCM_DVD) {
	    static int samplerates[] = { 48000, 96000, 44100, 32000 };
	    int samplerate;
	    int channels;
	    int bits_per_sample;

	    Debug(3, "[softhddev]%s: LPCM %d sr:%d bits:%d chan:%d\n",
		__FUNCTION__, id, data[5] >> 4,
		(((data[5] >> 6) & 0x3) + 4) * 4, (data[5] & 0x7) + 1);
	    CodecAudioClose(MyAudioDecoder);

	    bits_per_sample = (((data[5] >> 6) & 0x3) + 4) * 4;
	    if (bits_per_sample != 16) {
		Error(_
		    ("softhddev: LPCM %d bits per sample aren't supported\n"),
		    bits_per_sample);
		// FIXME: handle unsupported formats.
	    }
	    samplerate = samplerates[data[5] >> 4];
	    channels = (data[5] & 0x7) + 1;
	    AudioSetup(&samplerate, &channels, 0);
	    if (samplerate != samplerates[data[5] >> 4]) {
		Error(_("softhddev: LPCM %d sample-rate is unsupported\n"),
		    samplerates[data[5] >> 4]);
		// FIXME: support resample
	    }
	    if (channels != (data[5] & 0x7) + 1) {
		Error(_("softhddev: LPCM %d channels are unsupported\n"),
		    (data[5] & 0x7) + 1);
		// FIXME: support resample
	    }
	    //CodecAudioOpen(MyAudioDecoder, NULL, CODEC_ID_PCM_DVD);
	    AudioCodecID = CODEC_ID_PCM_DVD;
	}
    } else {
	// no start package
	// FIXME: Nick/Viva sends this shit, need to find sync in packet
	// FIXME: otherwise it takes too long until sound appears

	if (AudioCodecID == CODEC_ID_NONE) {
	    Debug(3, "[softhddev]%s: ??? %d\n", __FUNCTION__, id);
	    avpkt->data = (void *)data;
	    avpkt->size = size;
	    n = FindAudioSync(avpkt);
	    if (n < 0) {
		return osize;
	    }

	    avpkt->pts = AV_NOPTS_VALUE;
	    CodecAudioOpen(MyAudioDecoder, NULL, CODEC_ID_MP2);
	    AudioCodecID = CODEC_ID_MP2;
	    data += n;
	    size -= n;
	}
	// no decoder or codec known
	if (AudioCodecID == CODEC_ID_NONE) {
	    return osize;
	}
    }

    if (AudioCodecID == CODEC_ID_PCM_DVD) {
	if (size > 7) {
	    char *buf;

	    if (!(buf = malloc(size - 7))) {
		Error(_("softhddev: out of memory\n"));
	    } else {
		swab(data + 7, buf, size - 7);
		AudioEnqueue(buf, size - 7);
		free(buf);
	    }
	}
    } else {
	avpkt->data = (void *)data;
	avpkt->size = size;
	CodecAudioDecode(MyAudioDecoder, avpkt);
    }

    return osize;
}

/**
**	Mute audio device.
*/
void Mute(void)
{
    SkipAudio = 1;
    //AudioSetVolume(0);
}

/**
**	Set volume of audio device.
**
**	@param volume	VDR volume (0 .. 255)
*/
void SetVolumeDevice(int volume)
{
    AudioSetVolume((volume * 100) / 255);
}

//////////////////////////////////////////////////////////////////////////////
//	Video
//////////////////////////////////////////////////////////////////////////////

#include <alsa/iatomic.h>		// portable atomic_t

uint32_t VideoSwitch;			///< debug video switch ticks
static volatile char NewVideoStream;	///< flag new video stream
static VideoHwDecoder *MyHwDecoder;	///< video hw decoder
static VideoDecoder *MyVideoDecoder;	///< video decoder
static enum CodecID VideoCodecID;	///< current codec id

static const char *X11DisplayName;	///< x11 display name
static volatile char Usr1Signal;	///< true got usr1 signal

    /// video PES buffer default size
#define VIDEO_BUFFER_SIZE (512 * 1024)
#define VIDEO_PACKET_MAX 128		///< max number of video packets
    /// video PES packet ring buffer
static AVPacket VideoPacketRb[VIDEO_PACKET_MAX];
static int VideoPacketWrite;		///< write pointer
static int VideoPacketRead;		///< read pointer
atomic_t VideoPacketsFilled;		///< how many of the buffer is used
static volatile char VideoClearBuffers;	///< clear video buffers
static volatile char SkipVideo;		///< skip video

#ifdef DEBUG
static int VideoMaxPacketSize;		///< biggest used packet buffer
#endif

/**
**	Initialize video packet ringbuffer.
*/
static void VideoPacketInit(void)
{
    int i;

    for (i = 0; i < VIDEO_PACKET_MAX; ++i) {
	AVPacket *avpkt;

	avpkt = &VideoPacketRb[i];
	// build a clean ffmpeg av packet
	if (av_new_packet(avpkt, VIDEO_BUFFER_SIZE)) {
	    Fatal(_("[softhddev]: out of memory\n"));
	}
	avpkt->priv = NULL;
    }

    atomic_set(&VideoPacketsFilled, 0);
    VideoPacketRead = VideoPacketWrite = 0;
}

/**
**	Cleanup video packet ringbuffer.
*/
static void VideoPacketExit(void)
{
    int i;

    atomic_set(&VideoPacketsFilled, 0);

    for (i = 0; i < VIDEO_PACKET_MAX; ++i) {
	av_free_packet(&VideoPacketRb[i]);
    }
}

/**
**	Place video data in packet ringbuffer.
**
**	@param pts	presentation timestamp of pes packet
**	@param data	data of pes packet
**	@param data	size of pes packet
*/
static void VideoEnqueue(int64_t pts, const void *data, int size)
{
    AVPacket *avpkt;

    // Debug(3, "video: enqueue %d\n", size);

    avpkt = &VideoPacketRb[VideoPacketWrite];
    if (!avpkt->stream_index) {		// add pts only for first added
	avpkt->pts = pts;
    }
    if (avpkt->stream_index + size >= avpkt->size) {

	Warning(_("video: packet buffer too small for %d\n"),
	    avpkt->stream_index + size);

	// new + grow reserves FF_INPUT_BUFFER_PADDING_SIZE
	av_grow_packet(avpkt, ((size + VIDEO_BUFFER_SIZE / 2)
		/ (VIDEO_BUFFER_SIZE / 2)) * (VIDEO_BUFFER_SIZE / 2));
#ifdef DEBUG
	if (avpkt->size <= avpkt->stream_index + size) {
	    fprintf(stderr, "%d %d %d\n", avpkt->size, avpkt->stream_index,
		size);
	    fflush(stderr);
	    abort();
	}
#endif
    }

    memcpy(avpkt->data + avpkt->stream_index, data, size);
    avpkt->stream_index += size;
#ifdef DEBUG
    if (avpkt->stream_index > VideoMaxPacketSize) {
	VideoMaxPacketSize = avpkt->stream_index;
	Debug(3, "video: max used PES packet size: %d\n", VideoMaxPacketSize);
    }
#endif
}

/**
**	Finish current packet advance to next.
**
**	@param codec_id	codec id of packet (MPEG/H264)
*/
static void VideoNextPacket(int codec_id)
{
    AVPacket *avpkt;

    avpkt = &VideoPacketRb[VideoPacketWrite];
    if (!avpkt->stream_index) {		// ignore empty packets
	if (codec_id != CODEC_ID_NONE) {
	    return;
	}
	Debug(3, "video: possible stream change loss\n");
    }

    if (atomic_read(&VideoPacketsFilled) >= VIDEO_PACKET_MAX - 1) {
	// no free slot available drop last packet
	Error(_("video: no empty slot in packet ringbuffer\n"));
	avpkt->stream_index = 0;
	if (codec_id == CODEC_ID_NONE) {
	    Debug(3, "video: possible stream change loss\n");
	}
	return;
    }
    // clear area for decoder, always enough space allocated
    memset(avpkt->data + avpkt->stream_index, 0, FF_INPUT_BUFFER_PADDING_SIZE);

    avpkt->priv = (void *)(size_t) codec_id;

    // advance packet write
    VideoPacketWrite = (VideoPacketWrite + 1) % VIDEO_PACKET_MAX;
    atomic_inc(&VideoPacketsFilled);

    VideoDisplayWakeup();

    // intialize next package to use
    avpkt = &VideoPacketRb[VideoPacketWrite];
    avpkt->stream_index = 0;
    avpkt->pts = AV_NOPTS_VALUE;
    avpkt->dts = AV_NOPTS_VALUE;
}

/**
**	Fix packet for FFMpeg.
**
**	Some tv-stations sends mulitple pictures in a singe PES packet.
**	Current ffmpeg 0.10 and libav-0.8 has problems with this.
**	Split the packet into single picture packets.
*/
void FixPacketForFFMpeg(VideoDecoder * MyVideoDecoder, AVPacket * avpkt)
{
    uint8_t *p;
    int n;
    AVPacket tmp[1];
    int first;

    p = avpkt->data;
    n = avpkt->size;
    *tmp = *avpkt;

    first = 1;
    while (n > 4) {
	// scan for picture header 0x00000100
	if (!p[0] && !p[1] && p[2] == 0x01 && !p[3]) {
	    if (first) {
		first = 0;
		n -= 4;
		p += 4;
		continue;
	    }
	    // packet has already an picture header
	    tmp->size = p - tmp->data;
	    CodecVideoDecode(MyVideoDecoder, tmp);
	    // time-stamp only valid for first packet
	    tmp->pts = AV_NOPTS_VALUE;
	    tmp->dts = AV_NOPTS_VALUE;
	    tmp->data = p;
	    tmp->size = n;
	}
	--n;
	++p;
    }

    CodecVideoDecode(MyVideoDecoder, tmp);
}

/**
**	Decode from PES packet ringbuffer.
*/
int VideoDecode(void)
{
    int filled;
    AVPacket *avpkt;
    int saved_size;
    static int last_codec_id = CODEC_ID_NONE;

    if (VideoFreezed) {
	return 1;
    }
    if (VideoClearBuffers) {
	atomic_set(&VideoPacketsFilled, 0);
	VideoPacketRead = VideoPacketWrite;
	if (MyVideoDecoder) {
	    CodecVideoFlushBuffers(MyVideoDecoder);
	}
	VideoClearBuffers = 0;
	return 1;
    }

    filled = atomic_read(&VideoPacketsFilled);
    if (!filled) {
	return -1;
    }
#if 0
    // FIXME: flush buffers, if close is in the queue
    while (filled) {
	avpkt = &VideoPacketRb[VideoPacketRead];
	if ((int)(size_t) avpkt->priv == CODEC_ID_NONE) {
	}
    }
#endif
    avpkt = &VideoPacketRb[VideoPacketRead];

    //
    //	handle queued commands
    //
    switch ((int)(size_t) avpkt->priv) {
	case CODEC_ID_NONE:
	    if (last_codec_id != CODEC_ID_NONE) {
		last_codec_id = CODEC_ID_NONE;
		CodecVideoClose(MyVideoDecoder);
		goto skip;
	    }
	    // size can be zero
	    goto skip;
	case CODEC_ID_MPEG2VIDEO:
	    if (last_codec_id != CODEC_ID_MPEG2VIDEO) {
		last_codec_id = CODEC_ID_MPEG2VIDEO;
		CodecVideoOpen(MyVideoDecoder,
		    ConfigVdpauDecoder ? "mpegvideo_vdpau" : NULL,
		    CODEC_ID_MPEG2VIDEO);
	    }
	    break;
	case CODEC_ID_H264:
	    if (last_codec_id != CODEC_ID_H264) {
		last_codec_id = CODEC_ID_H264;
		CodecVideoOpen(MyVideoDecoder,
		    ConfigVdpauDecoder ? "h264video_vdpau" : NULL,
		    CODEC_ID_H264);
	    }
	    break;
	default:
	    break;
    }

    // avcodec_decode_video2 needs size
    saved_size = avpkt->size;
    avpkt->size = avpkt->stream_index;
    avpkt->stream_index = 0;

    if (0) {
	static int done;

	if (done < 2) {
	    int fildes;
	    int who_designed_this_is____;

	    if (done == 0)
		fildes =
		    open("frame0.pes", O_WRONLY | O_TRUNC | O_CREAT, 0666);
	    else if (done == 1)
		fildes =
		    open("frame1.pes", O_WRONLY | O_TRUNC | O_CREAT, 0666);
	    else
		fildes =
		    open("frame2.pes", O_WRONLY | O_TRUNC | O_CREAT, 0666);
	    done++;
	    who_designed_this_is____ = write(fildes, avpkt->data, avpkt->size);
	    close(fildes);
	}
    }

    if (last_codec_id == CODEC_ID_MPEG2VIDEO) {
	FixPacketForFFMpeg(MyVideoDecoder, avpkt);
    } else {
	CodecVideoDecode(MyVideoDecoder, avpkt);
    }

    avpkt->size = saved_size;

  skip:
    // advance packet read
    VideoPacketRead = (VideoPacketRead + 1) % VIDEO_PACKET_MAX;
    atomic_dec(&VideoPacketsFilled);

    return 0;
}

/**
**	Try video start.
**
**	NOT TRUE: Could be called, when already started.
*/
static void StartVideo(void)
{
    VideoInit(X11DisplayName);
    if (ConfigFullscreen) {
	// FIXME: not good looking, mapped and then resized.
	VideoSetFullscreen(1);
    }
    VideoOsdInit();
    if (!MyVideoDecoder) {
	if ((MyHwDecoder = VideoNewHwDecoder())) {
	    MyVideoDecoder = CodecVideoNewDecoder(MyHwDecoder);
	}
	VideoCodecID = CODEC_ID_NONE;
    }
    VideoPacketInit();
}

/**
**	Stop video.
*/
static void StopVideo(void)
{
    VideoOsdExit();
    VideoExit();
    if (MyVideoDecoder) {
	// FIXME: this can crash, hw decoder released by video exit
	CodecVideoClose(MyVideoDecoder);
	CodecVideoDelDecoder(MyVideoDecoder);
	MyVideoDecoder = NULL;
    }
    if (MyHwDecoder) {
	// done by exit: VideoDelHwDecoder(MyHwDecoder);
	MyHwDecoder = NULL;
    }
    VideoPacketExit();

    NewVideoStream = 1;
}

#ifdef DEBUG

/**
**	Validate mpeg video packet.
**
**	Function to validate a mpeg packet, not needed.
*/
static int ValidateMpeg(const uint8_t * data, int size)
{
    int pes_l;

    do {
	if (size < 9) {
	    return -1;
	}
	if (data[0] || data[1] || data[2] != 0x01) {
	    printf("%02x: %02x %02x %02x %02x %02x\n", data[-1], data[0],
		data[1], data[2], data[3], data[4]);
	    return -1;
	}

	pes_l = (data[4] << 8) | data[5];
	if (!pes_l) {			// contains unknown length
	    return 1;
	}

	if (6 + pes_l > size) {
	    return -1;
	}

	data += 6 + pes_l;
	size -= 6 + pes_l;
    } while (size);

    return 0;
}
#endif

/**
**	Play video packet.
**
**	@param data	data of exactly one complete PES packet
**	@param size	size of PES packet
**
**	@return number of bytes used, 0 if internal buffer are full.
**
**	@note vdr sends incomplete packets, va-api h264 decoder only
**	supports complete packets.
**	We buffer here until we receive an complete PES Packet, which
**	is no problem, the audio is always far behind us.
**	cTsToPes::GetPes splits the packets.
**
**	@todo FIXME: combine the 5 ifs at start of the function
*/
int PlayVideo(const uint8_t * data, int size)
{
    const uint8_t *check;
    int64_t pts;
    int n;

    if (Usr1Signal) {			// x11 server ready
	Usr1Signal = 0;
	StartVideo();
    }
    if (!MyVideoDecoder) {		// no x11 video started
	return size;
    }
    if (SkipVideo) {			// skip video
	return size;
    }
    if (VideoFreezed) {			// video freezed
	return 0;
    }
    if (NewVideoStream) {		// channel switched
	Debug(3, "video: new stream %d\n", GetMsTicks() - VideoSwitch);
	// FIXME: hack to test results
	if (atomic_read(&VideoPacketsFilled) >= VIDEO_PACKET_MAX - 1) {
	    Debug(3, "video: new video stream lost\n");
	    NewVideoStream = 0;
	    return 0;
	}
	VideoNextPacket(CODEC_ID_NONE);
	VideoCodecID = CODEC_ID_NONE;
	NewVideoStream = 0;
    }
    // must be a PES start code
    if (size < 9 || !data || data[0] || data[1] || data[2] != 0x01) {
	Error(_("[softhddev] invalid PES video packet\n"));
	return size;
    }
    n = data[8];			// header size
    // wrong size
    if (size < 9 + n + 4) {
	Error(_("[softhddev] invalid video packet %d bytes\n"), size);
	return size;
    }
    // buffer full: needed for replay
    if (atomic_read(&VideoPacketsFilled) >= VIDEO_PACKET_MAX - 1) {
	return 0;
    }
    // get pts/dts

    pts = AV_NOPTS_VALUE;
    if (data[7] & 0x80) {
	pts =
	    (int64_t) (data[9] & 0x0E) << 29 | data[10] << 22 | (data[11] &
	    0xFE) << 14 | data[12] << 7 | (data[13] & 0xFE) >> 1;
#ifdef DEBUG
	if (!(data[13] & 1) || !(data[11] & 1) || !(data[9] & 1)) {
	    Error(_("[softhddev] invalid pts in video packet\n"));
	    return size;
	}
	//Debug(3, "video: pts %#012" PRIx64 "\n", pts);
	if (data[13] != (((pts & 0x7F) << 1) | 1)) {
	    abort();
	}
	if (data[12] != ((pts >> 7) & 0xFF)) {
	    abort();
	}
	if (data[11] != ((((pts >> 15) & 0x7F) << 1) | 1)) {
	    abort();
	}
	if (data[10] != ((pts >> 22) & 0xFF)) {
	    abort();
	}
	if ((data[9] & 0x0F) != (((pts >> 30) << 1) | 1)) {
	    abort();
	}
#endif
    }

    check = data + 9 + n;
    if (0) {
	printf("%02x: %02x %02x %02x %02x %02x\n", data[6], check[0], check[1],
	    check[2], check[3], check[4]);
    }
    // FIXME: no valid mpeg2/h264 detection yet
    // FIXME: better skip all zero's >3 && 0x01 0x09 h264, >2 && 0x01 -> mpeg2

    // PES_VIDEO_STREAM 0xE0 or PES start code
    //(data[6] & 0xC0) != 0x80 ||
    if ((!check[0] && !check[1] && check[2] == 0x1)) {
	if (VideoCodecID == CODEC_ID_MPEG2VIDEO) {
	    VideoNextPacket(CODEC_ID_MPEG2VIDEO);
	} else {
	    Debug(3, "video: mpeg2 detected ID %02x\n", check[3]);
	    VideoCodecID = CODEC_ID_MPEG2VIDEO;
	}
#ifdef DEBUG
	if (ValidateMpeg(data, size)) {
	    Debug(3, "softhddev/video: invalid mpeg2 video packet\n");
	}
#endif
	// Access Unit Delimiter
    } else if ((data[6] & 0xC0) == 0x80 && !check[0] && !check[1]
	&& !check[2] && check[3] == 0x1 && check[4] == 0x09) {
	if (VideoCodecID == CODEC_ID_H264) {
	    VideoNextPacket(CODEC_ID_H264);
	} else {
	    Debug(3, "video: h264 detected\n");
	    VideoCodecID = CODEC_ID_H264;
	}
	// Access Unit Delimiter (BBC-HD)
	// FIXME: the 4 offset are try & error selected
    } else if ((data[6] & 0xC0) == 0x80 && !check[4 + 0] && !check[4 + 1]
	&& !check[4 + 2] && check[4 + 3] == 0x1 && check[4 + 4] == 0x09) {
	if (VideoCodecID == CODEC_ID_H264) {
	    VideoNextPacket(CODEC_ID_H264);
	} else {
	    Debug(3, "video: h264 detected\n");
	    VideoCodecID = CODEC_ID_H264;
	}
    } else {
	// this happens when vdr sends incomplete packets
	if (VideoCodecID == CODEC_ID_NONE) {
	    Debug(3, "video: not detected\n");
	    return size;
	}
	// incomplete packets produce artefacts after channel switch
	// packet < 65526 is the last split packet, detect it here for
	// better latency
	if (size < 65526 && VideoCodecID == CODEC_ID_MPEG2VIDEO) {
	    // mpeg codec supports incomplete packets
	    // waiting for a full complete packages, increases needed delays
	    VideoEnqueue(pts, check, size - 9 - n);
	    VideoNextPacket(CODEC_ID_MPEG2VIDEO);
	    return size;
	}
    }

    // SKIP PES header
    VideoEnqueue(pts, check, size - 9 - n);

    return size;
}

#ifdef USE_JPEG

uint8_t *CreateJpeg(uint8_t * image, int raw_size, int *size, int quality,
    int width, int height)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_ptr[1];
    int row_stride;
    uint8_t *outbuf;
    long unsigned int outsize;

    outbuf = NULL;
    outsize = 0;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &outbuf, &outsize);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = raw_size / height / width;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    row_stride = width * 3;
    while (cinfo.next_scanline < cinfo.image_height) {
	row_ptr[0] = &image[cinfo.next_scanline * row_stride];
	jpeg_write_scanlines(&cinfo, row_ptr, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    *size = outsize;

    return outbuf;
}

#endif

/**
**	Grabs the currently visible screen image.
**
**	@param size	size of the returned data
**	@param jpeg	flag true, create JPEG data
**	@param quality	JPEG quality
**	@param width	number of horizontal pixels in the frame
**	@param height	number of vertical pixels in the frame
*/
uint8_t *GrabImage(int *size, int jpeg, int quality, int width, int height)
{
    if (jpeg) {
#ifdef USE_JPEG
	int raw_size;
	uint8_t *image;
	uint8_t *jpg_image;

	raw_size = 0;
	image = VideoGrab(&raw_size, &width, &height, 0);
	jpg_image = CreateJpeg(image, raw_size, size, quality, width, height);
	free(image);
	return jpg_image;
#else
	(void)quality;
	Error(_("softhddev: jpeg grabbing not supported\n"));
	return NULL;
#endif
    }
    if (width != -1 && height != -1) {
	Warning(_("softhddev: scaling unsupported\n"));
    }
    return VideoGrab(size, &width, &height, 1);
}

//////////////////////////////////////////////////////////////////////////////

/**
**	Set play mode, called on channel switch.
*/
void SetPlayMode(void)
{
    if (ConfigStartSuspended) {		// ignore first call, if start suspended
	ConfigStartSuspended = 0;
	return;
    }
    Resume();
    if (MyVideoDecoder) {
	if (VideoCodecID != CODEC_ID_NONE) {
	    NewVideoStream = 1;
	    VideoSwitch = GetMsTicks();
	}
    }
    if (MyAudioDecoder) {
	if (AudioCodecID != CODEC_ID_NONE) {
	    NewAudioStream = 1;
	}
    }
    VideoFreezed = 0;
    // done by Resume: SkipAudio = 0;
    // done by Resume: SkipVideo = 0;
}

/**
**	Clears all video and audio data from the device.
*/
void Clear(void)
{
    int i;

    VideoNextPacket(VideoCodecID);	// terminate work
    VideoClearBuffers = 1;
    // FIXME: avcodec_flush_buffers
    AudioFlushBuffers();

    for (i = 0; VideoClearBuffers && i < 20; ++i) {
	usleep(1 * 1000);
    }
}

/**
**	Sets the device into play mode.
*/
void Play(void)
{
    VideoFreezed = 0;
    SkipAudio = 0;
    // FIXME: restart audio
}

/**
**	Sets the device into "freeze frame" mode.
*/
void Freeze(void)
{
    VideoFreezed = 1;
    // FIXME: freeze audio
    AudioFlushBuffers();
}

/**
**	Display the given I-frame as a still picture.
*/
void StillPicture(const uint8_t * data, int size)
{
    int i;
    static uint8_t seq_end_mpeg[] = { 0x00, 0x00, 0x01, 0xB7 };
    static uint8_t seq_end_h264[] = { 0x00, 0x00, 0x00, 0x01, 0x10 };

    // must be a PES start code
    if (size < 9 || !data || data[0] || data[1] || data[2] != 0x01) {
	Error(_("[softhddev] invalid still video packet\n"));
	return;
    }
    if (VideoCodecID == CODEC_ID_NONE) {
	// FIXME: should detect codec, see PlayVideo
	Error(_("[softhddev] no codec known for still picture\n"));
	return;
    }
    //Clear();				// flush video buffers

    // +1 future for deinterlace
    for (i = -1; i < (VideoCodecID == CODEC_ID_MPEG2VIDEO ? 3 : 17); ++i) {
	//if ( 1 ) {
	const uint8_t *split;
	int n;

	// split the I-frame into single pes packets
	split = data;
	n = size;
	do {
	    int len;

	    len = (split[4] << 8) + split[5];
	    if (len > n) {
		break;
	    }
	    PlayVideo(split, len + 6);	// feed it
	    split += 6 + len;
	    n -= 6 + len;
	} while (n > 6);
	VideoNextPacket(VideoCodecID);	// terminate last packet

	if (VideoCodecID == CODEC_ID_H264) {
	    VideoEnqueue(AV_NOPTS_VALUE, seq_end_h264, sizeof(seq_end_h264));
	} else {
	    VideoEnqueue(AV_NOPTS_VALUE, seq_end_mpeg, sizeof(seq_end_mpeg));
	}
	VideoNextPacket(VideoCodecID);	// terminate last packet
    }
}

/**
**	Poll if device is ready.  Called by replay.
**
**	@param timeout	timeout to become ready in ms
*/
int Poll(int timeout)
{
    // buffers are too full
    if (atomic_read(&VideoPacketsFilled) >= VIDEO_PACKET_MAX / 2) {
	if (timeout) {
	    // let display thread work
	    usleep(timeout * 1000);
	}
	return atomic_read(&VideoPacketsFilled) < VIDEO_PACKET_MAX / 2;
    }
    return 0;
}

/**
**	Flush the device output buffers.
**
**	@param timeout	timeout to flush in ms
*/
int Flush(int timeout)
{
    if (atomic_read(&VideoPacketsFilled)) {
	if (timeout) {			// let display thread work
	    usleep(timeout * 1000);
	}
	return !atomic_read(&VideoPacketsFilled);
    }
    return 1;
}

//////////////////////////////////////////////////////////////////////////////
//	OSD
//////////////////////////////////////////////////////////////////////////////

/**
**	Get OSD size and aspect.
*/
void GetOsdSize(int *width, int *height, double *aspect)
{
#ifdef DEBUG
    static int done_width;
    static int done_height;
#endif

    VideoGetOsdSize(width, height);
    *aspect = 16.0 / 9.0 / (double)*width * (double)*height;

#ifdef DEBUG
    if (done_width != *width || done_height != *height) {
	Debug(3, "[softhddev]%s: %dx%d %g\n", __FUNCTION__, *width, *height,
	    *aspect);
	done_width = *width;
	done_height = *height;
    }
#endif
}

/**
**	Close OSD.
*/
void OsdClose(void)
{
    VideoOsdClear();
}

/**
**	Draw an OSD pixmap.
*/
void OsdDrawARGB(int x, int y, int height, int width, const uint8_t * argb)
{
    VideoOsdDrawARGB(x, y, height, width, argb);
}

//////////////////////////////////////////////////////////////////////////////

/**
**	Return command line help string.
*/
const char *CommandLineHelp(void)
{
    return "  -a device\taudio device (fe. alsa: hw:0,0 oss: /dev/dsp)\n"
	"  -p device\taudio device (alsa only) for pass-through (hw:0,1)\n"
	"  -d display\tdisplay of x11 server (fe. :0.0)\n"
	"  -f\t\tstart with fullscreen window (only with window manager)\n"
	"  -g geometry\tx11 window geometry wxh+x+y\n"
	"  -x\t\tstart x11 server\n" "	-s\t\tstart in suspended mode\n"
	"  -w workaround\tenable/disable workarounds\n"
	"\tno-hw-decoder\t\tdisable hw decoder, use software decoder only\n"
	"\tno-mpeg-hw-decoder\tdisable hw decoder for mpeg only\n"
	"\talsa-driver-broken\tdisable broken alsa driver message\n";
}

/**
**	Process the command line arguments.
**
**	@param argc	number of arguments
**	@param argv	arguments vector
*/
int ProcessArgs(int argc, char *const argv[])
{
    //
    //	Parse arguments.
    //
    for (;;) {
	switch (getopt(argc, argv, "-a:d:fg:p:sw:x")) {
	    case 'a':			// audio device
		AudioSetDevice(optarg);
		continue;
	    case 'p':			// pass-through audio device
		AudioSetDeviceAC3(optarg);
		continue;
	    case 'd':			// x11 display name
		X11DisplayName = optarg;
		continue;
	    case 'f':			// fullscreen mode
		ConfigFullscreen = 1;
		continue;
	    case 'g':			// geometry
		if (VideoSetGeometry(optarg) < 0) {
		    fprintf(stderr,
			_
			("Bad formated geometry please use: [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]\n"));
		    return 0;
		}
		continue;
	    case 'x':			// x11 server
		ConfigStartX11Server = 1;
		continue;
	    case 's':			// start in suspend mode
		ConfigStartSuspended = 1;
		continue;
	    case 'w':			// workarounds
		if (!strcasecmp("no-hw-decoder", optarg)) {
		} else if (!strcasecmp("no-mpeg-hw-decoder", optarg)) {
		} else if (!strcasecmp("alsa-driver-broken", optarg)) {
		    AudioAlsaDriverBroken = 1;
		} else {
		    fprintf(stderr, _("Workaround '%s' unsupported\n"),
			optarg);
		    return 0;
		}
		continue;
	    case EOF:
		break;
	    case '-':
		fprintf(stderr, _("We need no long options\n"));
		return 0;
	    case ':':
		fprintf(stderr, _("Missing argument for option '%c'\n"),
		    optopt);
		return 0;
	    default:
		fprintf(stderr, _("Unkown option '%c'\n"), optopt);
		return 0;
	}
	break;
    }
    while (optind < argc) {
	fprintf(stderr, _("Unhandled argument '%s'\n"), argv[optind++]);
    }

    return 1;
}

//////////////////////////////////////////////////////////////////////////////
//	Init/Exit
//////////////////////////////////////////////////////////////////////////////

#include <sys/types.h>
#include <sys/wait.h>

#define XSERVER_MAX_ARGS 512		///< how many arguments support

static const char *X11Server = "/usr/bin/X";	///< default x11 server
static const char *X11ServerArguments;	///< default command arguments
static pid_t X11ServerPid;		///< x11 server pid

/**
**	USR1 signal handler.
**
**	@param sig	signal number
*/
static void Usr1Handler(int __attribute__ ((unused)) sig)
{
    ++Usr1Signal;

    Debug(3, "x-setup: got signal usr1\n");
}

/**
**	Start the X server
*/
static void StartXServer(void)
{
    struct sigaction usr1;
    pid_t pid;
    const char *sval;
    const char *args[XSERVER_MAX_ARGS];
    int argn;
    char *buf;

    //	X server
    if (X11Server) {
	args[0] = X11Server;
    } else {
	Error(_("x-setup: No X server configured!\n"));
	return;
    }

    argn = 1;
    if (X11DisplayName) {		// append display name
	args[argn++] = X11DisplayName;
    }
    //	split X server arguments string into words
    if ((sval = X11ServerArguments)) {
	char *s;

	s = buf = strdupa(sval);
	while ((sval = strsep(&s, " \t"))) {
	    args[argn++] = sval;

	    if (argn == XSERVER_MAX_ARGS - 1) {	// argument overflow
		Error(_("x-setup: too many arguments for xserver\n"));
		// argn = 1;
		break;
	    }
	}
    }
    // FIXME: auth
    // FIXME: append VTxx
    args[argn] = NULL;

    //	arm the signal
    memset(&usr1, 0, sizeof(struct sigaction));
    usr1.sa_handler = Usr1Handler;
    sigaction(SIGUSR1, &usr1, NULL);

    Debug(3, "x-setup: Starting X server '%s' '%s'\n", args[0],
	X11ServerArguments);
    //	fork
    if ((pid = vfork())) {		// parent

	X11ServerPid = pid;
	Debug(3, "x-setup: Started x-server pid=%d\n", X11ServerPid);

	return;
    }
    // child
    signal(SIGUSR1, SIG_IGN);		// ignore to force answer
    //	start the X server
    execvp(args[0], (char *const *)args);

    Error(_("x-setup: Failed to start X server '%s'\n"), args[0]);
}

/**
**	Exit + cleanup.
*/
void SoftHdDeviceExit(void)
{
    // lets hope that vdr does a good thread cleanup

    AudioExit();
    if (MyAudioDecoder) {
	CodecAudioClose(MyAudioDecoder);
	CodecAudioDelDecoder(MyAudioDecoder);
	MyAudioDecoder = NULL;
    }
    NewAudioStream = 0;

    StopVideo();

    CodecExit();
    VideoPacketExit();

    if (ConfigStartX11Server) {
	Debug(3, "x-setup: Stop x11 server\n");

	if (X11ServerPid) {
	    int waittime;
	    int timeout;
	    pid_t wpid;
	    int status;

	    kill(X11ServerPid, SIGTERM);
	    waittime = 0;
	    timeout = 500;		// 0.5s
	    // wait for x11 finishing, with timeout
	    do {
		wpid = waitpid(X11ServerPid, &status, WNOHANG);
		if (wpid) {
		    break;
		}
		if (waittime++ < timeout) {
		    usleep(1 * 1000);
		    continue;
		}
		kill(X11ServerPid, SIGKILL);
	    } while (waittime < timeout);
	    if (wpid && WIFEXITED(status)) {
		Debug(3, "x-setup: x11 server exited (%d)\n",
		    WEXITSTATUS(status));
	    }
	    if (wpid && WIFSIGNALED(status)) {
		Debug(3, "x-setup: x11 server killed (%d)\n",
		    WTERMSIG(status));
	    }
	}
    }

    pthread_mutex_destroy(&SuspendLockMutex);
}

/**
**	Prepare plugin.
*/
void Start(void)
{
    if (ConfigStartX11Server) {
	StartXServer();
    }
    CodecInit();

    if (!ConfigStartSuspended) {
	// FIXME: AudioInit for HDMI after X11 startup
	AudioInit();
	MyAudioDecoder = CodecAudioNewDecoder();
	AudioCodecID = CODEC_ID_NONE;

	if (!ConfigStartX11Server) {
	    StartVideo();
	}
    } else {
	SkipVideo = 1;
	SkipAudio = 1;
    }
    pthread_mutex_init(&SuspendLockMutex, NULL);
}

/**
**	Stop plugin.
**
**	@note stop everything, but don't cleanup, module is still called.
*/
void Stop(void)
{
#ifdef DEBUG
    Debug(3, "video: max used PES packet size: %d\n", VideoMaxPacketSize);
#endif
}

/**
**	Main thread hook, periodic called from main thread.
*/
void MainThreadHook(void)
{
}

//////////////////////////////////////////////////////////////////////////////
//	Suspend/Resume
//////////////////////////////////////////////////////////////////////////////

/**
**	Suspend plugin.
**
**	@param video	suspend closes video
**	@param audio	suspend closes audio
**	@param dox11	suspend closes x11 server
*/
void Suspend(int video, int audio, int dox11)
{
    pthread_mutex_lock(&SuspendLockMutex);
    if (SkipVideo && SkipAudio) {	// already suspended
	pthread_mutex_unlock(&SuspendLockMutex);
	return;
    }

    Debug(3, "[softhddev]%s:\n", __FUNCTION__);

    SkipVideo = 1;
    SkipAudio = 1;
    pthread_mutex_unlock(&SuspendLockMutex);

    if (audio || video) {
	pthread_mutex_lock(&SuspendLockMutex);

	if (audio) {
	    AudioExit();
	    if (MyAudioDecoder) {
		CodecAudioClose(MyAudioDecoder);
		CodecAudioDelDecoder(MyAudioDecoder);
		MyAudioDecoder = NULL;
	    }
	    NewAudioStream = 0;
	}
	if (video) {
	    StopVideo();
	}

	pthread_mutex_unlock(&SuspendLockMutex);
    }
    if (dox11) {
	// FIXME: stop x11, if started
    }
}

/**
**	Resume plugin.
*/
void Resume(void)
{
    if (!SkipVideo && !SkipAudio) {	// we are not suspended
	return;
    }

    Debug(3, "[softhddev]%s:\n", __FUNCTION__);

    pthread_mutex_lock(&SuspendLockMutex);
    // FIXME: start x11

    if (!MyHwDecoder) {			// video not running
	StartVideo();
    }
    if (!MyAudioDecoder) {		// audio not running
	AudioInit();
	MyAudioDecoder = CodecAudioNewDecoder();
	AudioCodecID = CODEC_ID_NONE;
    }

    SkipVideo = 0;
    SkipAudio = 0;

    pthread_mutex_unlock(&SuspendLockMutex);
}
