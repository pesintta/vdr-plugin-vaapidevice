/// Copyright (C) 2011 - 2015 by Johns. All Rights Reserved.
/// Copyright (C) 2018 by pesintta, rofafor.
///
/// SPDX-License-Identifier: AGPL-3.0-only

#include <sys/types.h>
#include <sys/stat.h>
#ifdef __FreeBSD__
#include <signal.h>
#endif
#include <fcntl.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,64,100)
#error "libavcodec is too old - please, upgrade!"
#endif
#include <libavutil/mem.h>

#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>

#include "iatomic.h"			// portable atomic_t
#include "misc.h"
#include "vaapidevice.h"

#include "audio.h"
#include "video.h"
#include "codec.h"

//////////////////////////////////////////////////////////////////////////////
//  Variables
//////////////////////////////////////////////////////////////////////////////

extern int ConfigAudioBufferTime;	///< config size ms of audio buffer
char ConfigStartX11Server;		///< flag start the x11 server
static signed char ConfigStartSuspended;    ///< flag to start in suspend mode
static char ConfigFullscreen;		///< fullscreen modus
static const char *X11ServerArguments;	///< default command arguments

static pthread_mutex_t SuspendLockMutex;    ///< suspend lock mutex

static volatile char StreamFreezed;	///< stream freezed

int TraceMode = 0;			///< Tracing mode for debugging

//////////////////////////////////////////////////////////////////////////////
//  Audio
//////////////////////////////////////////////////////////////////////////////

static volatile char NewAudioStream;	///< new audio stream
static volatile char SkipAudio;		///< skip audio stream
static AudioDecoder *MyAudioDecoder;	///< audio decoder
static enum AVCodecID AudioCodecID;	///< current codec id
static int AudioChannelID;		///< current audio channel id
static VideoStream *AudioSyncStream;	///< video stream for audio/video sync

    /// Minimum free space in audio buffer 8 packets for 8 channels
#define AUDIO_MIN_BUFFER_FREE (3072 * 8 * 8)
#define AUDIO_BUFFER_SIZE (512 * 1024)	///< audio PES buffer default size
static AVPacket AudioAvPkt[1];		///< audio a/v packet

//////////////////////////////////////////////////////////////////////////////
//  Audio codec parser
//////////////////////////////////////////////////////////////////////////////

///
/// Mpeg bitrate table.
///
/// BitRateTable[Version][Layer][Index]
///
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

///
/// Mpeg samperate table.
///
static const uint16_t SampleRateTable[4] = {
    44100, 48000, 32000, 0
};

///
/// Fast check for Mpeg audio.
///
/// 4 bytes 0xFFExxxxx Mpeg audio
///
static inline int FastMpegCheck(const uint8_t * p)
{
    if (p[0] != 0xFF) {			// 11bit frame sync
	return 0;
    }
    if ((p[1] & 0xE0) != 0xE0) {
	return 0;
    }
    if ((p[1] & 0x18) == 0x08) {	// version ID - 01 reserved
	return 0;
    }
    if (!(p[1] & 0x06)) {		// layer description - 00 reserved
	return 0;
    }
    if ((p[2] & 0xF0) == 0xF0) {	// bitrate index - 1111 reserved
	return 0;
    }
    if ((p[2] & 0x0C) == 0x0C) {	// sampling rate index - 11 reserved
	return 0;
    }
    return 1;
}

///
/// Check for Mpeg audio.
///
/// 0xFFEx already checked.
///
/// @param data incomplete PES packet
/// @param size number of bytes
///
/// @retval <0	possible mpeg audio, but need more data
/// @retval 0	no valid mpeg audio
/// @retval >0	valid mpeg audio
///
/// From: http://www.mpgedit.org/mpgedit/mpeg_format/mpeghdr.htm
///
/// AAAAAAAA AAABBCCD EEEEFFGH IIJJKLMM
///
/// o a 11x Frame sync
/// o b 2x  Mpeg audio version (2.5, reserved, 2, 1)
/// o c 2x  Layer (reserved, III, II, I)
/// o e 2x  BitRate index
/// o f 2x  SampleRate index (4100, 48000, 32000, 0)
/// o g 1x  Paddding bit
/// o ..    Doesn't care
///
/// frame length:
/// Layer I:
/// FrameLengthInBytes = (12 * BitRate / SampleRate + Padding) * 4
/// Layer II & III:
/// FrameLengthInBytes = 144 * BitRate / SampleRate + Padding
///
static int MpegCheck(const uint8_t * data, int size)
{
    int mpeg2;
    int mpeg25;
    int layer;
    int bit_rate_index;
    int sample_rate_index;
    int padding;
    int bit_rate;
    int sample_rate;
    int frame_size;

    mpeg2 = !(data[1] & 0x08) && (data[1] & 0x10);
    mpeg25 = !(data[1] & 0x08) && !(data[1] & 0x10);
    layer = 4 - ((data[1] >> 1) & 0x03);
    bit_rate_index = (data[2] >> 4) & 0x0F;
    sample_rate_index = (data[2] >> 2) & 0x03;
    padding = (data[2] >> 1) & 0x01;

    sample_rate = SampleRateTable[sample_rate_index];
    if (!sample_rate) {			// no valid sample rate try next
	// moved into fast check
	abort();
	return 0;
    }
    sample_rate >>= mpeg2;		// mpeg 2 half rate
    sample_rate >>= mpeg25;		// mpeg 2.5 quarter rate

    bit_rate = BitRateTable[mpeg2 | mpeg25][layer][bit_rate_index];
    if (!bit_rate) {			// no valid bit-rate try next
	// FIXME: move into fast check?
	return 0;
    }
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

    if (frame_size + 4 > size) {
	return -frame_size - 4;
    }
    // check if after this frame a new mpeg frame starts
    if (FastMpegCheck(data + frame_size)) {
	return frame_size;
    }

    return 0;
}

