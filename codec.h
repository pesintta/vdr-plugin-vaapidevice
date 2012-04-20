///
///	@file codec.h	@brief Codec module headerfile
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

/// @addtogroup Codec
/// @{

//----------------------------------------------------------------------------
//	Typedefs
//----------------------------------------------------------------------------

    /// Video decoder typedef.
typedef struct _video_decoder_ VideoDecoder;

    /// Audio decoder typedef.
typedef struct _audio_decoder_ AudioDecoder;

//----------------------------------------------------------------------------
//	Prototypes
//----------------------------------------------------------------------------

    /// Allocate a new video decoder context.
extern VideoDecoder *CodecVideoNewDecoder(VideoHwDecoder *);

    /// Deallocate a video decoder context.
extern void CodecVideoDelDecoder(VideoDecoder *);

    /// Open video codec.
extern void CodecVideoOpen(VideoDecoder *, const char *, int);

    /// Close video codec.
extern void CodecVideoClose(VideoDecoder *);

    /// Decode a video packet.
extern void CodecVideoDecode(VideoDecoder *, const AVPacket *);

    /// Flush video buffers.
extern void CodecVideoFlushBuffers(VideoDecoder *);

    /// Allocate a new audio decoder context.
extern AudioDecoder *CodecAudioNewDecoder(void);

    /// Deallocate an audio decoder context.
extern void CodecAudioDelDecoder(AudioDecoder *);

    /// Open audio codec.
extern void CodecAudioOpen(AudioDecoder *, const char *, int);

    /// Close audio codec.
extern void CodecAudioClose(AudioDecoder *);

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

/// @}
