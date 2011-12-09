///
///	@file softhddev.c	@brief A software HD device plugin for VDR.
///
///	Copyright (c) 2011 by Johns.  All Rights Reserved.
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

#include "misc.h"
#include "softhddev.h"

#include "audio.h"
#include "video.h"
#include "codec.h"

#define DEBUG

static char BrokenThreadsAndPlugins;	///< broken vdr threads and plugins

//////////////////////////////////////////////////////////////////////////////
//	Audio
//////////////////////////////////////////////////////////////////////////////

static AudioDecoder *MyAudioDecoder;	///< audio decoder
static enum CodecID AudioCodecID;	///< current codec id

extern void AudioTest(void);		// FIXME:

/**
**	Play audio packet.
**
**	@param data	data of exactly one complete PES packet
**	@param size	size of PES packet
**	@param id	PES packet type
*/
void PlayAudio(const uint8_t * data, int size, uint8_t id)
{
    int n;
    AVPacket avpkt[1];

    if (BrokenThreadsAndPlugins) {
	return;
    }
    // PES header 0x00 0x00 0x01 ID
    // ID 0xBD 0xC0-0xCF

    // channel switch: SetAudioChannelDevice: SetDigitalAudioDevice:

    // Detect audio code
    // MPEG-PS mp2 MPEG1, MPEG2, AC3

    if (size < 9) {
	Error(_("[softhddev] invalid audio packet\n"));
	return;
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

    data += 9 + n;
    size -= 9 + n;			// skip pes header
    if (size <= 0) {
	Error(_("[softhddev] invalid audio packet\n"));
	return;
    }
    // Syncword - 0x0B77
    if (data[0] == 0x0B && data[1] == 0x77) {
	if (!MyAudioDecoder) {
	    MyAudioDecoder = CodecAudioNewDecoder();
	    AudioCodecID = CODEC_ID_NONE;
	}
	if (AudioCodecID != CODEC_ID_AC3) {
	    Debug(3, "[softhddev]%s: AC-3 %d\n", __FUNCTION__, id);
	    CodecAudioClose(MyAudioDecoder);

	    CodecAudioOpen(MyAudioDecoder, NULL, CODEC_ID_AC3);
	    AudioCodecID = CODEC_ID_AC3;
	}
	// Syncword - 0xFFFC - 0xFFFF
    } else if (data[0] == 0xFF && (data[1] & 0xFC) == 0xFC) {
	if (!MyAudioDecoder) {
	    MyAudioDecoder = CodecAudioNewDecoder();
	    AudioCodecID = CODEC_ID_NONE;
	}
	if (AudioCodecID != CODEC_ID_MP2) {
	    Debug(3, "[softhddev]%s: MP2 %d\n", __FUNCTION__, id);
	    CodecAudioClose(MyAudioDecoder);

	    CodecAudioOpen(MyAudioDecoder, NULL, CODEC_ID_MP2);
	    AudioCodecID = CODEC_ID_MP2;
	}
    } else {
	// no start package
	// FIXME: Nick/Viva sends this shit, need to find sync in packet
	// FIXME: otherwise it takes too long until sound appears
	if (AudioCodecID == CODEC_ID_NONE) {
	    Debug(3, "[softhddev]%s: ??? %d\n", __FUNCTION__, id);
	    return;
	}
    }

    // no decoder or codec known
    if (!MyAudioDecoder || AudioCodecID == CODEC_ID_NONE) {
	return;
    }

    avpkt->data = (void *)data;
    avpkt->size = size;
    //memset(avpkt->data + avpkt->size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
    CodecAudioDecode(MyAudioDecoder, avpkt);
}

/**
**	Mute audio device.
*/
void Mute(void)
{
    if (BrokenThreadsAndPlugins) {
	return;
    }
    AudioSetVolume(0);
}

/**
**	Set volume of audio device.
**
**	@param volume	VDR volume (0 .. 255)
*/
void SetVolumeDevice(int volume)
{
    if (BrokenThreadsAndPlugins) {
	return;
    }
    AudioSetVolume((volume * 100) / 255);
}

//////////////////////////////////////////////////////////////////////////////
//	Video
//////////////////////////////////////////////////////////////////////////////

#include <alsa/iatomic.h>		// portable atomic_t

uint32_t VideoSwitch;
static int NewVideoStream;		///< new video stream
static VideoDecoder *MyVideoDecoder;	///< video decoder
static enum CodecID VideoCodecID;	///< current codec id

static const char *X11DisplayName;	///< x11 display name
static volatile int Usr1Signal;		///< true got usr1 signal

    /// video PES buffer default size
#define VIDEO_BUFFER_SIZE (512 * 1024)
#define VIDEO_PACKET_MAX 128		///< max number of video packets
    /// video PES packet ring buffer
static AVPacket VideoPacketRb[VIDEO_PACKET_MAX];
static int VideoPacketWrite;		///< write pointer
static int VideoPacketRead;		///< read pointer
static atomic_t VideoPacketsFilled;	///< how many of the buffer is used
static int VideoMaxPacketSize;		///< biggest used packet buffer
static uint32_t VideoStartTick;		///< video start tick

extern void VideoWakeup(void);		///< wakeup video handler

/**
**	Initialize video packet ringbuffer.
*/
static void VideoPacketInit(void)
{
    int i;
    AVPacket *avpkt;

    Debug(4, "[softhddev]: %s\n", __FUNCTION__);

    for (i = 0; i < VIDEO_PACKET_MAX; ++i) {
	avpkt = &VideoPacketRb[i];
	// build a clean ffmpeg av packet
	av_init_packet(avpkt);
	avpkt->destruct = av_destruct_packet;
	avpkt->data = av_malloc(VIDEO_BUFFER_SIZE);
	if (!avpkt->data) {
	    Fatal(_("[softhddev]: out of memory\n"));
	}
	avpkt->size = VIDEO_BUFFER_SIZE;
	avpkt->priv = NULL;
    }

    atomic_set(&VideoPacketsFilled, 0);
}

/**
**	Place video data in packet ringbuffer.
*/
static void VideoEnqueue(int64_t pts, const void *data, int size)
{
    AVPacket *avpkt;

    // Debug(3, "video: enqueue %d\n", size);

    avpkt = &VideoPacketRb[VideoPacketWrite];
    if (!avpkt->stream_index) {		// add pts only for first added
	avpkt->pts = pts;
	avpkt->dts = pts;
    }
    if (avpkt->stream_index + size + FF_INPUT_BUFFER_PADDING_SIZE >=
	avpkt->size) {

	Warning(_("video: packet buffer too small for %d\n"),
	    avpkt->stream_index + size + FF_INPUT_BUFFER_PADDING_SIZE);

	av_grow_packet(avpkt,
	    ((size + FF_INPUT_BUFFER_PADDING_SIZE + VIDEO_BUFFER_SIZE / 2)
		/ (VIDEO_BUFFER_SIZE / 2)) * (VIDEO_BUFFER_SIZE / 2));
	if (avpkt->size <
	    avpkt->stream_index + size + FF_INPUT_BUFFER_PADDING_SIZE) {
	    abort();
	}
    }
#ifdef xxDEBUG
    if (!avpkt->stream_index) {		// debug save time of first packet
	avpkt->pos = GetMsTicks();
    }
#endif
    if (!VideoStartTick) {		// tick of first valid packet
	VideoStartTick = GetMsTicks();
    }

    memcpy(avpkt->data + avpkt->stream_index, data, size);
    avpkt->stream_index += size;
    if (avpkt->stream_index > VideoMaxPacketSize) {
	VideoMaxPacketSize = avpkt->stream_index;
	Debug(3, "video: max used PES packet size: %d\n", VideoMaxPacketSize);
    }
}

/**
**	Finish current packet advance to next.
*/
static void VideoNextPacket(int codec_id)
{
    AVPacket *avpkt;

    avpkt = &VideoPacketRb[VideoPacketWrite];
    if (!avpkt->stream_index) {		// ignore empty packets
	if (codec_id == CODEC_ID_NONE) {
	    Debug(3, "video: possible stream change loss\n");
	}
	return;
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

    // intialize next package to use
    avpkt = &VideoPacketRb[VideoPacketWrite];
    avpkt->stream_index = 0;
    avpkt->pts = AV_NOPTS_VALUE;
    avpkt->dts = AV_NOPTS_VALUE;

    VideoWakeup();
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

    filled = atomic_read(&VideoPacketsFilled);
    //Debug(3, "video: decode %3d packets buffered\n", filled);
    if (!filled) {
	// Debug(3, "video: decode no packets buffered\n");
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
	    break;
	case CODEC_ID_MPEG2VIDEO:
	    if (last_codec_id != CODEC_ID_MPEG2VIDEO) {
		last_codec_id = CODEC_ID_MPEG2VIDEO;
		CodecVideoOpen(MyVideoDecoder, 0 ? "mpegvideo_vdpau" : NULL,
		    CODEC_ID_MPEG2VIDEO);
	    }
	    break;
	case CODEC_ID_H264:
	    if (last_codec_id != CODEC_ID_H264) {
		last_codec_id = CODEC_ID_H264;
		CodecVideoOpen(MyVideoDecoder, 0 ? "h264video_vdpau" : NULL,
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

    CodecVideoDecode(MyVideoDecoder, avpkt);

    avpkt->size = saved_size;

  skip:
    // advance packet read
    VideoPacketRead = (VideoPacketRead + 1) % VIDEO_PACKET_MAX;
    atomic_dec(&VideoPacketsFilled);

    return 0;
}

/**
**	Flush video buffer.
*/
void VideoFlushInput(void)
{
    // flush all buffered packets
    while (atomic_read(&VideoPacketsFilled)) {
	VideoPacketRead = (VideoPacketRead + 1) % VIDEO_PACKET_MAX;
	atomic_dec(&VideoPacketsFilled);
    }
    VideoStartTick = 0;
}

/**
**	Wakeup video handler.
*/
void VideoWakeup(void)
{
    int filled;
    uint32_t now;
    uint64_t delay;

    VideoDisplayHandler();
    return;

    filled = atomic_read(&VideoPacketsFilled);
    if (!filled) {
	Debug(3, "video: wakeup no packets buffered\n");
	return;
    }

    now = GetMsTicks();
    if (filled < VIDEO_PACKET_MAX && VideoStartTick + 1000 > now) {
	delay = AudioGetDelay() / 90;
	if (delay < 100) {		// no audio delay known
	    delay = 750;
	}
	delay -= 40;
	if (VideoStartTick + delay > now) {
	    Debug(3, "video: %d packets %u/%lu delayed\n", filled,
		(unsigned)(now - VideoStartTick), delay);
	    return;
	}
    }

    VideoDecode();

#if 0
    AVPacket *avpkt;

    while (filled) {
	avpkt = &VideoPacketRb[VideoPacketRead];
	now = GetMsTicks();
	if (avpkt->pos + 500 > now) {
	    Debug(3, "video: %d packets %u delayed\n", filled,
		(unsigned)(now - avpkt->pos));
	    return;
	}
	filled = atomic_read(&VideoPacketsFilled);
    }
#endif
}

/**
**	Try video start.
**
**	Could be called, when already started.
*/
static void StartVideo(void)
{
    VideoInit(X11DisplayName);
    VideoOsdInit();
    if (!MyVideoDecoder) {
	VideoHwDecoder *hw_decoder;

	if ((hw_decoder = VideoNewHwDecoder())) {
	    MyVideoDecoder = CodecVideoNewDecoder(hw_decoder);
	    VideoCodecID = CODEC_ID_NONE;
	}
    }
    VideoPacketInit();
}

/**
**	Play video packet.
**
**	@param data	data of exactly one complete PES packet
**	@param size	size of PES packet
**
**	@note vdr sends incomplete packets, va-api h264 decoder only
**	supports complete packets.
**	We buffer here until we receive an complete PES Packet, which
**	is no problem, the audio is always far behind us.
*/
void PlayVideo(const uint8_t * data, int size)
{
    const uint8_t *check;
    int64_t pts;
    int n;

    if (BrokenThreadsAndPlugins) {
	return;
    }
    if (Usr1Signal) {			// x11 server ready
	Usr1Signal = 0;
	StartVideo();
    }
    if (!MyVideoDecoder) {		// no x11 video started
	return;
    }
    if (NewVideoStream) {
	Debug(3, "video: new stream %d\n", GetMsTicks() - VideoSwitch);
	VideoNextPacket(CODEC_ID_NONE);
	VideoCodecID = CODEC_ID_NONE;
	NewVideoStream = 0;
    }
    // must be a PES start code
    if (data[0] || data[1] || data[2] != 0x01 || size < 9) {
	Error(_("[softhddev] invalid PES video packet\n"));
	return;
    }
    n = data[8];			// header size
    // wrong size
    if (size < 9 + n) {
	Error(_("[softhddev] invalid video packet\n"));
	return;
    }
    check = data + 9 + n;

    // FIXME: get pts/dts, when we need it

    pts = AV_NOPTS_VALUE;
    if (data[7] & 0x80) {
	pts =
	    (int64_t) (data[9] & 0x0E) << 29 | data[10] << 22 | (data[11] &
	    0xFE) << 14 | data[12] << 7 | (data[13] & 0xFE) >> 1;
	//Debug(3, "video: pts %#012" PRIx64 "\n", pts);
    }
    // FIXME: no valid mpeg2/h264 detection yet

    if (0) {
	printf("%02x: %02x %02x %02x %02x %02x\n", data[6], check[0], check[1],
	    check[2], check[3], check[4]);
    }
    // PES_VIDEO_STREAM 0xE0 or PES start code
    if ((data[6] & 0xC0) != 0x80 || (!check[0] && !check[1]
	    && check[2] == 0x1)) {
	if (VideoCodecID == CODEC_ID_MPEG2VIDEO) {
	    VideoNextPacket(CODEC_ID_MPEG2VIDEO);
	} else {
	    Debug(3, "video: mpeg2 detected\n");
	    VideoCodecID = CODEC_ID_MPEG2VIDEO;
	}
	// Access Unit Delimiter
    } else if (!check[0] && !check[1] && !check[2] && check[3] == 0x1
	&& check[4] == 0x09) {
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
	    return;
	}
	if (VideoCodecID == CODEC_ID_MPEG2VIDEO) {
	    // mpeg codec supports incomplete packages
	    VideoNextPacket(CODEC_ID_MPEG2VIDEO);
	}
    }

    // SKIP PES header
    size -= 9 + n;
    VideoEnqueue(pts, check, size);
}

//////////////////////////////////////////////////////////////////////////////

/**
**	Set play mode, called on channel switch.
*/
void SetPlayMode(void)
{
    if (BrokenThreadsAndPlugins) {
	return;
    }
    if (MyVideoDecoder) {
	if (VideoCodecID != CODEC_ID_NONE) {
	    NewVideoStream = 1;
	    VideoSwitch = GetMsTicks();
	}
    }
    if (MyAudioDecoder) {
	// FIXME: does this clear the audio ringbuffer?
	CodecAudioClose(MyAudioDecoder);
	AudioCodecID = CODEC_ID_NONE;
    }
}

//////////////////////////////////////////////////////////////////////////////
//	OSD
//////////////////////////////////////////////////////////////////////////////

/**
**	Get OSD size and aspect.
*/
void GetOsdSize(int *width, int *height, double *aspect)
{
    static char done;

    // FIXME: should be configured!
    *width = 1920;
    *height = 1080;
    //*width = 768;
    //*height = 576;

    *aspect = 16.0 / 9.0 / (double)*width * (double)*height;

    if (!done) {
	Debug(3, "[softhddev]%s: %dx%d %g\n", __FUNCTION__, *width, *height,
	    *aspect);
	done = 1;
    }
}

/**
**	Close OSD.
*/
void OsdClose(void)
{
    if (BrokenThreadsAndPlugins) {
	return;
    }
    VideoOsdClear();
}

/**
**	Draw an OSD pixmap.
*/
void OsdDrawARGB(int x, int y, int height, int width, const uint8_t * argb)
{
    if (BrokenThreadsAndPlugins) {
	return;
    }
    VideoOsdDrawARGB(x, y, height, width, argb);
}

//////////////////////////////////////////////////////////////////////////////

static char StartX11Server;		///< flag start the x11 server

/**
**	Return command line help string.
*/
const char *CommandLineHelp(void)
{
    return "  -a device\talsa audio device (fe. hw:0,0)\n"
	"  -d display\tdisplay of x11 server (f.e :0.0)\n"
	"  -g geometry\tx11 window geometry wxh+x+y\n"
	"  -x\tstart x11 server\n";
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
	switch (getopt(argc, argv, "-a:d:g:x")) {
	    case 'a':			// audio device
		AudioSetDevice(optarg);
		continue;
	    case 'd':			// x11 display name
		X11DisplayName = optarg;
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
		StartX11Server = 1;
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
}

/**
**	Prepare plugin.
*/
void Start(void)
{
    if (StartX11Server) {
	StartXServer();
    }
    CodecInit();
    // FIXME: AudioInit for HDMI after X11 startup
    AudioInit();
    if (!StartX11Server) {
	StartVideo();
    }
}

/**
**	Stop plugin.
*/
void Stop(void)
{
    Debug(3, "video: max used PES packet size: %d\n", VideoMaxPacketSize);

    // FIXME:
    // don't let any thread enter our plugin, but can still crash, when
    // a thread has called any function, while Stop is called.
    BrokenThreadsAndPlugins = 1;
    usleep(2 * 1000);

    // lets hope that vdr does a good thead cleanup
    // no it doesn't do a good thread cleanup
    if (MyVideoDecoder) {
	CodecVideoClose(MyVideoDecoder);
	MyVideoDecoder = NULL;
    }
    if (MyAudioDecoder) {
	CodecAudioClose(MyAudioDecoder);
	MyAudioDecoder = NULL;
    }

    VideoExit();
    AudioExit();
    CodecExit();

    if (StartX11Server) {
	Debug(3, "x-setup: Stop x11 server\n");

	if (X11ServerPid) {
	    kill(X11ServerPid, SIGTERM);
	}
    }
}

/**
**	Main thread hook, periodic called from main thread.
*/
void MainThreadHook(void)
{
    VideoDisplayHandler();
}