///
/// Fast check for AAC LATM audio.
///
/// 3 bytes 0x56Exxx AAC LATM audio
///
static inline int FastLatmCheck(const uint8_t * p)
{
    if (p[0] != 0x56) {			// 11bit sync
	return 0;
    }
    if ((p[1] & 0xE0) != 0xE0) {
	return 0;
    }
    return 1;
}

///
/// Check for AAC LATM audio.
///
/// 0x56Exxx already checked.
///
/// @param data incomplete PES packet
/// @param size number of bytes
///
/// @retval <0	possible AAC LATM audio, but need more data
/// @retval 0	no valid AAC LATM audio
/// @retval >0	valid AAC LATM audio
///
static int LatmCheck(const uint8_t * data, int size)
{
    int frame_size;

    // 13 bit frame size without header
    frame_size = ((data[1] & 0x1F) << 8) + data[2];
    frame_size += 3;

    if (frame_size + 2 > size) {
	return -frame_size - 2;
    }
    // check if after this frame a new AAC LATM frame starts
    if (FastLatmCheck(data + frame_size)) {
	return frame_size;
    }

    return 0;
}

///
/// Possible AC-3 frame sizes.
///
/// from ATSC A/52 table 5.18 frame size code table.
///
const uint16_t Ac3FrameSizeTable[38][3] = {
    {64, 69, 96}, {64, 70, 96}, {80, 87, 120}, {80, 88, 120},
    {96, 104, 144}, {96, 105, 144}, {112, 121, 168}, {112, 122, 168},
    {128, 139, 192}, {128, 140, 192}, {160, 174, 240}, {160, 175, 240},
    {192, 208, 288}, {192, 209, 288}, {224, 243, 336}, {224, 244, 336},
    {256, 278, 384}, {256, 279, 384}, {320, 348, 480}, {320, 349, 480},
    {384, 417, 576}, {384, 418, 576}, {448, 487, 672}, {448, 488, 672},
    {512, 557, 768}, {512, 558, 768}, {640, 696, 960}, {640, 697, 960},
    {768, 835, 1152}, {768, 836, 1152}, {896, 975, 1344}, {896, 976, 1344},
    {1024, 1114, 1536}, {1024, 1115, 1536}, {1152, 1253, 1728},
    {1152, 1254, 1728}, {1280, 1393, 1920}, {1280, 1394, 1920},
};

///
/// Fast check for (E-)AC-3 audio.
///
/// 5 bytes 0x0B77xxxxxx AC-3 audio
///
static inline int FastAc3Check(const uint8_t * p)
{
    if (p[0] != 0x0B) {			// 16bit sync
	return 0;
    }
    if (p[1] != 0x77) {
	return 0;
    }
    return 1;
}

///
/// Check for (E-)AC-3 audio.
///
/// 0x0B77xxxxxx already checked.
///
/// @param data incomplete PES packet
/// @param size number of bytes
///
/// @retval <0	possible AC-3 audio, but need more data
/// @retval 0	no valid AC-3 audio
/// @retval >0	valid AC-3 audio
///
/// o AC-3 Header
/// AAAAAAAA AAAAAAAA BBBBBBBB BBBBBBBB CCDDDDDD EEEEEFFF
///
/// o a 16x Frame sync, always 0x0B77
/// o b 16x CRC 16
/// o c 2x  Samplerate
/// o d 6x  Framesize code
/// o e 5x  Bitstream ID
/// o f 3x  Bitstream mode
///
/// o E-AC-3 Header
/// AAAAAAAA AAAAAAAA BBCCCDDD DDDDDDDD EEFFGGGH IIIII...
///
/// o a 16x Frame sync, always 0x0B77
/// o b 2x  Frame type
/// o c 3x  Sub stream ID
/// o d 10x Framesize - 1 in words
/// o e 2x  Framesize code
/// o f 2x  Framesize code 2
///
static int Ac3Check(const uint8_t * data, int size)
{
    int frame_size;

    if (size < 5) {			// need 5 bytes to see if AC-3/E-AC-3
	return -5;
    }

    if (data[5] > (10 << 3)) {		// E-AC-3
	if ((data[4] & 0xF0) == 0xF0) { // invalid fscod fscod2
	    return 0;
	}
	frame_size = ((data[2] & 0x03) << 8) + data[3] + 1;
	frame_size *= 2;
    } else {				// AC-3
	int fscod;
	int frmsizcod;

	// crc1 crc1 fscod|frmsizcod
	fscod = data[4] >> 6;
	if (fscod == 0x03) {		// invalid sample rate
	    return 0;
	}
	frmsizcod = data[4] & 0x3F;
	if (frmsizcod > 37) {		// invalid frame size
	    return 0;
	}
	// invalid is checked above
	frame_size = Ac3FrameSizeTable[frmsizcod][fscod] * 2;
    }

    if (frame_size + 5 > size) {
	return -frame_size - 5;
    }
    // FIXME: relaxed checks if codec is already detected
    // check if after this frame a new AC-3 frame starts
    if (FastAc3Check(data + frame_size)) {
	return frame_size;
    }

    return 0;
}

///
/// Fast check for ADTS Audio Data Transport Stream.
///
/// 7/9 bytes 0xFFFxxxxxxxxxxx(xxxx)  ADTS audio
///
static inline int FastAdtsCheck(const uint8_t * p)
{
    if (p[0] != 0xFF) {			// 12bit sync
	return 0;
    }
    if ((p[1] & 0xF6) != 0xF0) {	// sync + layer must be 0
	return 0;
    }
    if ((p[2] & 0x3C) == 0x3C) {	// sampling frequency index != 15
	return 0;
    }
    return 1;
}

