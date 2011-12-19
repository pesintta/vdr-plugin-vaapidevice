///
///	@file video.h	@brief Video module header file
///
///	Copyright (c) 2009 - 2011 by Johns.  All Rights Reserved.
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

/// @addtogroup Video
/// @{

//----------------------------------------------------------------------------
//	Typedefs
//----------------------------------------------------------------------------

    /// Video hardware decoder typedef
typedef struct _video_hw_decoder_ VideoHwDecoder;

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

//extern unsigned VideoWindowWidth;	///< current video output width
//extern unsigned VideoWindowHeight;	///< current video output height

//----------------------------------------------------------------------------
//	Prototypes
//----------------------------------------------------------------------------

    /// Allocate new video hardware decoder.
extern VideoHwDecoder *VideoNewHwDecoder(void);

    /// Get and allocate a video hardware surface.
extern unsigned VideoGetSurface(VideoHwDecoder *);

    /// Release a video hardware surface.
extern void VideoReleaseSurface(VideoHwDecoder *, unsigned);

#ifdef LIBAVCODEC_VERSION
    /// Render a ffmpeg frame
extern void VideoRenderFrame(VideoHwDecoder *, AVCodecContext *, AVFrame *);

    /// Get ffmpeg vaapi context
extern struct vaapi_context *VideoGetVaapiContext(VideoHwDecoder *);

    /// Callback to negotiate the PixelFormat.
extern enum PixelFormat Video_get_format(VideoHwDecoder *, AVCodecContext *,
    const enum PixelFormat *);
#endif

    /// Display video TEST
extern void VideoDisplayHandler(void);

    /// Poll video events
extern void VideoPollEvent(void);

    /// set video mode
//extern void VideoSetVideoMode(int, int, int, int);

    /// set video geometry
extern int VideoSetGeometry(const char *);

    /// set deinterlace
extern void VideoSetDeinterlace(int);

    /// set scaling
extern void VideoSetScaling(int);

    /// set audio delay
extern void VideoSetAudioDelay(int);

    /// Clear OSD
extern void VideoOsdClear(void);

    /// Draw an OSD ARGB image
extern void VideoOsdDrawARGB(int, int, int, int, const uint8_t *);

extern int64_t VideoGetClock(void);	///< get video clock

extern void VideoOsdInit(void);		///< setup osd
extern void VideoOsdExit(void);		///< cleanup osd

extern void VideoInit(const char *);	///< setup video module
extern void VideoExit(void);		///< cleanup and exit video module

extern void VideoFlushInput(void);	///< flush codec input buffers
extern int VideoDecode(void);		///< decode

/// @}
