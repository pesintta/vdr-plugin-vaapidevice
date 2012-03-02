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

static char ConfigStartSuspended;	///< flag to start in suspend mode
static char ConfigFullscreen;		///< fullscreen modus
static char ConfigStartX11Server;	///< flag start the x11 server

static pthread_mutex_t SuspendLockMutex;	///< suspend lock mutex

static volatile char StreamFreezed;	///< stream freezed

//////////////////////////////////////////////////////////////////////////////
//	Audio
//////////////////////////////////////////////////////////////////////////////

static volatile char NewAudioStream;	///< new audio stream
static volatile char SkipAudio;		///< skip audio stream
static AudioDecoder *MyAudioDecoder;	///< audio decoder
static enum CodecID AudioCodecID;	///< current codec id
static int AudioChannelID;		///< current audio channel id

    /// Minimum free space in audio buffer 8 packets for 8 channels
#define AUDIO_MIN_BUFFER_FREE (3072 * 8 * 8)
#define AUDIO_BUFFER_SIZE (512 * 1024)	///< audio PES buffer default size
static AVPacket AudioAvPkt[1];		///< audio a/v packet

//////////////////////////////////////////////////////////////////////////////
//	Audio codec parser
//////////////////////////////////////////////////////////////////////////////

///
///	Mpeg bitrate table.
///
///	BitRateTable[Version][Layer][Index]
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
///	Mpeg samperate table.
///
static const uint16_t SampleRateTable[4] = {
    44100, 48000, 32000, 0
};