///
/// Check for ADTS Audio Data Transport Stream.
///
/// 0xFFF already checked.
///
/// @param data incomplete PES packet
/// @param size number of bytes
///
/// @retval <0	possible ADTS audio, but need more data
/// @retval 0	no valid ADTS audio
/// @retval >0	valid AC-3 audio
///
/// AAAAAAAA AAAABCCD EEFFFFGH HHIJKLMM MMMMMMMM MMMOOOOO OOOOOOPP
/// (QQQQQQQQ QQQQQQQ)
///
/// o A*12  syncword 0xFFF
/// o B*1   MPEG Version: 0 for MPEG-4, 1 for MPEG-2
/// o C*2   layer: always 0
/// o ..
/// o F*4   sampling frequency index (15 is invalid)
/// o ..
/// o M*13  frame length
///
static int AdtsCheck(const uint8_t * data, int size)
{
    int frame_size;

    if (size < 6) {
	return -6;
    }

    frame_size = (data[3] & 0x03) << 11;
    frame_size |= (data[4] & 0xFF) << 3;
    frame_size |= (data[5] & 0xE0) >> 5;

    if (frame_size + 3 > size) {
	return -frame_size - 3;
    }
    // check if after this frame a new ADTS frame starts
    if (FastAdtsCheck(data + frame_size)) {
	return frame_size;
    }

    return 0;
}

/**
**	Set volume of audio device.
**
**	@param volume	VDR volume (0 .. 255)
*/
void SetVolumeDevice(int volume)
{
    AudioSetVolume((volume * 1000) / 255);
}

/**
***	Resets channel ID (restarts audio).
**/
void ResetChannelId(void)
{
    AudioChannelID = -1;
    Debug3("audio/demux: reset channel id");
}

//////////////////////////////////////////////////////////////////////////////
//  Video
//////////////////////////////////////////////////////////////////////////////

#define VIDEO_BUFFER_SIZE (512 * 1024)	///< video PES buffer default size
#define VIDEO_PACKET_MAX 192		///< max number of video packets

/**
**	Video output stream device structure.	Parser, decoder, display.
*/
struct __video_stream__
{
    VideoHwDecoder *HwDecoder;		///< video hardware decoder
    VideoDecoder *Decoder;		///< video decoder
    pthread_mutex_t DecoderLockMutex;	///< video decoder lock mutex

    volatile char SkipStream;		///< skip video stream
    volatile char Freezed;		///< stream freezed

    volatile char TrickSpeed;		///< current trick speed
    volatile char ClearBuffers;		///< command clear video buffers
    volatile char ClearClose;		///< clear video buffers for close
};

static VideoStream MyVideoStream[1];	///< normal video stream

const char *X11DisplayName;		///< x11 display name
static volatile char Usr1Signal;	///< true got usr1 signal

//////////////////////////////////////////////////////////////////////////////

/**
**	Cleanup video packet ringbuffer.
**
**	@param stream	video stream
*/
static void VideoPacketExit(VideoStream * stream)
{

}

/**
**	Open video stream.
**
**	@param stream	video stream
*/
static void VideoStreamOpen(VideoStream * stream)
{
    stream->SkipStream = 1;

    stream->HwDecoder = VideoNewHwDecoder(stream);
    if (stream->HwDecoder) {
	stream->Decoder = CodecVideoNewDecoder(stream->HwDecoder);
	stream->SkipStream = 0;
    }
}

/**
**	Close video stream.
**
**	@param stream	video stream
**	@param delhw	flag delete hardware decoder
**
**	@note must be called from the video thread, otherwise xcb has a
**	deadlock.
*/
static void VideoStreamClose(VideoStream * stream, int delhw)
{
    stream->SkipStream = 1;
    if (stream->Decoder) {
	VideoDecoder *decoder;

	decoder = stream->Decoder;
	// FIXME: remove this lock for main stream close
	pthread_mutex_lock(&stream->DecoderLockMutex);
	stream->Decoder = NULL;		// lock read thread
	pthread_mutex_unlock(&stream->DecoderLockMutex);
	CodecVideoClose(decoder);
	CodecVideoDelDecoder(decoder);
    }
    if (stream->HwDecoder) {
	if (delhw) {
	    VideoDelHwDecoder(stream->HwDecoder);
	}
	stream->HwDecoder = NULL;
	// FIXME: CodecVideoClose calls/uses hw decoder
    }
    VideoPacketExit(stream);
}

/**
**	Poll PES packet ringbuffer.
**
**	Called if video frame buffers are full.
**
**	@param stream	video stream
**
**	@retval	1	something todo
**	@retval	-1	empty stream
*/
int VideoPollInput(VideoStream * stream)
{
    if (!stream->Decoder) {		// closing
	return -1;
    }

    if (stream->ClearBuffers) {		// clear buffer request
	// FIXME: ->Decoder already checked
	if (stream->Decoder) {
	    CodecVideoFlushBuffers(stream->Decoder);
	    VideoResetStart(stream->HwDecoder);
	}
	stream->ClearBuffers = 0;
	return 1;
    }
    return 1;
}

