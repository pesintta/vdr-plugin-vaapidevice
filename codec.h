/// Copyright (C) 2009 - 2013, 2015 by Johns. All Rights Reserved.
/// Copyright (C) 2018 by pesintta, rofafor.
///
/// SPDX-License-Identifier: AGPL-3.0-only

#include <libavutil/hwcontext.h>
#include <libavformat/avformat.h>

//----------------------------------------------------------------------------
//  Defines
//----------------------------------------------------------------------------

#define CodecPCM 0x01			///< PCM bit mask
#define CodecMPA 0x02			///< MPA bit mask (planned)
#define CodecAC3 0x04			///< AC-3 bit mask
#define CodecEAC3 0x08			///< E-AC-3 bit mask
#define CodecDTS 0x10			///< DTS bit mask (planned)

#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000

///
/// Video decoder structure.
///
struct _video_decoder_
{
    VideoHwDecoder *HwDecoder;		///< video hardware decoder

    AVFormatContext *FmtCtx;		///< format context
    AVCodec *VideoCodec;		///< video codec
    AVCodecContext *VideoCtx;		///< video codec context
    int FirstKeyFrame;			///< flag first frame
    AVFrame *Frame;			///< decoded video frame
};

//----------------------------------------------------------------------------
//  Typedefs
//----------------------------------------------------------------------------

    /// Video decoder typedef.
typedef struct _video_decoder_ VideoDecoder;

    /// Audio decoder typedef.
typedef struct _audio_decoder_ AudioDecoder;

//----------------------------------------------------------------------------
//  Variables
//----------------------------------------------------------------------------

    /// x11 display name
extern const char *X11DisplayName;

    /// HW device context from video module
extern AVBufferRef *HwDeviceContext;

//----------------------------------------------------------------------------
//  Prototypes
//----------------------------------------------------------------------------

    /// Allocate a new video decoder context.
extern VideoDecoder *CodecVideoNewDecoder(VideoHwDecoder *);

    /// Deallocate a video decoder context.
extern void CodecVideoDelDecoder(VideoDecoder *);

    /// Open video codec.
extern void CodecVideoOpen(VideoDecoder *);

    /// Close video codec.
extern void CodecVideoClose(VideoDecoder *);

    /// Decode a video packet.
extern void CodecVideoDecode(VideoDecoder *);

    /// Flush video buffers.
extern void CodecVideoFlushBuffers(VideoDecoder *);

    /// Allocate a new audio decoder context.
extern AudioDecoder *CodecAudioNewDecoder(void);

    /// Deallocate an audio decoder context.
extern void CodecAudioDelDecoder(AudioDecoder *);

    /// Open audio codec.
extern void CodecAudioOpen(AudioDecoder *, int);

    /// Close audio codec.
extern void CodecAudioClose(AudioDecoder *);

   /// Get audio decoder info
extern char *CodecAudioGetInfo(AudioDecoder *, int);

    /// Set audio drift correction.
extern void CodecSetAudioDrift(int);

    /// Set audio pass-through.
extern void CodecSetAudioPassthrough(int);

    /// Set audio downmix.
extern void CodecSetAudioDownmix(int);

    /// Decode an audio packet.
extern void CodecAudioDecode(AudioDecoder *, const AVPacket *);

    /// Flush audio buffers.
extern void CodecAudioFlushBuffers(AudioDecoder *);

    /// Setup and initialize codec module.
extern void CodecInit(void);

    /// Cleanup and exit codec module.
extern void CodecExit(void);

    /// Read packet callback from c++
extern int device_read_video_data(void *opaque, unsigned char *data, int size);
extern int device_read_audio_data(void *opaque, unsigned char *data, int size);

    /// Set buffering mode for callback from codec
extern void device_set_mode(int mode);

    /// Get Videotype from vdr for codec
extern int device_get_vtype();