///
///	Fast check for Mpeg audio.
///
///	4 bytes 0xFFExxxxx Mpeg audio
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
///	Check for Mpeg audio.
///
///	0xFFEx already checked.
///
///	@param data	incomplete PES packet
///	@param size	number of bytes
///
///	@retval <0	possible mpeg audio, but need more data
///	@retval 0	no valid mpeg audio
///	@retval >0	valid mpeg audio
///
///	From: http://www.mpgedit.org/mpgedit/mpeg_format/mpeghdr.htm
///
///	AAAAAAAA AAABBCCD EEEEFFGH IIJJKLMM
///
///	o a 11x Frame sync
///	o b 2x	Mpeg audio version (2.5, reserved, 2, 1)
///	o c 2x	Layer (reserved, III, II, I)
///	o e 2x	BitRate index
///	o f 2x	SampleRate index (4100, 48000, 32000, 0)
///	o g 1x	Paddding bit
///	o ..	Doesn't care
///
///	frame length:
///	Layer I:
///		FrameLengthInBytes = (12 * BitRate / SampleRate + Padding) * 4
///	Layer II & III:
///		FrameLengthInBytes = 144 * BitRate / SampleRate + Padding
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
    if (0) {
	Debug(3,
	    "pesdemux: mpeg%s layer%d bitrate=%d samplerate=%d %d bytes\n",
	    mpeg25 ? "2.5" : mpeg2 ? "2" : "1", layer, bit_rate, sample_rate,
	    frame_size);
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
///	Fast check for AAC LATM audio.
///
///	3 bytes 0x56Exxx AAC LATM audio
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
///	Check for AAC LATM audio.
///
///	0x56Exxx already checked.
///
///	@param data	incomplete PES packet
///	@param size	number of bytes
///
///	@retval <0	possible AAC LATM audio, but need more data
///	@retval 0	no valid AAC LATM audio
///	@retval >0	valid AAC LATM audio
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
///	Possible AC3 frame sizes.
///
///	from ATSC A/52 table 5.18 frame size code table.
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
///	Fast check for AC3 audio.
///
///	5 bytes 0x0B77xxxxxx AC3 audio
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
///	Check for AC-3 audio.
///
///	0x0B77xxxxxx already checked.
///
///	@param data	incomplete PES packet
///	@param size	number of bytes
///
///	@retval <0	possible AC-3 audio, but need more data
///	@retval 0	no valid AC-3 audio
///	@retval >0	valid AC-3 audio
///
static int Ac3Check(const uint8_t * data, int size)
{
    int fscod;
    int frmsizcod;
    int frame_size;

    // crc1 crc1 fscod|frmsizcod
    fscod = data[4] >> 6;
    frmsizcod = data[4] & 0x3F;
    frame_size = Ac3FrameSizeTable[frmsizcod][fscod] * 2;

    if (frame_size + 2 > size) {
	return -frame_size - 2;
    }
    // check if after this frame a new AC-3 frame starts
    if (FastAc3Check(data + frame_size)) {
	return frame_size;
    }

    return 0;
}

#ifdef USE_TS_AUDIO

//////////////////////////////////////////////////////////////////////////////
//	PES Demux
//////////////////////////////////////////////////////////////////////////////

///
///	PES type.
///
enum
{
    PES_PROG_STREAM_MAP = 0xBC,
    PES_PRIVATE_STREAM1 = 0xBD,
    PES_PADDING_STREAM = 0xBE,		///< filler, padding stream
    PES_PRIVATE_STREAM2 = 0xBF,
    PES_AUDIO_STREAM_S = 0xC0,
    PES_AUDIO_STREAM_E = 0xDF,
    PES_VIDEO_STREAM_S = 0xE0,
    PES_VIDEO_STREAM_E = 0xEF,
    PES_ECM_STREAM = 0xF0,
    PES_EMM_STREAM = 0xF1,
    PES_DSM_CC_STREAM = 0xF2,
    PES_ISO13522_STREAM = 0xF3,
    PES_TYPE_E_STREAM = 0xF8,		///< ITU-T rec. h.222.1 type E stream
    PES_PROG_STREAM_DIR = 0xFF,
};

///
///	PES parser state.
///
enum
{
    PES_INIT,				///< unknown codec

    PES_SKIP,				///< skip packet
    PES_SYNC,				///< search packet sync byte
    PES_HEADER,				///< copy header
    PES_START,				///< pes packet start found
    PES_PAYLOAD,			///< copy payload

    PES_LPCM_HEADER,			///< copy lcpm header
    PES_LPCM_PAYLOAD,			///< copy lcpm payload
};

#define PES_START_CODE_SIZE 6		///< size of pes start code with length
#define PES_HEADER_SIZE 9		///< size of pes header
#define PES_MAX_HEADER_SIZE (PES_HEADER_SIZE + 256)	///< maximal header size
#define PES_MAX_PAYLOAD	(512 * 1024)	///< max pay load size

///
///	PES demuxer.
///
typedef struct _pes_demux_
{
    //int Pid;				///< packet id
    //int PcrPid;			///< program clock reference pid
    //int StreamType;			///< stream type

    int State;				///< parsing state
    uint8_t Header[PES_MAX_HEADER_SIZE];	///< buffer for pes header
    int HeaderIndex;			///< header index
    int HeaderSize;			///< size of pes header
    uint8_t *Buffer;			///< payload buffer
    int Index;				///< buffer index
    int Skip;				///< buffer skip
    int Size;				///< size of payload buffer

    uint8_t StartCode;			///< pes packet start code

    int64_t PTS;			///< presentation time stamp
    int64_t DTS;			///< decode time stamp
} PesDemux;

///
///	Initialize a packetized elementary stream demuxer.
///
///	@param pesdx	packetized elementary stream demuxer
///
void PesInit(PesDemux * pesdx)
{
    memset(pesdx, 0, sizeof(*pesdx));
    pesdx->Size = PES_MAX_PAYLOAD;
    pesdx->Buffer = av_malloc(PES_MAX_PAYLOAD + FF_INPUT_BUFFER_PADDING_SIZE);
    if (!pesdx->Buffer) {
	Fatal(_("pesdemux: out of memory\n"));
    }
    pesdx->PTS = AV_NOPTS_VALUE;	// reset
    pesdx->DTS = AV_NOPTS_VALUE;
}

///
///	Reset packetized elementary stream demuxer.
///
void PesReset(PesDemux * pesdx)
{
    pesdx->State = PES_INIT;
    pesdx->Index = 0;
    pesdx->Skip = 0;
    pesdx->PTS = AV_NOPTS_VALUE;
    pesdx->DTS = AV_NOPTS_VALUE;
}

///
///	Parse packetized elementary stream.
///
///	@param pesdx	packetized elementary stream demuxer
///	@param data	payload data of transport stream
///	@param size	number of payload data bytes
///	@param is_start flag, start of pes packet
///
void PesParse(PesDemux * pesdx, const uint8_t * data, int size, int is_start)
{
    const uint8_t *p;
    const uint8_t *q;

    if (is_start) {			// start of pes packet
	if (pesdx->Index && pesdx->Skip) {
	    // copy remaining bytes down
	    pesdx->Index -= pesdx->Skip;
	    memmove(pesdx->Buffer, pesdx->Buffer + pesdx->Skip, pesdx->Index);
	    pesdx->Skip = 0;
	}
	pesdx->State = PES_SYNC;
	pesdx->HeaderIndex = 0;
	pesdx->PTS = AV_NOPTS_VALUE;	// reset if not yet used
	pesdx->DTS = AV_NOPTS_VALUE;
    }
    // cleanup, if too much cruft
    if (pesdx->Skip > PES_MAX_PAYLOAD / 2) {
	// copy remaining bytes down
	pesdx->Index -= pesdx->Skip;
	memmove(pesdx->Buffer, pesdx->Buffer + pesdx->Skip, pesdx->Index);
	pesdx->Skip = 0;
    }

    p = data;
    do {
	int n;

	switch (pesdx->State) {
	    case PES_SKIP:		// skip this packet
		return;

	    case PES_START:		// at start of pes packet payload
		// FIXME: need 0x80 -- 0xA0 state
		if (AudioCodecID == CODEC_ID_NONE) {
		    if ((*p & 0xF0) == 0x80) {	// AC-3 & DTS
			Debug(3, "pesdemux: dvd ac-3\n");
		    } else if ((*p & 0xFF) == 0xA0) {	// LPCM
			Debug(3, "pesdemux: dvd lpcm\n");
			pesdx->State = PES_LPCM_HEADER;
			pesdx->HeaderIndex = 0;
			pesdx->HeaderSize = 7;
			break;
		    }
		}

	    case PES_INIT:		// find start of audio packet
		// FIXME: increase if needed the buffer

		// fill buffer
		n = pesdx->Size - pesdx->Index;
		if (n > size) {
		    n = size;
		}
		memcpy(pesdx->Buffer + pesdx->Index, p, n);
		pesdx->Index += n;
		p += n;
		size -= n;

		q = pesdx->Buffer + pesdx->Skip;
		n = pesdx->Index - pesdx->Skip;
		while (n >= 5) {
		    int r;
		    unsigned codec_id;

		    // 4 bytes 0xFFExxxxx Mpeg audio
		    // 3 bytes 0x56Exxx AAC LATM audio
		    // 5 bytes 0x0B77xxxxxx AC3 audio
		    // PCM audio can't be found
		    r = 0;
		    if (FastMpegCheck(q)) {
			r = MpegCheck(q, n);
			codec_id = CODEC_ID_MP2;
		    }
		    if (!r && FastAc3Check(q)) {
			r = Ac3Check(q, n);
			codec_id = CODEC_ID_AC3;
		    }
		    if (!r && FastLatmCheck(q)) {
			r = LatmCheck(q, n);
			codec_id = CODEC_ID_AAC_LATM;
		    }
		    if (r < 0) {	// need more bytes
			break;
		    }
		    if (r > 0) {
			AVPacket avpkt[1];

			// new codec id, close and open new
			if (AudioCodecID != codec_id) {
			    Debug(3, "pesdemux: new codec %#06x -> %#06x\n",
				AudioCodecID, codec_id);
			    CodecAudioClose(MyAudioDecoder);
			    CodecAudioOpen(MyAudioDecoder, NULL, codec_id);
			    AudioCodecID = codec_id;
			}
			av_init_packet(avpkt);
			avpkt->data = (void *)q;
			avpkt->size = r;
			avpkt->pts = pesdx->PTS;
			avpkt->dts = pesdx->DTS;
			CodecAudioDecode(MyAudioDecoder, avpkt);
			pesdx->PTS = AV_NOPTS_VALUE;
			pesdx->DTS = AV_NOPTS_VALUE;
			pesdx->Skip += r;
			// FIXME: switch to decoder state
			//pesdx->State = PES_MPEG_DECODE;
			break;
		    }
		    // try next byte
		    ++pesdx->Skip;
		    ++q;
		    --n;
		}
		break;

	    case PES_SYNC:		// wait for pes sync
		n = PES_START_CODE_SIZE - pesdx->HeaderIndex;
		if (n > size) {
		    n = size;
		}
		memcpy(pesdx->Header + pesdx->HeaderIndex, p, n);
		pesdx->HeaderIndex += n;
		p += n;
		size -= n;

		// have complete packet start code
		if (pesdx->HeaderIndex >= PES_START_CODE_SIZE) {
		    unsigned code;

		    // bad mpeg pes packet start code prefix 0x00001xx
		    if (pesdx->Header[0] || pesdx->Header[1]
			|| pesdx->Header[2] != 0x01) {
			Debug(3, "pesdemux: bad pes packet\n");
			pesdx->State = PES_SKIP;
			return;
		    }
		    code = pesdx->Header[3];
		    if (code != pesdx->StartCode) {
			Debug(3, "pesdemux: pes start code id %#02x\n", code);
			// FIXME: need to save start code id?
			pesdx->StartCode = code;
		    }

		    pesdx->State = PES_HEADER;
		    pesdx->HeaderSize = PES_HEADER_SIZE;
		}
		break;

	    case PES_HEADER:		// parse PES header
		n = pesdx->HeaderSize - pesdx->HeaderIndex;
		if (n > size) {
		    n = size;
		}
		memcpy(pesdx->Header + pesdx->HeaderIndex, p, n);
		pesdx->HeaderIndex += n;
		p += n;
		size -= n;

		// have header upto size bits
		if (pesdx->HeaderIndex == PES_HEADER_SIZE) {
		    if ((pesdx->Header[6] & 0xC0) == 0x80) {
			pesdx->HeaderSize += pesdx->Header[8];
		    } else {
			Error(_("pesdemux: mpeg1 pes packet unsupported\n"));
			pesdx->State = PES_SKIP;
			return;
		    }
		    // have complete header
		} else if (pesdx->HeaderIndex == pesdx->HeaderSize) {
		    int64_t pts;
		    int64_t dts;

		    if ((pesdx->Header[7] & 0xC0) == 0x80) {
			pts =
			    (int64_t) (data[9] & 0x0E) << 29 | data[10] << 22 |
			    (data[11] & 0xFE) << 14 | data[12] << 7 | (data[13]
			    & 0xFE) >> 1;
			Debug(4, "pesdemux: pts %#012" PRIx64 "\n", pts);
			pesdx->PTS = pts;
		    } else if ((pesdx->Header[7] & 0xC0) == 0xC0) {
			pts =
			    (int64_t) (data[9] & 0x0E) << 29 | data[10] << 22 |
			    (data[11] & 0xFE) << 14 | data[12] << 7 | (data[13]
			    & 0xFE) >> 1;
			pesdx->PTS = pts;
			dts =
			    (int64_t) (data[14] & 0x0E) << 29 | data[15] << 22
			    | (data[16] & 0xFE) << 14 | data[17] << 7 |
			    (data[18] & 0xFE) >> 1;
			pesdx->DTS = dts;
			Debug(4,
			    "pesdemux: pts %#012" PRIx64 " %#012" PRIx64 "\n",
			    pts, dts);
		    }

		    pesdx->State = PES_INIT;
		    if (pesdx->StartCode == PES_PRIVATE_STREAM1) {
			// only private stream 1, has sub streams
			pesdx->State = PES_START;
		    }
		    //pesdx->HeaderIndex = 0;
		    //pesdx->Index = 0;
		}
		break;

	    case PES_LPCM_HEADER:	// lpcm header
		n = pesdx->HeaderSize - pesdx->HeaderIndex;
		if (n > size) {
		    n = size;
		}
		memcpy(pesdx->Header + pesdx->HeaderIndex, p, n);
		pesdx->HeaderIndex += n;
		p += n;
		size -= n;

		if (pesdx->HeaderIndex == pesdx->HeaderSize) {
		    static int samplerates[] = { 48000, 96000, 44100, 32000 };
		    int samplerate;
		    int channels;
		    int bits_per_sample;
		    const uint8_t *q;

		    if (AudioCodecID != CODEC_ID_PCM_DVD) {

			q = pesdx->Header;
			Debug(3, "pesdemux: LPCM %d sr:%d bits:%d chan:%d\n",
			    q[0], q[5] >> 4, (((q[5] >> 6) & 0x3) + 4) * 4,
			    (q[5] & 0x7) + 1);
			CodecAudioClose(MyAudioDecoder);

			bits_per_sample = (((q[5] >> 6) & 0x3) + 4) * 4;
			if (bits_per_sample != 16) {
			    Error(_
				("softhddev: LPCM %d bits per sample aren't supported\n"),
				bits_per_sample);
			    // FIXME: handle unsupported formats.
			}
			samplerate = samplerates[q[5] >> 4];
			channels = (q[5] & 0x7) + 1;
			AudioSetup(&samplerate, &channels, 0);
			if (samplerate != samplerates[q[5] >> 4]) {
			    Error(_
				("softhddev: LPCM %d sample-rate is unsupported\n"),
				samplerates[q[5] >> 4]);
			    // FIXME: support resample
			}
			if (channels != (q[5] & 0x7) + 1) {
			    Error(_
				("softhddev: LPCM %d channels are unsupported\n"),
				(q[5] & 0x7) + 1);
			    // FIXME: support resample
			}
			//CodecAudioOpen(MyAudioDecoder, NULL, CODEC_ID_PCM_DVD);
			AudioCodecID = CODEC_ID_PCM_DVD;
		    }
		    pesdx->State = PES_LPCM_PAYLOAD;
		    pesdx->Index = 0;
		    pesdx->Skip = 0;
		}
		break;

	    case PES_LPCM_PAYLOAD:	// lpcm payload
		// fill buffer
		n = pesdx->Size - pesdx->Index;
		if (n > size) {
		    n = size;
		}
		memcpy(pesdx->Buffer + pesdx->Index, p, n);
		pesdx->Index += n;
		p += n;
		size -= n;

		if (pesdx->PTS != (int64_t) AV_NOPTS_VALUE) {
		    // FIXME: needs bigger buffer
		    AudioSetClock(pesdx->PTS);
		    pesdx->PTS = AV_NOPTS_VALUE;
		}
		swab(pesdx->Buffer, pesdx->Buffer, pesdx->Index);
		AudioEnqueue(pesdx->Buffer, pesdx->Index);
		pesdx->Index = 0;
		break;
	}
    } while (size > 0);
}

//////////////////////////////////////////////////////////////////////////////
//	Transport stream demux
//////////////////////////////////////////////////////////////////////////////

    /// Transport stream packet size
#define TS_PACKET_SIZE	188
    /// Transport stream packet sync byte
#define TS_PACKET_SYNC	0x47

///
///	transport stream demuxer typedef.
///
typedef struct _ts_demux_ TsDemux;

///
///	transport stream demuxer structure.
///
struct _ts_demux_
{
    int Packets;			///< packets between PCR
};

static PesDemux PesDemuxAudio[1];	///< audio demuxer

///
///	Transport stream demuxer.
///
///	@param tsdx	transport stream demuxer
///	@param data	buffer of transport stream packets
///	@param size	size of buffer
///
///	@returns number of bytes consumed from buffer.
///
int TsDemuxer(TsDemux * tsdx, const uint8_t * data, int size)
{
    const uint8_t *p;

    p = data;
    while (size >= TS_PACKET_SIZE) {
	int pid;
	int payload;

	if (p[0] != TS_PACKET_SYNC) {
	    Error(_("tsdemux: transport stream out of sync\n"));
	    // FIXME: kill all buffers
	    return size;
	}
	++tsdx->Packets;
	if (p[1] & 0x80) {		// error indicator
	    Debug(3, "tsdemux: transport error\n");
	    // FIXME: kill all buffers
	    goto next_packet;
	}

	pid = (p[1] & 0x1F) << 8 | p[2];
	Debug(4, "tsdemux: PID: %#04x%s%s\n", pid, p[1] & 0x40 ? " start" : "",
	    p[3] & 0x10 ? " payload" : "");

	// skip adaptation field
	switch (p[3] & 0x30) {		// adaption field
	    case 0x00:			// reserved
	    case 0x20:			// adaptation field only
	    default:
		goto next_packet;
	    case 0x10:			// only payload
		payload = 4;
		break;
	    case 0x30:			// skip adapation field
		payload = 5 + p[4];
		// illegal length, ignore packet
		if (payload >= TS_PACKET_SIZE) {
		    Debug(3, "tsdemux: illegal adaption field length\n");
		    goto next_packet;
		}
		break;
	}

	PesParse(PesDemuxAudio, p + payload, TS_PACKET_SIZE - payload,
	    p[1] & 0x40);
#if 0
	int tmp;

	//	check continuity
	tmp = p[3] & 0x0F;		// continuity counter
	if (((tsdx->CC + 1) & 0x0F) != tmp) {
	    Debug(3, "tsdemux: OUT OF SYNC: %d %d\n", tmp, tsdx->CC);
	    //TS discontinuity (received 8, expected 0) for PID
	}
	tsdx->CC = tmp;
#endif

      next_packet:
	p += TS_PACKET_SIZE;
	size -= TS_PACKET_SIZE;
    }

    return p - data;
}

#endif

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

    if (StreamFreezed) {		// stream freezed
	return 0;
    }
    if (SkipAudio || !MyAudioDecoder) {	// skip audio
	return size;
    }

    if (NewAudioStream) {
	// FIXME: does this clear the audio ringbuffer?
	CodecAudioClose(MyAudioDecoder);
	AudioSetBufferTime(0);
	AudioCodecID = CODEC_ID_NONE;
	AudioChannelID = -1;
	NewAudioStream = 0;
    }
    // Don't overrun audio buffers on replay
    if (AudioFreeBytes() < AUDIO_MIN_BUFFER_FREE) {
	return 0;
    }
    // PES header 0x00 0x00 0x01 ID
    // ID 0xBD 0xC0-0xCF

    // must be a PES start code
    if (size < 9 || !data || data[0] || data[1] || data[2] != 0x01) {
	Error(_("[softhddev] invalid PES audio packet\n"));
	return size;
    }
    n = data[8];			// header size

    if (size < 9 + n + 4) {		// wrong size
	if (size == 9 + n) {
	    Warning(_("[softhddev] empty audio packet\n"));
	} else {
	    Error(_("[softhddev] invalid audio packet %d bytes\n"), size);
	}
	return size;
    }

    if (data[7] & 0x80 && n >= 5) {
	AudioAvPkt->pts =
	    (int64_t) (data[9] & 0x0E) << 29 | data[10] << 22 | (data[11] &
	    0xFE) << 14 | data[12] << 7 | (data[13] & 0xFE) >> 1;
	//Debug(3, "audio: pts %#012" PRIx64 "\n", AudioAvPkt->pts);
    }
    if (0) {				// dts is unused
	if (data[7] & 0x40) {
	    AudioAvPkt->dts =
		(int64_t) (data[14] & 0x0E) << 29 | data[15] << 22 | (data[16]
		& 0xFE) << 14 | data[17] << 7 | (data[18] & 0xFE) >> 1;
	    Debug(3, "audio: dts %#012" PRIx64 "\n", AudioAvPkt->dts);
	}
    }

    p = data + 9 + n;
    n = size - 9 - n;			// skip pes header
    if (n + AudioAvPkt->stream_index > AudioAvPkt->size) {
	Fatal(_("[softhddev] audio buffer too small\n"));
	AudioAvPkt->stream_index = 0;
    }

    if (AudioChannelID != id) {
	AudioChannelID = id;
	AudioCodecID = CODEC_ID_NONE;
    }
    // Private stream + LPCM ID
    if ((id & 0xF0) == 0xA0) {
	if (n < 7) {
	    Error(_("[softhddev] invalid LPCM audio packet %d bytes\n"), size);
	    return size;
	}
	if (AudioCodecID != CODEC_ID_PCM_DVD) {
	    static int samplerates[] = { 48000, 96000, 44100, 32000 };
	    int samplerate;
	    int channels;
	    int bits_per_sample;

	    Debug(3, "[softhddev]%s: LPCM %d sr:%d bits:%d chan:%d\n",
		__FUNCTION__, id, p[5] >> 4, (((p[5] >> 6) & 0x3) + 4) * 4,
		(p[5] & 0x7) + 1);
	    CodecAudioClose(MyAudioDecoder);

	    bits_per_sample = (((p[5] >> 6) & 0x3) + 4) * 4;
	    if (bits_per_sample != 16) {
		Error(_
		    ("[softhddev] LPCM %d bits per sample aren't supported\n"),
		    bits_per_sample);
		// FIXME: handle unsupported formats.
	    }
	    samplerate = samplerates[p[5] >> 4];
	    channels = (p[5] & 0x7) + 1;

	    AudioSetBufferTime(400);
	    AudioSetup(&samplerate, &channels, 0);
	    if (samplerate != samplerates[p[5] >> 4]) {
		Error(_("[softhddev] LPCM %d sample-rate is unsupported\n"),
		    samplerates[p[5] >> 4]);
		// FIXME: support resample
	    }
	    if (channels != (p[5] & 0x7) + 1) {
		Error(_("[softhddev] LPCM %d channels are unsupported\n"),
		    (p[5] & 0x7) + 1);
		// FIXME: support resample
	    }
	    //CodecAudioOpen(MyAudioDecoder, NULL, CODEC_ID_PCM_DVD);
	    AudioCodecID = CODEC_ID_PCM_DVD;
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
	if (AudioCodecID == CODEC_ID_NONE) {
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
	// 5 bytes 0x0B77xxxxxx AC3 audio
	// PCM audio can't be found
	r = 0;
	codec_id = CODEC_ID_NONE;	// keep compiler happy
	if (id != 0xbd && FastMpegCheck(p)) {
	    r = MpegCheck(p, n);
	    codec_id = CODEC_ID_MP2;
	}
	if (id != 0xbd && !r && FastLatmCheck(p)) {
	    r = LatmCheck(p, n);
	    codec_id = CODEC_ID_AAC_LATM;
	}
	if ((id == 0xbd || (id & 0xF0) == 0x80) && !r && FastAc3Check(p)) {
	    r = Ac3Check(p, n);
	    codec_id = CODEC_ID_AC3;
	    /* faster ac3 detection at end of pes packet (no improvemnts)
	       if (AudioCodecID == codec_id && -r - 2 == n) {
	       r = n;
	       }
	     */
	}
	if (r < 0) {			// need more bytes
	    break;
	}
	if (r > 0) {
	    AVPacket avpkt[1];

	    // new codec id, close and open new
	    if (AudioCodecID != codec_id) {
		CodecAudioClose(MyAudioDecoder);
		CodecAudioOpen(MyAudioDecoder, NULL, codec_id);
		AudioCodecID = codec_id;
	    }
	    av_init_packet(avpkt);
	    avpkt->data = (void *)p;
	    avpkt->size = r;
	    avpkt->pts = AudioAvPkt->pts;
	    avpkt->dts = AudioAvPkt->dts;
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

#ifdef USE_TS_AUDIO

/**
**	Play transport stream audio packet.
**
**	@param data	data of exactly one complete TS packet
**	@param size	size of TS packet (always TS_PACKET_SIZE)
**
**	@returns number of bytes consumed;
*/
int PlayTsAudio(const uint8_t * data, int size)
{
    static TsDemux tsdx[1];

    if (StreamFreezed) {		// stream freezed
	return 0;
    }
    if (SkipAudio || !MyAudioDecoder) {	// skip audio
	return size;
    }
    // Don't overrun audio buffers on replay
    if (AudioFreeBytes() < AUDIO_MIN_BUFFER_FREE) {
	return 0;
    }

    if (NewAudioStream) {
	// FIXME: does this clear the audio ringbuffer?
	CodecAudioClose(MyAudioDecoder);
	AudioSetBufferTime(264);
	AudioCodecID = CODEC_ID_NONE;
	NewAudioStream = 0;
	PesReset(PesDemuxAudio);
    }
    return TsDemuxer(tsdx, data, size);
}

#endif

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

#define VIDEO_BUFFER_SIZE (512 * 1024)	///< video PES buffer default size
#define VIDEO_PACKET_MAX 192		///< max number of video packets
    /// video PES packet ring buffer
static AVPacket VideoPacketRb[VIDEO_PACKET_MAX];
static int VideoPacketWrite;		///< write pointer
static int VideoPacketRead;		///< read pointer
static atomic_t VideoPacketsFilled;	///< how many of the buffer is used

static volatile char VideoClearBuffers;	///< clear video buffers
static volatile char SkipVideo;		///< skip video
static volatile char VideoTrickSpeed;	///< current trick speed
static volatile char VideoTrickCounter;	///< current trick speed counter

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
	    Fatal(_("[softhddev] out of memory\n"));
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
	// FIXME: out of memory!
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
**	Reset current packet.
*/
static void VideoResetPacket(void)
{
    AVPacket *avpkt;

    avpkt = &VideoPacketRb[VideoPacketWrite];
    avpkt->stream_index = 0;
    avpkt->pts = AV_NOPTS_VALUE;
    avpkt->dts = AV_NOPTS_VALUE;
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
    VideoResetPacket();
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

    if (StreamFreezed) {		// stream freezed
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
    if (VideoTrickSpeed) {
	if (VideoTrickCounter++ < VideoTrickSpeed * 2) {
	    usleep(5 * 1000);
	    return 1;
	}
	VideoTrickCounter = 0;
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
**	Get number of video buffers.
*/
int VideoGetBuffers(void)
{
    return atomic_read(&VideoPacketsFilled);
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
    int z;
    int l;

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
    if (StreamFreezed) {		// stream freezed
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

    if (size < 9 + n + 4) {		// wrong size
	if (size == 9 + n) {
	    Warning(_("[softhddev] empty video packet\n"));
	} else {
	    Error(_("[softhddev] invalid video packet %d bytes\n"), size);
	}
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
	printf("%02x: %02x %02x %02x %02x %02x %02x %02x\n", data[6], check[0],
	    check[1], check[2], check[3], check[4], check[5], check[6]);
    }
#if 1					// FIXME: test code for better h264 detection
    z = 0;
    l = size - 9 - n;
    while (!*check) {			// count leading zeros
	if (--l < 4) {
	    Warning(_("[softhddev] empty video packet %d bytes\n"), size);
	    return size;
	}
	++check;
	++z;
    }

    // H264 Access Unit Delimiter 0x00 0x00 0x00 0x01 0x09
    if ((data[6] & 0xC0) == 0x80 && z > 2 && check[0] == 0x01
	&& check[1] == 0x09) {
	if (VideoCodecID == CODEC_ID_H264) {
	    VideoNextPacket(CODEC_ID_H264);
	} else {
	    Debug(3, "video: h264 detected\n");
	    VideoCodecID = CODEC_ID_H264;
	}
	// SKIP PES header
	VideoEnqueue(pts, check - 3, l + 3);
	return size;
    }
    // PES start code 0x00 0x00 0x01
    if (z > 1 && check[0] == 0x01) {
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
	// SKIP PES header
	VideoEnqueue(pts, check - 2, l + 2);
	return size;
    }
    // this happens when vdr sends incomplete packets

    if (VideoCodecID == CODEC_ID_NONE) {
	Debug(3, "video: not detected\n");
	return size;
    }
    // SKIP PES header
    VideoEnqueue(pts, data + 9 + n, size - 9 - n);

    // incomplete packets produce artefacts after channel switch
    // packet < 65526 is the last split packet, detect it here for
    // better latency
    if (size < 65526 && VideoCodecID == CODEC_ID_MPEG2VIDEO) {
	// mpeg codec supports incomplete packets
	// waiting for a full complete packages, increases needed delays
	VideoNextPacket(CODEC_ID_MPEG2VIDEO);
    }

    return size;
#else
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
#endif
}

    /// call VDR support function
extern uint8_t *CreateJpeg(uint8_t *, int *, int, int, int);

#if defined(USE_JPEG) && JPEG_LIB_VERSION >= 80

/**
**	Create a jpeg image in memory.
**
**	@param image		raw RGB image
**	@param raw_size		size of raw image
**	@param size[out]	size of jpeg image
**	@param quality		jpeg quality
**	@param width		number of horizontal pixels in image
**	@param height		number of vertical pixels in image
**
**	@returns allocated jpeg image.
*/
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
    if (width != -1 && height != -1) {
	Warning(_("softhddev: scaling unsupported\n"));
    }
    return VideoGrab(size, &width, &height, 1);
}

//////////////////////////////////////////////////////////////////////////////

/**
**	Set play mode, called on channel switch.
**
**	@param play_mode	play mode (none, video+audio, audio-only, ...)
*/
int SetPlayMode(int play_mode)
{
    VideoDisplayWakeup();
    if (MyVideoDecoder) {		// tell video parser we have new stream
	if (VideoCodecID != CODEC_ID_NONE) {
	    NewVideoStream = 1;
	    VideoSwitch = GetMsTicks();
	}
    }
    if (MyAudioDecoder) {		// tell audio parser we have new stream
	if (AudioCodecID != CODEC_ID_NONE) {
	    NewAudioStream = 1;
	}
    }
    if (play_mode == 2 || play_mode == 3) {
	Debug(3, "softhddev: FIXME: audio only, silence video errors\n");
    }
    Play();

    return 1;
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
    VideoTrickSpeed = speed;
    VideoTrickCounter = 0;
    StreamFreezed = 0;
}

/**
**	Clears all video and audio data from the device.
*/
void Clear(void)
{
    int i;

    VideoResetPacket();			// terminate work
    VideoClearBuffers = 1;
    AudioFlushBuffers();
    //NewAudioStream = 1;
    // FIXME: audio avcodec_flush_buffers, video is done by VideoClearBuffers

    for (i = 0; VideoClearBuffers && i < 20; ++i) {
	usleep(1 * 1000);
    }
}

/**
**	Sets the device into play mode.
*/
void Play(void)
{
    VideoTrickSpeed = 0;
    VideoTrickCounter = 0;
    StreamFreezed = 0;
    SkipAudio = 0;
    AudioPlay();
}

/**
**	Sets the device into "freeze frame" mode.
*/
void Freeze(void)
{
    StreamFreezed = 1;
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
    }
    //Clear();				// flush video buffers

    // +1 future for deinterlace
    for (i = -1; i < (VideoCodecID == CODEC_ID_MPEG2VIDEO ? 3 : 17); ++i) {
	//if ( 1 ) {
	const uint8_t *split;
	int n;

	if ((data[3] & 0xF0) == 0xE0) {	// PES packet
	    split = data;
	    n = size;
	    // split the I-frame into single pes packets
	    do {
		int len;

		len = (split[4] << 8) + split[5];
		if (!len || len + 6 > n) {
		    PlayVideo(split, n);	// feed remaining bytes
		    break;
		}
		PlayVideo(split, len + 6);	// feed it
		split += 6 + len;
		n -= 6 + len;
	    } while (n > 6);
	    VideoNextPacket(VideoCodecID);	// terminate last packet

	    if (VideoCodecID == CODEC_ID_H264) {
		VideoEnqueue(AV_NOPTS_VALUE, seq_end_h264,
		    sizeof(seq_end_h264));
	    } else {
		VideoEnqueue(AV_NOPTS_VALUE, seq_end_mpeg,
		    sizeof(seq_end_mpeg));
	    }
	    VideoNextPacket(VideoCodecID);	// terminate last packet
	} else {			// ES packet

	    if (VideoCodecID != CODEC_ID_MPEG2VIDEO) {
		VideoNextPacket(CODEC_ID_NONE);	// close last stream
		VideoCodecID = CODEC_ID_MPEG2VIDEO;
	    }
	    VideoEnqueue(AV_NOPTS_VALUE, data, size);
	    VideoEnqueue(AV_NOPTS_VALUE, seq_end_mpeg, sizeof(seq_end_mpeg));
	    VideoNextPacket(VideoCodecID);	// terminate last packet
	}
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
    if (atomic_read(&VideoPacketsFilled) >= VIDEO_PACKET_MAX * 2 / 3
	|| AudioFreeBytes() < AUDIO_MIN_BUFFER_FREE * 2) {
	if (timeout) {			// let display thread work
	    usleep(timeout * 1000);
	}
	return atomic_read(&VideoPacketsFilled) < VIDEO_PACKET_MAX * 2 / 3
	    && AudioFreeBytes() > AUDIO_MIN_BUFFER_FREE;
    }
    return 1;
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
	"  -p device\taudio device for pass-through (hw:0,1 or /dev/dsp1)\n"
	"  -c channel\taudio mixer channel name (fe. PCM)\n"
	"  -d display\tdisplay of x11 server (fe. :0.0)\n"
	"  -f\t\tstart with fullscreen window (only with window manager)\n"
	"  -g geometry\tx11 window geometry wxh+x+y\n"
	"  -s\t\tstart in suspended mode\n" "  -x\t\tstart x11 server\n"
	"  -w workaround\tenable/disable workarounds\n"
	"\tno-hw-decoder\t\tdisable hw decoder, use software decoder only\n"
	"\tno-mpeg-hw-decoder\tdisable hw decoder for mpeg only\n"
	"\talsa-driver-broken\tdisable broken alsa driver message\n"
	"\tignore-repeat-pict\tdisable repeat pict message\n";
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
	switch (getopt(argc, argv, "-a:c:d:fg:p:sw:x")) {
	    case 'a':			// audio device for pcm
		AudioSetDevice(optarg);
		continue;
	    case 'c':			// channel of audio mixer
		AudioSetChannel(optarg);
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
		} else if (!strcasecmp("ignore-repeat-pict", optarg)) {
		    VideoIgnoreRepeatPict = 1;
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
    av_free_packet(AudioAvPkt);

    StopVideo();

    CodecExit();
    //VideoPacketExit();

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
int Start(void)
{
    if (ConfigStartX11Server) {
	StartXServer();
    }
    CodecInit();

    if (!ConfigStartSuspended) {
	// FIXME: AudioInit for HDMI after X11 startup
	// StartAudio();
	AudioInit();
	av_new_packet(AudioAvPkt, AUDIO_BUFFER_SIZE);
	MyAudioDecoder = CodecAudioNewDecoder();
	AudioCodecID = CODEC_ID_NONE;
	AudioChannelID = -1;

	if (!ConfigStartX11Server) {
	    StartVideo();
	}
    } else {
	SkipVideo = 1;
	SkipAudio = 1;
    }
    pthread_mutex_init(&SuspendLockMutex, NULL);

#ifdef USE_TS_AUDIO
    PesInit(PesDemuxAudio);
#endif
    Info(_("[softhddev] ready%s\n"), ConfigStartSuspended ? " suspended" : "");

    return !ConfigStartSuspended;
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
	    av_free_packet(AudioAvPkt);
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
	// StartAudio();
	AudioInit();
	av_new_packet(AudioAvPkt, AUDIO_BUFFER_SIZE);
	MyAudioDecoder = CodecAudioNewDecoder();
	AudioCodecID = CODEC_ID_NONE;
	AudioChannelID = -1;
    }

    SkipVideo = 0;
    SkipAudio = 0;

    pthread_mutex_unlock(&SuspendLockMutex);
}