/**
**	Decode from PES packet ringbuffer.
**
**	@param stream	video stream
**
**	@retval 0	packet decoded
**	@retval	1	stream paused
**	@retval	-1	empty stream
*/
int VideoDecodeInput(VideoStream * stream)
{
    if (!stream->Decoder) {		// closing
	return -1;
    }

    if (stream->ClearBuffers) {		// clear buffer request
	// FIXME: ->Decoder already checked
	if (stream->Decoder) {
	    CodecVideoFlushBuffers(stream->Decoder);
	    VideoResetStart(stream->HwDecoder);
	}
	stream->ClearBuffers = 0;
	return 1;
    }
    if (stream->Freezed) {		// stream freezed
	// clear is called during freezed
	return 1;
    }
    //
    //	handle queued commands
    //

    // old version
    if (stream->Decoder) {
	CodecVideoDecode(stream->Decoder);
    }

    return 0;
}

/**
**	Get number of video buffers.
**
**	@param stream	video stream
*/
int VideoGetBuffers(const VideoStream * stream)
{
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

    if (!MyVideoStream->Decoder) {
	VideoStreamOpen(MyVideoStream);
	AudioSyncStream = MyVideoStream;
    }
    VideoOsdInit();
}

/**
**	Stop video.
*/
static void StopVideo(void)
{
    VideoOsdExit();
    VideoExit();
    AudioSyncStream = NULL;
    // FIXME: done by exit: VideoDelHwDecoder(MyVideoStream->HwDecoder);
    VideoStreamClose(MyVideoStream, 0);
}

//////////////////////////////////////////////////////////////////////////////
//  Play audio video
//////////////////////////////////////////////////////////////////////////////

/**
**	Play audio packet.
**
**	@param data	data of exactly one complete PES packet
**	@param size	size of PES packet
**	@param id	PES packet type
*/
int PlayAudio(const uint8_t * data, int size, uint8_t id)
{
    int n;
    const uint8_t *p;

    // channel switch: SetAudioChannelDevice: SetDigitalAudioDevice:

    if (SkipAudio || !MyAudioDecoder) { // skip audio
	return size;
    }
    if (StreamFreezed) {		// stream freezed
	return 0;
    }
    if (NewAudioStream) {
	// this clears the audio ringbuffer indirect, open and setup does it
	CodecAudioClose(MyAudioDecoder);
	AudioFlushBuffers();
	AudioSetBufferTime(ConfigAudioBufferTime);
	AudioCodecID = AV_CODEC_ID_NONE;
	AudioChannelID = -1;
	NewAudioStream = 0;
    }
    // hard limit buffer full: don't overrun audio buffers on replay
    if (AudioFreeBytes() < AUDIO_MIN_BUFFER_FREE) {
	return 0;
    }
    // PES header 0x00 0x00 0x01 ID
    // ID 0xBD 0xC0-0xCF

    // must be a PES start code
    if (size < 9 || !data || data[0] || data[1] || data[2] != 0x01) {
	Error("Invalid PES audio packet");
	return size;
    }
    n = data[8];			// header size

    if (size < 9 + n + 4) {		// wrong size
	if (size == 9 + n) {
	    Error("Empty audio packet");
	} else {
	    Error("Invalid audio packet %d bytes", size);
	}
	return size;
    }

    if (data[7] & 0x80 && n >= 5) {
	AudioAvPkt->pts =
	    (int64_t) (data[9] & 0x0E) << 29 | data[10] << 22 | (data[11] & 0xFE) << 14 | data[12] << 7 | (data[13] &
	    0xFE) >> 1;
    }

    p = data + 9 + n;
    n = size - 9 - n;			// skip pes header
    if (n + AudioAvPkt->stream_index > AudioAvPkt->size) {
	Fatal("Audio buffer too small");
	AudioAvPkt->stream_index = 0;
    }

    if (AudioChannelID != id) {		// id changed audio track changed
	AudioChannelID = id;
	AudioCodecID = AV_CODEC_ID_NONE;
	Debug3("audio/demux: new channel id");
    }
    // Private stream + LPCM ID
    if ((id & 0xF0) == 0xA0) {
	if (n < 7) {
	    Error("Invalid LPCM audio packet %d bytes", size);
	    return size;
	}
	if (AudioCodecID != AV_CODEC_ID_PCM_DVD) {
	    static int samplerates[] = { 48000, 96000, 44100, 32000 };
	    int samplerate;
	    int channels;
	    int bits_per_sample;

	    Debug3("LPCM %d sr:%d bits:%d chan:%d", id, p[5] >> 4, (((p[5] >> 6) & 0x3) + 4) * 4, (p[5] & 0x7) + 1);
	    CodecAudioClose(MyAudioDecoder);

	    bits_per_sample = (((p[5] >> 6) & 0x3) + 4) * 4;
	    if (bits_per_sample != 16) {
		Error("LPCM %d bits per sample aren't supported", bits_per_sample);
		// FIXME: handle unsupported formats.
	    }
	    samplerate = samplerates[p[5] >> 4];
	    channels = (p[5] & 0x7) + 1;

	    // FIXME: ConfigAudioBufferTime + x
	    AudioSetBufferTime(400);
	    AudioSetup(&samplerate, &channels, 0);
	    if (samplerate != samplerates[p[5] >> 4]) {
		Error("LPCM %d sample-rate is unsupported", samplerates[p[5] >> 4]);
		// FIXME: support resample
	    }
	    if (channels != (p[5] & 0x7) + 1) {
		Error("LPCM %d channels are unsupported", (p[5] & 0x7) + 1);
		// FIXME: support resample
	    }
	    //CodecAudioOpen(MyAudioDecoder, AV_CODEC_ID_PCM_DVD);
	    AudioCodecID = AV_CODEC_ID_PCM_DVD;
	}

	if (AudioAvPkt->pts != (int64_t) AV_NOPTS_VALUE) {
	    AudioSetClock(AudioAvPkt->pts);
	    AudioAvPkt->pts = AV_NOPTS_VALUE;
	}
	swab(p + 7, AudioAvPkt->data, n - 7);
	AudioEnqueue(AudioAvPkt->data, n - 7);

	return size;
    }
    // DVD track header
    if ((id & 0xF0) == 0x80 && (p[0] & 0xF0) == 0x80) {
	p += 4;
	n -= 4;				// skip track header
	if (AudioCodecID == AV_CODEC_ID_NONE) {
	    // FIXME: ConfigAudioBufferTime + x
	    AudioSetBufferTime(400);
	}
    }
    // append new packet, to partial old data
    memcpy(AudioAvPkt->data + AudioAvPkt->stream_index, p, n);
    AudioAvPkt->stream_index += n;

    n = AudioAvPkt->stream_index;
    p = AudioAvPkt->data;
    while (n >= 5) {
	int r;
	unsigned codec_id;

	// 4 bytes 0xFFExxxxx Mpeg audio
	// 3 bytes 0x56Exxx AAC LATM audio
	// 5 bytes 0x0B77xxxxxx AC-3 audio
	// 6 bytes 0x0B77xxxxxxxx E-AC-3 audio
	// 7/9 bytes 0xFFFxxxxxxxxxxx ADTS audio
	// PCM audio can't be found
	r = 0;
	codec_id = AV_CODEC_ID_NONE;	// keep compiler happy
	if (id != 0xbd && FastMpegCheck(p)) {
	    r = MpegCheck(p, n);
	    codec_id = AV_CODEC_ID_MP2;
	}
	if (id != 0xbd && !r && FastLatmCheck(p)) {
	    r = LatmCheck(p, n);
	    codec_id = AV_CODEC_ID_AAC_LATM;
	}
	if ((id == 0xbd || (id & 0xF0) == 0x80) && !r && FastAc3Check(p)) {
	    r = Ac3Check(p, n);
	    codec_id = AV_CODEC_ID_AC3;
	    if (r > 0 && p[5] > (10 << 3)) {
		codec_id = AV_CODEC_ID_EAC3;
	    }
	    /* faster ac3 detection at end of pes packet (no improvements)
	       if (AudioCodecID == codec_id && -r - 2 == n) {
	       r = n;
	       }
	     */
	}
	if (id != 0xbd && !r && FastAdtsCheck(p)) {
	    r = AdtsCheck(p, n);
	    codec_id = AV_CODEC_ID_AAC;
	}
	if (r < 0) {			// need more bytes
	    break;
	}
	if (r > 0) {
	    AVPacket avpkt[1];

	    // new codec id, close and open new
	    if (AudioCodecID != codec_id) {
		CodecAudioClose(MyAudioDecoder);
		CodecAudioOpen(MyAudioDecoder, codec_id);
		AudioCodecID = codec_id;
	    }
	    av_init_packet(avpkt);
	    avpkt->data = (void *)p;
	    avpkt->size = r;
	    avpkt->pts = AudioAvPkt->pts;
	    avpkt->dts = AudioAvPkt->dts;
	    // FIXME: not aligned for ffmpeg
	    CodecAudioDecode(MyAudioDecoder, avpkt);
	    AudioAvPkt->pts = AV_NOPTS_VALUE;
	    AudioAvPkt->dts = AV_NOPTS_VALUE;
	    p += r;
	    n -= r;
	    continue;
	}
	++p;
	--n;
    }

    // copy remaining bytes to start of packet
    if (n) {
	memmove(AudioAvPkt->data, p, n);
    }
    AudioAvPkt->stream_index = n;

    return size;
}

    /// call VDR support function
extern uint8_t *CreateJpeg(uint8_t *, int *, int, int, int);

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
	uint8_t *image;
	int raw_size;

	raw_size = 0;
	image = VideoGrab(&raw_size, &width, &height, 0);
	if (image) {			// can fail, suspended, ...
	    uint8_t *jpg_image;

	    jpg_image = CreateJpeg(image, size, quality, width, height);

	    free(image);
	    return jpg_image;
	}
	return NULL;
    }
    return VideoGrab(size, &width, &height, 1);
}

//////////////////////////////////////////////////////////////////////////////

/**
**	Gets the current System Time Counter, which can be used to
**	synchronize audio, video and subtitles.
*/
int64_t GetSTC(void)
{
    if (MyVideoStream->HwDecoder) {
	return VideoGetClock(MyVideoStream->HwDecoder);
    }
    // could happen during dettached
    Error("Called without hw decoder");
    return AV_NOPTS_VALUE;
}

/**
**	Get video stream size and aspect.
**
**	@param width[OUT]	width of video stream
**	@param height[OUT]	height of video stream
**	@param aspect[OUT]	aspect ratio (4/3, 16/9, ...) of video stream
*/
void GetVideoSize(int *width, int *height, double *aspect)
{
#ifdef DEBUG
    static int done_width;
    static int done_height;
#endif
    int aspect_num;
    int aspect_den;

    if (MyVideoStream->HwDecoder) {
	VideoGetVideoSize(MyVideoStream->HwDecoder, width, height, &aspect_num, &aspect_den);
	*aspect = (double)aspect_num / (double)aspect_den;
    } else {
	*width = 0;
	*height = 0;
	*aspect = 1.0;			// like default cDevice::GetVideoSize
    }

#ifdef DEBUG
    if (done_width != *width || done_height != *height) {
	Debug1("Video size: %dx%d %g", *width, *height, *aspect);
	done_width = *width;
	done_height = *height;
    }
#endif
}

/**
**	Set trick play speed.
**
**	Every single frame shall then be displayed the given number of
**	times.
**
**	@param speed	trick speed
*/
void TrickSpeed(int speed)
{
    MyVideoStream->TrickSpeed = speed;
    if (MyVideoStream->HwDecoder) {
	VideoSetTrickSpeed(MyVideoStream->HwDecoder, speed);
    } else {
	// can happen, during startup
	Debug1("Trickspeed called without hw decoder");
    }
    StreamFreezed = 0;
    MyVideoStream->Freezed = 0;
}

/**
**	Clears all video and audio data from the device.
*/
void Clear(void)
{
    AudioFlushBuffers();
    CodecAudioFlushBuffers(MyAudioDecoder);
    CodecVideoFlushBuffers(MyVideoStream->Decoder);
}

/**
**	Sets the device into play mode.
*/
void Play(void)
{
    TrickSpeed(0);			// normal play
    SkipAudio = 0;
    AudioPlay();
}

/**
**	Sets the device into "freeze frame" mode.
*/
void Freeze(void)
{
    StreamFreezed = 1;
    MyVideoStream->Freezed = 1;
    AudioPause();
}

/**
**	Turns off audio while replaying.
*/
void Mute(void)
{
    SkipAudio = 1;
    AudioFlushBuffers();
    //AudioSetVolume(0);
}

/**
**	Display the given I-frame as a still picture.
**
**	@param data	pes frame data
**	@param size	number of bytes in frame
*/
void StillPicture(const uint8_t * data, int size)
{
    // TODO: Implement with ffmpeg
}

/**
**	Poll if device is ready.  Called by replay.
**
**	This function is useless, the return value is ignored and
**	all buffers are overrun by vdr.
**
**	The dvd plugin is using this correct.
**
**	@param timeout	timeout to become ready in ms
**
**	@retval true	if ready
**	@retval false	if busy
*/
int Poll(int timeout)
{
    // poll is only called during replay, flush buffers after replay
    MyVideoStream->ClearClose = 1;
    for (;;) {
	int full;
	int t;
	int used;

	used = AudioUsedBytes();
	// FIXME: no video!
	// soft limit + hard limit
	full = (used > AUDIO_MIN_BUFFER_FREE)
	    || AudioFreeBytes() < AUDIO_MIN_BUFFER_FREE;

	if (!full || !timeout) {
	    return !full;
	}

	t = 15;
	if (timeout < t) {
	    t = timeout;
	}
	usleep(t * 1000);		// let display thread work
	timeout -= t;
    }
}

/**
**	Flush the device output buffers.
**
**	@param timeout	timeout to flush in ms
*/
int Flush(int timeout)
{
    AudioFlushBuffers();
    CodecAudioFlushBuffers(MyAudioDecoder);
    CodecVideoFlushBuffers(MyVideoStream->Decoder);

    return 1;
}

//////////////////////////////////////////////////////////////////////////////
//  OSD
//////////////////////////////////////////////////////////////////////////////

/**
**	Get OSD size and aspect.
**
**	@param width[OUT]	width of OSD
**	@param height[OUT]	height of OSD
**	@param aspect[OUT]	aspect ratio (4/3, 16/9, ...) of OSD
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
	Debug1("Osd size: %dx%d %g", *width, *height, *aspect);
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
**
**	@param xi	x-coordinate in argb image
**	@param yi	y-coordinate in argb image
**	@paran height	height in pixel in argb image
**	@paran width	width in pixel in argb image
**	@param pitch	pitch of argb image
**	@param argb	32bit ARGB image data
**	@param x	x-coordinate on screen of argb image
**	@param y	y-coordinate on screen of argb image
*/
void OsdDrawARGB(int xi, int yi, int height, int width, int pitch, const uint8_t * argb, int x, int y)
{
    // wakeup display for showing remote learning dialog
    VideoDisplayWakeup();
    VideoOsdDrawARGB(xi, yi, height, width, pitch, argb, x, y);
}

//////////////////////////////////////////////////////////////////////////////

/**
**	Return command line help string.
*/
const char *CommandLineHelp(void)
{
    return "  -a device\taudio device (fe. alsa: hw:0,0 oss: /dev/dsp)\n"
	"  -p device\taudio device for pass-through (hw:0,1 or /dev/dsp1)\n"
	"  -c channel\taudio mixer channel name (fe. PCM)\n" "	-d display\tdisplay of x11 server (fe. :0.0)\n"
	"  -f\t\tstart with fullscreen window (only with window manager)\n"
	"  -g geometry\tx11 window geometry wxh+x+y\n" "  -t tracemode\tset the trace mode for debugging\n"
	"  -v device\tvideo driver device (vaapi, noop)\n" "  -s\t\tstart in suspended mode\n"
	"  -x\t\tstart x11 server, with -xx try to connect, if this fails\n"
	"  -X args\tX11 server arguments (f.e. -nocursor)\n" "	-w workaround\tenable/disable workarounds\n"
	"\tno-hw-decoder\t\tdisable hw decoder, use software decoder only\n"
	"\tno-mpeg-hw-decoder\tdisable hw decoder for mpeg only\n"
	"\tstill-hw-decoder\tenable hardware decoder for still-pictures\n"
	"\tstill-h264-hw-decoder\tenable h264 hw decoder for still-pictures\n"
	"\talsa-driver-broken\tdisable broken alsa driver message\n"
	"\talsa-no-close-open\tdisable close open to fix alsa no sound bug\n"
	"\talsa-close-open-delay\tenable close open delay to fix no sound bug\n"
	"\tignore-repeat-pict\tdisable repeat pict message\n" "	 -D\t\tstart in detached mode\n";
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
#ifdef __FreeBSD__
    if (!strcmp(*argv, "vaapidevice")) {
	++argv;
	--argc;
    }
#endif

    for (;;) {
	switch (getopt(argc, argv, "-a:c:d:fg:t:p:sv:w:xDX:")) {
	    case 'a':		       // audio device for pcm
		AudioSetDevice(optarg);
		continue;
	    case 'c':		       // channel of audio mixer
		AudioSetChannel(optarg);
		continue;
	    case 'p':		       // pass-through audio device
		AudioSetPassthroughDevice(optarg);
		continue;
	    case 'd':		       // x11 display name
		X11DisplayName = optarg;
		continue;
	    case 'f':		       // fullscreen mode
		ConfigFullscreen = 1;
		continue;
	    case 'g':		       // geometry
		if (VideoSetGeometry(optarg) < 0) {
		    fprintf(stderr,
			"Bad formated geometry please use: [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]\n");
		    return 0;
		}
		continue;
	    case 't':		       // trace mode for debugging
		TraceMode = strtol(optarg, NULL, 0) & 0xFFFF;
		continue;
	    case 'v':		       // video driver
		VideoSetDevice(optarg);
		continue;
	    case 'x':		       // x11 server
		ConfigStartX11Server++;
		continue;
	    case 'X':		       // x11 server arguments
		X11ServerArguments = optarg;
		continue;
	    case 's':		       // start in suspend mode
		ConfigStartSuspended = 1;
		continue;
	    case 'D':		       // start in detached mode
		ConfigStartSuspended = -1;
		continue;
	    case 'w':		       // workarounds
		if (!strcasecmp("alsa-driver-broken", optarg)) {
		    AudioAlsaDriverBroken = 1;
		} else if (!strcasecmp("alsa-no-close-open", optarg)) {
		    AudioAlsaNoCloseOpen = 1;
		} else if (!strcasecmp("alsa-close-open-delay", optarg)) {
		    AudioAlsaCloseOpenDelay = 1;
		} else if (!strcasecmp("ignore-repeat-pict", optarg)) {
		    VideoIgnoreRepeatPict = 1;
		} else {
		    fprintf(stderr, "Workaround '%s' unsupported\n", optarg);
		    return 0;
		}
		continue;
	    case EOF:
		break;
	    case '-':
		fprintf(stderr, "We need no long options\n");
		return 0;
	    case ':':
		fprintf(stderr, "Missing argument for option '%c'\n", optopt);
		return 0;
	    default:
		fprintf(stderr, "Unknown option '%c'\n", optopt);
		return 0;
	}
	break;
    }
    while (optind < argc) {
	fprintf(stderr, "Unhandled argument '%s'\n", argv[optind++]);
    }

    return 1;
}

//////////////////////////////////////////////////////////////////////////////
//  Init/Exit
//////////////////////////////////////////////////////////////////////////////

#include <sys/types.h>
#include <sys/wait.h>

#define XSERVER_MAX_ARGS 512		///< how many arguments support

#ifndef __FreeBSD__
static const char *X11Server = "/usr/bin/X";	///< default x11 server
#else
static const char *X11Server = LOCALBASE "/bin/X";  ///< default x11 server
#endif
static pid_t X11ServerPid;		///< x11 server pid

/**
**	USR1 signal handler.
**
**	@param sig	signal number
*/
static void Usr1Handler(int __attribute__ ((unused)) sig)
{
    ++Usr1Signal;

    Debug2("X11: got signal USR1");
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
    int maxfd;
    int fd;

    //	X server
    if (X11Server) {
	args[0] = X11Server;
    } else {
	Error("X11: No X server configured!");
	return;
    }

    argn = 1;
    if (X11DisplayName) {		// append display name
	args[argn++] = X11DisplayName;
	// export display for childs
	setenv("DISPLAY", X11DisplayName, 1);
    }
    //	split X server arguments string into words
    if ((sval = X11ServerArguments)) {
	char *s;

#ifndef __FreeBSD__
	s = buf = strdupa(sval);
#else
	s = buf = alloca(strlen(sval) + 1);
	strcpy(buf, sval);
#endif
	while ((sval = strsep(&s, " \t"))) {
	    args[argn++] = sval;

	    if (argn == XSERVER_MAX_ARGS - 1) { // argument overflow
		Error("X11: too many arguments for X server");
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

    Debug2("X11: Starting X server '%s' '%s'", args[0], X11ServerArguments);
    //	fork
    if ((pid = fork())) {		// parent

	X11ServerPid = pid;
	Debug2("X11: Started X server pid=%d", X11ServerPid);

	return;
    }
    // child
    signal(SIGUSR1, SIG_IGN);		// ignore to force answer
    //setpgid(0,getpid());
    setpgid(pid, 0);

    // close all open file-handles
    maxfd = sysconf(_SC_OPEN_MAX);
    for (fd = 3; fd < maxfd; fd++) {	// keep stdin, stdout, stderr
	close(fd);			// vdr should open with O_CLOEXEC
    }

    //	start the X server
    execvp(args[0], (char *const *)args);

    Error("X11: Failed to start X server '%s'", args[0]);
    exit(-1);
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
    av_packet_unref(AudioAvPkt);

    StopVideo();

    CodecExit();

    if (ConfigStartX11Server) {
	Debug2("X11: Stop X server");

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
		Debug2("X11: X server exited (%d)", WEXITSTATUS(status));
	    }
	    if (wpid && WIFSIGNALED(status)) {
		Debug2("X11: X server killed (%d)", WTERMSIG(status));
	    }
	}
    }

    pthread_mutex_destroy(&SuspendLockMutex);
    pthread_mutex_destroy(&MyVideoStream->DecoderLockMutex);
}

/**
**	Prepare plugin.
**
**	@retval 0	normal start
**	@retval 1	suspended start
**	@retval -1	detached start
*/
int Start(void)
{
    if (ConfigStartX11Server) {
	StartXServer();
    }
    CodecInit();

    pthread_mutex_init(&MyVideoStream->DecoderLockMutex, NULL);
    pthread_mutex_init(&SuspendLockMutex, NULL);

    if (!ConfigStartSuspended) {
	// FIXME: AudioInit for HDMI after X11 startup
	// StartAudio();
	AudioInit();
	av_new_packet(AudioAvPkt, AUDIO_BUFFER_SIZE);
	MyAudioDecoder = CodecAudioNewDecoder();
	AudioCodecID = AV_CODEC_ID_NONE;
	AudioChannelID = -1;

	if (!ConfigStartX11Server) {
	    StartVideo();
	}
    } else {
	MyVideoStream->SkipStream = 1;
	SkipAudio = 1;
    }
    Info("Device ready%s", ConfigStartSuspended ? ConfigStartSuspended == -1 ? " detached" : " suspended" : "");

    return ConfigStartSuspended;
}

/**
**	Stop plugin.
**
**	@note stop everything, but don't cleanup, module is still called.
*/
void Stop(void)
{
}

/**
**	Perform any cleanup or other regular tasks.
*/
void Housekeeping(void)
{
    //
    //	when starting an own X11 server fails, try to connect to a already
    //	running X11 server.  This can take some time.
    //
    if (X11ServerPid) {			// check if X11 server still running
	pid_t wpid;
	int status;

	wpid = waitpid(X11ServerPid, &status, WNOHANG);
	if (wpid) {
	    if (WIFEXITED(status)) {
		Debug2("X11: X server exited (%d)", WEXITSTATUS(status));
	    }
	    if (WIFSIGNALED(status)) {
		Debug2("X11: X server killed (%d)", WTERMSIG(status));
	    }
	    X11ServerPid = 0;
	    // video not running
	    if (ConfigStartX11Server > 1 && !MyVideoStream->HwDecoder) {
		StartVideo();
	    }
	}
    }
}

/**
**	Main thread hook, periodic called from main thread.
*/
void MainThreadHook(void)
{
    if (Usr1Signal) {			// x11 server ready
	// FIYME: x11 server keeps sending sigusr1 signals
	signal(SIGUSR1, SIG_IGN);	// ignore further signals
	Usr1Signal = 0;
	StartVideo();
	VideoDisplayWakeup();
    }
}

//////////////////////////////////////////////////////////////////////////////
//  Suspend/Resume
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
    if (MyVideoStream->SkipStream && SkipAudio) {   // already suspended
	pthread_mutex_unlock(&SuspendLockMutex);
	return;
    }
    // FIXME: should not be correct, if not both are suspended!
    // Move down into if (video) ...
    MyVideoStream->SkipStream = 1;
    SkipAudio = 1;

    if (audio) {
	AudioExit();
	if (MyAudioDecoder) {
	    CodecAudioClose(MyAudioDecoder);
	    CodecAudioDelDecoder(MyAudioDecoder);
	    MyAudioDecoder = NULL;
	}
	NewAudioStream = 0;
	av_packet_unref(AudioAvPkt);
    }
    if (video) {
	StopVideo();
    }

    if (dox11) {
	// FIXME: stop x11, if started
    }

    pthread_mutex_unlock(&SuspendLockMutex);
}

/**
**	Resume plugin.
*/
void Resume(void)
{
    if (!MyVideoStream->SkipStream && !SkipAudio) { // we are not suspended
	return;
    }

    pthread_mutex_lock(&SuspendLockMutex);
    // FIXME: start x11

    if (!MyVideoStream->HwDecoder) {	// video not running
	StartVideo();
    }
    if (!MyAudioDecoder) {		// audio not running
	// StartAudio();
	AudioInit();
	av_new_packet(AudioAvPkt, AUDIO_BUFFER_SIZE);
	MyAudioDecoder = CodecAudioNewDecoder();
	AudioCodecID = AV_CODEC_ID_NONE;
	AudioChannelID = -1;
    }

    if (MyVideoStream->Decoder) {
	MyVideoStream->SkipStream = 0;
    }
    SkipAudio = 0;

    pthread_mutex_unlock(&SuspendLockMutex);
}

/*
**	Get video decoder statistics.
*/
char *GetVideoStats(void)
{
    return MyVideoStream->HwDecoder ? VideoGetStats(MyVideoStream->HwDecoder) : NULL;
}

/*
**	Get video decoder info.
**
*/
char *GetVideoInfo(void)
{
    const char *codec = CodecVideoGetCodecName(MyVideoStream->Decoder);

    return MyVideoStream->HwDecoder ? VideoGetInfo(MyVideoStream->HwDecoder, codec) : NULL;
}

/*
**	Get audio decoder info.
**
*/
char *GetAudioInfo(void)
{
    return CodecAudioGetInfo(MyAudioDecoder, AudioCodecID);
}

/**
**	Scale the currently shown video.
**
**	@param x	video window x coordinate OSD relative
**	@param y	video window y coordinate OSD relative
**	@param width	video window width OSD relative
**	@param height	video window height OSD relative
*/
void ScaleVideo(int x, int y, int width, int height)
{
    if (MyVideoStream->HwDecoder) {
	VideoSetOutputPosition(MyVideoStream->HwDecoder, x, y, width, height);
    }
}

int IsReplay(void)
{
    return !AudioSyncStream || AudioSyncStream->ClearClose;
}
