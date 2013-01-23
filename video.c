///
///	@file video.c	@brief Video module
///
///	Copyright (c) 2009 - 2013 by Johns.  All Rights Reserved.
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

///
///	@defgroup Video The video module.
///
///	This module contains all video rendering functions.
///
///	@todo disable screen saver support
///
///	Uses Xlib where it is needed for VA-API or vdpau.  XCB is used for
///	everything else.
///
///	- X11
///	- OpenGL rendering
///	- OpenGL rendering with GLX texture-from-pixmap
///	- Xrender rendering
///
///	@todo FIXME: use vaErrorStr for all VA-API errors.
///

#define USE_XLIB_XCB			///< use xlib/xcb backend
#define USE_SCREENSAVER			///< support disable screensaver
#define USE_AUTOCROP			///< compile auto-crop support
#define USE_GRAB			///< experimental grab code
#define noUSE_GLX			///< outdated GLX code
#define noUSE_DOUBLEBUFFER		///< use GLX double buffers
//#define USE_VAAPI				///< enable vaapi support
//#define USE_VDPAU				///< enable vdpau support
#define noUSE_BITMAP			///< use vdpau bitmap surface
//#define AV_INFO				///< log a/v sync informations
#ifndef AV_INFO_TIME
#define AV_INFO_TIME (50 * 60)		///< a/v info every minute
#endif

#define USE_VIDEO_THREAD		///< run decoder in an own thread
//#define USE_VIDEO_THREAD2		///< run decoder+display in own threads

#include <sys/time.h>
#include <sys/shm.h>
#include <sys/ipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#include <alsa/iatomic.h>		// portable atomic_t

#ifdef USE_VIDEO_THREAD
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>
#include <time.h>
#include <signal.h>
#ifndef HAVE_PTHREAD_NAME
    /// only available with newer glibc
#define pthread_setname_np(thread, name)
#endif
#endif

#ifdef USE_XLIB_XCB
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <xcb/xcb.h>
//#include <xcb/bigreq.h>
//#include <xcb/glx.h>
//#include <xcb/randr.h>
#ifdef USE_SCREENSAVER
#include <xcb/screensaver.h>
#include <xcb/dpms.h>
#endif
//#include <xcb/shm.h>
//#include <xcb/xv.h>

//#include <xcb/xcb_image.h>
//#include <xcb/xcb_event.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#ifdef XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS
#include <xcb/xcb_ewmh.h>
#else // compatibility hack for old xcb-util

/**
 * @brief Action on the _NET_WM_STATE property
 */
typedef enum
{
    /* Remove/unset property */
    XCB_EWMH_WM_STATE_REMOVE = 0,
    /* Add/set property */
    XCB_EWMH_WM_STATE_ADD = 1,
    /* Toggle property	*/
    XCB_EWMH_WM_STATE_TOGGLE = 2
} xcb_ewmh_wm_state_action_t;
#endif
#endif

#ifdef USE_GLX
#include <GL/gl.h>			// For GL_COLOR_BUFFER_BIT
#include <GL/glx.h>
// only for gluErrorString
#include <GL/glu.h>
#endif

#ifdef USE_VAAPI
#include <va/va_x11.h>
#ifdef USE_GLX
#include <va/va_glx.h>
#endif
#ifndef VA_SURFACE_ATTRIB_SETTABLE
/// make source compatible with old libva
#define vaCreateSurfaces(d, f, w, h, s, ns, a, na) \
    vaCreateSurfaces(d, w, h, f, ns, s)
#endif
#endif

#ifdef USE_VDPAU
#include <vdpau/vdpau_x11.h>
#include <libavcodec/vdpau.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavcodec/vaapi.h>
#include <libavutil/pixdesc.h>

#include "misc.h"
#include "video.h"
#include "audio.h"

#ifdef USE_XLIB_XCB

//----------------------------------------------------------------------------
//	Declarations
//----------------------------------------------------------------------------

///
///	Video resolutions selector.
///
typedef enum _video_resolutions_
{
    VideoResolution576i,		///< ...x576 interlaced
    VideoResolution720p,		///< ...x720 progressive
    VideoResolutionFake1080i,		///< 1280x1080 1440x1080 interlaced
    VideoResolution1080i,		///< 1920x1080 interlaced
    VideoResolutionMax			///< number of resolution indexs
} VideoResolutions;

///
///	Video deinterlace modes.
///
typedef enum _video_deinterlace_modes_
{
    VideoDeinterlaceBob,		///< bob deinterlace
    VideoDeinterlaceWeave,		///< weave deinterlace
    VideoDeinterlaceTemporal,		///< temporal deinterlace
    VideoDeinterlaceTemporalSpatial,	///< temporal spatial deinterlace
    VideoDeinterlaceSoftBob,		///< software bob deinterlace
    VideoDeinterlaceSoftSpatial,	///< software spatial deinterlace
} VideoDeinterlaceModes;

///
///	Video scaleing modes.
///
typedef enum _video_scaling_modes_
{
    VideoScalingNormal,			///< normal scaling
    VideoScalingFast,			///< fastest scaling
    VideoScalingHQ,			///< high quality scaling
    VideoScalingAnamorphic,		///< anamorphic scaling
} VideoScalingModes;

///
///	Video zoom modes.
///
typedef enum _video_zoom_modes_
{
    VideoNormal,			///< normal
    VideoStretch,			///< stretch to all edges
    VideoCenterCutOut,			///< center and cut out
    VideoAnamorphic,			///< anamorphic scaled (unsupported)
} VideoZoomModes;

///
///	Video color space conversions.
///
typedef enum _video_color_space_
{
    VideoColorSpaceNone,		///< no conversion
    VideoColorSpaceBt601,		///< ITU.BT-601 Y'CbCr
    VideoColorSpaceBt709,		///< ITU.BT-709 HDTV Y'CbCr
    VideoColorSpaceSmpte240		///< SMPTE-240M Y'PbPr
} VideoColorSpace;

///
///	Video output module structure and typedef.
///
typedef struct _video_module_
{
    const char *Name;			///< video output module name
    char Enabled;			///< flag output module enabled

    /// allocate new video hw decoder
    VideoHwDecoder *(*const NewHwDecoder)(VideoStream *);
    void (*const DelHwDecoder) (VideoHwDecoder *);
    unsigned (*const GetSurface) (VideoHwDecoder *, const AVCodecContext *);
    void (*const ReleaseSurface) (VideoHwDecoder *, unsigned);
    enum PixelFormat (*const get_format) (VideoHwDecoder *, AVCodecContext *,
	const enum PixelFormat *);
    void (*const RenderFrame) (VideoHwDecoder *, const AVCodecContext *,
	const AVFrame *);
    void (*const SetClock) (VideoHwDecoder *, int64_t);
     int64_t(*const GetClock) (const VideoHwDecoder *);
    void (*const SetTrickSpeed) (const VideoHwDecoder *, int);
    uint8_t *(*const GrabOutput)(int *, int *, int *);
    void (*const SetBackground) (uint32_t);
    void (*const SetVideoMode) (void);
    void (*const ResetAutoCrop) (void);

    /// module display handler thread
    void (*const DisplayHandlerThread) (void);

    void (*const OsdClear) (void);	///< clear OSD
    /// draw OSD ARGB area
    void (*const OsdDrawARGB) (int, int, int, int, const uint8_t *);
    void (*const OsdInit) (int, int);	///< initialize OSD
    void (*const OsdExit) (void);	///< cleanup OSD

    int (*const Init) (const char *);	///< initialize video output module
    void (*const Exit) (void);		///< cleanup video output module
} VideoModule;

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

#define CODEC_SURFACES_MAX	31	///< maximal of surfaces

#define CODEC_SURFACES_DEFAULT	21	///< default of surfaces
// FIXME: video-xvba only supports 14
#define xCODEC_SURFACES_DEFAULT	14	///< default of surfaces

#define CODEC_SURFACES_MPEG2	3	///< 1 decode, up to  2 references
#define CODEC_SURFACES_MPEG4	3	///< 1 decode, up to  2 references
#define CODEC_SURFACES_H264	21	///< 1 decode, up to 20 references
#define CODEC_SURFACES_VC1	3	///< 1 decode, up to  2 references

#define VIDEO_SURFACES_MAX	4	///< video output surfaces for queue
#define OUTPUT_SURFACES_MAX	4	///< output surfaces for flip page

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

char VideoIgnoreRepeatPict;		///< disable repeat pict warning

static const char *VideoDriverName;	///< video output device
static Display *XlibDisplay;		///< Xlib X11 display
static xcb_connection_t *Connection;	///< xcb connection
static xcb_colormap_t VideoColormap;	///< video colormap
static xcb_window_t VideoWindow;	///< video window
static xcb_screen_t const *VideoScreen;	///< video screen
static uint32_t VideoBlankTick;		///< blank cursor timer
static xcb_pixmap_t VideoCursorPixmap;	///< blank curosr pixmap
static xcb_cursor_t VideoBlankCursor;	///< empty invisible cursor

static int VideoWindowX;		///< video output window x coordinate
static int VideoWindowY;		///< video outout window y coordinate
static unsigned VideoWindowWidth;	///< video output window width
static unsigned VideoWindowHeight;	///< video output window height

static const VideoModule NoopModule;	///< forward definition of noop module

    /// selected video module
static const VideoModule *VideoUsedModule = &NoopModule;

char VideoHardwareDecoder = -1;		///< flag use hardware decoder

static char VideoSurfaceModesChanged;	///< flag surface modes changed

    /// flag use transparent OSD.
static const char VideoTransparentOsd = 1;

static uint32_t VideoBackground;	///< video background color
static char VideoStudioLevels;		///< flag use studio levels

    /// Default deinterlace mode.
static VideoDeinterlaceModes VideoDeinterlace[VideoResolutionMax];

    /// Default number of deinterlace surfaces
static const int VideoDeinterlaceSurfaces = 4;

    /// Default skip chroma deinterlace flag (VDPAU only).
static char VideoSkipChromaDeinterlace[VideoResolutionMax];

    /// Default inverse telecine flag (VDPAU only).
static char VideoInverseTelecine[VideoResolutionMax];

    /// Default amount of noise reduction algorithm to apply (0 .. 1000).
static int VideoDenoise[VideoResolutionMax];

    /// Default amount of sharpening, or blurring, to apply (-1000 .. 1000).
static int VideoSharpen[VideoResolutionMax];

    /// Default cut top and bottom in pixels
static int VideoCutTopBottom[VideoResolutionMax];

    /// Default cut left and right in pixels
static int VideoCutLeftRight[VideoResolutionMax];

    /// Color space ITU-R BT.601, ITU-R BT.709, ...
static const VideoColorSpace VideoColorSpaces[VideoResolutionMax] = {
    VideoColorSpaceBt601, VideoColorSpaceBt709, VideoColorSpaceBt709,
    VideoColorSpaceBt709
};

    /// Default scaling mode
static VideoScalingModes VideoScaling[VideoResolutionMax];

    /// Default audio/video delay
int VideoAudioDelay;

    /// Default zoom mode for 4:3
static VideoZoomModes Video4to3ZoomMode;

    /// Default zoom mode for 16:9 and others
static VideoZoomModes VideoOtherZoomMode;

static char Video60HzMode;		///< handle 60hz displays
static char VideoSoftStartSync;		///< soft start sync audio/video
static const int VideoSoftStartFrames = 100;	///< soft start frames
static char VideoShowBlackPicture;	///< flag show black picture

static xcb_atom_t WmDeleteWindowAtom;	///< WM delete message atom
static xcb_atom_t NetWmState;		///< wm-state message atom
static xcb_atom_t NetWmStateFullscreen;	///< fullscreen wm-state message atom

#ifdef DEBUG
extern uint32_t VideoSwitch;		///< ticks for channel switch
#endif
extern void AudioVideoReady(int64_t);	///< tell audio video is ready

#ifdef USE_VIDEO_THREAD

static pthread_t VideoThread;		///< video decode thread
static pthread_cond_t VideoWakeupCond;	///< wakeup condition variable
static pthread_mutex_t VideoMutex;	///< video condition mutex
static pthread_mutex_t VideoLockMutex;	///< video lock mutex

#endif

#ifdef USE_VIDEO_THREAD2

static pthread_t VideoDisplayThread;	///< video decode thread
static pthread_cond_t VideoWakeupCond;	///< wakeup condition variable
static pthread_mutex_t VideoDisplayMutex;	///< video condition mutex
static pthread_mutex_t VideoDisplayLockMutex;	///< video lock mutex

#endif

static int OsdConfigWidth;		///< osd configured width
static int OsdConfigHeight;		///< osd configured height
static char OsdShown;			///< flag show osd
static char Osd3DMode;			///< 3D OSD mode
static int OsdWidth;			///< osd width
static int OsdHeight;			///< osd height
static int OsdDirtyX;			///< osd dirty area x
static int OsdDirtyY;			///< osd dirty area y
static int OsdDirtyWidth;		///< osd dirty area width
static int OsdDirtyHeight;		///< osd dirty area height

static int64_t VideoDeltaPTS;		///< FIXME: fix pts

//----------------------------------------------------------------------------
//	Common Functions
//----------------------------------------------------------------------------

static void VideoThreadLock(void);	///< lock video thread
static void VideoThreadUnlock(void);	///< unlock video thread
static void VideoThreadExit(void);	///< exit/kill video thread

///
///	Update video pts.
///
///	@param pts_p		pointer to pts
///	@param interlaced	interlaced flag (frame isn't right)
///	@param frame		frame to display
///
///	@note frame->interlaced_frame can't be used for interlace detection
///
static void VideoSetPts(int64_t * pts_p, int interlaced, const AVFrame * frame)
{
    int64_t pts;

    // update video clock
    if (*pts_p != (int64_t) AV_NOPTS_VALUE) {
	*pts_p += interlaced ? 40 * 90 : 20 * 90;
	//Info("video: %s +pts\n", Timestamp2String(*pts_p));
    }
    //av_opt_ptr(avcodec_get_frame_class(), frame, "best_effort_timestamp");
    //pts = frame->best_effort_timestamp;
    pts = frame->pkt_pts;
    if (pts == (int64_t) AV_NOPTS_VALUE || !pts) {
	// libav: 0.8pre didn't set pts
	pts = frame->pkt_dts;
    }
    // libav: sets only pkt_dts which can be 0
    if (pts && pts != (int64_t) AV_NOPTS_VALUE) {
	// build a monotonic pts
	if (*pts_p != (int64_t) AV_NOPTS_VALUE) {
	    int64_t delta;

	    delta = pts - *pts_p;
	    // ignore negative jumps
	    if (delta > -600 * 90 && delta <= -40 * 90) {
		if (-delta > VideoDeltaPTS) {
		    VideoDeltaPTS = -delta;
		    Debug(4,
			"video: %#012" PRIx64 "->%#012" PRIx64 " delta%+4"
			PRId64 " pts\n", *pts_p, pts, pts - *pts_p);
		}
		return;
	    }
	} else {			// first new clock value
	    AudioVideoReady(pts);
	}
	if (*pts_p != pts) {
	    Debug(4,
		"video: %#012" PRIx64 "->%#012" PRIx64 " delta=%4" PRId64
		" pts\n", *pts_p, pts, pts - *pts_p);
	    *pts_p = pts;
	}
    }
}

///
///	Update output for new size or aspect ratio.
///
///	@param input_aspect_ratio	video stream aspect
///
static void VideoUpdateOutput(AVRational input_aspect_ratio, int input_width,
    int input_height, VideoResolutions resolution, int video_x, int video_y,
    int video_width, int video_height, int *output_x, int *output_y,
    int *output_width, int *output_height, int *crop_x, int *crop_y,
    int *crop_width, int *crop_height)
{
    AVRational display_aspect_ratio;
    AVRational tmp_ratio;

    if (!input_aspect_ratio.num || !input_aspect_ratio.den) {
	input_aspect_ratio.num = 1;
	input_aspect_ratio.den = 1;
	Debug(3, "video: aspect defaults to %d:%d\n", input_aspect_ratio.num,
	    input_aspect_ratio.den);
    }

    av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den,
	input_width * input_aspect_ratio.num,
	input_height * input_aspect_ratio.den, 1024 * 1024);

    // InputWidth/Height can be zero = uninitialized
    if (!input_aspect_ratio.num || !input_aspect_ratio.den) {
	input_aspect_ratio.num = 1;
	input_aspect_ratio.den = 1;
    }

    display_aspect_ratio.num =
	VideoScreen->width_in_pixels * VideoScreen->height_in_millimeters;
    display_aspect_ratio.den =
	VideoScreen->height_in_pixels * VideoScreen->width_in_millimeters;

    display_aspect_ratio = av_mul_q(input_aspect_ratio, display_aspect_ratio);
    Debug(3, "video: aspect %d:%d\n", display_aspect_ratio.num,
	display_aspect_ratio.den);

    *crop_x = VideoCutLeftRight[resolution];
    *crop_y = VideoCutTopBottom[resolution];
    *crop_width = input_width - VideoCutLeftRight[resolution] * 2;
    *crop_height = input_height - VideoCutTopBottom[resolution] * 2;

    // FIXME: store different positions for the ratios
    tmp_ratio.num = 4;
    tmp_ratio.den = 3;
#ifdef DEBUG
    fprintf(stderr, "ratio: %d:%d %d:%d\n", input_aspect_ratio.num,
	input_aspect_ratio.den, display_aspect_ratio.num,
	display_aspect_ratio.den);
#endif
    if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	switch (Video4to3ZoomMode) {
	    case VideoNormal:
		goto normal;
	    case VideoStretch:
		goto stretch;
	    case VideoCenterCutOut:
		goto center_cut_out;
	    case VideoAnamorphic:
		// FIXME: rest should be done by hardware
		goto stretch;
	}
    }
    switch (VideoOtherZoomMode) {
	case VideoNormal:
	    goto normal;
	case VideoStretch:
	    goto stretch;
	case VideoCenterCutOut:
	    goto center_cut_out;
	case VideoAnamorphic:
	    // FIXME: rest should be done by hardware
	    goto stretch;
    }

  normal:
    *output_x = video_x;
    *output_y = video_y;
    *output_width =
	(video_height * display_aspect_ratio.num + display_aspect_ratio.den -
	1) / display_aspect_ratio.den;
    *output_height =
	(video_width * display_aspect_ratio.den + display_aspect_ratio.num -
	1) / display_aspect_ratio.num;
    if (*output_width > video_width) {
	*output_width = video_width;
	*output_y += (video_height - *output_height) / 2;
    } else if (*output_height > video_height) {
	*output_height = video_height;
	*output_x += (video_width - *output_width) / 2;
    }
    Debug(3, "video: aspect output %dx%d%+d%+d\n", *output_width,
	*output_height, *output_x, *output_y);
    return;

  stretch:
    *output_x = video_x;
    *output_y = video_y;
    *output_width = video_width;
    *output_height = video_height;
    Debug(3, "video: stretch output %dx%d%+d%+d\n", *output_width,
	*output_height, *output_x, *output_y);
    return;

  center_cut_out:
    *output_x = video_x;
    *output_y = video_y;
    *output_height = video_height;
    *output_width = video_width;

    *crop_width =
	(video_height * display_aspect_ratio.num + display_aspect_ratio.den -
	1) / display_aspect_ratio.den;
    *crop_height =
	(video_width * display_aspect_ratio.den + display_aspect_ratio.num -
	1) / display_aspect_ratio.num;

    // look which side must be cut
    if (*crop_width > video_width) {
	*crop_height = input_height;

	// adjust scaling
	*crop_x = ((*crop_width - video_width) * input_width)
	    / (2 * video_width);
	*crop_width = input_width - *crop_x * 2;
	// FIXME: round failure?
    } else if (*crop_height > video_height) {
	*crop_width = input_width;

	// adjust scaling
	*crop_y = ((*crop_height - video_height) * input_height)
	    / (2 * video_height);
	*crop_height = input_height - *crop_y * 2;
	// FIXME: round failure?
    } else {
	*crop_width = input_width;
	*crop_height = input_height;
    }
    Debug(3, "video: aspect crop %dx%d%+d%+d\n", *crop_width, *crop_height,
	*crop_x, *crop_y);
}

//----------------------------------------------------------------------------
//	GLX
//----------------------------------------------------------------------------

#ifdef USE_GLX

static int GlxEnabled = 1;		///< use GLX
static int GlxVSyncEnabled = 0;		///< enable/disable v-sync
static GLXContext GlxSharedContext;	///< shared gl context
static GLXContext GlxContext;		///< our gl context
static XVisualInfo *GlxVisualInfo;	///< our gl visual

static GLuint OsdGlTextures[2];		///< gl texture for OSD
static int OsdIndex;			///< index into OsdGlTextures

///
///	GLX extension functions
///@{
#ifdef GLX_MESA_swap_control
static PFNGLXSWAPINTERVALMESAPROC GlxSwapIntervalMESA;
#endif
#ifdef GLX_SGI_video_sync
static PFNGLXGETVIDEOSYNCSGIPROC GlxGetVideoSyncSGI;
#endif
#ifdef GLX_SGI_swap_control
static PFNGLXSWAPINTERVALSGIPROC GlxSwapIntervalSGI;
#endif

///@}

///
///	GLX check error.
///
static void GlxCheck(void)
{
    GLenum err;

    if ((err = glGetError()) != GL_NO_ERROR) {
	Debug(3, "video/glx: error %d '%s'\n", err, gluErrorString(err));
    }
}

///
///	GLX check if a GLX extension is supported.
///
///	@param ext	extension to query
///	@returns true if supported, false otherwise
///
static int GlxIsExtensionSupported(const char *ext)
{
    const char *extensions;

    if ((extensions =
	    glXQueryExtensionsString(XlibDisplay,
		DefaultScreen(XlibDisplay)))) {
	const char *s;
	int l;

	s = strstr(extensions, ext);
	l = strlen(ext);
	return s && (s[l] == ' ' || s[l] == '\0');
    }
    return 0;
}

#if 0
///
///	Setup GLX decoder
///
///	@param decoder	VA-API decoder
///
void GlxSetupDecoder(VaapiDecoder * decoder)
{
    int width;
    int height;
    int i;

    width = decoder->InputWidth;
    height = decoder->InputHeight;

    glEnable(GL_TEXTURE_2D);		// create 2d texture
    glGenTextures(2, decoder->GlTexture);
    GlxCheck();
    for (i = 0; i < 2; ++i) {
	glBindTexture(GL_TEXTURE_2D, decoder->GlTexture[i]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA,
	    GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
    }
    glDisable(GL_TEXTURE_2D);

    GlxCheck();
}
#endif

///
///	Render texture.
///
///	@param texture	2d texture
///
static inline void GlxRenderTexture(GLuint texture, int x, int y, int width,
    int height)
{
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);	// no color
    glBegin(GL_QUADS); {
	glTexCoord2f(1.0f, 1.0f);
	glVertex2i(x + width, y + height);
	glTexCoord2f(0.0f, 1.0f);
	glVertex2i(x, y + height);
	glTexCoord2f(0.0f, 0.0f);
	glVertex2i(x, y);
	glTexCoord2f(1.0f, 0.0f);
	glVertex2i(x + width, y);
#if 0
	glTexCoord2f(0.0f, 0.0f);
	glVertex2i(x, y);
	glTexCoord2f(0.0f, 1.0f);
	glVertex2i(x, y + height);
	glTexCoord2f(1.0f, 1.0f);
	glVertex2i(x + width, y + height);
	glTexCoord2f(1.0f, 0.0f);
	glVertex2i(x + width, y);
#endif
    }
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

///
///	Upload texture.
///
static void GlxUploadTexture(int x, int y, int width, int height,
    const uint8_t * argb)
{
    // FIXME: use other / faster uploads
    // ARB_pixelbuffer_object GL_PIXEL_UNPACK_BUFFER glBindBufferARB()
    // glMapBuffer() glUnmapBuffer()
    // glTexSubImage2D

    glEnable(GL_TEXTURE_2D);		// upload 2d texture

    glBindTexture(GL_TEXTURE_2D, OsdGlTextures[OsdIndex]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_BGRA,
	GL_UNSIGNED_BYTE, argb);
    glBindTexture(GL_TEXTURE_2D, 0);

    glDisable(GL_TEXTURE_2D);
}

///
///	Render to glx texture.
///
static void GlxRender(int osd_width, int osd_height)
{
    static uint8_t *image;
    static uint8_t cycle;
    int x;
    int y;

    if (!OsdGlTextures[0] || !OsdGlTextures[1]) {
	return;
    }
    // render each frame kills performance

    // osd 1920 * 1080 * 4 (RGBA) * 50 (HZ) = 396 Mb/s

    // too big for alloca
    if (!image) {
	image = malloc(4 * osd_width * osd_height);
	memset(image, 0x00, 4 * osd_width * osd_height);
    }
    for (y = 0; y < osd_height; ++y) {
	for (x = 0; x < osd_width; ++x) {
	    ((uint32_t *) image)[x + y * osd_width] =
		0x00FFFFFF | (cycle++) << 24;
	}
    }
    cycle++;

    // FIXME: convert is for GLX texture unneeded
    // convert internal osd to image
    //GfxConvert(image, 0, 4 * osd_width);
    //

    GlxUploadTexture(0, 0, osd_width, osd_height, image);
}

///
///	Setup GLX window.
///
static void GlxSetupWindow(xcb_window_t window, int width, int height)
{
    uint32_t start;
    uint32_t end;
    int i;
    unsigned count;

    Debug(3, "video/glx: %s\n %x %dx%d", __FUNCTION__, window, width, height);

    // set glx context
    if (!glXMakeCurrent(XlibDisplay, window, GlxContext)) {
	Fatal(_("video/glx: can't make glx context current\n"));
	// FIXME: disable glx
	return;
    }

    Debug(3, "video/glx: ok\n");

#ifdef DEBUG
    // check if v-sync is working correct
    end = GetMsTicks();
    for (i = 0; i < 10; ++i) {
	start = end;

	glClear(GL_COLOR_BUFFER_BIT);
	glXSwapBuffers(XlibDisplay, window);
	end = GetMsTicks();

	GlxGetVideoSyncSGI(&count);
	Debug(3, "video/glx: %5d frame rate %dms\n", count, end - start);
	// nvidia can queue 5 swaps
	if (i > 5 && (end - start) < 15) {
	    Warning(_("video/glx: no v-sync\n"));
	}
    }
#endif

    // viewpoint
    GlxCheck();
    glViewport(0, 0, width, height);
    glDepthRange(-1.0, 1.0);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glColor3f(1.0f, 1.0f, 1.0f);
    glClearDepth(1.0);
    GlxCheck();

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
    GlxCheck();

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);		// setup 2d drawing
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
#ifdef USE_DOUBLEBUFFER
    glDrawBuffer(GL_BACK);
#else
    glDrawBuffer(GL_FRONT);
#endif
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

#ifdef DEBUG
#ifdef USE_DOUBLEBUFFER
    glDrawBuffer(GL_FRONT);
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawBuffer(GL_BACK);
#endif
#endif

    // clear
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);	// intial background color
    glClear(GL_COLOR_BUFFER_BIT);
#ifdef DEBUG
    glClearColor(1.0f, 1.0f, 0.0f, 1.0f);	// background color
#endif
    GlxCheck();
}

///
///	Initialize GLX.
///
static void GlxInit(void)
{
    static GLint visual_attr[] = {
	GLX_RGBA,
	GLX_RED_SIZE, 8,
	GLX_GREEN_SIZE, 8,
	GLX_BLUE_SIZE, 8,
#ifdef USE_DOUBLEBUFFER
	GLX_DOUBLEBUFFER,
#endif
	None
    };
    XVisualInfo *vi;
    GLXContext context;
    int major;
    int minor;
    int glx_GLX_EXT_swap_control;
    int glx_GLX_MESA_swap_control;
    int glx_GLX_SGI_swap_control;
    int glx_GLX_SGI_video_sync;

    if (!glXQueryVersion(XlibDisplay, &major, &minor)) {
	Error(_("video/glx: no GLX support\n"));
	GlxEnabled = 0;
	return;
    }
    Info(_("video/glx: glx version %d.%d\n"), major, minor);

    //
    //	check which extension are supported
    //
    glx_GLX_EXT_swap_control = GlxIsExtensionSupported("GLX_EXT_swap_control");
    glx_GLX_MESA_swap_control =
	GlxIsExtensionSupported("GLX_MESA_swap_control");
    glx_GLX_SGI_swap_control = GlxIsExtensionSupported("GLX_SGI_swap_control");
    glx_GLX_SGI_video_sync = GlxIsExtensionSupported("GLX_SGI_video_sync");

#ifdef GLX_MESA_swap_control
    if (glx_GLX_MESA_swap_control) {
	GlxSwapIntervalMESA = (PFNGLXSWAPINTERVALMESAPROC)
	    glXGetProcAddress((const GLubyte *)"glXSwapIntervalMESA");
    }
    Debug(3, "video/glx: GlxSwapIntervalMESA=%p\n", GlxSwapIntervalMESA);
#endif
#ifdef GLX_SGI_swap_control
    if (glx_GLX_SGI_swap_control) {
	GlxSwapIntervalSGI = (PFNGLXSWAPINTERVALSGIPROC)
	    glXGetProcAddress((const GLubyte *)"glXSwapIntervalSGI");
    }
    Debug(3, "video/glx: GlxSwapIntervalSGI=%p\n", GlxSwapIntervalSGI);
#endif
#ifdef GLX_SGI_video_sync
    if (glx_GLX_SGI_video_sync) {
	GlxGetVideoSyncSGI = (PFNGLXGETVIDEOSYNCSGIPROC)
	    glXGetProcAddress((const GLubyte *)"glXGetVideoSyncSGI");
    }
    Debug(3, "video/glx: GlxGetVideoSyncSGI=%p\n", GlxGetVideoSyncSGI);
#endif
    // glXGetVideoSyncSGI glXWaitVideoSyncSGI

#if 0
    // FIXME: use xcb: xcb_glx_create_context
#endif

    // create glx context
    glXMakeCurrent(XlibDisplay, None, NULL);
    vi = glXChooseVisual(XlibDisplay, DefaultScreen(XlibDisplay), visual_attr);
    if (!vi) {
	Error(_("video/glx: can't get a RGB visual\n"));
	GlxEnabled = 0;
	return;
    }
    if (!vi->visual) {
	Error(_("video/glx: no valid visual found\n"));
	GlxEnabled = 0;
	return;
    }
    if (vi->bits_per_rgb < 8) {
	Error(_("video/glx: need atleast 8-bits per RGB\n"));
	GlxEnabled = 0;
	return;
    }
    context = glXCreateContext(XlibDisplay, vi, NULL, GL_TRUE);
    if (!context) {
	Error(_("video/glx: can't create glx context\n"));
	GlxEnabled = 0;
	return;
    }
    GlxSharedContext = context;
    context = glXCreateContext(XlibDisplay, vi, GlxSharedContext, GL_TRUE);
    if (!context) {
	Error(_("video/glx: can't create glx context\n"));
	GlxEnabled = 0;
	// FIXME: destroy GlxSharedContext
	return;
    }
    GlxContext = context;

    GlxVisualInfo = vi;
    Debug(3, "video/glx: visual %#02x depth %u\n", (unsigned)vi->visualid,
	vi->depth);

    //
    //	query default v-sync state
    //
    if (glx_GLX_EXT_swap_control) {
	unsigned tmp;

	tmp = -1;
	glXQueryDrawable(XlibDisplay, DefaultRootWindow(XlibDisplay),
	    GLX_SWAP_INTERVAL_EXT, &tmp);
	GlxCheck();

	Debug(3, "video/glx: default v-sync is %d\n", tmp);
    } else {
	Debug(3, "video/glx: default v-sync is unknown\n");
    }

    //
    //	disable wait on v-sync
    //
    // FIXME: sleep before swap / busy waiting hardware
    // FIXME: 60hz lcd panel
    // FIXME: config: default, on, off
#ifdef GLX_SGI_swap_control
    if (GlxVSyncEnabled < 0 && GlxSwapIntervalSGI) {
	if (GlxSwapIntervalSGI(0)) {
	    GlxCheck();
	    Warning(_("video/glx: can't disable v-sync\n"));
	} else {
	    Info(_("video/glx: v-sync disabled\n"));
	}
    } else
#endif
#ifdef GLX_MESA_swap_control
    if (GlxVSyncEnabled < 0 && GlxSwapIntervalMESA) {
	if (GlxSwapIntervalMESA(0)) {
	    GlxCheck();
	    Warning(_("video/glx: can't disable v-sync\n"));
	} else {
	    Info(_("video/glx: v-sync disabled\n"));
	}
    }
#endif

    //
    //	enable wait on v-sync
    //
#ifdef GLX_SGI_swap_control
    if (GlxVSyncEnabled > 0 && GlxSwapIntervalMESA) {
	if (GlxSwapIntervalMESA(1)) {
	    GlxCheck();
	    Warning(_("video/glx: can't enable v-sync\n"));
	} else {
	    Info(_("video/glx: v-sync enabled\n"));
	}
    } else
#endif
#ifdef GLX_MESA_swap_control
    if (GlxVSyncEnabled > 0 && GlxSwapIntervalSGI) {
	if (GlxSwapIntervalSGI(1)) {
	    GlxCheck();
	    Warning(_("video/glx: can't enable v-sync\n"));
	} else {
	    Info(_("video/glx: v-sync enabled\n"));
	}
    }
#endif
}

///
///	Cleanup GLX.
///
static void GlxExit(void)
{
    Debug(3, "video/glx: %s\n", __FUNCTION__);

    glFinish();

    // must destroy glx
    if (glXGetCurrentContext() == GlxContext) {
	// if currently used, set to none
	glXMakeCurrent(XlibDisplay, None, NULL);
    }
    if (GlxSharedContext) {
	glXDestroyContext(XlibDisplay, GlxSharedContext);
    }
    if (GlxContext) {
	glXDestroyContext(XlibDisplay, GlxContext);
    }
#if 0
    if (GlxThreadContext) {
	glXDestroyContext(XlibDisplay, GlxThreadContext);
    }
    // FIXME: must free GlxVisualInfo
#endif
}

#endif

//----------------------------------------------------------------------------
//	common functions
//----------------------------------------------------------------------------

///
///	Calculate resolution group.
///
///	@param width		video picture raw width
///	@param height		video picture raw height
///	@param interlace	flag interlaced video picture
///
///	@note interlace isn't used yet and probably wrong set by caller.
///
static VideoResolutions VideoResolutionGroup(int width, int height,
    __attribute__ ((unused))
    int interlace)
{
    if (height <= 576) {
	return VideoResolution576i;
    }
    if (height <= 720) {
	return VideoResolution720p;
    }
    if (height < 1080) {
	return VideoResolutionFake1080i;
    }
    if (width < 1920) {
	return VideoResolutionFake1080i;
    }
    return VideoResolution1080i;
}

//----------------------------------------------------------------------------
//	auto-crop
//----------------------------------------------------------------------------

///
///	auto-crop context structure and typedef.
///
typedef struct _auto_crop_ctx_
{
    int X1;				///< detected left border
    int X2;				///< detected right border
    int Y1;				///< detected top border
    int Y2;				///< detected bottom border

    int Count;				///< counter to delay switch
    int State;				///< auto-crop state (0, 14, 16)

} AutoCropCtx;

#ifdef USE_AUTOCROP

#define YBLACK 0x20			///< below is black
#define UVBLACK 0x80			///< around is black
#define M64 UINT64_C(0x0101010101010101)	///< 64bit multiplicator

    /// auto-crop percent of video width to ignore logos
static const int AutoCropLogoIgnore = 24;
static int AutoCropInterval;		///< auto-crop check interval
static int AutoCropDelay;		///< auto-crop switch delay
static int AutoCropTolerance;		///< auto-crop tolerance

///
///	Detect black line Y.
///
///	@param data	Y plane pixel data
///	@param length	number of pixel to check
///	@param stride	offset of pixels
///
///	@note 8 pixel are checked at once, all values must be 8 aligned
///
static int AutoCropIsBlackLineY(const uint8_t * data, int length, int stride)
{
    int n;
    int o;
    uint64_t r;
    const uint64_t *p;

#ifdef DEBUG
    if ((size_t) data & 0x7 || stride & 0x7) {
	abort();
    }
#endif
    p = (const uint64_t *)data;
    n = length;				// FIXME: can remove n
    o = stride / 8;

    r = 0UL;
    while (--n >= 0) {
	r |= *p;
	p += o;
    }

    // below YBLACK(0x20) is black
    return !(r & ~((YBLACK - 1) * M64));
}

///
///	Auto detect black borders and crop them.
///
///	@param autocrop auto-crop variables
///	@param width	frame width in pixel
///	@param height	frame height in pixel
///	@param data	frame planes data (Y, U, V)
///	@param pitches	frame planes pitches (Y, U, V)
///
///	@note FIXME: can reduce the checked range, left, right crop isn't
///		used yet.
///
///	@note FIXME: only Y is checked, for black.
///
static void AutoCropDetect(AutoCropCtx * autocrop, int width, int height,
    void *data[3], uint32_t pitches[3])
{
    const void *data_y;
    unsigned length_y;
    int x;
    int y;
    int x1;
    int x2;
    int y1;
    int y2;
    int logo_skip;

    //
    //	ignore top+bottom 6 lines and left+right 8 pixels
    //
#define SKIP_X	8
#define SKIP_Y	6
    x1 = width - 1;
    x2 = 0;
    y1 = height - 1;
    y2 = 0;
    logo_skip = SKIP_X + (((width * AutoCropLogoIgnore) / 100 + 8) / 8) * 8;

    data_y = data[0];
    length_y = pitches[0];

    //
    //	search top
    //
    for (y = SKIP_Y; y < y1; ++y) {
	if (!AutoCropIsBlackLineY(data_y + logo_skip + y * length_y,
		(width - 2 * logo_skip) / 8, 8)) {
	    if (y == SKIP_Y) {
		y = 0;
	    }
	    y1 = y;
	    break;
	}
    }
    //
    //	search bottom
    //
    for (y = height - SKIP_Y - 1; y > y2; --y) {
	if (!AutoCropIsBlackLineY(data_y + logo_skip + y * length_y,
		(width - 2 * logo_skip) / 8, 8)) {
	    if (y == height - SKIP_Y - 1) {
		y = height - 1;
	    }
	    y2 = y;
	    break;
	}
    }
    //
    //	search left
    //
    for (x = SKIP_X; x < x1; x += 8) {
	if (!AutoCropIsBlackLineY(data_y + x + SKIP_Y * length_y,
		height - 2 * SKIP_Y, length_y)) {
	    if (x == SKIP_X) {
		x = 0;
	    }
	    x1 = x;
	    break;
	}
    }
    //
    //	search right
    //
    for (x = width - SKIP_X - 8; x > x2; x -= 8) {
	if (!AutoCropIsBlackLineY(data_y + x + SKIP_Y * length_y,
		height - 2 * SKIP_Y * 8, length_y)) {
	    if (x == width - SKIP_X - 8) {
		x = width - 1;
	    }
	    x2 = x;
	    break;
	}
    }

    if (0 && (y1 > SKIP_Y || x1 > SKIP_X)) {
	Debug(3, "video/autocrop: top=%d bottom=%d left=%d right=%d\n", y1, y2,
	    x1, x2);
    }

    autocrop->X1 = x1;
    autocrop->X2 = x2;
    autocrop->Y1 = y1;
    autocrop->Y2 = y2;
}

#endif

//----------------------------------------------------------------------------
//	software - deinterlace
//----------------------------------------------------------------------------

// FIXME: move general software deinterlace functions to here.

//----------------------------------------------------------------------------
//	VA-API
//----------------------------------------------------------------------------

#ifdef USE_VAAPI

static char VaapiBuggyXvBA;		///< fix xvba-video bugs
static char VaapiBuggyVdpau;		///< fix libva-driver-vdpau bugs
static char VaapiBuggyIntel;		///< fix libva-driver-intel bugs
static char VaapiNewIntel;		///< new libva-driver-intel driver

static VADisplay *VaDisplay;		///< VA-API display

static VAImage VaOsdImage = {
    .image_id = VA_INVALID_ID
};					///< osd VA-API image

static VASubpictureID VaOsdSubpicture = VA_INVALID_ID;	///< osd VA-API subpicture
static char VaapiUnscaledOsd;		///< unscaled osd supported

    /// VA-API decoder typedef
typedef struct _vaapi_decoder_ VaapiDecoder;

///
///	VA-API decoder
///
struct _vaapi_decoder_
{
    VADisplay *VaDisplay;		///< VA-API display

    xcb_window_t Window;		///< output window

    int VideoX;				///< video base x coordinate
    int VideoY;				///< video base y coordinate
    int VideoWidth;			///< video base width
    int VideoHeight;			///< video base height

    int OutputX;			///< real video output x coordinate
    int OutputY;			///< real video output y coordinate
    int OutputWidth;			///< real video output width
    int OutputHeight;			///< real video output height

    /// flags for put surface for different resolutions groups
    unsigned SurfaceFlagsTable[VideoResolutionMax];

    enum PixelFormat PixFmt;		///< ffmpeg frame pixfmt
    int WrongInterlacedWarned;		///< warning about interlace flag issued
    int Interlaced;			///< ffmpeg interlaced flag
    int TopFieldFirst;			///< ffmpeg top field displayed first

    VAImage DeintImages[5];		///< deinterlace image buffers

    int GetPutImage;			///< flag get/put image can be used
    VAImage Image[1];			///< image buffer to update surface

    VAProfile Profile;			///< VA-API profile
    VAEntrypoint Entrypoint;		///< VA-API entrypoint
    struct vaapi_context VaapiContext[1];	///< ffmpeg VA-API context

    int SurfacesNeeded;			///< number of surface to request
    int SurfaceUsedN;			///< number of used surfaces
    /// used surface ids
    VASurfaceID SurfacesUsed[CODEC_SURFACES_MAX];
    int SurfaceFreeN;			///< number of free surfaces
    /// free surface ids
    VASurfaceID SurfacesFree[CODEC_SURFACES_MAX];

    int InputWidth;			///< video input width
    int InputHeight;			///< video input height
    AVRational InputAspect;		///< video input aspect ratio
    VideoResolutions Resolution;	///< resolution group

    int CropX;				///< video crop x
    int CropY;				///< video crop y
    int CropWidth;			///< video crop width
    int CropHeight;			///< video crop height
#ifdef USE_AUTOCROP
    AutoCropCtx AutoCrop[1];		///< auto-crop variables
#endif
#ifdef USE_GLX
    GLuint GlTexture[2];		///< gl texture for VA-API
    void *GlxSurface[2];		///< VA-API/GLX surface
#endif
    VASurfaceID BlackSurface;		///< empty black surface

    /// video surface ring buffer
    VASurfaceID SurfacesRb[VIDEO_SURFACES_MAX];
#ifdef VA_EXP
    VASurfaceID LastSurface;		///< last surface
#endif
    int SurfaceWrite;			///< write pointer
    int SurfaceRead;			///< read pointer
    atomic_t SurfacesFilled;		///< how many of the buffer is used

    int SurfaceField;			///< current displayed field
    int TrickSpeed;			///< current trick speed
    int TrickCounter;			///< current trick speed counter
    struct timespec FrameTime;		///< time of last display
    VideoStream *Stream;		///< video stream
    int Closing;			///< flag about closing current stream
    int SyncOnAudio;			///< flag sync to audio
    int64_t PTS;			///< video PTS clock

    int SyncCounter;			///< counter to sync frames
    int StartCounter;			///< counter for video start
    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
};

static VaapiDecoder *VaapiDecoders[1];	///< open decoder streams
static int VaapiDecoderN;		///< number of decoder streams

    /// forward display back surface
static void VaapiBlackSurface(VaapiDecoder *);

    /// forward destroy deinterlace images
static void VaapiDestroyDeinterlaceImages(VaapiDecoder *);

    /// forward definition release surface
static void VaapiReleaseSurface(VaapiDecoder *, VASurfaceID);

//----------------------------------------------------------------------------
//	VA-API Functions
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

///
///	Output video messages.
///
///	Reduce output.
///
///	@param level	message level (Error, Warning, Info, Debug, ...)
///	@param format	printf format string (NULL to flush messages)
///	@param ...	printf arguments
///
///	@returns true, if message shown
///
///	@todo FIXME: combine VdpauMessage and VaapiMessage
///
static int VaapiMessage(int level, const char *format, ...)
{
    if (SysLogLevel > level || DebugLevel > level) {
	static const char *last_format;
	static char buf[256];
	va_list ap;

	va_start(ap, format);
	if (format != last_format) {	// don't repeat same message
	    if (buf[0]) {		// print last repeated message
		syslog(LOG_ERR, "%s", buf);
		buf[0] = '\0';
	    }

	    if (format) {
		last_format = format;
		vsyslog(LOG_ERR, format, ap);
	    }
	    va_end(ap);
	    return 1;
	}
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
    }
    return 0;
}

//	Surfaces -------------------------------------------------------------

///
///	Associate OSD with surface.
///
///	@param decoder	VA-API decoder
///
static void VaapiAssociate(VaapiDecoder * decoder)
{
    int x;
    int y;
    int w;
    int h;

    if (VaOsdSubpicture == VA_INVALID_ID) {
	Warning(_("video/vaapi: no osd subpicture yet\n"));
	return;
    }

    x = 0;
    y = 0;
    w = VaOsdImage.width;
    h = VaOsdImage.height;

    // FIXME: associate only if osd is displayed
    if (VaapiUnscaledOsd) {
	if (decoder->SurfaceFreeN
	    && vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN, x, y, w, h, 0, 0,
		VideoWindowWidth, VideoWindowHeight,
		VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
	if (decoder->SurfaceUsedN
	    && vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesUsed, decoder->SurfaceUsedN, x, y, w, h, 0, 0,
		VideoWindowWidth, VideoWindowHeight,
		VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
    } else {
	if (decoder->SurfaceFreeN
	    && vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN, x, y, w, h,
		decoder->CropX, decoder->CropY / 2, decoder->CropWidth,
		decoder->CropHeight, 0)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
	if (decoder->SurfaceUsedN
	    && vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesUsed, decoder->SurfaceUsedN, x, y, w, h,
		decoder->CropX, decoder->CropY / 2, decoder->CropWidth,
		decoder->CropHeight, 0)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
    }
}

///
///	Deassociate OSD with surface.
///
///	@param decoder	VA-API decoder
///
static void VaapiDeassociate(VaapiDecoder * decoder)
{
    if (VaOsdSubpicture != VA_INVALID_ID) {
	if (decoder->SurfaceFreeN
	    && vaDeassociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't deassociate %d surfaces\n"),
		decoder->SurfaceFreeN);
	}

	if (decoder->SurfaceUsedN
	    && vaDeassociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesUsed, decoder->SurfaceUsedN)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't deassociate %d surfaces\n"),
		decoder->SurfaceUsedN);
	}
    }
}

///
///	Create surfaces for VA-API decoder.
///
///	@param decoder	VA-API decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void VaapiCreateSurfaces(VaapiDecoder * decoder, int width, int height)
{
#ifdef DEBUG
    if (!decoder->SurfacesNeeded) {
	Error(_("video/vaapi: surface needed not set\n"));
	decoder->SurfacesNeeded = 3 + VIDEO_SURFACES_MAX;
    }
#endif
    Debug(3, "video/vaapi: %s: %dx%d * %d\n", __FUNCTION__, width, height,
	decoder->SurfacesNeeded);

    decoder->SurfaceFreeN = decoder->SurfacesNeeded;
    // VA_RT_FORMAT_YUV420 VA_RT_FORMAT_YUV422 VA_RT_FORMAT_YUV444
    if (vaCreateSurfaces(decoder->VaDisplay, VA_RT_FORMAT_YUV420, width,
	    height, decoder->SurfacesFree, decoder->SurfaceFreeN, NULL,
	    0) != VA_STATUS_SUCCESS) {
	Fatal(_("video/vaapi: can't create %d surfaces\n"),
	    decoder->SurfaceFreeN);
	// FIXME: write error handler / fallback
    }
}

///
///	Destroy surfaces of VA-API decoder.
///
///	@param decoder	VA-API decoder
///
static void VaapiDestroySurfaces(VaapiDecoder * decoder)
{
    Debug(3, "video/vaapi: %s:\n", __FUNCTION__);

    //
    //	update OSD associate
    //
    VaapiDeassociate(decoder);

    if (vaDestroySurfaces(decoder->VaDisplay, decoder->SurfacesFree,
	    decoder->SurfaceFreeN)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy %d surfaces\n"),
	    decoder->SurfaceFreeN);
    }
    decoder->SurfaceFreeN = 0;
    if (vaDestroySurfaces(decoder->VaDisplay, decoder->SurfacesUsed,
	    decoder->SurfaceUsedN)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy %d surfaces\n"),
	    decoder->SurfaceUsedN);
    }
    decoder->SurfaceUsedN = 0;

    // FIXME surfaces used for output
}

///
///	Get a free surface.
///
///	@param decoder	VA-API decoder
///
///	@returns the oldest free surface
///
static VASurfaceID VaapiGetSurface0(VaapiDecoder * decoder)
{
    VASurfaceID surface;
    VASurfaceStatus status;
    int i;

    // try to use oldest surface
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	surface = decoder->SurfacesFree[i];
	if (vaQuerySurfaceStatus(decoder->VaDisplay, surface, &status)
	    != VA_STATUS_SUCCESS) {
	    // this fails with XvBA und mpeg softdecoder
	    if (!VaapiBuggyXvBA) {
		Error(_("video/vaapi: vaQuerySurface failed\n"));
	    }
	    status = VASurfaceReady;
	}
	// surface still in use, try next
	if (status != VASurfaceReady) {
	    Debug(4, "video/vaapi: surface %#010x not ready: %d\n", surface,
		status);
	    if (!VaapiBuggyVdpau || i < 1) {
		continue;
	    }
	    usleep(1 * 1000);
	}
	// copy remaining surfaces down
	decoder->SurfaceFreeN--;
	for (; i < decoder->SurfaceFreeN; ++i) {
	    decoder->SurfacesFree[i] = decoder->SurfacesFree[i + 1];
	}
	decoder->SurfacesFree[i] = VA_INVALID_ID;

	// save as used
	decoder->SurfacesUsed[decoder->SurfaceUsedN++] = surface;

	return surface;
    }

    Error(_("video/vaapi: out of surfaces\n"));
    return VA_INVALID_ID;
}

///
///	Release a surface.
///
///	@param decoder	VA-API decoder
///	@param surface	surface no longer used
///
static void VaapiReleaseSurface(VaapiDecoder * decoder, VASurfaceID surface)
{
    int i;

    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	if (decoder->SurfacesUsed[i] == surface) {
	    // no problem, with last used
	    decoder->SurfacesUsed[i] =
		decoder->SurfacesUsed[--decoder->SurfaceUsedN];
	    decoder->SurfacesFree[decoder->SurfaceFreeN++] = surface;
	    return;
	}
    }
    Error(_("video/vaapi: release surface %#010x, which is not in use\n"),
	surface);
}

//	Init/Exit ------------------------------------------------------------

///
///	Debug VA-API decoder frames drop...
///
///	@param decoder	video hardware decoder
///
static void VaapiPrintFrames(const VaapiDecoder * decoder)
{
    Debug(3, "video/vaapi: %d missed, %d duped, %d dropped frames of %d,%d\n",
	decoder->FramesMissed, decoder->FramesDuped, decoder->FramesDropped,
	decoder->FrameCounter, decoder->FramesDisplayed);
#ifndef DEBUG
    (void)decoder;
#endif
}

///
///	Initialize surface flags.
///
///	@param decoder	video hardware decoder
///
static void VaapiInitSurfaceFlags(VaapiDecoder * decoder)
{
    int i;

    for (i = 0; i < VideoResolutionMax; ++i) {
	decoder->SurfaceFlagsTable[i] = VA_CLEAR_DRAWABLE;
	// color space conversion none, ITU-R BT.601, ITU-R BT.709, ...
	switch (VideoColorSpaces[i]) {
	    case VideoColorSpaceNone:
		break;
	    case VideoColorSpaceBt601:
		decoder->SurfaceFlagsTable[i] |= VA_SRC_BT601;
		break;
	    case VideoColorSpaceBt709:
		decoder->SurfaceFlagsTable[i] |= VA_SRC_BT709;
		break;
	    case VideoColorSpaceSmpte240:
		decoder->SurfaceFlagsTable[i] |= VA_SRC_SMPTE_240;
		break;
	}

	// scaling flags FAST, HQ, NL_ANAMORPHIC
	switch (VideoScaling[i]) {
	    case VideoScalingNormal:
		decoder->SurfaceFlagsTable[i] |= VA_FILTER_SCALING_DEFAULT;
		break;
	    case VideoScalingFast:
		decoder->SurfaceFlagsTable[i] |= VA_FILTER_SCALING_FAST;
		break;
	    case VideoScalingHQ:
		// vdpau backend supports only VA_FILTER_SCALING_HQ
		// vdpau backend with advanced deinterlacer and my GT-210
		// is too slow
		decoder->SurfaceFlagsTable[i] |= VA_FILTER_SCALING_HQ;
		break;
	    case VideoScalingAnamorphic:
		// intel backend supports only VA_FILTER_SCALING_NL_ANAMORPHIC;
		// FIXME: Highlevel should display 4:3 as 16:9 to support this
		decoder->SurfaceFlagsTable[i] |=
		    VA_FILTER_SCALING_NL_ANAMORPHIC;
		break;
	}

	// deinterlace flags (not yet supported by libva)
	switch (VideoDeinterlace[i]) {
	    case VideoDeinterlaceBob:
		break;
	    case VideoDeinterlaceWeave:
		break;
	    case VideoDeinterlaceTemporal:
		//FIXME: private hack
		//decoder->SurfaceFlagsTable[i] |= 0x00002000;
		break;
	    case VideoDeinterlaceTemporalSpatial:
		//FIXME: private hack
		//decoder->SurfaceFlagsTable[i] |= 0x00006000;
		break;
	    default:
		break;
	}
    }
}

///
///	Allocate new VA-API decoder.
///
///	@returns a new prepared VA-API hardware decoder.
///
static VaapiDecoder *VaapiNewHwDecoder(VideoStream * stream)
{
    VaapiDecoder *decoder;
    int i;

    if (VaapiDecoderN == 1) {
	Fatal(_("video/vaapi: out of decoders\n"));
    }

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
	Fatal(_("video/vaapi: out of memory\n"));
    }
    decoder->VaDisplay = VaDisplay;
    decoder->Window = VideoWindow;
    decoder->VideoX = 0;
    decoder->VideoY = 0;
    decoder->VideoWidth = VideoWindowWidth;
    decoder->VideoHeight = VideoWindowHeight;

    VaapiInitSurfaceFlags(decoder);

    decoder->DeintImages[0].image_id = VA_INVALID_ID;
    decoder->DeintImages[1].image_id = VA_INVALID_ID;
    decoder->DeintImages[2].image_id = VA_INVALID_ID;
    decoder->DeintImages[3].image_id = VA_INVALID_ID;
    decoder->DeintImages[4].image_id = VA_INVALID_ID;

    decoder->Image->image_id = VA_INVALID_ID;

    for (i = 0; i < CODEC_SURFACES_MAX; ++i) {
	decoder->SurfacesUsed[i] = VA_INVALID_ID;
	decoder->SurfacesFree[i] = VA_INVALID_ID;
    }

    // setup video surface ring buffer
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VA_INVALID_ID;
    }
#ifdef VA_EXP
    decoder->LastSurface = VA_INVALID_ID;
#endif

    decoder->BlackSurface = VA_INVALID_ID;

    //
    //	Setup ffmpeg vaapi context
    //
    decoder->Profile = VA_INVALID_ID;
    decoder->Entrypoint = VA_INVALID_ID;
    decoder->VaapiContext->display = VaDisplay;
    decoder->VaapiContext->config_id = VA_INVALID_ID;
    decoder->VaapiContext->context_id = VA_INVALID_ID;

#ifdef USE_GLX
    decoder->GlxSurface[0] = VA_INVALID_ID;
    decoder->GlxSurface[1] = VA_INVALID_ID;
    if (GlxEnabled) {
	// FIXME: create GLX context here
    }
#endif

    decoder->OutputWidth = VideoWindowWidth;
    decoder->OutputHeight = VideoWindowHeight;

    decoder->Stream = stream;
    decoder->Closing = -300 - 1;

    decoder->PTS = AV_NOPTS_VALUE;

    // get/put still not working
    //decoder->GetPutImage = !VaapiBuggyIntel || VaapiNewIntel;
    decoder->GetPutImage = !VaapiBuggyIntel;

    VaapiDecoders[VaapiDecoderN++] = decoder;

    return decoder;
}

///
///	Cleanup VA-API.
///
///	@param decoder	va-api hw decoder
///
static void VaapiCleanup(VaapiDecoder * decoder)
{
    int filled;
    VASurfaceID surface;
    int i;

    // flush output queue, only 1-2 frames buffered, no big loss
    while ((filled = atomic_read(&decoder->SurfacesFilled))) {
	decoder->SurfaceRead = (decoder->SurfaceRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&decoder->SurfacesFilled);

	surface = decoder->SurfacesRb[decoder->SurfaceRead];
	if (surface == VA_INVALID_ID) {
	    Error(_("video/vaapi: invalid surface in ringbuffer\n"));
	    continue;
	}
	// can crash and hang
	if (0 && vaSyncSurface(decoder->VaDisplay, surface)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
    }

#ifdef DEBUG
    if (decoder->SurfaceRead != decoder->SurfaceWrite) {
	abort();
    }
#endif

    // clear ring buffer
    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VA_INVALID_ID;
    }
#ifdef VA_EXP
    decoder->LastSurface = VA_INVALID_ID;
#endif

    decoder->WrongInterlacedWarned = 0;

    //	cleanup image
    if (decoder->Image->image_id != VA_INVALID_ID) {
	if (vaDestroyImage(VaDisplay,
		decoder->Image->image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	decoder->Image->image_id = VA_INVALID_ID;
    }
    //	cleanup context and config
    if (decoder->VaapiContext) {

	if (decoder->VaapiContext->context_id != VA_INVALID_ID) {
	    if (vaDestroyContext(VaDisplay,
		    decoder->VaapiContext->context_id) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't destroy context!\n"));
	    }
	    decoder->VaapiContext->context_id = VA_INVALID_ID;
	}

	if (decoder->VaapiContext->config_id != VA_INVALID_ID) {
	    if (vaDestroyConfig(VaDisplay,
		    decoder->VaapiContext->config_id) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't destroy config!\n"));
	    }
	    decoder->VaapiContext->config_id = VA_INVALID_ID;
	}
    }
    //	cleanup surfaces
    if (decoder->SurfaceFreeN || decoder->SurfaceUsedN) {
	VaapiDestroySurfaces(decoder);
    }
    // cleanup images
    if (decoder->DeintImages[0].image_id != VA_INVALID_ID) {
	VaapiDestroyDeinterlaceImages(decoder);
    }
    decoder->SurfaceRead = 0;
    decoder->SurfaceWrite = 0;
    decoder->SurfaceField = 0;

    decoder->SyncCounter = 0;
    decoder->FrameCounter = 0;
    decoder->FramesDisplayed = 0;
    decoder->StartCounter = 0;
    decoder->Closing = 0;
    decoder->PTS = AV_NOPTS_VALUE;
    VideoDeltaPTS = 0;
}

///
///	Destroy a VA-API decoder.
///
///	@param decoder	VA-API decoder
///
static void VaapiDelHwDecoder(VaapiDecoder * decoder)
{
    int i;

    for (i = 0; i < VaapiDecoderN; ++i) {
	if (VaapiDecoders[i] == decoder) {
	    VaapiDecoders[i] = NULL;
	    VaapiDecoderN--;
	    // FIXME: must copy last slot into empty slot and --
	    break;
	}
    }

    VaapiCleanup(decoder);

    if (decoder->BlackSurface != VA_INVALID_ID) {
	//
	//	update OSD associate
	//
	if (VaOsdSubpicture != VA_INVALID_ID) {
	    if (vaDeassociateSubpicture(VaDisplay, VaOsdSubpicture,
		    &decoder->BlackSurface, 1) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't deassociate black surfaces\n"));
	    }
	}
	if (vaDestroySurfaces(decoder->VaDisplay, &decoder->BlackSurface, 1)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy a surface\n"));
	}
    }
#ifdef USE_GLX
    if (decoder->GlxSurface[0] != VA_INVALID_ID) {
	if (vaDestroySurfaceGLX(VaDisplay, decoder->GlxSurface[0])
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy glx surface!\n"));
	}
    }
    if (decoder->GlxSurface[1] != VA_INVALID_ID) {
	if (vaDestroySurfaceGLX(VaDisplay, decoder->GlxSurface[1])
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy glx surface!\n"));
	}
    }
    if (decoder->GlTexture[0]) {
	glDeleteTextures(2, decoder->GlTexture);
    }
#endif

    VaapiPrintFrames(decoder);

    free(decoder);
}

#ifdef DEBUG				// currently unused, keep it for later

static VAProfile VaapiFindProfile(const VAProfile * profiles, unsigned n,
    VAProfile profile);
static VAEntrypoint VaapiFindEntrypoint(const VAEntrypoint * entrypoints,
    unsigned n, VAEntrypoint entrypoint);

///
///	1080i
///
static void Vaapi1080i(void)
{
    VAProfile profiles[vaMaxNumProfiles(VaDisplay)];
    int profile_n;
    VAEntrypoint entrypoints[vaMaxNumEntrypoints(VaDisplay)];
    int entrypoint_n;
    int p;
    int e;
    VAConfigAttrib attrib;
    VAConfigID config_id;
    VAContextID context_id;
    VASurfaceID surfaces[32];
    VAImage image[1];
    int n;
    uint32_t start_tick;
    uint32_t tick;

    p = -1;
    e = -1;

    //	prepare va-api profiles
    if (vaQueryConfigProfiles(VaDisplay, profiles, &profile_n)) {
	Error(_("codec: vaQueryConfigProfiles failed"));
	return;
    }
    // check profile
    p = VaapiFindProfile(profiles, profile_n, VAProfileH264High);
    if (p == -1) {
	Debug(3, "\tno profile found\n");
	return;
    }
    // prepare va-api entry points
    if (vaQueryConfigEntrypoints(VaDisplay, p, entrypoints, &entrypoint_n)) {
	Error(_("codec: vaQueryConfigEntrypoints failed"));
	return;
    }
    e = VaapiFindEntrypoint(entrypoints, entrypoint_n, VAEntrypointVLD);
    if (e == -1) {
	Warning(_("codec: unsupported: slow path\n"));
	return;
    }
    memset(&attrib, 0, sizeof(attrib));
    attrib.type = VAConfigAttribRTFormat;
    attrib.value = VA_RT_FORMAT_YUV420;
    // create a configuration for the decode pipeline
    if (vaCreateConfig(VaDisplay, p, e, &attrib, 1, &config_id)) {
	Error(_("codec: can't create config"));
	return;
    }
    if (vaCreateSurfaces(VaDisplay, VA_RT_FORMAT_YUV420, 1920, 1080, surfaces,
	    32, NULL, 0) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create surfaces\n"));
	return;
    }
    // bind surfaces to context
    if (vaCreateContext(VaDisplay, config_id, 1920, 1080, VA_PROGRESSIVE,
	    surfaces, 32, &context_id)) {
	Error(_("codec: can't create context"));
	return;
    }
#if 1
    // without this 1080i will crash
    image->image_id = VA_INVALID_ID;
    if (vaDeriveImage(VaDisplay, surfaces[0], image)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed\n"));
    }
    if (image->image_id != VA_INVALID_ID) {
	if (vaDestroyImage(VaDisplay, image->image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
    }
#else
    vaBeginPicture(VaDisplay, context_id, surfaces[0]);
    vaRenderPicture(VaDisplay, context_id, NULL, 0);
    // aborts without valid buffers upload
    vaEndPicture(VaDisplay, context_id);
#endif

    start_tick = GetMsTicks();
    for (n = 1; n < 2; ++n) {
	if (vaPutSurface(VaDisplay, surfaces[0], VideoWindow,
		// decoder src
		0, 0, 1920, 1080,
		// video dst
		0, 0, 1920, 1080, NULL, 0, VA_TOP_FIELD | VA_CLEAR_DRAWABLE)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaPutSurface failed\n"));
	}
	if (vaPutSurface(VaDisplay, surfaces[0], VideoWindow,
		// decoder src
		0, 0, 1920, 1080,
		// video dst
		0, 0, 1920, 1080, NULL, 0, VA_BOTTOM_FIELD | VA_CLEAR_DRAWABLE)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaPutSurface failed\n"));
	}
	tick = GetMsTicks();
	if (!(n % 10)) {
	    fprintf(stderr, "%dms / frame\n", (tick - start_tick) / n);
	}
    }

    // destory the stuff.
    if (vaDestroyContext(VaDisplay, context_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy context!\n"));
    }
    if (vaDestroySurfaces(VaDisplay, surfaces, 32) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy surfaces\n"));
    }
    if (vaDestroyConfig(VaDisplay, config_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy config!\n"));
    }
    fprintf(stderr, "done\n");
}

#endif

///
///	VA-API setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns true if VA-API could be initialized, false otherwise.
///
static int VaapiInit(const char *display_name)
{
    int major;
    int minor;
    VADisplayAttribute attr;
    const char *s;

    VaOsdImage.image_id = VA_INVALID_ID;
    VaOsdSubpicture = VA_INVALID_ID;

#ifdef USE_GLX
    if (GlxEnabled) {			// support glx
	VaDisplay = vaGetDisplayGLX(XlibDisplay);
    } else
#endif
    {
	VaDisplay = vaGetDisplay(XlibDisplay);
    }
    if (!VaDisplay) {
	Error(_("video/vaapi: Can't connect VA-API to X11 server on '%s'\n"),
	    display_name);
	return 0;
    }
    // XvBA needs this:
    setenv("DISPLAY", display_name, 1);

    if (vaInitialize(VaDisplay, &major, &minor) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: Can't inititialize VA-API on '%s'\n"),
	    display_name);
	vaTerminate(VaDisplay);
	VaDisplay = NULL;
	return 0;
    }
    s = vaQueryVendorString(VaDisplay);
    Info(_("video/vaapi: libva %d.%d (%s) initialized\n"), major, minor, s);

    //
    //	Setup fixes for driver bugs.
    //
    if (strstr(s, "VDPAU")) {
	Info(_("video/vaapi: use vdpau bug workaround\n"));
	setenv("VDPAU_VIDEO_PUTSURFACE_FAST", "0", 0);
	VaapiBuggyVdpau = 1;
    }
    if (strstr(s, "XvBA")) {
	VaapiBuggyXvBA = 1;
    }
    if (strstr(s, "Intel i965")) {
	VaapiBuggyIntel = 1;
    }
    if (strstr(s, "Intel i965 driver - 1.0.16.")) {
	VaapiNewIntel = 1;
    }
    //
    //	check which attributes are supported
    //
    attr.type = VADisplayAttribBackgroundColor;
    attr.flags = VA_DISPLAY_ATTRIB_SETTABLE;
    if (vaGetDisplayAttributes(VaDisplay, &attr, 1) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: Can't get background-color attribute\n"));
	attr.value = 1;
    }
    Info(_("video/vaapi: background-color is %s\n"),
	attr.value ? _("supported") : _("unsupported"));

    // FIXME: VaapiSetBackground(VideoBackground);

#if 0
    //
    //	check the chroma format
    //
    attr.type = VAConfigAttribRTFormat attr.flags = VA_DISPLAY_ATTRIB_GETTABLE;
    Vaapi1080i();
#endif

    return 1;
}

///
///	VA-API cleanup
///
static void VaapiExit(void)
{
    int i;

    // FIXME: more VA-API cleanups...
    // FIXME: can hang with vdpau in pthread_rwlock_wrlock

    for (i = 0; i < VaapiDecoderN; ++i) {
	if (VaapiDecoders[i]) {
	    VaapiDelHwDecoder(VaapiDecoders[i]);
	    VaapiDecoders[i] = NULL;
	}
    }
    VaapiDecoderN = 0;

    if (!VaDisplay) {
	vaTerminate(VaDisplay);
	VaDisplay = NULL;
    }
}

//----------------------------------------------------------------------------

///
///	Update output for new size or aspect ratio.
///
///	@param decoder	VA-API decoder
///
static void VaapiUpdateOutput(VaapiDecoder * decoder)
{
    VideoUpdateOutput(decoder->InputAspect, decoder->InputWidth,
	decoder->InputHeight, decoder->Resolution, decoder->VideoX,
	decoder->VideoY, decoder->VideoWidth, decoder->VideoHeight,
	&decoder->OutputX, &decoder->OutputY, &decoder->OutputWidth,
	&decoder->OutputHeight, &decoder->CropX, &decoder->CropY,
	&decoder->CropWidth, &decoder->CropHeight);
#ifdef USE_AUTOCROP
    decoder->AutoCrop->State = 0;
    decoder->AutoCrop->Count = AutoCropDelay;
#endif
}

///
///	Find VA-API image format.
///
///	@param decoder	VA-API decoder
///	@param pix_fmt		ffmpeg pixel format
///	@param[out] format	image format
///
///	FIXME: can fallback from I420 to YV12, if not supported
///	FIXME: must check if put/get with this format is supported (see intel)
///
static int VaapiFindImageFormat(VaapiDecoder * decoder,
    enum PixelFormat pix_fmt, VAImageFormat * format)
{
    VAImageFormat *imgfrmts;
    int imgfrmt_n;
    int i;
    unsigned fourcc;

    switch (pix_fmt) {			// convert ffmpeg to VA-API
	    // NV12, YV12, I420, BGRA
	    // intel: I420 is native format for MPEG-2 decoded surfaces
	    // intel: NV12 is native format for H.264 decoded surfaces
	case PIX_FMT_YUV420P:
	    // fourcc = VA_FOURCC_YV12; // YVU
	    fourcc = VA_FOURCC('I', '4', '2', '0');	// YUV
	    break;
	case PIX_FMT_NV12:
	    fourcc = VA_FOURCC_NV12;
	    break;
	default:
	    Fatal(_("video/vaapi: unsupported pixel format %d\n"), pix_fmt);
    }

    imgfrmt_n = vaMaxNumImageFormats(decoder->VaDisplay);
    imgfrmts = alloca(imgfrmt_n * sizeof(*imgfrmts));

    if (vaQueryImageFormats(decoder->VaDisplay, imgfrmts, &imgfrmt_n)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaQueryImageFormats failed\n"));
	return 0;
    }
    Debug(3, "video/vaapi: search format %c%c%c%c in %d image formats\n",
	fourcc, fourcc >> 8, fourcc >> 16, fourcc >> 24, imgfrmt_n);
    Debug(3, "video/vaapi: supported image formats:\n");
    for (i = 0; i < imgfrmt_n; ++i) {
	Debug(3, "video/vaapi:\t%c%c%c%c\t%d\n", imgfrmts[i].fourcc,
	    imgfrmts[i].fourcc >> 8, imgfrmts[i].fourcc >> 16,
	    imgfrmts[i].fourcc >> 24, imgfrmts[i].depth);
    }
    //
    //	search image format
    //
    for (i = 0; i < imgfrmt_n; ++i) {
	if (imgfrmts[i].fourcc == fourcc) {
	    *format = imgfrmts[i];
	    Debug(3, "video/vaapi: use\t%c%c%c%c\t%d\n", imgfrmts[i].fourcc,
		imgfrmts[i].fourcc >> 8, imgfrmts[i].fourcc >> 16,
		imgfrmts[i].fourcc >> 24, imgfrmts[i].depth);
	    return 1;
	}
    }

    Fatal("video/vaapi: pixel format %d unsupported by VA-API\n", pix_fmt);
    // FIXME: no fatal error!

    return 0;
}

///
///	Configure VA-API for new video format.
///
///	@param decoder	VA-API decoder
///
static void VaapiSetup(VaapiDecoder * decoder,
    const AVCodecContext * video_ctx)
{
    int width;
    int height;
    VAImageFormat format[1];

    // create initial black surface and display
    VaapiBlackSurface(decoder);
    // cleanup last context
    VaapiCleanup(decoder);

    width = video_ctx->width;
    height = video_ctx->height;
#ifdef DEBUG
    // FIXME: remove this if
    if (decoder->Image->image_id != VA_INVALID_ID) {
	abort();			// should be done by VaapiCleanup()
    }
#endif
    // FIXME: PixFmt not set!
    //VaapiFindImageFormat(decoder, decoder->PixFmt, format);
    VaapiFindImageFormat(decoder, PIX_FMT_NV12, format);

    // FIXME: this image is only needed for software decoder and auto-crop
    if (decoder->GetPutImage
	&& vaCreateImage(VaDisplay, format, width, height,
	    decoder->Image) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create image!\n"));
    }
    Debug(3,
	"video/vaapi: created image %dx%d with id 0x%08x and buffer id 0x%08x\n",
	width, height, decoder->Image->image_id, decoder->Image->buf);

    // FIXME: interlaced not valid here?
    decoder->Resolution =
	VideoResolutionGroup(width, height, decoder->Interlaced);
    VaapiCreateSurfaces(decoder, width, height);

#ifdef USE_GLX
    if (GlxEnabled) {
	// FIXME: destroy old context

	GlxSetupDecoder(decoder);
	// FIXME: try two textures
	if (vaCreateSurfaceGLX(decoder->VaDisplay, GL_TEXTURE_2D,
		decoder->GlTexture[0], &decoder->GlxSurface[0])
	    != VA_STATUS_SUCCESS) {
	    Fatal(_("video/glx: can't create glx surfaces\n"));
	}
	/*
	   if (vaCreateSurfaceGLX(decoder->VaDisplay, GL_TEXTURE_2D,
	   decoder->GlTexture[1], &decoder->GlxSurface[1])
	   != VA_STATUS_SUCCESS) {
	   Fatal(_("video/glx: can't create glx surfaces\n"));
	   }
	 */
    }
#endif
    VaapiUpdateOutput(decoder);

    //
    //	update OSD associate
    //
    VaapiAssociate(decoder);
}

///
///	Get a free surface.  Called from ffmpeg.
///
///	@param decoder		VA-API decoder
///	@param video_ctx	ffmpeg video codec context
///
///	@returns the oldest free surface
///
static VASurfaceID VaapiGetSurface(VaapiDecoder * decoder,
    const AVCodecContext * video_ctx)
{
    // get_format not called with valid informations.
    if (video_ctx->width != decoder->InputWidth
	|| video_ctx->height != decoder->InputHeight) {
	VAStatus status;

	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;

	VaapiSetup(decoder, video_ctx);

	// create a configuration for the decode pipeline
	if ((status =
		vaCreateConfig(decoder->VaDisplay, decoder->Profile,
		    decoder->Entrypoint, NULL, 0,
		    &decoder->VaapiContext->config_id))) {
	    Error(_("video/vaapi: can't create config '%s'\n"),
		vaErrorStr(status));
	    // bind surfaces to context
	} else if ((status =
		vaCreateContext(decoder->VaDisplay,
		    decoder->VaapiContext->config_id, video_ctx->width,
		    video_ctx->height, VA_PROGRESSIVE, decoder->SurfacesFree,
		    decoder->SurfaceFreeN,
		    &decoder->VaapiContext->context_id))) {
	    Error(_("video/vaapi: can't create context '%s'\n"),
		vaErrorStr(status));
	}
	// FIXME: too late to switch to software rending on failures
    }
    return VaapiGetSurface0(decoder);
}

///
///	Find VA-API profile.
///
///	Check if the requested profile is supported by VA-API.
///
///	@param profiles a table of all supported profiles
///	@param n	number of supported profiles
///	@param profile	requested profile
///
///	@returns the profile if supported, -1 if unsupported.
///
static VAProfile VaapiFindProfile(const VAProfile * profiles, unsigned n,
    VAProfile profile)
{
    unsigned u;

    for (u = 0; u < n; ++u) {
	if (profiles[u] == profile) {
	    return profile;
	}
    }
    return -1;
}

///
///	Find VA-API entry point.
///
///	Check if the requested entry point is supported by VA-API.
///
///	@param entrypoints	a table of all supported entrypoints
///	@param n		number of supported entrypoints
///	@param entrypoint	requested entrypoint
///
///	@returns the entry point if supported, -1 if unsupported.
///
static VAEntrypoint VaapiFindEntrypoint(const VAEntrypoint * entrypoints,
    unsigned n, VAEntrypoint entrypoint)
{
    unsigned u;

    for (u = 0; u < n; ++u) {
	if (entrypoints[u] == entrypoint) {
	    return entrypoint;
	}
    }
    return -1;
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param fmt	is the list of formats which are supported by the codec,
///			it is terminated by -1 as 0 is a valid format, the
///			formats are ordered by quality.
///
///	@note + 2 surface for software deinterlace
///
static enum PixelFormat Vaapi_get_format(VaapiDecoder * decoder,
    AVCodecContext * video_ctx, const enum PixelFormat *fmt)
{
    const enum PixelFormat *fmt_idx;
    VAProfile profiles[vaMaxNumProfiles(VaDisplay)];
    int profile_n;
    VAEntrypoint entrypoints[vaMaxNumEntrypoints(VaDisplay)];
    int entrypoint_n;
    int p;
    int e;
    VAConfigAttrib attrib;

    Debug(3, "video: new stream format %dms\n", GetMsTicks() - VideoSwitch);

    if (!VideoHardwareDecoder || (video_ctx->codec_id == CODEC_ID_MPEG2VIDEO
	    && VideoHardwareDecoder == 1)
	) {				// hardware disabled by config
	Debug(3, "codec: hardware acceleration disabled\n");
	goto slow_path;
    }

    p = -1;
    e = -1;

    //	prepare va-api profiles
    if (vaQueryConfigProfiles(VaDisplay, profiles, &profile_n)) {
	Error(_("codec: vaQueryConfigProfiles failed"));
	goto slow_path;
    }
    Debug(3, "codec: %d profiles\n", profile_n);

    // check profile
    switch (video_ctx->codec_id) {
	case CODEC_ID_MPEG2VIDEO:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_MPEG2 + VIDEO_SURFACES_MAX + 2;
	    p = VaapiFindProfile(profiles, profile_n, VAProfileMPEG2Main);
	    break;
	case CODEC_ID_MPEG4:
	case CODEC_ID_H263:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_MPEG4 + VIDEO_SURFACES_MAX + 2;
	    p = VaapiFindProfile(profiles, profile_n,
		VAProfileMPEG4AdvancedSimple);
	    break;
	case CODEC_ID_H264:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_H264 + VIDEO_SURFACES_MAX + 2;
	    // try more simple formats, fallback to better
	    if (video_ctx->profile == FF_PROFILE_H264_BASELINE) {
		p = VaapiFindProfile(profiles, profile_n,
		    VAProfileH264Baseline);
		if (p == -1) {
		    p = VaapiFindProfile(profiles, profile_n,
			VAProfileH264Main);
		}
	    } else if (video_ctx->profile == FF_PROFILE_H264_MAIN) {
		p = VaapiFindProfile(profiles, profile_n, VAProfileH264Main);
	    }
	    if (p == -1) {
		p = VaapiFindProfile(profiles, profile_n, VAProfileH264High);
	    }
	    break;
	case CODEC_ID_WMV3:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_VC1 + VIDEO_SURFACES_MAX + 2;
	    p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Main);
	    break;
	case CODEC_ID_VC1:
	    decoder->SurfacesNeeded =
		CODEC_SURFACES_VC1 + VIDEO_SURFACES_MAX + 2;
	    p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Advanced);
	    break;
	default:
	    goto slow_path;
    }
    if (p == -1) {
	Debug(3, "\tno profile found\n");
	goto slow_path;
    }
    Debug(3, "\tprofile %d\n", p);

    // prepare va-api entry points
    if (vaQueryConfigEntrypoints(VaDisplay, p, entrypoints, &entrypoint_n)) {
	Error(_("codec: vaQueryConfigEntrypoints failed"));
	goto slow_path;
    }
    Debug(3, "codec: %d entrypoints\n", entrypoint_n);
    //	look through formats
    for (fmt_idx = fmt; *fmt_idx != PIX_FMT_NONE; fmt_idx++) {
	Debug(3, "\t%#010x %s\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx));
	// check supported pixel format with entry point
	switch (*fmt_idx) {
	    case PIX_FMT_VAAPI_VLD:
		e = VaapiFindEntrypoint(entrypoints, entrypoint_n,
		    VAEntrypointVLD);
		break;
	    case PIX_FMT_VAAPI_MOCO:
	    case PIX_FMT_VAAPI_IDCT:
		Debug(3, "codec: this VA-API pixel format is not supported\n");
	    default:
		continue;
	}
	if (e != -1) {
	    Debug(3, "\tentry point %d\n", e);
	    break;
	}
    }
    if (e == -1) {
	Warning(_("codec: unsupported: slow path\n"));
	goto slow_path;
    }
    //
    //	prepare decoder config
    //
    memset(&attrib, 0, sizeof(attrib));
    attrib.type = VAConfigAttribRTFormat;
    if (vaGetConfigAttributes(decoder->VaDisplay, p, e, &attrib, 1)) {
	Error(_("codec: can't get attributes"));
	goto slow_path;
    }
    if (attrib.value & VA_RT_FORMAT_YUV420) {
	Info(_("codec: YUV 420 supported\n"));
    }
    if (attrib.value & VA_RT_FORMAT_YUV422) {
	Info(_("codec: YUV 422 supported\n"));
    }
    if (attrib.value & VA_RT_FORMAT_YUV444) {
	Info(_("codec: YUV 444 supported\n"));
    }

    if (!(attrib.value & VA_RT_FORMAT_YUV420)) {
	Warning(_("codec: YUV 420 not supported\n"));
	goto slow_path;
    }

    decoder->Profile = p;
    decoder->Entrypoint = e;
    decoder->PixFmt = *fmt_idx;
    decoder->InputWidth = 0;
    decoder->InputHeight = 0;

#if 0
    if (video_ctx->width && video_ctx->height) {
	VAStatus status;

	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;

	VaapiSetup(decoder, video_ctx);

	// FIXME: move the following into VaapiSetup
	// create a configuration for the decode pipeline
	if ((status =
		vaCreateConfig(decoder->VaDisplay, p, e, &attrib, 1,
		    &decoder->VaapiContext->config_id))) {
	    Error(_("codec: can't create config '%s'\n"), vaErrorStr(status));
	    goto slow_path;
	}
	// bind surfaces to context
	if ((status =
		vaCreateContext(decoder->VaDisplay,
		    decoder->VaapiContext->config_id, video_ctx->width,
		    video_ctx->height, VA_PROGRESSIVE, decoder->SurfacesFree,
		    decoder->SurfaceFreeN,
		    &decoder->VaapiContext->context_id))) {
	    Error(_("codec: can't create context '%s'\n"), vaErrorStr(status));
	    goto slow_path;
	}
    }
#endif

    Debug(3, "\t%#010x %s\n", fmt_idx[0], av_get_pix_fmt_name(fmt_idx[0]));
    return *fmt_idx;

  slow_path:
    // no accelerated format found
    decoder->Profile = VA_INVALID_ID;
    decoder->Entrypoint = VA_INVALID_ID;
    decoder->VaapiContext->config_id = VA_INVALID_ID;
    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX + 2;
    decoder->PixFmt = PIX_FMT_NONE;

    decoder->InputWidth = 0;
    decoder->InputHeight = 0;
    video_ctx->hwaccel_context = NULL;

    return avcodec_default_get_format(video_ctx, fmt);
}

///
///	Draw surface of the VA-API decoder with x11.
///
///	vaPutSurface with intel backend does sync on v-sync.
///
///	@param decoder	VA-API decoder
///	@param surface		VA-API surface id
///	@param interlaced	flag interlaced source
///	@param top_field_first	flag top_field_first for interlaced source
///	@param field		interlaced draw: 0 first field, 1 second field
///
static void VaapiPutSurfaceX11(VaapiDecoder * decoder, VASurfaceID surface,
    int interlaced, int top_field_first, int field)
{
    unsigned type;
    VAStatus status;
    uint32_t s;
    uint32_t e;

    // deinterlace
    if (interlaced
	&& VideoDeinterlace[decoder->Resolution] < VideoDeinterlaceSoftBob
	&& VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceWeave) {
	if (top_field_first) {
	    if (field) {
		type = VA_BOTTOM_FIELD;
	    } else {
		type = VA_TOP_FIELD;
	    }
	} else {
	    if (field) {
		type = VA_TOP_FIELD;
	    } else {
		type = VA_BOTTOM_FIELD;
	    }
	}
    } else {
	type = VA_FRAME_PICTURE;
    }

    s = GetMsTicks();
    xcb_flush(Connection);
    if ((status = vaPutSurface(decoder->VaDisplay, surface, decoder->Window,
		// decoder src
		decoder->CropX, decoder->CropY, decoder->CropWidth,
		decoder->CropHeight,
		// video dst
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth,
		decoder->OutputHeight, NULL, 0,
		type | decoder->SurfaceFlagsTable[decoder->Resolution]))
	!= VA_STATUS_SUCCESS) {
	// switching video kills VdpPresentationQueueBlockUntilSurfaceIdle
	Error(_("video/vaapi: vaPutSurface failed %d\n"), status);
    }

    if (0 && vaSyncSurface(decoder->VaDisplay, surface) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }
    e = GetMsTicks();
    if (e - s > 2000) {
	Error(_("video/vaapi: gpu hung %dms %d\n"), e - s,
	    decoder->FrameCounter);
	fprintf(stderr, _("video/vaapi: gpu hung %dms %d\n"), e - s,
	    decoder->FrameCounter);
    }

    if (0) {
	// check if surface is really ready
	// VDPAU backend, says always ready
	VASurfaceStatus status;

	if (vaQuerySurfaceStatus(decoder->VaDisplay, surface, &status)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaQuerySurface failed\n"));
	    status = VASurfaceReady;
	}
	if (status != VASurfaceReady) {
	    Warning(_
		("video/vaapi: surface %#010x not ready: still displayed %d\n"),
		surface, status);
	    return;
	}
    }

    if (0) {
	int i;

	// look how the status changes the next 40ms
	for (i = 0; i < 40; ++i) {
	    VASurfaceStatus status;

	    if (vaQuerySurfaceStatus(VaDisplay, surface,
		    &status) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaQuerySurface failed\n"));
	    }
	    Debug(3, "video/vaapi: %2d %d\n", i, status);
	    usleep(1 * 1000);
	}
    }
    usleep(1 * 1000);
}

#ifdef USE_GLX

///
///	Render texture.
///
///	@param texture	2d texture
///
static inline void VideoRenderTexture(GLuint texture, int x, int y, int width,
    int height)
{
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);	// no color
    glBegin(GL_QUADS); {
	glTexCoord2f(1.0f, 1.0f);
	glVertex2i(x + width, y + height);
	glTexCoord2f(0.0f, 1.0f);
	glVertex2i(x, y + height);
	glTexCoord2f(0.0f, 0.0f);
	glVertex2i(x, y);
	glTexCoord2f(1.0f, 0.0f);
	glVertex2i(x + width, y);
#if 0
	glTexCoord2f(0.0f, 0.0f);
	glVertex2i(x, y);
	glTexCoord2f(0.0f, 1.0f);
	glVertex2i(x, y + height);
	glTexCoord2f(1.0f, 1.0f);
	glVertex2i(x + width, y + height);
	glTexCoord2f(1.0f, 0.0f);
	glVertex2i(x + width, y);
#endif
    }
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

///
///	Draw surface of the VA-API decoder with glx.
///
///	@param decoder	VA-API decoder
///	@param surface		VA-API surface id
///	@param interlaced	flag interlaced source
///	@param top_field_first	flag top_field_first for interlaced source
///	@param field		interlaced draw: 0 first field, 1 second field
///
static void VaapiPutSurfaceGLX(VaapiDecoder * decoder, VASurfaceID surface,
    int interlaced, int top_field_first, int field)
{
    unsigned type;
    uint32_t start;
    uint32_t copy;
    uint32_t end;

    // deinterlace
    if (interlaced
	&& VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceWeave) {
	if (top_field_first) {
	    if (field) {
		type = VA_BOTTOM_FIELD;
	    } else {
		type = VA_TOP_FIELD;
	    }
	} else {
	    if (field) {
		type = VA_TOP_FIELD;
	    } else {
		type = VA_BOTTOM_FIELD;
	    }
	}
    } else {
	type = VA_FRAME_PICTURE;
    }
    start = GetMsTicks();
    if (vaCopySurfaceGLX(decoder->VaDisplay, decoder->GlxSurface[0], surface,
	    type | decoder->SurfaceFlagsTable[decoder->Resolution]) !=
	VA_STATUS_SUCCESS) {
	Error(_("video/glx: vaCopySurfaceGLX failed\n"));
	return;
    }
    copy = GetMsTicks();
    // hardware surfaces are always busy
    VideoRenderTexture(decoder->GlTexture[0], decoder->OutputX,
	decoder->OutputY, decoder->OutputWidth, decoder->OutputHeight);
    end = GetMsTicks();
    //Debug(3, "video/vaapi/glx: %d copy %d render\n", copy - start, end - copy);
}

#endif

#ifdef USE_AUTOCROP

///
///	VA-API auto-crop support.
///
///	@param decoder	VA-API hw decoder
///
static void VaapiAutoCrop(VaapiDecoder * decoder)
{
    VASurfaceID surface;
    uint32_t width;
    uint32_t height;
    void *va_image_data;
    void *data[3];
    uint32_t pitches[3];
    int crop14;
    int crop16;
    int next_state;
    int i;

    width = decoder->InputWidth;
    height = decoder->InputHeight;

  again:
    if (decoder->GetPutImage && decoder->Image->image_id == VA_INVALID_ID) {
	VAImageFormat format[1];

	Debug(3, "video/vaapi: download image not available\n");

	// FIXME: PixFmt not set!
	//VaapiFindImageFormat(decoder, decoder->PixFmt, format);
	VaapiFindImageFormat(decoder, PIX_FMT_NV12, format);
	//VaapiFindImageFormat(decoder, PIX_FMT_YUV420P, format);
	if (vaCreateImage(VaDisplay, format, width, height,
		decoder->Image) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create image!\n"));
	    return;
	}
    }
    // no problem to go back, we just wrote it
    // FIXME: we can pass the surface through.
    surface =
	decoder->SurfacesRb[(decoder->SurfaceWrite + VIDEO_SURFACES_MAX -
	    1) % VIDEO_SURFACES_MAX];

    //	Copy data from frame to image
    if (!decoder->GetPutImage
	&& vaDeriveImage(decoder->VaDisplay, surface,
	    decoder->Image) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed\n"));
	decoder->GetPutImage = 1;
	goto again;
    }
    if (decoder->GetPutImage
	&& (i =
	    vaGetImage(decoder->VaDisplay, surface, 0, 0, decoder->InputWidth,
		decoder->InputHeight,
		decoder->Image->image_id)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't get auto-crop image %d\n"), i);
	printf(_("video/vaapi: can't get auto-crop image %d\n"), i);
	return;
    }
    if (vaMapBuffer(VaDisplay, decoder->Image->buf, &va_image_data)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't map auto-crop image!\n"));
	return;
    }
    // convert vaapi to our frame format
    for (i = 0; (unsigned)i < decoder->Image->num_planes; ++i) {
	data[i] = va_image_data + decoder->Image->offsets[i];
	pitches[i] = decoder->Image->pitches[i];
    }

    AutoCropDetect(decoder->AutoCrop, width, height, data, pitches);

    if (vaUnmapBuffer(VaDisplay, decoder->Image->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap auto-crop image!\n"));
    }
    if (!decoder->GetPutImage) {
	if (vaDestroyImage(VaDisplay, decoder->Image->image_id)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	decoder->Image->image_id = VA_INVALID_ID;
    }
    // FIXME: this a copy of vdpau, combine the two same things

    // ignore black frames
    if (decoder->AutoCrop->Y1 >= decoder->AutoCrop->Y2) {
	return;
    }

    crop14 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 14);
    crop14 = (decoder->InputHeight - crop14) / 2;
    crop16 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 16);
    crop16 = (decoder->InputHeight - crop16) / 2;

    if (decoder->AutoCrop->Y1 >= crop16 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop16 - AutoCropTolerance) {
	next_state = 16;
    } else if (decoder->AutoCrop->Y1 >= crop14 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop14 - AutoCropTolerance) {
	next_state = 14;
    } else {
	next_state = 0;
    }

    if (decoder->AutoCrop->State == next_state) {
	return;
    }

    Debug(3, "video: crop aspect %d:%d %d/%d %+d%+d\n",
	decoder->InputAspect.num, decoder->InputAspect.den, crop14, crop16,
	decoder->AutoCrop->Y1, decoder->InputHeight - decoder->AutoCrop->Y2);

    Debug(3, "video: crop aspect %d -> %d\n", decoder->AutoCrop->State,
	next_state);

    switch (decoder->AutoCrop->State) {
	case 16:
	case 14:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay / 2) {
		return;
	    }
	    break;
	case 0:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay) {
		return;
	    }
	    break;
    }

    decoder->AutoCrop->State = next_state;
    if (next_state) {
	decoder->CropX = VideoCutLeftRight[decoder->Resolution];
	decoder->CropY =
	    (next_state ==
	    16 ? crop16 : crop14) + VideoCutTopBottom[decoder->Resolution];
	decoder->CropWidth = decoder->InputWidth - decoder->CropX * 2;
	decoder->CropHeight = decoder->InputHeight - decoder->CropY * 2;

	// FIXME: this overwrites user choosen output position
	// FIXME: resize kills the auto crop values
	// FIXME: support other 4:3 zoom modes
	decoder->OutputX = decoder->VideoX;
	decoder->OutputY = decoder->VideoY;
	decoder->OutputWidth = (decoder->VideoHeight * next_state) / 9;
	decoder->OutputHeight = (decoder->VideoWidth * 9) / next_state;
	if (decoder->OutputWidth > decoder->VideoWidth) {
	    decoder->OutputWidth = decoder->VideoWidth;
	    decoder->OutputY =
		(decoder->VideoHeight - decoder->OutputHeight) / 2;
	} else if (decoder->OutputHeight > decoder->VideoHeight) {
	    decoder->OutputHeight = decoder->VideoHeight;
	    decoder->OutputX =
		(decoder->VideoWidth - decoder->OutputWidth) / 2;
	}
	Debug(3, "video: aspect output %dx%d %dx%d%+d%+d\n",
	    decoder->InputWidth, decoder->InputHeight, decoder->OutputWidth,
	    decoder->OutputHeight, decoder->OutputX, decoder->OutputY);
    } else {
	// sets AutoCrop->Count
	VaapiUpdateOutput(decoder);
    }
    decoder->AutoCrop->Count = 0;

    //
    //	update OSD associate
    //
    VaapiDeassociate(decoder);
    VaapiAssociate(decoder);
}

///
///	VA-API check if auto-crop todo.
///
///	@param decoder	VA-API hw decoder
///
///	@note a copy of VdpauCheckAutoCrop
///	@note auto-crop only supported with normal 4:3 display mode
///
static void VaapiCheckAutoCrop(VaapiDecoder * decoder)
{
    // reduce load, check only n frames
    if (Video4to3ZoomMode == VideoNormal && AutoCropInterval
	&& !(decoder->FrameCounter % AutoCropInterval)) {
	AVRational input_aspect_ratio;
	AVRational tmp_ratio;

	av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den,
	    decoder->InputWidth * decoder->InputAspect.num,
	    decoder->InputHeight * decoder->InputAspect.den, 1024 * 1024);

	tmp_ratio.num = 4;
	tmp_ratio.den = 3;
	// only 4:3 with 16:9/14:9 inside supported
	if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	    VaapiAutoCrop(decoder);
	} else {
	    decoder->AutoCrop->Count = 0;
	    decoder->AutoCrop->State = 0;
	}
    }
}

///
///	VA-API reset auto-crop.
///
static void VaapiResetAutoCrop(void)
{
    int i;

    for (i = 0; i < VaapiDecoderN; ++i) {
	VaapiDecoders[i]->AutoCrop->State = 0;
	VaapiDecoders[i]->AutoCrop->Count = 0;
    }
}

#endif

///
///	Queue output surface.
///
///	@param decoder	VA-API decoder
///	@param surface	output surface
///	@param softdec	software decoder
///
///	@note we can't mix software and hardware decoder surfaces
///
static void VaapiQueueSurface(VaapiDecoder * decoder, VASurfaceID surface,
    int softdec)
{
    VASurfaceID old;

    ++decoder->FrameCounter;

    if (1) {				// can't wait for output queue empty
	if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    ++decoder->FramesDropped;
	    Warning(_("video: output buffer full, dropping frame (%d/%d)\n"),
		decoder->FramesDropped, decoder->FrameCounter);
	    if (!(decoder->FramesDisplayed % 300)) {
		VaapiPrintFrames(decoder);
	    }
	    if (softdec) {		// software surfaces only
		VaapiReleaseSurface(decoder, surface);
	    }
	    return;
	}
#if 0
    } else {				// wait for output queue empty
	while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    VideoDisplayHandler();
	}
#endif
    }

    //
    //	    Check and release, old surface
    //
    if ((old = decoder->SurfacesRb[decoder->SurfaceWrite])
	!= VA_INVALID_ID) {

#if 0
	if (vaSyncSurface(decoder->VaDisplay, old) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
	VASurfaceStatus status;

	if (vaQuerySurfaceStatus(decoder->VaDisplay, old, &status)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaQuerySurface failed\n"));
	    status = VASurfaceReady;
	}
	if (status != VASurfaceReady) {
	    Warning(_
		("video/vaapi: surface %#010x not ready: still displayed %d\n"),
		old, status);
	    if (0
		&& vaSyncSurface(decoder->VaDisplay,
		    old) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaSyncSurface failed\n"));
	    }
	}
#endif

	// now we can release the surface
	if (softdec) {			// software surfaces only
	    VaapiReleaseSurface(decoder, old);
	}
    }
#if 0
    // FIXME: intel seems to forget this, nvidia GT 210 has speed problems here
    if (VaapiBuggyIntel && VaOsdSubpicture != VA_INVALID_ID) {
	// FIXME: associate only if osd is displayed

	//
	//	associate the OSD with surface
	//
	if (VaapiUnscaledOsd) {
	    if (vaAssociateSubpicture(VaDisplay, VaOsdSubpicture, &surface, 1,
		    0, 0, VaOsdImage.width, VaOsdImage.height, 0, 0,
		    VideoWindowWidth, VideoWindowHeight,
		    VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't associate subpicture\n"));
	    }
	} else {
	    // FIXME: auto-crop wrong position
	    if (vaAssociateSubpicture(VaDisplay, VaOsdSubpicture, &surface, 1,
		    0, 0, VaOsdImage.width, VaOsdImage.height, 0, 0,
		    decoder->InputWidth, decoder->InputHeight, 0)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't associate subpicture\n"));
	    }
	}
    }
#endif

    decoder->SurfacesRb[decoder->SurfaceWrite] = surface;
    decoder->SurfaceWrite = (decoder->SurfaceWrite + 1)
	% VIDEO_SURFACES_MAX;
    atomic_inc(&decoder->SurfacesFilled);

    Debug(4, "video/vaapi: yy video surface %#010x ready\n", surface);
}

///
///	Create and display a black empty surface.
///
///	@param decoder	VA-API decoder
///
static void VaapiBlackSurface(VaapiDecoder * decoder)
{
    VAStatus status;

#ifdef DEBUG
    uint32_t start;
#endif
    uint32_t sync;
    uint32_t put1;

    // wait until we have osd subpicture
    if (VaOsdSubpicture == VA_INVALID_ID) {
	Warning(_("video/vaapi: no osd subpicture yet\n"));
	return;
    }

    if (decoder->BlackSurface == VA_INVALID_ID) {
	if (vaCreateSurfaces(decoder->VaDisplay, VA_RT_FORMAT_YUV420,
		VideoWindowWidth, VideoWindowHeight, &decoder->BlackSurface, 1,
		NULL, 0) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create a surface\n"));
	    return;
	}
	// full sized surface, no difference unscaled/scaled osd
	if (vaAssociateSubpicture(decoder->VaDisplay, VaOsdSubpicture,
		&decoder->BlackSurface, 1, 0, 0, VaOsdImage.width,
		VaOsdImage.height, 0, 0, VideoWindowWidth, VideoWindowHeight,
		0) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
	Debug(3, "video/vaapi: associate %08x\n", decoder->BlackSurface);
	// FIXME: check if intel forgets this also

	if (0 && decoder->Image->image_id == VA_INVALID_ID) {
	    VAImageFormat format[1];
	    void *va_image_data;
	    int i;

	    printf("No image\n");
	    VaapiFindImageFormat(decoder, PIX_FMT_NV12, format);
	    if ((status =
		    vaDeriveImage(decoder->VaDisplay, decoder->BlackSurface,
			decoder->Image)) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaDeriveImage failed %d\n"), status);
		if (vaCreateImage(VaDisplay, format, VideoWindowWidth,
			VideoWindowHeight,
			decoder->Image) != VA_STATUS_SUCCESS) {
		    Error(_("video/vaapi: can't create image!\n"));
		}
	    }
	    if (vaMapBuffer(VaDisplay, decoder->Image->buf, &va_image_data)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't map the image!\n"));
	    }

	    for (i = 0; (unsigned)i < decoder->Image->data_size; i += 2) {
		((uint8_t *) va_image_data)[i + 0] = 0xFF;
		((uint8_t *) va_image_data)[i + 1] = 0xFF;
	    }

	    if (vaUnmapBuffer(VaDisplay,
		    decoder->Image->buf) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't unmap the image!\n"));
	    }
	    if (vaDestroyImage(VaDisplay,
		    decoder->Image->image_id) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't destroy image!\n"));
	    }
	}
	// FIXME: intel didn't support put image.
	if (0
	    && vaPutImage(VaDisplay, decoder->BlackSurface,
		decoder->Image->image_id, 0, 0, VideoWindowWidth,
		VideoWindowHeight, 0, 0, VideoWindowWidth, VideoWindowHeight)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't put image!\n"));
	}
#ifdef DEBUG
	start = GetMsTicks();
#endif
	if (vaSyncSurface(decoder->VaDisplay,
		decoder->BlackSurface) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
    } else {
#ifdef DEBUG
	start = GetMsTicks();
#endif
    }

    Debug(4, "video/vaapi: yy black video surface %#010x displayed\n",
	decoder->BlackSurface);
    sync = GetMsTicks();
    xcb_flush(Connection);
    if ((status =
	    vaPutSurface(decoder->VaDisplay, decoder->BlackSurface,
		decoder->Window,
		// decoder src
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth,
		decoder->OutputHeight,
		// video dst
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth,
		decoder->OutputHeight, NULL, 0,
		VA_FRAME_PICTURE)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaPutSurface failed %d\n"), status);
    }
    clock_gettime(CLOCK_REALTIME, &decoder->FrameTime);

    put1 = GetMsTicks();
    if (put1 - sync > 2000) {
	Error(_("video/vaapi: gpu hung %dms %d\n"), put1 - sync,
	    decoder->FrameCounter);
	fprintf(stderr, _("video/vaapi: gpu hung %dms %d\n"), put1 - sync,
	    decoder->FrameCounter);
    }
    Debug(4, "video/vaapi: sync %2u put1 %2u\n", sync - start, put1 - sync);

    if (0 && vaSyncSurface(decoder->VaDisplay, decoder->BlackSurface)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }
    usleep(1 * 1000);
}

#define noUSE_VECTOR			///< use gcc vector extension
#ifdef USE_VECTOR

typedef char v16qi __attribute__ ((vector_size(16)));
typedef char v8qi __attribute__ ((vector_size(8)));
typedef int16_t v4hi __attribute__ ((vector_size(4)));
typedef int16_t v8hi __attribute__ ((vector_size(8)));

///
///	ELA Edge-based Line Averaging
///	Low-Complexity Interpolation Method
///
///	abcdefg	   abcdefg	abcdefg	 abcdefg    abcdefg
///	   x	     x		  x	    x		 x
///	hijklmn	 hijklmn    hijklmn	   hijklmn	 hijklmn
///
static void FilterLineSpatial(uint8_t * dst, const uint8_t * cur, int width,
    int above, int below, int next)
{
    int x;

    // 8/16 128bit xmm register

    for (x = 0; x < width; x += 8) {
	v8qi c;
	v8qi d;
	v8qi e;
	v8qi j;
	v8qi k;
	v8qi l;
	v8qi t1;
	v8qi t2;
	v8qi pred;
	v8qi score_l;
	v8qi score_h;
	v8qi t_l;
	v8qi t_h;
	v8qi zero;

	// ignore bound violation
	d = *(v8qi *) & cur[above + x];
	k = *(v8qi *) & cur[below + x];
	pred = __builtin_ia32_pavgb(d, k);

	// score = ABS(c - j) + ABS(d - k) + ABS(e - l);
	c = *(v8qi *) & cur[above + x - 1 * next];
	e = *(v8qi *) & cur[above + x + 1 * next];
	j = *(v8qi *) & cur[below + x - 1 * next];
	l = *(v8qi *) & cur[below + x + 1 * next];

	t1 = __builtin_ia32_psubusb(c, j);
	t2 = __builtin_ia32_psubusb(j, c);
	t1 = __builtin_ia32_pmaxub(t1, t2);
	zero ^= zero;
	score_l = __builtin_ia32_punpcklbw(t1, zero);
	score_h = __builtin_ia32_punpckhbw(t1, zero);

	t1 = __builtin_ia32_psubusb(d, k);
	t2 = __builtin_ia32_psubusb(k, d);
	t1 = __builtin_ia32_pmaxub(t1, t2);
	t_l = __builtin_ia32_punpcklbw(t1, zero);
	t_h = __builtin_ia32_punpckhbw(t1, zero);
	score_l = __builtin_ia32_paddw(score_l, t_l);
	score_h = __builtin_ia32_paddw(score_h, t_h);

	t1 = __builtin_ia32_psubusb(e, l);
	t2 = __builtin_ia32_psubusb(l, e);
	t1 = __builtin_ia32_pmaxub(t1, t2);
	t_l = __builtin_ia32_punpcklbw(t1, zero);
	t_h = __builtin_ia32_punpckhbw(t1, zero);
	score_l = __builtin_ia32_paddw(score_l, t_l);
	score_h = __builtin_ia32_paddw(score_h, t_h);

	*(v8qi *) & dst[x] = pred;
    }
}

#else

    /// Return the absolute value of an integer.
#define ABS(i)	((i) >= 0 ? (i) : (-(i)))

///
///	ELA Edge-based Line Averaging
///	Low-Complexity Interpolation Method
///
///	abcdefg	   abcdefg	abcdefg	 abcdefg    abcdefg
///	   x	     x		  x	    x		 x
///	hijklmn	 hijklmn    hijklmn	   hijklmn	 hijklmn
///
static void FilterLineSpatial(uint8_t * dst, const uint8_t * cur, int width,
    int above, int below, int next)
{
    int a, b, c, d, e, f, g, h, i, j, k, l, m, n;
    int spatial_pred;
    int spatial_score;
    int score;
    int x;

    for (x = 0; x < width; ++x) {
	a = cur[above + x - 3 * next];	// ignore bound violation
	b = cur[above + x - 2 * next];
	c = cur[above + x - 1 * next];
	d = cur[above + x + 0 * next];
	e = cur[above + x + 1 * next];
	f = cur[above + x + 2 * next];
	g = cur[above + x + 3 * next];

	h = cur[below + x - 3 * next];
	i = cur[below + x - 2 * next];
	j = cur[below + x - 1 * next];
	k = cur[below + x + 0 * next];
	l = cur[below + x + 1 * next];
	m = cur[below + x + 2 * next];
	n = cur[below + x + 3 * next];

	spatial_pred = (d + k) / 2;	// 0 pixel
	spatial_score = ABS(c - j) + ABS(d - k) + ABS(e - l);

	score = ABS(b - k) + ABS(c - l) + ABS(d - m);
	if (score < spatial_score) {
	    spatial_pred = (c + l) / 2;	// 1 pixel
	    spatial_score = score;
	    score = ABS(a - l) + ABS(b - m) + ABS(c - n);
	    if (score < spatial_score) {
		spatial_pred = (b + m) / 2;	// 2 pixel
		spatial_score = score;
	    }
	}
	score = ABS(d - i) + ABS(e - j) + ABS(f - k);
	if (score < spatial_score) {
	    spatial_pred = (e + j) / 2;	// -1 pixel
	    spatial_score = score;
	    score = ABS(e - h) + ABS(f - i) + ABS(g - j);
	    if (score < spatial_score) {
		spatial_pred = (f + i) / 2;	// -2 pixel
		spatial_score = score;
	    }
	}

	dst[x + 0] = spatial_pred;
    }
}

#endif

///
///	Vaapi spatial deinterlace.
///
///	@note FIXME: use common software deinterlace functions.
///
static void VaapiSpatial(VaapiDecoder * decoder, VAImage * src, VAImage * dst1,
    VAImage * dst2)
{
#ifdef DEBUG
    uint32_t tick1;
    uint32_t tick2;
    uint32_t tick3;
    uint32_t tick4;
    uint32_t tick5;
    uint32_t tick6;
    uint32_t tick7;
    uint32_t tick8;
#endif
    void *src_base;
    void *dst1_base;
    void *dst2_base;
    unsigned y;
    unsigned p;
    uint8_t *tmp;
    int pitch;
    int width;

#ifdef DEBUG
    tick1 = GetMsTicks();
#endif
    if (vaMapBuffer(decoder->VaDisplay, src->buf,
	    &src_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
#ifdef DEBUG
    tick2 = GetMsTicks();
#endif
    if (vaMapBuffer(decoder->VaDisplay, dst1->buf,
	    &dst1_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
#ifdef DEBUG
    tick3 = GetMsTicks();
#endif
    if (vaMapBuffer(decoder->VaDisplay, dst2->buf,
	    &dst2_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
#ifdef DEBUG
    tick4 = GetMsTicks();
#endif

    if (0) {				// test all updated
	memset(dst1_base, 0x00, dst1->data_size);
	memset(dst2_base, 0xFF, dst2->data_size);
    }
    // use tmp copy FIXME: only for intel needed
    tmp = malloc(src->data_size);
    memcpy(tmp, src_base, src->data_size);

    if (src->num_planes == 2) {		// NV12
	pitch = src->pitches[0];
	width = src->width;
	for (y = 0; y < (unsigned)src->height; y++) {	// Y
	    const uint8_t *cur;

	    cur = tmp + src->offsets[0] + y * pitch;
	    if (y & 1) {
		// copy to 2nd
		memcpy(dst2_base + src->offsets[0] + y * pitch, cur, width);
		// create 1st
		FilterLineSpatial(dst1_base + src->offsets[0] + y * pitch, cur,
		    width, y ? -pitch : pitch,
		    y + 1 < (unsigned)src->height ? pitch : -pitch, 1);
	    } else {
		// copy to 1st
		memcpy(dst1_base + src->offsets[0] + y * pitch, cur, width);
		// create 2nd
		FilterLineSpatial(dst2_base + src->offsets[0] + y * pitch, cur,
		    width, y ? -pitch : pitch,
		    y + 1 < (unsigned)src->height ? pitch : -pitch, 1);
	    }
	}
	if (VideoSkipChromaDeinterlace[decoder->Resolution]) {
	    for (y = 0; y < (unsigned)src->height / 2; y++) {	// UV
		const uint8_t *cur;

		cur = tmp + src->offsets[1] + y * pitch;
		// copy to 1st
		memcpy(dst1_base + src->offsets[1] + y * pitch, cur, width);
		// copy to 2nd
		memcpy(dst2_base + src->offsets[1] + y * pitch, cur, width);
	    }
	} else {
	    for (y = 0; y < (unsigned)src->height / 2; y++) {	// UV
		const uint8_t *cur;

		cur = tmp + src->offsets[1] + y * pitch;
		if (y & 1) {
		    // copy to 2nd
		    memcpy(dst2_base + src->offsets[1] + y * pitch, cur,
			width);
		    // create 1st
		    FilterLineSpatial(dst1_base + src->offsets[1] + y * pitch,
			cur, width, y ? -pitch : pitch,
			y + 1 < (unsigned)src->height / 2 ? pitch : -pitch, 2);
		} else {
		    // copy to 1st
		    memcpy(dst1_base + src->offsets[1] + y * pitch, cur,
			width);
		    // create 2nd
		    FilterLineSpatial(dst2_base + src->offsets[1] + y * pitch,
			cur, width, y ? -pitch : pitch,
			y + 1 < (unsigned)src->height / 2 ? pitch : -pitch, 2);
		}
	    }
	}
    } else {				// YV12 or I420
	for (p = 0; p < src->num_planes; ++p) {
	    pitch = src->pitches[p];
	    width = src->width >> (p != 0);
	    if (VideoSkipChromaDeinterlace[decoder->Resolution] && p) {
		for (y = 0; y < (unsigned)(src->height >> 1); y++) {
		    const uint8_t *cur;

		    cur = tmp + src->offsets[p] + y * pitch;
		    // copy to 1st
		    memcpy(dst1_base + src->offsets[p] + y * pitch, cur,
			width);
		    // copy to 2nd
		    memcpy(dst2_base + src->offsets[p] + y * pitch, cur,
			width);
		}
	    } else {
		for (y = 0; y < (unsigned)(src->height >> (p != 0)); y++) {
		    const uint8_t *cur;

		    cur = tmp + src->offsets[p] + y * pitch;
		    if (y & 1) {
			// copy to 2nd
			memcpy(dst2_base + src->offsets[p] + y * pitch, cur,
			    width);
			// create 1st
			FilterLineSpatial(dst1_base + src->offsets[p] +
			    y * pitch, cur, width, y ? -pitch : pitch,
			    y + 1 < (unsigned)(src->height >> (p != 0))
			    ? pitch : -pitch, 1);
		    } else {
			// copy to 1st
			memcpy(dst1_base + src->offsets[p] + y * pitch, cur,
			    width);
			// create 2nd
			FilterLineSpatial(dst2_base + src->offsets[p] +
			    y * pitch, cur, width, y ? -pitch : pitch,
			    y + 1 < (unsigned)(src->height >> (p != 0))
			    ? pitch : -pitch, 1);
		    }
		}
	    }
	}
    }
    free(tmp);

#ifdef DEBUG
    tick5 = GetMsTicks();
#endif
    if (vaUnmapBuffer(decoder->VaDisplay, dst2->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
#ifdef DEBUG
    tick6 = GetMsTicks();
#endif
    if (vaUnmapBuffer(decoder->VaDisplay, dst1->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
#ifdef DEBUG
    tick7 = GetMsTicks();
#endif
    if (vaUnmapBuffer(decoder->VaDisplay, src->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
#ifdef DEBUG
    tick8 = GetMsTicks();

    Debug(3, "video/vaapi: map=%2d/%2d/%2d deint=%2d umap=%2d/%2d/%2d\n",
	tick2 - tick1, tick3 - tick2, tick4 - tick3, tick5 - tick4,
	tick6 - tick5, tick7 - tick6, tick8 - tick7);
#endif
}

///
///	Vaapi bob deinterlace.
///
///	@note FIXME: use common software deinterlace functions.
///
static void VaapiBob(VaapiDecoder * decoder, VAImage * src, VAImage * dst1,
    VAImage * dst2)
{
#ifdef DEBUG
    uint32_t tick1;
    uint32_t tick2;
    uint32_t tick3;
    uint32_t tick4;
    uint32_t tick5;
    uint32_t tick6;
    uint32_t tick7;
    uint32_t tick8;
#endif
    void *src_base;
    void *dst1_base;
    void *dst2_base;
    unsigned y;
    unsigned p;

#ifdef DEBUG
    tick1 = GetMsTicks();
#endif
    if (vaMapBuffer(decoder->VaDisplay, src->buf,
	    &src_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
#ifdef DEBUG
    tick2 = GetMsTicks();
#endif
    if (vaMapBuffer(decoder->VaDisplay, dst1->buf,
	    &dst1_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
#ifdef DEBUG
    tick3 = GetMsTicks();
#endif
    if (vaMapBuffer(decoder->VaDisplay, dst2->buf,
	    &dst2_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
#ifdef DEBUG
    tick4 = GetMsTicks();
#endif

    if (0) {				// test all updated
	memset(dst1_base, 0x00, dst1->data_size);
	memset(dst2_base, 0xFF, dst2->data_size);
	return;
    }
#if 0
    // interleave
    for (p = 0; p < src->num_planes; ++p) {
	for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
	    memcpy(dst1_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst1_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src->pitches[p]);

	    memcpy(dst2_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst2_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	}
    }
#endif
#if 1
    // use tmp copy
    if (1) {
	uint8_t *tmp;

	tmp = malloc(src->data_size);
	memcpy(tmp, src_base, src->data_size);

	for (p = 0; p < src->num_planes; ++p) {
	    for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
		memcpy(dst1_base + src->offsets[p] + (y + 0) * src->pitches[p],
		    tmp + src->offsets[p] + (y + 0) * src->pitches[p],
		    src->pitches[p]);
		memcpy(dst1_base + src->offsets[p] + (y + 1) * src->pitches[p],
		    tmp + src->offsets[p] + (y + 0) * src->pitches[p],
		    src->pitches[p]);

		memcpy(dst2_base + src->offsets[p] + (y + 0) * src->pitches[p],
		    tmp + src->offsets[p] + (y + 1) * src->pitches[p],
		    src->pitches[p]);
		memcpy(dst2_base + src->offsets[p] + (y + 1) * src->pitches[p],
		    tmp + src->offsets[p] + (y + 1) * src->pitches[p],
		    src->pitches[p]);
	    }

	}
	free(tmp);
    }
#endif
#if 0
    // use multiple tmp copy
    if (1) {
	uint8_t *tmp_src;
	uint8_t *tmp_dst1;
	uint8_t *tmp_dst2;

	tmp_src = malloc(src->data_size);
	memcpy(tmp_src, src_base, src->data_size);
	tmp_dst1 = malloc(src->data_size);
	tmp_dst2 = malloc(src->data_size);

	for (p = 0; p < src->num_planes; ++p) {
	    for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
		memcpy(tmp_dst1 + src->offsets[p] + (y + 0) * src->pitches[p],
		    tmp_src + src->offsets[p] + (y + 0) * src->pitches[p],
		    src->pitches[p]);
		memcpy(tmp_dst1 + src->offsets[p] + (y + 1) * src->pitches[p],
		    tmp_src + src->offsets[p] + (y + 0) * src->pitches[p],
		    src->pitches[p]);

		memcpy(tmp_dst2 + src->offsets[p] + (y + 0) * src->pitches[p],
		    tmp_src + src->offsets[p] + (y + 1) * src->pitches[p],
		    src->pitches[p]);
		memcpy(tmp_dst2 + src->offsets[p] + (y + 1) * src->pitches[p],
		    tmp_src + src->offsets[p] + (y + 1) * src->pitches[p],
		    src->pitches[p]);
	    }
	}
	memcpy(dst1_base, tmp_dst1, src->data_size);
	memcpy(dst2_base, tmp_dst2, src->data_size);

	free(tmp_src);
	free(tmp_dst1);
	free(tmp_dst2);
    }
#endif
#if 0
    // dst1 first
    for (p = 0; p < src->num_planes; ++p) {
	for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
	    memcpy(dst1_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst1_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src->pitches[p]);
	}
    }
    // dst2 next
    for (p = 0; p < src->num_planes; ++p) {
	for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
	    memcpy(dst2_base + src->offsets[p] + (y + 0) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst2_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	}
    }
#endif

#ifdef DEBUG
    tick5 = GetMsTicks();
#endif

    if (vaUnmapBuffer(decoder->VaDisplay, dst2->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
#ifdef DEBUG
    tick6 = GetMsTicks();
#endif
    if (vaUnmapBuffer(decoder->VaDisplay, dst1->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
#ifdef DEBUG
    tick7 = GetMsTicks();
#endif
    if (vaUnmapBuffer(decoder->VaDisplay, src->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
#ifdef DEBUG
    tick8 = GetMsTicks();

    Debug(4, "video/vaapi: map=%2d/%2d/%2d deint=%2d umap=%2d/%2d/%2d\n",
	tick2 - tick1, tick3 - tick2, tick4 - tick3, tick5 - tick4,
	tick6 - tick5, tick7 - tick6, tick8 - tick7);
#endif
}

///
///	Create software deinterlace images.
///
///	@param decoder		VA-API decoder
///
static void VaapiCreateDeinterlaceImages(VaapiDecoder * decoder)
{
    VAImageFormat format[1];
    int i;

    // NV12, YV12, I420, BGRA
    // NV12 Y U/V 2x2
    // YV12 Y V U 2x2
    // I420 Y U V 2x2

    // Intel needs NV12
    VaapiFindImageFormat(decoder, PIX_FMT_NV12, format);
    //VaapiFindImageFormat(decoder, PIX_FMT_YUV420P, format);
    for (i = 0; i < 5; ++i) {
	if (vaCreateImage(decoder->VaDisplay, format, decoder->InputWidth,
		decoder->InputHeight,
		decoder->DeintImages + i) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create image!\n"));
	}
    }
#ifdef DEBUG
    if (1) {
	VAImage *img;

	img = decoder->DeintImages;
	Debug(3, "video/vaapi: %c%c%c%c %dx%d*%d\n", img->format.fourcc,
	    img->format.fourcc >> 8, img->format.fourcc >> 16,
	    img->format.fourcc >> 24, img->width, img->height,
	    img->num_planes);
    }
#endif
}

///
///	Destroy software deinterlace images.
///
///	@param decoder	VA-API decoder
///
static void VaapiDestroyDeinterlaceImages(VaapiDecoder * decoder)
{
    int i;

    for (i = 0; i < 5; ++i) {
	if (vaDestroyImage(decoder->VaDisplay,
		decoder->DeintImages[i].image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	decoder->DeintImages[i].image_id = VA_INVALID_ID;
    }
}

///
///	Vaapi software deinterlace.
///
///	@param decoder	VA-API decoder
///	@param surface	interlaced hardware surface
///
static void VaapiCpuDerive(VaapiDecoder * decoder, VASurfaceID surface)
{
    //
    //	vaPutImage not working, vaDeriveImage
    //
#ifdef DEBUG
    uint32_t tick1;
    uint32_t tick2;
    uint32_t tick3;
    uint32_t tick4;
    uint32_t tick5;
#endif
    VAImage image[1];
    VAImage dest1[1];
    VAImage dest2[1];
    VAStatus status;
    VASurfaceID out1;
    VASurfaceID out2;

#ifdef DEBUG
    tick1 = GetMsTicks();
#endif
#if 0
    // get image test
    if (decoder->Image->image_id == VA_INVALID_ID) {
	VAImageFormat format[1];

	VaapiFindImageFormat(decoder, PIX_FMT_NV12, format);
	if (vaCreateImage(VaDisplay, format, decoder->InputWidth,
		decoder->InputHeight, decoder->Image) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't create image!\n"));
	}
    }
    if (vaGetImage(decoder->VaDisplay, surface, 0, 0, decoder->InputWidth,
	    decoder->InputHeight, decoder->Image->image_id)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't get source image\n"));
	VaapiQueueSurface(decoder, surface, 0);
	VaapiQueueSurface(decoder, surface, 0);
	return;
    }
    *image = *decoder->Image;
#else
    if ((status =
	    vaDeriveImage(decoder->VaDisplay, surface,
		image)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed %d\n"), status);
	VaapiQueueSurface(decoder, surface, 0);
	VaapiQueueSurface(decoder, surface, 0);
	return;
    }
#endif
#ifdef DEBUG
    tick2 = GetMsTicks();
#endif

    Debug(4, "video/vaapi: %c%c%c%c %dx%d*%d\n", image->format.fourcc,
	image->format.fourcc >> 8, image->format.fourcc >> 16,
	image->format.fourcc >> 24, image->width, image->height,
	image->num_planes);

    // get a free surfaces
    out1 = VaapiGetSurface0(decoder);
    if (out1 == VA_INVALID_ID) {
	abort();
    }
    if ((status =
	    vaDeriveImage(decoder->VaDisplay, out1,
		dest1)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed %d\n"), status);
    }
#ifdef DEBUG
    tick3 = GetMsTicks();
#endif
    out2 = VaapiGetSurface0(decoder);
    if (out2 == VA_INVALID_ID) {
	abort();
    }
    if ((status =
	    vaDeriveImage(decoder->VaDisplay, out2,
		dest2)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaDeriveImage failed %d\n"), status);
    }
#ifdef DEBUG
    tick4 = GetMsTicks();
#endif

    switch (VideoDeinterlace[decoder->Resolution]) {
	case VideoDeinterlaceSoftBob:
	default:
	    VaapiBob(decoder, image, dest1, dest2);
	    break;
	case VideoDeinterlaceSoftSpatial:
	    VaapiSpatial(decoder, image, dest1, dest2);
	    break;
    }
#ifdef DEBUG
    tick5 = GetMsTicks();
#endif

#if 1
    if (vaDestroyImage(VaDisplay, image->image_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy image!\n"));
    }
#endif
    if (vaDestroyImage(VaDisplay, dest1->image_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy image!\n"));
    }
    if (vaDestroyImage(VaDisplay, dest2->image_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy image!\n"));
    }

    VaapiQueueSurface(decoder, out1, 1);
    VaapiQueueSurface(decoder, out2, 1);

#ifdef DEBUG
    tick5 = GetMsTicks();

    Debug(4, "video/vaapi: get=%2d get1=%2d get2=%d deint=%2d\n",
	tick2 - tick1, tick3 - tick2, tick4 - tick3, tick5 - tick4);
#endif
}

///
///	Vaapi software deinterlace.
///
///	@param decoder	VA-API decoder
///	@param surface	interlaced hardware surface
///
static void VaapiCpuPut(VaapiDecoder * decoder, VASurfaceID surface)
{
    //
    //	vaPutImage working
    //
#ifdef DEBUG
    uint32_t tick1;
    uint32_t tick2;
    uint32_t tick3;
    uint32_t tick4;
    uint32_t tick5;
#endif
    VAImage *img1;
    VAImage *img2;
    VAImage *img3;
    VASurfaceID out;
    VAStatus status;

    //
    //	Create deinterlace images.
    //
    if (decoder->DeintImages[0].image_id == VA_INVALID_ID) {
	VaapiCreateDeinterlaceImages(decoder);
    }

    if (0 && vaSyncSurface(decoder->VaDisplay, surface) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }

    img1 = decoder->DeintImages;
    img2 = decoder->DeintImages + 1;
    img3 = decoder->DeintImages + 2;

#ifdef DEBUG
    tick1 = GetMsTicks();
#endif
    if (vaGetImage(decoder->VaDisplay, surface, 0, 0, decoder->InputWidth,
	    decoder->InputHeight, img1->image_id) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't get source image\n"));
	VaapiQueueSurface(decoder, surface, 0);
	VaapiQueueSurface(decoder, surface, 0);
	return;
    }
#ifdef DEBUG
    tick2 = GetMsTicks();
#endif

    // FIXME: handle top_field_first

    switch (VideoDeinterlace[decoder->Resolution]) {
	case VideoDeinterlaceSoftBob:
	default:
	    VaapiBob(decoder, img1, img2, img3);
	    break;
	case VideoDeinterlaceSoftSpatial:
	    VaapiSpatial(decoder, img1, img2, img3);
	    break;
    }
#ifdef DEBUG
    tick3 = GetMsTicks();
#endif

    // get a free surface and upload the image
    out = VaapiGetSurface0(decoder);
    if (out == VA_INVALID_ID) {
	abort();
    }
    if ((status =
	    vaPutImage(VaDisplay, out, img2->image_id, 0, 0, img2->width,
		img2->height, 0, 0, img2->width,
		img2->height)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't put image: %d!\n"), status);
	abort();
    }
    VaapiQueueSurface(decoder, out, 1);
    if (0 && vaSyncSurface(decoder->VaDisplay, out) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }
#ifdef DEBUG
    tick4 = GetMsTicks();

    Debug(4, "video/vaapi: deint %d %#010x -> %#010x\n", decoder->SurfaceField,
	surface, out);
#endif

    // get a free surface and upload the image
    out = VaapiGetSurface0(decoder);
    if (out == VA_INVALID_ID) {
	abort();
    }
    if (vaPutImage(VaDisplay, out, img3->image_id, 0, 0, img3->width,
	    img3->height, 0, 0, img3->width,
	    img3->height) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't put image!\n"));
    }
    VaapiQueueSurface(decoder, out, 1);
    if (0 && vaSyncSurface(decoder->VaDisplay, out) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }
#ifdef DEBUG
    tick5 = GetMsTicks();

    Debug(4, "video/vaapi: get=%2d deint=%2d put1=%2d put2=%2d\n",
	tick2 - tick1, tick3 - tick2, tick4 - tick3, tick5 - tick4);
#endif
}

///
///	Vaapi software deinterlace.
///
///	@param decoder	VA-API decoder
///	@param surface	interlaced hardware surface
///
static void VaapiCpuDeinterlace(VaapiDecoder * decoder, VASurfaceID surface)
{
    if (decoder->GetPutImage) {
	VaapiCpuPut(decoder, surface);
    } else {
	VaapiCpuDerive(decoder, surface);
    }
    // FIXME: must release software input surface
}

///
///	Render a ffmpeg frame
///
///	@param decoder		VA-API decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VaapiRenderFrame(VaapiDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    VASurfaceID surface;
    int interlaced;

    // FIXME: some tv-stations toggle interlace on/off
    // frame->interlaced_frame isn't always correct set
    interlaced = frame->interlaced_frame;
    if (video_ctx->height == 720) {
	if (interlaced && !decoder->WrongInterlacedWarned) {
	    Debug(3, "video/vaapi: wrong interlace flag fixed\n");
	    decoder->WrongInterlacedWarned = 1;
	}
	interlaced = 0;
    } else {
	if (!interlaced && !decoder->WrongInterlacedWarned) {
	    Debug(3, "video/vaapi: wrong interlace flag fixed\n");
	    decoder->WrongInterlacedWarned = 1;
	}
	interlaced = 1;
    }

    // FIXME: should be done by init video_ctx->field_order
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != frame->top_field_first) {

#if 0
	// field_order only in git
	Debug(3, "video/vaapi: interlaced %d top-field-first %d - %d\n",
	    interlaced, frame->top_field_first, video_ctx->field_order);
#else
	Debug(3, "video/vaapi: interlaced %d top-field-first %d\n", interlaced,
	    frame->top_field_first);
#endif

	decoder->Interlaced = interlaced;
	decoder->TopFieldFirst = frame->top_field_first;
	decoder->SurfaceField = 0;
    }
    // update aspect ratio changes
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(53,60,100)
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, frame->sample_aspect_ratio)) {
	Debug(3, "video/vaapi: aspect ratio changed\n");

	decoder->InputAspect = frame->sample_aspect_ratio;
	VaapiUpdateOutput(decoder);
    }
#else
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, video_ctx->sample_aspect_ratio)) {
	Debug(3, "video/vaapi: aspect ratio changed\n");

	decoder->InputAspect = video_ctx->sample_aspect_ratio;
	VaapiUpdateOutput(decoder);
    }
#endif

    //
    // Hardware render
    //
    if (video_ctx->hwaccel_context) {

	if (video_ctx->height != decoder->InputHeight
	    || video_ctx->width != decoder->InputWidth) {
	    Error(_("video/vaapi: stream <-> surface size mismatch\n"));
	    return;
	}

	surface = (unsigned)(size_t) frame->data[3];
	Debug(4, "video/vaapi: hw render hw surface %#010x\n", surface);

	if (interlaced
	    && VideoDeinterlace[decoder->Resolution] >=
	    VideoDeinterlaceSoftBob) {
	    VaapiCpuDeinterlace(decoder, surface);
	} else {
	    VaapiQueueSurface(decoder, surface, 0);
	}

	//
	// VAImage render
	//
    } else {
	void *va_image_data;
	int i;
	AVPicture picture[1];
	int width;
	int height;

	Debug(4, "video/vaapi: hw render sw surface\n");

	width = video_ctx->width;
	height = video_ctx->height;
	//
	//	Check image, format, size
	//
	if ((decoder->GetPutImage && decoder->Image->image_id == VA_INVALID_ID)
	    || decoder->PixFmt != video_ctx->pix_fmt
	    || width != decoder->InputWidth
	    || height != decoder->InputHeight) {

	    Debug(3,
		"video/vaapi: stream <-> surface size/interlace mismatch\n");

	    decoder->PixFmt = video_ctx->pix_fmt;
	    // FIXME: aspect done above!
	    decoder->InputWidth = width;
	    decoder->InputHeight = height;

	    VaapiSetup(decoder, video_ctx);
	}
	// FIXME: Need to insert software deinterlace here
	// FIXME: can/must insert auto-crop here (is done after upload)

	// get a free surface and upload the image
	surface = VaapiGetSurface0(decoder);
	Debug(4, "video/vaapi: video surface %#010x displayed\n", surface);

	if (!decoder->GetPutImage
	    && vaDeriveImage(decoder->VaDisplay, surface,
		decoder->Image) != VA_STATUS_SUCCESS) {
	    VAImageFormat format[1];

	    Error(_("video/vaapi: vaDeriveImage failed\n"));

	    decoder->GetPutImage = 1;
	    VaapiFindImageFormat(decoder, decoder->PixFmt, format);
	    if (vaCreateImage(VaDisplay, format, width, height,
		    decoder->Image) != VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't create image!\n"));
	    }
	}
	//
	//	Copy data from frame to image
	//
	if (vaMapBuffer(VaDisplay, decoder->Image->buf, &va_image_data)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't map the image!\n"));
	}
	// crazy: intel mixes YV12 and NV12 with mpeg
	if (decoder->Image->format.fourcc == VA_FOURCC_NV12) {
	    int x;

	    // intel NV12 convert YV12 to NV12

	    // copy Y
	    for (i = 0; i < height; ++i) {
		memcpy(va_image_data + decoder->Image->offsets[0] +
		    decoder->Image->pitches[0] * i,
		    frame->data[0] + frame->linesize[0] * i,
		    frame->linesize[0]);
	    }
	    // copy UV
	    for (i = 0; i < height / 2; ++i) {
		for (x = 0; x < width / 2; ++x) {
		    ((uint8_t *) va_image_data)[decoder->Image->offsets[1]
			+ decoder->Image->pitches[1] * i + x * 2 + 0]
			= frame->data[1][i * frame->linesize[1] + x];
		    ((uint8_t *) va_image_data)[decoder->Image->offsets[1]
			+ decoder->Image->pitches[1] * i + x * 2 + 1]
			= frame->data[2][i * frame->linesize[2] + x];
		}
	    }
	    // vdpau uses this
	} else if (decoder->Image->format.fourcc == VA_FOURCC('I', '4', '2',
		'0')) {
	    picture->data[0] = va_image_data + decoder->Image->offsets[0];
	    picture->linesize[0] = decoder->Image->pitches[0];
	    picture->data[1] = va_image_data + decoder->Image->offsets[1];
	    picture->linesize[1] = decoder->Image->pitches[2];
	    picture->data[2] = va_image_data + decoder->Image->offsets[2];
	    picture->linesize[2] = decoder->Image->pitches[1];

	    av_picture_copy(picture, (AVPicture *) frame, video_ctx->pix_fmt,
		width, height);
	} else if (decoder->Image->num_planes == 3) {
	    picture->data[0] = va_image_data + decoder->Image->offsets[0];
	    picture->linesize[0] = decoder->Image->pitches[0];
	    picture->data[1] = va_image_data + decoder->Image->offsets[2];
	    picture->linesize[1] = decoder->Image->pitches[2];
	    picture->data[2] = va_image_data + decoder->Image->offsets[1];
	    picture->linesize[2] = decoder->Image->pitches[1];

	    av_picture_copy(picture, (AVPicture *) frame, video_ctx->pix_fmt,
		width, height);
	}

	if (vaUnmapBuffer(VaDisplay, decoder->Image->buf)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't unmap the image!\n"));
	}

	Debug(4, "video/vaapi: buffer %dx%d <- %dx%d\n", decoder->Image->width,
	    decoder->Image->height, width, height);

	if (decoder->GetPutImage
	    && (i =
		vaPutImage(VaDisplay, surface, decoder->Image->image_id, 0, 0,
		    width, height, 0, 0, width,
		    height)) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't put image err:%d!\n"), i);
	}

	if (!decoder->GetPutImage) {
	    if (vaDestroyImage(VaDisplay, decoder->Image->image_id)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: can't destroy image!\n"));
	    }
	    decoder->Image->image_id = VA_INVALID_ID;
	}

	VaapiQueueSurface(decoder, surface, 1);
    }

    if (decoder->Interlaced) {
	++decoder->FrameCounter;
    }
}

///
///	Advance displayed frame of decoder.
///
///	@param decoder	VA-API hw decoder
///
static void VaapiAdvanceDecoderFrame(VaapiDecoder * decoder)
{
    // next surface, if complete frame is displayed (1 -> 0)
    if (decoder->SurfaceField) {
	VASurfaceID surface;
	int filled;

	filled = atomic_read(&decoder->SurfacesFilled);
	// FIXME: this should check the caller
	// check decoder, if new surface is available
	if (filled <= 1) {
	    // keep use of last surface
	    ++decoder->FramesDuped;
	    // FIXME: don't warn after stream start, don't warn during pause
	    Error(_("video: display buffer empty, duping frame (%d/%d) %d\n"),
		decoder->FramesDuped, decoder->FrameCounter,
		VideoGetBuffers(decoder->Stream));
	    return;
	}
	// wait for rendering finished
	surface = decoder->SurfacesRb[decoder->SurfaceRead];
	if (vaSyncSurface(decoder->VaDisplay, surface) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}

	decoder->SurfaceRead = (decoder->SurfaceRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&decoder->SurfacesFilled);

	// progressiv oder software deinterlacer
	decoder->SurfaceField = !decoder->Interlaced
	    || VideoDeinterlace[decoder->Resolution]
	    >= VideoDeinterlaceSoftBob;
	return;
    }
    decoder->SurfaceField = 1;
}

///
///	Display a video frame.
///
///	@todo FIXME: add detection of missed frames
///
static void VaapiDisplayFrame(void)
{
    struct timespec nowtime;

#ifdef DEBUG
    uint32_t start;
    uint32_t put1;
    uint32_t put2;
#endif
    int i;
    VaapiDecoder *decoder;

    if (VideoSurfaceModesChanged) {	// handle changed modes
	VideoSurfaceModesChanged = 0;
	for (i = 0; i < VaapiDecoderN; ++i) {
	    VaapiInitSurfaceFlags(VaapiDecoders[i]);
	}
    }
    // look if any stream have a new surface available
    for (i = 0; i < VaapiDecoderN; ++i) {
	VASurfaceID surface;
	int filled;

	decoder = VaapiDecoders[i];
	decoder->FramesDisplayed++;
	decoder->StartCounter++;

#ifdef VA_EXP
	// wait for display finished
	if (decoder->LastSurface != VA_INVALID_ID) {
	    if (vaSyncSurface(decoder->VaDisplay, decoder->LastSurface)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaSyncSurface failed\n"));
	    }
	}
#endif

	filled = atomic_read(&decoder->SurfacesFilled);
	// no surface availble show black with possible osd
	if (!filled) {
	    VaapiBlackSurface(decoder);
#ifdef VA_EXP
	    decoder->LastSurface = decoder->BlackSurface;
#endif
	    VaapiMessage(3, "video/vaapi: black surface displayed\n");
	    continue;
	}

	surface = decoder->SurfacesRb[decoder->SurfaceRead];
#ifdef VA_EXP
	decoder->LastSurface = surface;
#endif
#ifdef DEBUG
	if (surface == VA_INVALID_ID) {
	    printf(_("video/vaapi: invalid surface in ringbuffer\n"));
	}
	Debug(4, "video/vaapi: yy video surface %#010x displayed\n", surface);

	start = GetMsTicks();
#endif

	// VDPAU driver + INTEL driver does no v-sync with 1080
	if (0 && decoder->Interlaced
	    // FIXME: buggy libva-driver-vdpau, buggy libva-driver-intel
	    && (VaapiBuggyVdpau || (0 && VaapiBuggyIntel
		    && decoder->InputHeight == 1080))
	    && VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceWeave) {
	    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		decoder->TopFieldFirst, 0);
#ifdef DEBUG
	    put1 = GetMsTicks();
#endif

	    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		decoder->TopFieldFirst, 1);
#ifdef DEBUG
	    put2 = GetMsTicks();
#endif
	} else {
	    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		decoder->TopFieldFirst, decoder->SurfaceField);
#ifdef DEBUG
	    put1 = GetMsTicks();
	    put2 = put1;
#endif
	}
	clock_gettime(CLOCK_REALTIME, &nowtime);
	// FIXME: 31 only correct for 50Hz
	if ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
	    * 1000 * 1000 * 1000 + (nowtime.tv_nsec -
		decoder->FrameTime.tv_nsec) > 31 * 1000 * 1000) {
	    // FIXME: ignore still-frame, trick-speed
	    Debug(3, "video/vaapi: time/frame too long %ldms\n",
		((nowtime.tv_sec - decoder->FrameTime.tv_sec)
		    * 1000 * 1000 * 1000 + (nowtime.tv_nsec -
			decoder->FrameTime.tv_nsec)) / (1000 * 1000));
	    Debug(4, "video/vaapi: put1 %2u put2 %2u\n", put1 - start,
		put2 - put1);
	}
#ifdef noDEBUG
	Debug(3, "video/vaapi: time/frame %ldms\n",
	    ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
		* 1000 * 1000 * 1000 + (nowtime.tv_nsec -
		    decoder->FrameTime.tv_nsec)) / (1000 * 1000));
	if (put2 > start + 20) {
	    Debug(3, "video/vaapi: putsurface too long %ums\n", put2 - start);
	}
	Debug(4, "video/vaapi: put1 %2u put2 %2u\n", put1 - start,
	    put2 - put1);
#endif

	decoder->FrameTime = nowtime;
    }
}

///
///	Set VA-API decoder video clock.
///
///	@param decoder	VA-API hardware decoder
///	@param pts	audio presentation timestamp
///
void VaapiSetClock(VaapiDecoder * decoder, int64_t pts)
{
    decoder->PTS = pts;
}

///
///	Get VA-API decoder video clock.
///
///	@param decoder	VA-API decoder
///
static int64_t VaapiGetClock(const VaapiDecoder * decoder)
{
    // pts is the timestamp of the latest decoded frame
    if (decoder->PTS == (int64_t) AV_NOPTS_VALUE) {
	return AV_NOPTS_VALUE;
    }
    // subtract buffered decoded frames
    if (decoder->Interlaced) {
	return decoder->PTS -
	    20 * 90 * (2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField);
    }
    return decoder->PTS - 20 * 90 * (atomic_read(&decoder->SurfacesFilled) +
	2);
}

///
///	Set trick play speed.
///
///	@param decoder		VA-API decoder
///	@param speed		trick speed (0 = normal)
///
static void VaapiSetTrickSpeed(VaapiDecoder * decoder, int speed)
{
    decoder->TrickSpeed = speed;
    decoder->TrickCounter = speed;
    if (speed) {
	decoder->Closing = 0;
    }
}

///
///	Sync decoder output to audio.
///
///	trick-speed	show frame <n> times
///	still-picture	show frame until new frame arrives
///	60hz-mode	repeat every 5th picture
///	video>audio	slow down video by duplicating frames
///	video<audio	speed up video by skipping frames
///	soft-start	show every second frame
///
///	@param decoder	VDPAU hw decoder
///
static void VaapiSyncDecoder(VaapiDecoder * decoder)
{
    int err;
    int filled;
    int64_t audio_clock;
    int64_t video_clock;

    err = 0;
    audio_clock = AudioGetClock();
    video_clock = VaapiGetClock(decoder);
    filled = atomic_read(&decoder->SurfacesFilled);

    // 60Hz: repeat every 5th field
    if (Video60HzMode && !(decoder->FramesDisplayed % 6)) {
	if (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock == (int64_t) AV_NOPTS_VALUE) {
	    goto out;
	}
	// both clocks are known
	if (audio_clock + VideoAudioDelay <= video_clock + 15 * 90) {
	    goto out;
	}
	// out of sync: audio before video
	if (!decoder->TrickSpeed) {
	    goto skip_sync;
	}
    }
    // TrickSpeed
    if (decoder->TrickSpeed) {
	if (decoder->TrickCounter--) {
	    goto out;
	}
	decoder->TrickCounter = decoder->TrickSpeed;
	goto skip_sync;
    }
    // at start of new video stream, soft or hard sync video to audio
    // FIXME: video waits for audio, audio for video
    if (!VideoSoftStartSync && decoder->StartCounter < VideoSoftStartFrames
	&& video_clock != (int64_t) AV_NOPTS_VALUE
	&& (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock > audio_clock + VideoAudioDelay + 120 * 90)) {
	err =
	    VaapiMessage(3, "video: initial slow down video, frame %d\n",
	    decoder->StartCounter);
	goto out;
    }

    if (decoder->SyncCounter && decoder->SyncCounter--) {
	goto skip_sync;
    }

    if (audio_clock != (int64_t) AV_NOPTS_VALUE
	&& video_clock != (int64_t) AV_NOPTS_VALUE) {
	// both clocks are known

	if (abs(video_clock - audio_clock + VideoAudioDelay) > 5000 * 90) {
	    err = VaapiMessage(3, "video: audio/video difference too big\n");
	} else if (video_clock > audio_clock + VideoAudioDelay + 100 * 90) {
	    // FIXME: this quicker sync step, did not work with new code!
	    err = VaapiMessage(3, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    decoder->SyncCounter = 1;
	    goto out;
	} else if (video_clock > audio_clock + VideoAudioDelay + 45 * 90) {
	    err = VaapiMessage(3, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    decoder->SyncCounter = 1;
	    goto out;
	} else if (audio_clock + VideoAudioDelay > video_clock + 15 * 90
	    && filled > 1 + 2 * decoder->Interlaced) {
	    err = VaapiMessage(3, "video: speed up video, droping frame\n");
	    ++decoder->FramesDropped;
	    VaapiAdvanceDecoderFrame(decoder);
	    decoder->SyncCounter = 1;
	}
#if defined(DEBUG) || defined(AV_INFO)
	if (!decoder->SyncCounter && decoder->StartCounter < 1000) {
	    Debug(3, "video/vaapi: synced after %d frames\n",
		decoder->StartCounter);
	    decoder->StartCounter += 1000;
	}
#endif
    }

  skip_sync:
    // check if next field is available
    if (decoder->SurfaceField && filled <= 1) {
	if (filled == 1) {
	    ++decoder->FramesDuped;
	    // FIXME: don't warn after stream start, don't warn during pause
	    err =
		VaapiMessage(1,
		_("video: decoder buffer empty, "
		    "duping frame (%d/%d) %d v-buf\n"), decoder->FramesDuped,
		decoder->FrameCounter, VideoGetBuffers(decoder->Stream));
	    if (decoder->Closing < -300) {
		atomic_set(&decoder->SurfacesFilled, 0);
	    }
	}
	goto out;
    }

    VaapiAdvanceDecoderFrame(decoder);
  out:
#if defined(DEBUG) || defined(AV_INFO)
    // debug audio/video sync
    if (err || !(decoder->FramesDisplayed % AV_INFO_TIME)) {
	if (!err) {
	    VaapiMessage(0, NULL);
	}
	Info("video: %s%+5" PRId64 " %4" PRId64 " %3d/\\ms %3d%+d v-buf\n",
	    Timestamp2String(video_clock),
	    abs((video_clock - audio_clock) / 90) <
	    8888 ? ((video_clock - audio_clock) / 90) : 8888,
	    AudioGetDelay() / 90, (int)VideoDeltaPTS / 90,
	    VideoGetBuffers(decoder->Stream),
	    (1 + decoder->Interlaced) * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField);
	if (!(decoder->FramesDisplayed % (5 * 60 * 60))) {
	    VaapiPrintFrames(decoder);
	}
    }
#endif
    return;				// fix gcc bug!
}

///
///	Sync a video frame.
///
static void VaapiSyncFrame(void)
{
    int i;

    //
    //	Sync video decoder to audio
    //
    for (i = 0; i < VaapiDecoderN; ++i) {
	VaapiSyncDecoder(VaapiDecoders[i]);
    }
}

///
///	Sync and display surface.
///
static void VaapiSyncDisplayFrame(void)
{
    VaapiDisplayFrame();
    VaapiSyncFrame();
}

///
///	Sync and render a ffmpeg frame
///
///	@param decoder		VA-API decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VaapiSyncRenderFrame(VaapiDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
#ifdef DEBUG
    if (!atomic_read(&decoder->SurfacesFilled)) {
	Debug(3, "video: new stream frame %dms\n", GetMsTicks() - VideoSwitch);
    }
#endif

    // if video output buffer is full, wait and display surface.
    // loop for interlace
    while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX - 1) {
	struct timespec abstime;

	pthread_mutex_unlock(&VideoLockMutex);

	abstime = decoder->FrameTime;
	abstime.tv_nsec += 14 * 1000 * 1000;
	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
	    // avoid overflow
	    abstime.tv_sec++;
	    abstime.tv_nsec -= 1000 * 1000 * 1000;
	}

	VideoPollEvent();

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_mutex_lock(&VideoLockMutex);
	// give osd some time slot
	while (pthread_cond_timedwait(&VideoWakeupCond, &VideoLockMutex,
		&abstime) != ETIMEDOUT) {
	    // SIGUSR1
	    Debug(3, "video/vaapi: pthread_cond_timedwait error\n");
	}
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	VaapiSyncDisplayFrame();
    }

    if (!decoder->Closing) {
	VideoSetPts(&decoder->PTS, decoder->Interlaced, frame);
    }
    VaapiRenderFrame(decoder, video_ctx, frame);
#ifdef USE_AUTOCROP
    VaapiCheckAutoCrop(decoder);
#endif
}

///
///	Set VA-API background color.
///
///	@param rgba	32 bit RGBA color.
///
static void VaapiSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
    Error(_("video/vaapi: FIXME: SetBackground not supported\n"));
}

///
///	Set VA-API video mode.
///
static void VaapiSetVideoMode(void)
{
    int i;

    for (i = 0; i < VaapiDecoderN; ++i) {
	// reset video window, upper level needs to fix the positions
	VaapiDecoders[i]->VideoX = 0;
	VaapiDecoders[i]->VideoY = 0;
	VaapiDecoders[i]->VideoWidth = VideoWindowWidth;
	VaapiDecoders[i]->VideoHeight = VideoWindowHeight;
	VaapiUpdateOutput(VaapiDecoders[i]);
    }
}

#ifdef USE_VIDEO_THREAD

///
///	Handle a va-api display.
///
///	@todo FIXME: only a single decoder supported.
///
static void VaapiDisplayHandlerThread(void)
{
    int err;
    int filled;
    struct timespec nowtime;
    VaapiDecoder *decoder;

    if (!(decoder = VaapiDecoders[0])) {	// no stream available
	return;
    }
    //
    // fill frame output ring buffer
    //
    filled = atomic_read(&decoder->SurfacesFilled);
    if (filled < VIDEO_SURFACES_MAX - 1) {
	// FIXME: hot polling
	pthread_mutex_lock(&VideoLockMutex);
	// fetch+decode or reopen
	err = VideoDecodeInput(decoder->Stream);
	pthread_mutex_unlock(&VideoLockMutex);
    } else {
	err = VideoPollInput(decoder->Stream);
    }
    if (err) {
	// FIXME: sleep on wakeup
	usleep(5 * 1000);		// nothing buffered
	if (err == -1 && decoder->Closing) {
	    decoder->Closing--;
	    if (!decoder->Closing) {
		Debug(3, "video/vaapi: closing eof\n");
		decoder->Closing = -1;
	    }
	}
    }

    clock_gettime(CLOCK_REALTIME, &nowtime);
    // time for one frame over?
    if ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
	* 1000 * 1000 * 1000 + (nowtime.tv_nsec - decoder->FrameTime.tv_nsec) <
	15 * 1000 * 1000) {
	return;
    }

    pthread_mutex_lock(&VideoLockMutex);
    VaapiSyncDisplayFrame();
    pthread_mutex_unlock(&VideoLockMutex);
}

#else

#define VaapiDisplayHandlerThread	NULL

#endif

//----------------------------------------------------------------------------
//	VA-API OSD
//----------------------------------------------------------------------------

///
///	Clear subpicture image.
///
///	@note looked by caller
///
static void VaapiOsdClear(void)
{
    void *image_buffer;

    // osd image available?
    if (VaOsdImage.image_id == VA_INVALID_ID) {
	return;
    }

    Debug(3, "video/vaapi: clear image\n");

    // map osd surface/image into memory.
    if (vaMapBuffer(VaDisplay, VaOsdImage.buf, &image_buffer)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't map osd image buffer\n"));
	return;
    }
    // have dirty area.
    if (OsdDirtyWidth && OsdDirtyHeight) {
	int o;

	for (o = 0; o < OsdDirtyHeight; ++o) {
	    memset(image_buffer + (OsdDirtyX + (o +
			OsdDirtyY) * VaOsdImage.width) * 4, 0x00,
		OsdDirtyWidth * 4);
	}
    } else {
	// 100% transparent
	memset(image_buffer, 0x00, VaOsdImage.data_size);
    }

    if (vaUnmapBuffer(VaDisplay, VaOsdImage.buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap osd image buffer\n"));
    }
}

///
///	Upload ARGB to subpicture image.
///
///	@param x	x position of image in osd
///	@param y	y position of image in osd
///	@param width	width of image
///	@param height	height of image
///	@param argb	argb image
///
///	@note looked by caller
///
static void VaapiOsdDrawARGB(int x, int y, int width, int height,
    const uint8_t * argb)
{
#ifdef DEBUG
    uint32_t start;
    uint32_t end;
#endif
    void *image_buffer;
    int o;

    // osd image available?
    if (VaOsdImage.image_id == VA_INVALID_ID) {
	return;
    }
#ifdef DEBUG
    start = GetMsTicks();
#endif
    // map osd surface/image into memory.
    if (vaMapBuffer(VaDisplay, VaOsdImage.buf, &image_buffer)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't map osd image buffer\n"));
	return;
    }
    // FIXME: convert image from ARGB to subpicture format, if not argb

    // copy argb to image
    for (o = 0; o < height; ++o) {
	memcpy(image_buffer + (x + (y + o) * VaOsdImage.width) * 4,
	    argb + o * width * 4, width * 4);
    }

    if (vaUnmapBuffer(VaDisplay, VaOsdImage.buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap osd image buffer\n"));
    }
#ifdef DEBUG
    end = GetMsTicks();

    Debug(3, "video/vaapi: osd upload %dx%d%+d%+d %dms %d\n", width, height, x,
	y, end - start, width * height * 4);
#endif
}

///
///	VA-API initialize OSD.
///
///	@param width	osd width
///	@param height	osd height
///
///	@note subpicture is unusable, it can be scaled with the video image.
///
static void VaapiOsdInit(int width, int height)
{
    VAImageFormat *formats;
    unsigned *flags;
    unsigned format_n;
    unsigned u;
    unsigned v;
    int i;
    static uint32_t wanted_formats[] =
	{ VA_FOURCC('B', 'G', 'R', 'A'), VA_FOURCC_RGBA };

    if (VaOsdImage.image_id != VA_INVALID_ID) {
	Debug(3, "video/vaapi: osd already setup\n");
	return;
    }
    if (!VaDisplay) {
	Debug(3, "video/vaapi: va-api not setup\n");
	return;
    }
    //
    //	look through subpicture formats
    //
    format_n = vaMaxNumSubpictureFormats(VaDisplay);
    formats = alloca(format_n * sizeof(*formats));
    flags = alloca(format_n * sizeof(*formats));
    if (vaQuerySubpictureFormats(VaDisplay, formats, flags,
	    &format_n) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't get subpicture formats"));
	return;
    }
#ifdef DEBUG
    Debug(3, "video/vaapi: supported subpicture formats:\n");
    for (u = 0; u < format_n; ++u) {
	Debug(3, "video/vaapi:\t%c%c%c%c flags %#x %s\n", formats[u].fourcc,
	    formats[u].fourcc >> 8, formats[u].fourcc >> 16,
	    formats[u].fourcc >> 24, flags[u],
	    flags[u] & VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD ?
	    "screen coord" : "");
    }
#endif
    for (v = 0; v < sizeof(wanted_formats) / sizeof(*wanted_formats); ++v) {
	for (u = 0; u < format_n; ++u) {
	    if (formats[u].fourcc == wanted_formats[v]) {
		goto found;
	    }
	}
    }
    Error(_("video/vaapi: can't find a supported subpicture format"));
    return;

  found:
    Debug(3, "video/vaapi: use %c%c%c%c subpicture format with flags %#x\n",
	formats[u].fourcc, formats[u].fourcc >> 8, formats[u].fourcc >> 16,
	formats[u].fourcc >> 24, flags[u]);

    VaapiUnscaledOsd = 0;
    if (flags[u] & VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD) {
	Info(_("video/vaapi: vaapi supports unscaled osd\n"));
	VaapiUnscaledOsd = 1;
    }
    //VaapiUnscaledOsd = 0;
    //Info(_("video/vaapi: unscaled osd disabled\n"));

    if (vaCreateImage(VaDisplay, &formats[u], width, height,
	    &VaOsdImage) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create osd image\n"));
	return;
    }
    if (vaCreateSubpicture(VaDisplay, VaOsdImage.image_id,
	    &VaOsdSubpicture) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create subpicture\n"));

	if (vaDestroyImage(VaDisplay,
		VaOsdImage.image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	VaOsdImage.image_id = VA_INVALID_ID;

	return;
    }
    // FIXME: must store format, to convert ARGB to it.

    // restore osd association
    for (i = 0; i < VaapiDecoderN; ++i) {
	// only if input already setup
	if (VaapiDecoders[i]->InputWidth && VaapiDecoders[i]->InputHeight) {
	    VaapiAssociate(VaapiDecoders[i]);
	}
    }
}

///
///	VA-API cleanup osd.
///
static void VaapiOsdExit(void)
{
    if (VaOsdImage.image_id != VA_INVALID_ID) {
	if (vaDestroyImage(VaDisplay,
		VaOsdImage.image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	VaOsdImage.image_id = VA_INVALID_ID;
    }

    if (VaOsdSubpicture != VA_INVALID_ID) {
	int i;

	for (i = 0; i < VaapiDecoderN; ++i) {
	    VaapiDeassociate(VaapiDecoders[i]);
	}

	if (vaDestroySubpicture(VaDisplay, VaOsdSubpicture)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy subpicture\n"));
	}
	VaOsdSubpicture = VA_INVALID_ID;
    }
}

///
///	VA-API module.
///
static const VideoModule VaapiModule = {
    .Name = "va-api",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) VaapiNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))VaapiDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))VaapiGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))VaapiReleaseSurface,
    .get_format = (enum PixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum PixelFormat *))Vaapi_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))VaapiSyncRenderFrame,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))VaapiSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))VaapiGetClock,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))VaapiSetTrickSpeed,
    .GrabOutput = NULL,
    .SetBackground = VaapiSetBackground,
    .SetVideoMode = VaapiSetVideoMode,
    .ResetAutoCrop = VaapiResetAutoCrop,
    .DisplayHandlerThread = VaapiDisplayHandlerThread,
    .OsdClear = VaapiOsdClear,
    .OsdDrawARGB = VaapiOsdDrawARGB,
    .OsdInit = VaapiOsdInit,
    .OsdExit = VaapiOsdExit,
    .Init = VaapiInit,
    .Exit = VaapiExit,
};

#endif

//----------------------------------------------------------------------------
//	VDPAU
//----------------------------------------------------------------------------

#ifdef USE_VDPAU

///
///	VDPAU decoder
///
typedef struct _vdpau_decoder_
{
    VdpDevice Device;			///< VDPAU device

    xcb_window_t Window;		///< output window

    int VideoX;				///< video base x coordinate
    int VideoY;				///< video base y coordinate
    int VideoWidth;			///< video base width
    int VideoHeight;			///< video base height

    int OutputX;			///< real video output x coordinate
    int OutputY;			///< real video output y coordinate
    int OutputWidth;			///< real video output width
    int OutputHeight;			///< real video output height

    enum PixelFormat PixFmt;		///< ffmpeg frame pixfmt
    int WrongInterlacedWarned;		///< warning about interlace flag issued
    int Interlaced;			///< ffmpeg interlaced flag
    int TopFieldFirst;			///< ffmpeg top field displayed first

    int InputWidth;			///< video input width
    int InputHeight;			///< video input height
    AVRational InputAspect;		///< video input aspect ratio
    VideoResolutions Resolution;	///< resolution group

    int CropX;				///< video crop x
    int CropY;				///< video crop y
    int CropWidth;			///< video crop width
    int CropHeight;			///< video crop height

#ifdef USE_AUTOCROP
    AutoCropCtx AutoCrop[1];		///< auto-crop variables
#endif
#ifdef noyetUSE_GLX
    GLuint GlTexture[2];		///< gl texture for VDPAU
    void *GlxSurface[2];		///< VDPAU/GLX surface
#endif

    VdpDecoderProfile Profile;		///< vdp decoder profile
    VdpDecoder VideoDecoder;		///< vdp video decoder
    VdpVideoMixer VideoMixer;		///< vdp video mixer
    VdpChromaType ChromaType;		///< vdp video surface chroma format
    VdpProcamp Procamp;			///< vdp procamp parameterization data

    int SurfacesNeeded;			///< number of surface to request
    int SurfaceUsedN;			///< number of used video surfaces
    /// used video surface ids
    VdpVideoSurface SurfacesUsed[CODEC_SURFACES_MAX];
    int SurfaceFreeN;			///< number of free video surfaces
    /// free video surface ids
    VdpVideoSurface SurfacesFree[CODEC_SURFACES_MAX];

    /// video surface ring buffer
    VdpVideoSurface SurfacesRb[VIDEO_SURFACES_MAX];
    int SurfaceWrite;			///< write pointer
    int SurfaceRead;			///< read pointer
    atomic_t SurfacesFilled;		///< how many of the buffer is used

    int SurfaceField;			///< current displayed field
    int TrickSpeed;			///< current trick speed
    int TrickCounter;			///< current trick speed counter
    struct timespec FrameTime;		///< time of last display
    VideoStream *Stream;		///< video stream
    int Closing;			///< flag about closing current stream
    int SyncOnAudio;			///< flag sync to audio
    int64_t PTS;			///< video PTS clock

    int SyncCounter;			///< counter to sync frames
    int StartCounter;			///< counter for video start
    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
} VdpauDecoder;

static volatile char VdpauPreemption;	///< flag preemption happened.

static VdpauDecoder *VdpauDecoders[2];	///< open decoder streams
static int VdpauDecoderN;		///< number of decoder streams

static VdpDevice VdpauDevice;		///< VDPAU device
static VdpGetProcAddress *VdpauGetProcAddress;	///< entry point to use

    /// presentation queue target
static VdpPresentationQueueTarget VdpauQueueTarget;
static VdpPresentationQueue VdpauQueue;	///< presentation queue
static VdpColor VdpauQueueBackgroundColor[1];	///< queue background color

static int VdpauBackground;		///< background supported
static int VdpauHqScalingMax;		///< highest supported scaling level
static int VdpauTemporal;		///< temporal deinterlacer supported
static int VdpauTemporalSpatial;	///< temporal spatial deint. supported
static int VdpauInverseTelecine;	///< inverse telecine deint. supported
static int VdpauNoiseReduction;		///< noise reduction supported
static int VdpauSharpness;		///< sharpness supported
static int VdpauSkipChroma;		///< skip chroma deint. supported
static VdpChromaType VdpauChromaType;	///< best video surface chroma format

    /// display surface ring buffer
static VdpOutputSurface VdpauSurfacesRb[OUTPUT_SURFACES_MAX];
static int VdpauSurfaceIndex;		///< current display surface
static struct timespec VdpauFrameTime;	///< time of last display

#ifdef USE_BITMAP
    /// bitmap surfaces for osd
static VdpBitmapSurface VdpauOsdBitmapSurface[2] = {
    VDP_INVALID_HANDLE, VDP_INVALID_HANDLE
};
#else
    /// output surfaces for osd
static VdpOutputSurface VdpauOsdOutputSurface[2] = {
    VDP_INVALID_HANDLE, VDP_INVALID_HANDLE
};
#endif
static int VdpauOsdSurfaceIndex;	///< index into double buffered osd

    /// grab render output surface
static VdpOutputSurface VdpauGrabRenderSurface = VDP_INVALID_HANDLE;
static pthread_mutex_t VdpauGrabMutex;

///
///	Function pointer of the VDPAU device.
///
///@{
static VdpGetErrorString *VdpauGetErrorString;
static VdpDeviceDestroy *VdpauDeviceDestroy;
static VdpGenerateCSCMatrix *VdpauGenerateCSCMatrix;
static VdpVideoSurfaceQueryCapabilities *VdpauVideoSurfaceQueryCapabilities;
static VdpVideoSurfaceQueryGetPutBitsYCbCrCapabilities *
    VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities;
static VdpVideoSurfaceCreate *VdpauVideoSurfaceCreate;
static VdpVideoSurfaceDestroy *VdpauVideoSurfaceDestroy;
static VdpVideoSurfaceGetParameters *VdpauVideoSurfaceGetParameters;
static VdpVideoSurfaceGetBitsYCbCr *VdpauVideoSurfaceGetBitsYCbCr;
static VdpVideoSurfacePutBitsYCbCr *VdpauVideoSurfacePutBitsYCbCr;

static VdpOutputSurfaceQueryCapabilities *VdpauOutputSurfaceQueryCapabilities;

static VdpOutputSurfaceCreate *VdpauOutputSurfaceCreate;
static VdpOutputSurfaceDestroy *VdpauOutputSurfaceDestroy;
static VdpOutputSurfaceGetParameters *VdpauOutputSurfaceGetParameters;
static VdpOutputSurfaceGetBitsNative *VdpauOutputSurfaceGetBitsNative;
static VdpOutputSurfacePutBitsNative *VdpauOutputSurfacePutBitsNative;

static VdpBitmapSurfaceQueryCapabilities *VdpauBitmapSurfaceQueryCapabilities;
static VdpBitmapSurfaceCreate *VdpauBitmapSurfaceCreate;
static VdpBitmapSurfaceDestroy *VdpauBitmapSurfaceDestroy;

static VdpBitmapSurfacePutBitsNative *VdpauBitmapSurfacePutBitsNative;

static VdpOutputSurfaceRenderOutputSurface
    *VdpauOutputSurfaceRenderOutputSurface;
static VdpOutputSurfaceRenderBitmapSurface
    *VdpauOutputSurfaceRenderBitmapSurface;

static VdpDecoderQueryCapabilities *VdpauDecoderQueryCapabilities;
static VdpDecoderCreate *VdpauDecoderCreate;
static VdpDecoderDestroy *VdpauDecoderDestroy;

static VdpDecoderRender *VdpauDecoderRender;

static VdpVideoMixerQueryFeatureSupport *VdpauVideoMixerQueryFeatureSupport;
static VdpVideoMixerQueryAttributeSupport
    *VdpauVideoMixerQueryAttributeSupport;

static VdpVideoMixerCreate *VdpauVideoMixerCreate;
static VdpVideoMixerSetFeatureEnables *VdpauVideoMixerSetFeatureEnables;
static VdpVideoMixerSetAttributeValues *VdpauVideoMixerSetAttributeValues;

static VdpVideoMixerDestroy *VdpauVideoMixerDestroy;
static VdpVideoMixerRender *VdpauVideoMixerRender;
static VdpPresentationQueueTargetDestroy *VdpauPresentationQueueTargetDestroy;
static VdpPresentationQueueCreate *VdpauPresentationQueueCreate;
static VdpPresentationQueueDestroy *VdpauPresentationQueueDestroy;
static VdpPresentationQueueSetBackgroundColor *
    VdpauPresentationQueueSetBackgroundColor;

static VdpPresentationQueueGetTime *VdpauPresentationQueueGetTime;
static VdpPresentationQueueDisplay *VdpauPresentationQueueDisplay;
static VdpPresentationQueueBlockUntilSurfaceIdle
    *VdpauPresentationQueueBlockUntilSurfaceIdle;
static VdpPresentationQueueQuerySurfaceStatus
    *VdpauPresentationQueueQuerySurfaceStatus;
static VdpPreemptionCallbackRegister *VdpauPreemptionCallbackRegister;

static VdpPresentationQueueTargetCreateX11 *
    VdpauPresentationQueueTargetCreateX11;
///@}

static void VdpauOsdInit(int, int);	///< forward definition

//----------------------------------------------------------------------------

///
///	Output video messages.
///
///	Reduce output.
///
///	@param level	message level (Error, Warning, Info, Debug, ...)
///	@param format	printf format string (NULL to flush messages)
///	@param ...	printf arguments
///
///	@returns true, if message shown
///
static int VdpauMessage(int level, const char *format, ...)
{
    if (SysLogLevel > level || DebugLevel > level) {
	static const char *last_format;
	static char buf[256];
	va_list ap;

	va_start(ap, format);
	if (format != last_format) {	// don't repeat same message
	    if (buf[0]) {		// print last repeated message
		syslog(LOG_ERR, "%s", buf);
		buf[0] = '\0';
	    }

	    if (format) {
		last_format = format;
		vsyslog(LOG_ERR, format, ap);
	    }
	    va_end(ap);
	    return 1;
	}
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
    }
    return 0;
}

//	Surfaces -------------------------------------------------------------

///
///	Create surfaces for VDPAU decoder.
///
///	@param decoder	VDPAU hw decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void VdpauCreateSurfaces(VdpauDecoder * decoder, int width, int height)
{
    int i;

#ifdef DEBUG
    if (!decoder->SurfacesNeeded) {
	Error(_("video/vdpau: surface needed not set\n"));
	decoder->SurfacesNeeded = 3 + VIDEO_SURFACES_MAX;
    }
#endif
    Debug(3, "video/vdpau: %s: %dx%d * %d\n", __FUNCTION__, width, height,
	decoder->SurfacesNeeded);

    // allocate only the number of needed surfaces
    decoder->SurfaceFreeN = decoder->SurfacesNeeded;
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	VdpStatus status;

	status =
	    VdpauVideoSurfaceCreate(decoder->Device, decoder->ChromaType,
	    width, height, decoder->SurfacesFree + i);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't create video surface: %s\n"),
		VdpauGetErrorString(status));
	    decoder->SurfacesFree[i] = VDP_INVALID_HANDLE;
	    // FIXME: better error handling
	}
	Debug(4, "video/vdpau: created video surface %dx%d with id 0x%08x\n",
	    width, height, decoder->SurfacesFree[i]);
    }
}

///
///	Destroy surfaces of VDPAU decoder.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauDestroySurfaces(VdpauDecoder * decoder)
{
    int i;
    VdpStatus status;

    Debug(3, "video/vdpau: %s\n", __FUNCTION__);

    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
#ifdef DEBUG
	if (decoder->SurfacesFree[i] == VDP_INVALID_HANDLE) {
	    Debug(3, "video/vdpau: invalid surface\n");
	}
#endif
	Debug(4, "video/vdpau: destroy video surface with id 0x%08x\n",
	    decoder->SurfacesFree[i]);
	status = VdpauVideoSurfaceDestroy(decoder->SurfacesFree[i]);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video surface: %s\n"),
		VdpauGetErrorString(status));
	}
	decoder->SurfacesFree[i] = VDP_INVALID_HANDLE;
    }
    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
#ifdef DEBUG
	if (decoder->SurfacesUsed[i] == VDP_INVALID_HANDLE) {
	    Debug(3, "video/vdpau: invalid surface\n");
	}
#endif
	Debug(4, "video/vdpau: destroy video surface with id 0x%08x\n",
	    decoder->SurfacesUsed[i]);
	status = VdpauVideoSurfaceDestroy(decoder->SurfacesUsed[i]);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video surface: %s\n"),
		VdpauGetErrorString(status));
	}
	decoder->SurfacesUsed[i] = VDP_INVALID_HANDLE;
    }
    decoder->SurfaceFreeN = 0;
    decoder->SurfaceUsedN = 0;
}

///
///	Get a free surface.
///
///	@param decoder	VDPAU hw decoder
///
///	@returns the oldest free surface
///
static unsigned VdpauGetSurface0(VdpauDecoder * decoder)
{
    VdpVideoSurface surface;
    int i;

    if (!decoder->SurfaceFreeN) {
	Error(_("video/vdpau: out of surfaces\n"));

	return VDP_INVALID_HANDLE;
    }
    // use oldest surface
    surface = decoder->SurfacesFree[0];

    decoder->SurfaceFreeN--;
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	decoder->SurfacesFree[i] = decoder->SurfacesFree[i + 1];
    }
    decoder->SurfacesFree[i] = VDP_INVALID_HANDLE;

    // save as used
    decoder->SurfacesUsed[decoder->SurfaceUsedN++] = surface;

    return surface;
}

///
///	Release a surface.
///
///	@param decoder	VDPAU hw decoder
///	@param surface	surface no longer used
///
static void VdpauReleaseSurface(VdpauDecoder * decoder, unsigned surface)
{
    int i;

    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	if (decoder->SurfacesUsed[i] == surface) {
	    // no problem, with last used
	    decoder->SurfacesUsed[i] =
		decoder->SurfacesUsed[--decoder->SurfaceUsedN];
	    decoder->SurfacesFree[decoder->SurfaceFreeN++] = surface;
	    return;
	}
    }
    Error(_("video/vdpau: release surface %#08x, which is not in use\n"),
	surface);
}

///
///	Debug VDPAU decoder frames drop...
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauPrintFrames(const VdpauDecoder * decoder)
{
    Debug(3, "video/vdpau: %d missed, %d duped, %d dropped frames of %d,%d\n",
	decoder->FramesMissed, decoder->FramesDuped, decoder->FramesDropped,
	decoder->FrameCounter, decoder->FramesDisplayed);
#ifndef DEBUG
    (void)decoder;
#endif
}

///
///	Create and setup VDPAU mixer.
///
///	@param decoder	VDPAU hw decoder
///
///	@note don't forget to update features, paramaters, attributes table
///	size, if more is add.
///
static void VdpauMixerSetup(VdpauDecoder * decoder)
{
    VdpStatus status;
    int i;
    VdpVideoMixerFeature features[15];
    VdpBool enables[15];
    int feature_n;
    VdpVideoMixerAttribute attributes[5];
    void const *attribute_value_ptrs[5];
    int attribute_n;
    VdpColor background_color[1];
    uint8_t skip_chroma_value;
    float noise_reduction_level;
    float sharpness_level;
    VdpColorStandard color_standard;
    VdpCSCMatrix csc_matrix;

    //
    //	Build enables table
    //
    feature_n = 0;
    if (VdpauTemporal) {
	enables[feature_n] =
	    (VideoDeinterlace[decoder->Resolution] == VideoDeinterlaceTemporal
	    || VideoDeinterlace[decoder->Resolution] ==
	    VideoDeinterlaceTemporalSpatial) ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL;
	Debug(3, "video/vdpau: temporal deinterlace %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauTemporalSpatial) {
	enables[feature_n] =
	    VideoDeinterlace[decoder->Resolution] ==
	    VideoDeinterlaceTemporalSpatial ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] =
	    VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL;
	Debug(3, "video/vdpau: temporal spatial deinterlace %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauInverseTelecine) {
	enables[feature_n] =
	    VideoInverseTelecine[decoder->Resolution] ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE;
	Debug(3, "video/vdpau: inverse telecine %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauNoiseReduction) {
	enables[feature_n] =
	    VideoDenoise[decoder->Resolution] ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION;
	Debug(3, "video/vdpau: noise reduction %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauSharpness) {
	enables[feature_n] =
	    VideoSharpen[decoder->Resolution] ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_SHARPNESS;
	Debug(3, "video/vdpau: sharpness %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    for (i = VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1;
	i <= VdpauHqScalingMax; ++i) {
	enables[feature_n] =
	    VideoScaling[decoder->Resolution] ==
	    VideoScalingHQ ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = i;
	Debug(3, "video/vdpau: high quality scaling %d %s\n",
	    1 + i - VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1,
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    status =
	VdpauVideoMixerSetFeatureEnables(decoder->VideoMixer, feature_n,
	features, enables);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't set mixer feature enables: %s\n"),
	    VdpauGetErrorString(status));
    }
    //
    //	build attributes table
    //

    /*
       FIXME:
       VDP_VIDEO_MIXER_ATTRIBUTE_LUMA_KEY_MIN_LUMA
       VDP_VIDEO_MIXER_ATTRIBUTE_LUMA_KEY_MAX_LUMA
     */

    attribute_n = 0;
    // none video-area background color
    if (VdpauBackground) {
	background_color->red = (VideoBackground >> 24) / 255.0;
	background_color->green = ((VideoBackground >> 16) & 0xFF) / 255.0;
	background_color->blue = ((VideoBackground >> 8) & 0xFF) / 255.0;
	background_color->alpha = (VideoBackground & 0xFF) / 255.0;
	attributes[attribute_n] = VDP_VIDEO_MIXER_ATTRIBUTE_BACKGROUND_COLOR;
	attribute_value_ptrs[attribute_n++] = background_color;
	Debug(3, "video/vdpau: background color %f/%f/%f/%f\n",
	    background_color->red, background_color->green,
	    background_color->blue, background_color->alpha);
    }
    if (VdpauSkipChroma) {
	skip_chroma_value = VideoSkipChromaDeinterlace[decoder->Resolution];
	attributes[attribute_n]
	    = VDP_VIDEO_MIXER_ATTRIBUTE_SKIP_CHROMA_DEINTERLACE;
	attribute_value_ptrs[attribute_n++] = &skip_chroma_value;
	Debug(3, "video/vdpau: skip chroma deinterlace %s\n",
	    skip_chroma_value ? "enabled" : "disabled");
    }
    if (VdpauNoiseReduction) {
	noise_reduction_level = VideoDenoise[decoder->Resolution] / 1000.0;
	attributes[attribute_n]
	    = VDP_VIDEO_MIXER_ATTRIBUTE_NOISE_REDUCTION_LEVEL;
	attribute_value_ptrs[attribute_n++] = &noise_reduction_level;
	Debug(3, "video/vdpau: noise reduction level %1.3f\n",
	    noise_reduction_level);
    }
    if (VdpauSharpness) {
	sharpness_level = VideoSharpen[decoder->Resolution] / 1000.0;
	attributes[attribute_n]
	    = VDP_VIDEO_MIXER_ATTRIBUTE_SHARPNESS_LEVEL;
	attribute_value_ptrs[attribute_n++] = &sharpness_level;
	Debug(3, "video/vdpau: sharpness level %+1.3f\n", sharpness_level);
    }
    switch (VideoColorSpaces[decoder->Resolution]) {
	case VideoColorSpaceNone:
	default:
	    color_standard = 0;
	    break;
	case VideoColorSpaceBt601:
	    color_standard = VDP_COLOR_STANDARD_ITUR_BT_601;
	    Debug(3, "video/vdpau: color space ITU-R BT.601\n");
	    break;
	case VideoColorSpaceBt709:
	    color_standard = VDP_COLOR_STANDARD_ITUR_BT_709;
	    Debug(3, "video/vdpau: color space ITU-R BT.709\n");
	    break;
	case VideoColorSpaceSmpte240:
	    color_standard = VDP_COLOR_STANDARD_SMPTE_240M;
	    Debug(3, "video/vdpau: color space SMPTE-240M\n");
	    break;
    }
    //
    //	Studio levels
    //
    //	based on www.nvnews.net forum thread
    //
    if (VideoStudioLevels) {
	static const float color_coeffs[][3] = {
	    {0.299, 0.587, 0.114},
	    {0.2125, 0.7154, 0.0721},
	    {0.2122, 0.7013, 0.0865}
	};
	float uvcos, uvsin;
	float uv_coeffs[3][2];
	float Kr, Kg, Kb;
	const int rgbmin = 16;
	const int rgbr = 235 - rgbmin;

	Kr = color_coeffs[color_standard][0];
	Kg = color_coeffs[color_standard][1];
	Kb = color_coeffs[color_standard][2];

	uv_coeffs[0][0] = 0.0;
	uv_coeffs[0][1] = (rgbr / 112.0) * (1.0 - Kr);
	uv_coeffs[1][0] = -(rgbr / 112.0) * (1.0 - Kb) * Kb / Kg;
	uv_coeffs[1][1] = -(rgbr / 112.0) * (1.0 - Kr) * Kr / Kg;
	uv_coeffs[2][0] = (rgbr / 112.0) * (1.0 - Kb);
	uv_coeffs[2][1] = 0.0;

	uvcos = decoder->Procamp.saturation * cos(decoder->Procamp.hue);
	uvsin = decoder->Procamp.saturation * sin(decoder->Procamp.hue);

	for (i = 0; i < 3; ++i) {
	    csc_matrix[i][3] = decoder->Procamp.brightness;
	    csc_matrix[i][0] = rgbr * decoder->Procamp.contrast / 219;
	    csc_matrix[i][3] += (-16 / 255.0) * csc_matrix[i][0];
	    csc_matrix[i][1] =
		uv_coeffs[i][0] * uvcos + uv_coeffs[i][1] * uvsin;
	    csc_matrix[i][3] += (-128 / 255.0) * csc_matrix[i][1];
	    csc_matrix[i][2] =
		uv_coeffs[i][0] * uvsin + uv_coeffs[i][1] * uvcos;
	    csc_matrix[i][3] += (-128 / 255.0) * csc_matrix[i][2];
	    csc_matrix[i][3] += rgbmin / 255.0;
	    csc_matrix[i][3] += 0.5 - decoder->Procamp.contrast / 2.0;
	}
    } else {
	status =
	    VdpauGenerateCSCMatrix(&decoder->Procamp, color_standard,
	    &csc_matrix);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't generate CSC matrix: %s\n"),
		VdpauGetErrorString(status));
	}
    }

    attributes[attribute_n] = VDP_VIDEO_MIXER_ATTRIBUTE_CSC_MATRIX;
    attribute_value_ptrs[attribute_n++] = &csc_matrix;

    status =
	VdpauVideoMixerSetAttributeValues(decoder->VideoMixer, attribute_n,
	attributes, attribute_value_ptrs);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't set mixer attribute values: %s\n"),
	    VdpauGetErrorString(status));
    }
}

///
///	Create and setup VDPAU mixer.
///
///	@param decoder	VDPAU hw decoder
///
///	@note don't forget to update features, paramaters, attributes table
///	size, if more is add.
///
static void VdpauMixerCreate(VdpauDecoder * decoder)
{
    VdpStatus status;
    int i;
    VdpVideoMixerFeature features[15];
    int feature_n;
    VdpVideoMixerParameter paramaters[4];
    void const *value_ptrs[4];
    int parameter_n;
    VdpChromaType chroma_type;
    int layers;

    //
    //	Build feature table
    //
    feature_n = 0;
    if (VdpauTemporal) {
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL;
    }
    if (VdpauTemporalSpatial) {
	features[feature_n++] =
	    VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL;
    }
    if (VdpauInverseTelecine) {
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE;
    }
    if (VdpauNoiseReduction) {
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION;
    }
    if (VdpauSharpness) {
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_SHARPNESS;
    }
    for (i = VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1;
	i <= VdpauHqScalingMax; ++i) {
	features[feature_n++] = i;
    }

    decoder->ChromaType = chroma_type = VdpauChromaType;

    //
    //	Setup parameter/value tables
    //
    paramaters[0] = VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH;
    value_ptrs[0] = &decoder->InputWidth;
    paramaters[1] = VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT;
    value_ptrs[1] = &decoder->InputHeight;
    paramaters[2] = VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE;
    value_ptrs[2] = &chroma_type;
    layers = 0;
    paramaters[3] = VDP_VIDEO_MIXER_PARAMETER_LAYERS;
    value_ptrs[3] = &layers;
    parameter_n = 4;

    status =
	VdpauVideoMixerCreate(VdpauDevice, feature_n, features, parameter_n,
	paramaters, value_ptrs, &decoder->VideoMixer);
    if (status != VDP_STATUS_OK) {
	Fatal(_("video/vdpau: can't create video mixer: %s\n"),
	    VdpauGetErrorString(status));
	// FIXME: no fatal errors
    }

    VdpauMixerSetup(decoder);
}

///
///	Allocate new VDPAU decoder.
///
///	@param stream	video stream
///
///	@returns a new prepared vdpau hardware decoder.
///
static VdpauDecoder *VdpauNewHwDecoder(VideoStream * stream)
{
    VdpauDecoder *decoder;
    int i;

    if ((unsigned)VdpauDecoderN >=
	sizeof(VdpauDecoders) / sizeof(*VdpauDecoders)) {
	Error(_("video/vdpau: out of decoders\n"));
	return NULL;
    }

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
	Error(_("video/vdpau: out of memory\n"));
	return NULL;
    }
    decoder->Device = VdpauDevice;
    decoder->Window = VideoWindow;
    decoder->VideoX = 0;
    decoder->VideoY = 0;
    decoder->VideoWidth = VideoWindowWidth;
    decoder->VideoHeight = VideoWindowHeight;

    decoder->Profile = VDP_INVALID_HANDLE;
    decoder->VideoDecoder = VDP_INVALID_HANDLE;
    decoder->VideoMixer = VDP_INVALID_HANDLE;

    for (i = 0; i < CODEC_SURFACES_MAX; ++i) {
	decoder->SurfacesUsed[i] = VDP_INVALID_HANDLE;
	decoder->SurfacesFree[i] = VDP_INVALID_HANDLE;
    }

    //
    // setup video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VDP_INVALID_HANDLE;
    }

#ifdef DEBUG
    if (VIDEO_SURFACES_MAX < 1 + 1 + 1 + 1) {
	Error(_
	    ("video/vdpau: need 1 future, 1 current, 1 back and 1 work surface\n"));
    }
#endif

    // Procamp operation parameterization data
    decoder->Procamp.struct_version = VDP_PROCAMP_VERSION;
    decoder->Procamp.brightness = 0.0;
    decoder->Procamp.contrast = 1.0;
    decoder->Procamp.saturation = 1.0;
    decoder->Procamp.hue = 0.0;		// default values

    decoder->OutputWidth = VideoWindowWidth;
    decoder->OutputHeight = VideoWindowHeight;

    decoder->Stream = stream;
    if (!VdpauDecoderN) {		// FIXME: hack sync on audio
	decoder->SyncOnAudio = 1;
    }
    decoder->Closing = -300 - 1;

    decoder->PTS = AV_NOPTS_VALUE;

    VdpauDecoders[VdpauDecoderN++] = decoder;

    return decoder;
}

///
///	Cleanup VDPAU.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauCleanup(VdpauDecoder * decoder)
{
    VdpStatus status;
    int i;

    if (decoder->VideoDecoder != VDP_INVALID_HANDLE) {
	// hangs in lock
	status = VdpauDecoderDestroy(decoder->VideoDecoder);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video decoder: %s\n"),
		VdpauGetErrorString(status));
	}
	decoder->VideoDecoder = VDP_INVALID_HANDLE;
	// don't overwrite: decoder->Profile = VDP_INVALID_HANDLE;
    }

    if (decoder->VideoMixer != VDP_INVALID_HANDLE) {
	status = VdpauVideoMixerDestroy(decoder->VideoMixer);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video mixer: %s\n"),
		VdpauGetErrorString(status));
	}
	decoder->VideoMixer = VDP_INVALID_HANDLE;
    }

    if (decoder->SurfaceFreeN || decoder->SurfaceUsedN) {
	VdpauDestroySurfaces(decoder);
    }
    //
    // reset video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VDP_INVALID_HANDLE;
    }
    decoder->SurfaceRead = 0;
    decoder->SurfaceWrite = 0;
    decoder->SurfaceField = 0;

    decoder->SyncCounter = 0;
    decoder->FrameCounter = 0;
    decoder->FramesDisplayed = 0;
    decoder->StartCounter = 0;
    decoder->Closing = 0;
    decoder->PTS = AV_NOPTS_VALUE;
    VideoDeltaPTS = 0;
}

///
///	Destroy a VDPAU decoder.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauDelHwDecoder(VdpauDecoder * decoder)
{
    int i;

    for (i = 0; i < VdpauDecoderN; ++i) {
	if (VdpauDecoders[i] == decoder) {
	    VdpauDecoders[i] = NULL;
	    // copy last slot into empty slot
	    if (i < --VdpauDecoderN) {
		VdpauDecoders[i] = VdpauDecoders[VdpauDecoderN];
	    }

	    VdpauCleanup(decoder);
	    VdpauPrintFrames(decoder);
	    free(decoder);

	    return;
	}
    }
    Error(_("video/vdpau: decoder not in decoder list.\n"));
}

///
///	Get the proc address.
///
///	@param id		VDP function id
///	@param[out] addr	address of VDP function
///	@param name		name of function for error message
///
static inline void VdpauGetProc(const VdpFuncId id, void *addr,
    const char *name)
{
    VdpStatus status;

    status = VdpauGetProcAddress(VdpauDevice, id, addr);
    if (status != VDP_STATUS_OK) {
	Fatal(_("video/vdpau: Can't get function address of '%s': %s\n"), name,
	    VdpauGetErrorString(status));
	// FIXME: rewrite none fatal
    }
}

///
///	Initialize output queue.
///
static void VdpauInitOutputQueue(void)
{
    VdpStatus status;
    VdpRGBAFormat format;
    int i;

    status =
	VdpauPresentationQueueTargetCreateX11(VdpauDevice, VideoWindow,
	&VdpauQueueTarget);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't create presentation queue target: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }

    status =
	VdpauPresentationQueueCreate(VdpauDevice, VdpauQueueTarget,
	&VdpauQueue);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't create presentation queue: %s\n"),
	    VdpauGetErrorString(status));
	VdpauPresentationQueueTargetDestroy(VdpauQueueTarget);
	VdpauQueueTarget = 0;
	return;
    }

    VdpauQueueBackgroundColor->red = 0.01;
    VdpauQueueBackgroundColor->green = 0.02;
    VdpauQueueBackgroundColor->blue = 0.03;
    VdpauQueueBackgroundColor->alpha = 1.00;
    VdpauPresentationQueueSetBackgroundColor(VdpauQueue,
	VdpauQueueBackgroundColor);

    //
    //	Create display output surfaces
    //
    format = VDP_RGBA_FORMAT_B8G8R8A8;
    // FIXME: does a 10bit rgba produce a better output?
    // format = VDP_RGBA_FORMAT_R10G10B10A2;
    for (i = 0; i < OUTPUT_SURFACES_MAX; ++i) {
	status =
	    VdpauOutputSurfaceCreate(VdpauDevice, format, VideoWindowWidth,
	    VideoWindowHeight, VdpauSurfacesRb + i);
	if (status != VDP_STATUS_OK) {
	    Fatal(_("video/vdpau: can't create output surface: %s\n"),
		VdpauGetErrorString(status));
	}
	Debug(3, "video/vdpau: created output surface %dx%d with id 0x%08x\n",
	    VideoWindowWidth, VideoWindowHeight, VdpauSurfacesRb[i]);
    }

    //
    //	 Create render output surface for grabbing
    //
    status =
	VdpauOutputSurfaceCreate(VdpauDevice, format, VideoWindowWidth,
	VideoWindowHeight, &VdpauGrabRenderSurface);
    if (status != VDP_STATUS_OK) {
	Fatal(_("video/vdpau: can't create grab render output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    Debug(3,
	"video/vdpau: created grab render output surface %dx%d with id 0x%08x\n",
	VideoWindowWidth, VideoWindowHeight, VdpauGrabRenderSurface);
}

///
///	Cleanup output queue.
///
static void VdpauExitOutputQueue(void)
{
    int i;
    VdpStatus status;

    //
    //	destroy display output surfaces
    //
    for (i = 0; i < OUTPUT_SURFACES_MAX; ++i) {
	Debug(4, "video/vdpau: destroy output surface with id 0x%08x\n",
	    VdpauSurfacesRb[i]);
	if (VdpauSurfacesRb[i] != VDP_INVALID_HANDLE) {
	    status = VdpauOutputSurfaceDestroy(VdpauSurfacesRb[i]);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't destroy output surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    VdpauSurfacesRb[i] = VDP_INVALID_HANDLE;
	}
    }
    if (VdpauGrabRenderSurface != VDP_INVALID_HANDLE) {
	status = VdpauOutputSurfaceDestroy(VdpauGrabRenderSurface);
	if (status != VDP_STATUS_OK) {
	    Error(_
		("video/vdpau: can't destroy grab render output surface: %s\n"),
		VdpauGetErrorString(status));
	}
	VdpauGrabRenderSurface = VDP_INVALID_HANDLE;
    }
    if (VdpauQueue) {
	VdpauPresentationQueueDestroy(VdpauQueue);
	VdpauQueue = 0;
    }
    if (VdpauQueueTarget) {
	VdpauPresentationQueueTargetDestroy(VdpauQueueTarget);
	VdpauQueueTarget = 0;
    }
}

///
///	Display preemption callback.
///
///	@param device	device that had its display preempted
///	@param context	client-supplied callback context
///
static void VdpauPreemptionCallback(VdpDevice device, __attribute__ ((unused))
    void *context)
{
    Debug(3, "video/vdpau: display preemption\n");
    if (device != VdpauDevice) {
	Error(_("video/vdpau preemption device not our device\n"));
	return;
    }
    VdpauPreemption = 1;		// set flag for video thread
}

///
///	VDPAU setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns true if VDPAU could be initialized, false otherwise.
///
static int VdpauInit(const char *display_name)
{
    VdpStatus status;
    VdpGetApiVersion *get_api_version;
    uint32_t api_version;
    VdpGetInformationString *get_information_string;
    const char *information_string;
    int i;
    VdpBool flag;
    uint32_t max_width;
    uint32_t max_height;

    pthread_mutex_init(&VdpauGrabMutex, NULL);

    status =
	vdp_device_create_x11(XlibDisplay, DefaultScreen(XlibDisplay),
	&VdpauDevice, &VdpauGetProcAddress);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: Can't create vdp device on display '%s'\n"),
	    display_name);
	return 0;
    }
    // get error function first, for better error messages
    status =
	VdpauGetProcAddress(VdpauDevice, VDP_FUNC_ID_GET_ERROR_STRING,
	(void **)&VdpauGetErrorString);
    if (status != VDP_STATUS_OK) {
	Error(_
	    ("video/vdpau: Can't get function address of 'GetErrorString'\n"));
	// FIXME: destroy_x11 VdpauDeviceDestroy
	return 0;
    }
    // get destroy device next, for cleaning up
    VdpauGetProc(VDP_FUNC_ID_DEVICE_DESTROY, &VdpauDeviceDestroy,
	"DeviceDestroy");

    // get version
    VdpauGetProc(VDP_FUNC_ID_GET_API_VERSION, &get_api_version,
	"GetApiVersion");
    VdpauGetProc(VDP_FUNC_ID_GET_INFORMATION_STRING, &get_information_string,
	"VdpauGetProc");
    status = get_api_version(&api_version);
    // FIXME: check status
    status = get_information_string(&information_string);
    // FIXME: check status

    Info(_("video/vdpau: VDPAU API version: %u\n"), api_version);
    Info(_("video/vdpau: VDPAU information: %s\n"), information_string);

    // FIXME: check if needed capabilities are available

    VdpauGetProc(VDP_FUNC_ID_GENERATE_CSC_MATRIX, &VdpauGenerateCSCMatrix,
	"GenerateCSCMatrix");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_QUERY_CAPABILITIES,
	&VdpauVideoSurfaceQueryCapabilities, "VideoSurfaceQueryCapabilities");
    VdpauGetProc
	(VDP_FUNC_ID_VIDEO_SURFACE_QUERY_GET_PUT_BITS_Y_CB_CR_CAPABILITIES,
	&VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities,
	"VideoSurfaceQueryGetPutBitsYCbCrCapabilities");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_CREATE, &VdpauVideoSurfaceCreate,
	"VideoSurfaceCreate");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_DESTROY, &VdpauVideoSurfaceDestroy,
	"VideoSurfaceDestroy");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_GET_PARAMETERS,
	&VdpauVideoSurfaceGetParameters, "VideoSurfaceGetParameters");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_GET_BITS_Y_CB_CR,
	&VdpauVideoSurfaceGetBitsYCbCr, "VideoSurfaceGetBitsYCbCr");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_PUT_BITS_Y_CB_CR,
	&VdpauVideoSurfacePutBitsYCbCr, "VideoSurfacePutBitsYCbCr");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_CAPABILITIES,
	&VdpauOutputSurfaceQueryCapabilities,
	"OutputSurfaceQueryCapabilities");
#if 0
    VdpauGetProc
	(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_GET_PUT_BITS_NATIVE_CAPABILITIES, &,
	"");
    VdpauGetProc
	(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_PUT_BITS_INDEXED_CAPABILITIES, &,
	"");
    VdpauGetProc
	(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_PUT_BITS_Y_CB_CR_CAPABILITIES, &,
	"");
#endif
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_CREATE, &VdpauOutputSurfaceCreate,
	"OutputSurfaceCreate");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_DESTROY,
	&VdpauOutputSurfaceDestroy, "OutputSurfaceDestroy");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_GET_PARAMETERS,
	&VdpauOutputSurfaceGetParameters, "OutputSurfaceGetParameters");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_GET_BITS_NATIVE,
	&VdpauOutputSurfaceGetBitsNative, "OutputSurfaceGetBitsNative");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_NATIVE,
	&VdpauOutputSurfacePutBitsNative, "OutputSurfacePutBitsNative");
#if 0
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_INDEXED, &, "");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_Y_CB_CR, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_QUERY_CAPABILITIES,
	&VdpauBitmapSurfaceQueryCapabilities,
	"BitmapSurfaceQueryCapabilities");
    VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_CREATE, &VdpauBitmapSurfaceCreate,
	"BitmapSurfaceCreate");
    VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_DESTROY,
	&VdpauBitmapSurfaceDestroy, "BitmapSurfaceDestroy");
    // VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_GET_PARAMETERS, &VdpauBitmapSurfaceGetParameters, "BitmapSurfaceGetParameters");
    VdpauGetProc(VDP_FUNC_ID_BITMAP_SURFACE_PUT_BITS_NATIVE,
	&VdpauBitmapSurfacePutBitsNative, "BitmapSurfacePutBitsNative");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_OUTPUT_SURFACE,
	&VdpauOutputSurfaceRenderOutputSurface,
	"OutputSurfaceRenderOutputSurface");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_BITMAP_SURFACE,
	&VdpauOutputSurfaceRenderBitmapSurface,
	"OutputSurfaceRenderBitmapSurface");
#if 0
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_VIDEO_SURFACE_LUMA, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_DECODER_QUERY_CAPABILITIES,
	&VdpauDecoderQueryCapabilities, "DecoderQueryCapabilities");
    VdpauGetProc(VDP_FUNC_ID_DECODER_CREATE, &VdpauDecoderCreate,
	"DecoderCreate");
    VdpauGetProc(VDP_FUNC_ID_DECODER_DESTROY, &VdpauDecoderDestroy,
	"DecoderDestroy");
#if 0
    VdpauGetProc(VDP_FUNC_ID_DECODER_GET_PARAMETERS,
	&VdpauDecoderGetParameters, "DecoderGetParameters");
#endif
    VdpauGetProc(VDP_FUNC_ID_DECODER_RENDER, &VdpauDecoderRender,
	"DecoderRender");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_FEATURE_SUPPORT,
	&VdpauVideoMixerQueryFeatureSupport, "VideoMixerQueryFeatureSupport");
#if 0
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_PARAMETER_SUPPORT, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_ATTRIBUTE_SUPPORT,
	&VdpauVideoMixerQueryAttributeSupport,
	"VideoMixerQueryAttributeSupport");
#if 0
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_PARAMETER_VALUE_RANGE, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_ATTRIBUTE_VALUE_RANGE, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_CREATE, &VdpauVideoMixerCreate,
	"VideoMixerCreate");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_SET_FEATURE_ENABLES,
	&VdpauVideoMixerSetFeatureEnables, "VideoMixerSetFeatureEnables");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_SET_ATTRIBUTE_VALUES,
	&VdpauVideoMixerSetAttributeValues, "VideoMixerSetAttributeValues");
#if 0
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_GET_FEATURE_SUPPORT, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_GET_FEATURE_ENABLES, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_GET_PARAMETER_VALUES, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_GET_ATTRIBUTE_VALUES, &, "");
#endif
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_DESTROY, &VdpauVideoMixerDestroy,
	"VideoMixerDestroy");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_RENDER, &VdpauVideoMixerRender,
	"VideoMixerRender");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_DESTROY,
	&VdpauPresentationQueueTargetDestroy,
	"PresentationQueueTargetDestroy");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_CREATE,
	&VdpauPresentationQueueCreate, "PresentationQueueCreate");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_DESTROY,
	&VdpauPresentationQueueDestroy, "PresentationQueueDestroy");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_SET_BACKGROUND_COLOR,
	&VdpauPresentationQueueSetBackgroundColor,
	"PresentationQueueSetBackgroundColor");
#if 0
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_GET_BACKGROUND_COLOR,
	&VdpauPresentationQueueGetBackgroundColor,
	"PresentationQueueGetBackgroundColor");
#endif
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_GET_TIME,
	&VdpauPresentationQueueGetTime, "PresentationQueueGetTime");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_DISPLAY,
	&VdpauPresentationQueueDisplay, "PresentationQueueDisplay");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_BLOCK_UNTIL_SURFACE_IDLE,
	&VdpauPresentationQueueBlockUntilSurfaceIdle,
	"PresentationQueueBlockUntilSurfaceIdle");
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_QUERY_SURFACE_STATUS,
	&VdpauPresentationQueueQuerySurfaceStatus,
	"PresentationQueueQuerySurfaceStatus");
    VdpauGetProc(VDP_FUNC_ID_PREEMPTION_CALLBACK_REGISTER,
	&VdpauPreemptionCallbackRegister, "PreemptionCallbackRegister");

    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_CREATE_X11,
	&VdpauPresentationQueueTargetCreateX11,
	"PresentationQueueTargetCreateX11");

    status =
	VdpauPreemptionCallbackRegister(VdpauDevice, VdpauPreemptionCallback,
	NULL);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't register preemption callback: %s\n"),
	    VdpauGetErrorString(status));
    }
    //
    //	Look which levels of high quality scaling are supported
    //
    for (i = 0; i < 9; ++i) {
	status =
	    VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	    VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + i, &flag);
	if (status != VDP_STATUS_OK) {
	    Warning(_("video/vdpau: can't query feature '%s': %s\n"),
		"high-quality-scaling", VdpauGetErrorString(status));
	    break;
	}
	if (!flag) {
	    break;
	}
	VdpauHqScalingMax =
	    VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + i;
    }

    //
    //	Cache some features
    //
    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_ATTRIBUTE_BACKGROUND_COLOR, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "background-color", VdpauGetErrorString(status));
    } else {
	VdpauBackground = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "deinterlace-temporal", VdpauGetErrorString(status));
    } else {
	VdpauTemporal = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "deinterlace-temporal-spatial", VdpauGetErrorString(status));
    } else {
	VdpauTemporalSpatial = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "inverse-telecine", VdpauGetErrorString(status));
    } else {
	VdpauInverseTelecine = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "noise-reduction", VdpauGetErrorString(status));
    } else {
	VdpauNoiseReduction = flag;
    }

    status =
	VdpauVideoMixerQueryFeatureSupport(VdpauDevice,
	VDP_VIDEO_MIXER_FEATURE_SHARPNESS, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"), "sharpness",
	    VdpauGetErrorString(status));
    } else {
	VdpauSharpness = flag;
    }

    status =
	VdpauVideoMixerQueryAttributeSupport(VdpauDevice,
	VDP_VIDEO_MIXER_ATTRIBUTE_SKIP_CHROMA_DEINTERLACE, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "skip-chroma-deinterlace", VdpauGetErrorString(status));
    } else {
	VdpauSkipChroma = flag;
    }

    if (VdpauHqScalingMax) {
	Info(_("video/vdpau: highest supported high quality scaling %d\n"),
	    VdpauHqScalingMax -
	    VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + 1);
    } else {
	Info(_("video/vdpau: high quality scaling unsupported\n"));
    }
    Info(_("video/vdpau: feature deinterlace temporal %s\n"),
	VdpauTemporal ? _("supported") : _("unsupported"));
    Info(_("video/vdpau: feature deinterlace temporal spatial %s\n"),
	VdpauTemporalSpatial ? _("supported") : _("unsupported"));
    Info(_("video/vdpau: attribute skip chroma deinterlace %s\n"),
	VdpauSkipChroma ? _("supported") : _("unsupported"));

    //
    //	video formats
    //
    flag = VDP_FALSE;
    status =
	VdpauVideoSurfaceQueryCapabilities(VdpauDevice, VDP_CHROMA_TYPE_420,
	&flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query video surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 4:2:0 chroma format with %dx%d supported\n"),
	    max_width, max_height);
	VdpauChromaType = VDP_CHROMA_TYPE_420;
    }
    flag = VDP_FALSE;
    status =
	VdpauVideoSurfaceQueryCapabilities(VdpauDevice, VDP_CHROMA_TYPE_422,
	&flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query video surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 4:2:2 chroma format with %dx%d supported\n"),
	    max_width, max_height);
	VdpauChromaType = VDP_CHROMA_TYPE_422;
    }
    flag = VDP_FALSE;
    status =
	VdpauVideoSurfaceQueryCapabilities(VdpauDevice, VDP_CHROMA_TYPE_444,
	&flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query video surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 4:4:4 chroma format with %dx%d supported\n"),
	    max_width, max_height);
	VdpauChromaType = VDP_CHROMA_TYPE_444;
    }
    // FIXME: check if all chroma-types failed
    // FIXME: vdpau didn't support decode of other chroma types
    VdpauChromaType = VDP_CHROMA_TYPE_420;

    // FIXME: does only check for chroma formats, but no action
    status =
	VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities(VdpauDevice,
	VDP_CHROMA_TYPE_422, VDP_YCBCR_FORMAT_YUYV, &flag);
    if (status != VDP_STATUS_OK || !flag) {
	Error(_("video/vdpau: doesn't support yuvy video surface\n"));
    }
    status =
	VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities(VdpauDevice,
	VDP_CHROMA_TYPE_420, VDP_YCBCR_FORMAT_YV12, &flag);
    if (status != VDP_STATUS_OK || !flag) {
	Error(_("video/vdpau: doesn't support yv12 video surface\n"));
    }

    flag = VDP_FALSE;
    status =
	VdpauOutputSurfaceQueryCapabilities(VdpauDevice,
	VDP_RGBA_FORMAT_B8G8R8A8, &flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 8bit BGRA format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauOutputSurfaceQueryCapabilities(VdpauDevice,
	VDP_RGBA_FORMAT_R8G8B8A8, &flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 8bit RGBA format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauOutputSurfaceQueryCapabilities(VdpauDevice,
	VDP_RGBA_FORMAT_R10G10B10A2, &flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 10bit RGBA format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauOutputSurfaceQueryCapabilities(VdpauDevice,
	VDP_RGBA_FORMAT_B10G10R10A2, &flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 8bit BRGA format with %dx%d supported\n"),
	    max_width, max_height);
    }
    // FIXME: does only check for rgba formats, but no action

    // FIXME: what if preemption happens during setup?

    //
    //	Create presentation queue, only one queue pro window
    //
    VdpauInitOutputQueue();

    return 1;
}

///
///	VDPAU cleanup.
///
static void VdpauExit(void)
{
    int i;

    for (i = 0; i < VdpauDecoderN; ++i) {
	if (VdpauDecoders[i]) {
	    VdpauDelHwDecoder(VdpauDecoders[i]);
	    VdpauDecoders[i] = NULL;
	}
    }
    VdpauDecoderN = 0;

    if (VdpauDevice) {
	VdpauExitOutputQueue();

	// FIXME: more VDPAU cleanups...

	if (VdpauDeviceDestroy) {
	    VdpauDeviceDestroy(VdpauDevice);
	}
	VdpauDevice = 0;
    }

    pthread_mutex_destroy(&VdpauGrabMutex);
}

///
///	Update output for new size or aspect ratio.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauUpdateOutput(VdpauDecoder * decoder)
{
    VideoUpdateOutput(decoder->InputAspect, decoder->InputWidth,
	decoder->InputHeight, decoder->Resolution, decoder->VideoX,
	decoder->VideoY, decoder->VideoWidth, decoder->VideoHeight,
	&decoder->OutputX, &decoder->OutputY, &decoder->OutputWidth,
	&decoder->OutputHeight, &decoder->CropX, &decoder->CropY,
	&decoder->CropWidth, &decoder->CropHeight);
#ifdef USE_AUTOCROP
    decoder->AutoCrop->State = 0;
    decoder->AutoCrop->Count = AutoCropDelay;
#endif
}

///
///	Configure VDPAU for new video format.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauSetupOutput(VdpauDecoder * decoder)
{
    VdpStatus status;
    VdpChromaType chroma_type;
    uint32_t width;
    uint32_t height;

    // FIXME: need only to create and destroy surfaces for size changes
    //		or when number of needed surfaces changed!
    decoder->Resolution =
	VideoResolutionGroup(decoder->InputWidth, decoder->InputHeight,
	decoder->Interlaced);
    VdpauCreateSurfaces(decoder, decoder->InputWidth, decoder->InputHeight);

    VdpauMixerCreate(decoder);

    VdpauUpdateOutput(decoder);		// update aspect/scaling

    //	get real surface size
    status =
	VdpauVideoSurfaceGetParameters(decoder->SurfacesFree[0], &chroma_type,
	&width, &height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface parameters: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }
    // vdpau can choose different sizes, must use them for putbits
    if (chroma_type != decoder->ChromaType) {
	// I request 422 if supported, but get only 420
	Warning(_("video/vdpau: video surface chroma type mismatch\n"));
    }
    if (width != (uint32_t) decoder->InputWidth
	|| height != (uint32_t) decoder->InputHeight) {
	// FIXME: must rewrite the code to support this case
	Fatal(_("video/vdpau: video surface size mismatch\n"));
    }
}

///
///	Get a free surface.  Called from ffmpeg.
///
///	@param decoder		VDPAU hw decoder
///	@param video_ctx	ffmpeg video codec context
///
///	@returns the oldest free surface
///
static unsigned VdpauGetSurface(VdpauDecoder * decoder,
    const AVCodecContext * video_ctx)
{
    // get_format not called with valid informations.
    if (video_ctx->width != decoder->InputWidth
	|| video_ctx->height != decoder->InputHeight) {
	VdpStatus status;

	VdpauCleanup(decoder);

	status =
	    VdpauDecoderCreate(VdpauDevice, decoder->Profile, video_ctx->width,
	    video_ctx->height,
	    decoder->SurfacesNeeded - VIDEO_SURFACES_MAX - 1,
	    &decoder->VideoDecoder);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't create decoder: %s\n"),
		VdpauGetErrorString(status));
	}

	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;

	VdpauSetupOutput(decoder);
    }
    return VdpauGetSurface0(decoder);
}

///
///	Check profile supported.
///
///	@param decoder		VDPAU hw decoder
///	@param profile		VDPAU profile requested
///
static VdpDecoderProfile VdpauCheckProfile(VdpauDecoder * decoder,
    VdpDecoderProfile profile)
{
    VdpStatus status;
    VdpBool is_supported;
    uint32_t max_level;
    uint32_t max_macroblocks;
    uint32_t max_width;
    uint32_t max_height;

    status =
	VdpauDecoderQueryCapabilities(decoder->Device, profile, &is_supported,
	&max_level, &max_macroblocks, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query decoder capabilities: %s\n"),
	    VdpauGetErrorString(status));
	return VDP_INVALID_HANDLE;
    }
    Debug(3,
	"video/vdpau: profile %d with level %d, macro blocks %d, width %d, height %d %ssupported\n",
	profile, max_level, max_macroblocks, max_width, max_height,
	is_supported ? "" : "not ");
    return is_supported ? profile : VDP_INVALID_HANDLE;
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param fmt	is the list of formats which are supported by the codec,
///			it is terminated by -1 as 0 is a valid format, the
///			formats are ordered by quality.
///
static enum PixelFormat Vdpau_get_format(VdpauDecoder * decoder,
    AVCodecContext * video_ctx, const enum PixelFormat *fmt)
{
    const enum PixelFormat *fmt_idx;
    VdpDecoderProfile profile;
    int max_refs;

    Debug(3, "video: new stream format %dms\n", GetMsTicks() - VideoSwitch);

    if (!VideoHardwareDecoder || (video_ctx->codec_id == CODEC_ID_MPEG2VIDEO
	    && VideoHardwareDecoder == 1)
	) {				// hardware disabled by config
	Debug(3, "codec: hardware acceleration disabled\n");
	goto slow_path;
    }
    //
    //	look through formats
    //
    Debug(3, "%s: codec %d fmts:\n", __FUNCTION__, video_ctx->codec_id);
    for (fmt_idx = fmt; *fmt_idx != PIX_FMT_NONE; fmt_idx++) {
	Debug(3, "\t%#010x %s\n", *fmt_idx, av_get_pix_fmt_name(*fmt_idx));
	// check supported pixel format with entry point
	switch (*fmt_idx) {
	    case PIX_FMT_VDPAU_H264:
	    case PIX_FMT_VDPAU_MPEG1:
	    case PIX_FMT_VDPAU_MPEG2:
	    case PIX_FMT_VDPAU_WMV3:
	    case PIX_FMT_VDPAU_VC1:
	    case PIX_FMT_VDPAU_MPEG4:
		break;
	    default:
		continue;
	}
	break;
    }

    if (*fmt_idx == PIX_FMT_NONE) {
	Error(_("video/vdpau: no valid vdpau pixfmt found\n"));
	goto slow_path;
    }

    max_refs = CODEC_SURFACES_DEFAULT;
    // check profile
    switch (video_ctx->codec_id) {
	case CODEC_ID_MPEG1VIDEO:
	    max_refs = CODEC_SURFACES_MPEG2;
	    profile = VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_MPEG1);
	    break;
	case CODEC_ID_MPEG2VIDEO:
	    max_refs = CODEC_SURFACES_MPEG2;
	    profile =
		VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_MPEG2_MAIN);
	    break;
	case CODEC_ID_MPEG4:
	case CODEC_ID_H263:
	    /*
	       p = VaapiFindProfile(profiles, profile_n,
	       VAProfileMPEG4AdvancedSimple);
	     */
	    goto slow_path;
	case CODEC_ID_H264:
	    // FIXME: can calculate level 4.1 limits
	    // vdpau supports only 16 references
	    max_refs = 16;
	    // try more simple formats, fallback to better
	    if (video_ctx->profile == FF_PROFILE_H264_BASELINE) {
		profile =
		    VdpauCheckProfile(decoder,
		    VDP_DECODER_PROFILE_H264_BASELINE);
		if (profile == VDP_INVALID_HANDLE) {
		    profile =
			VdpauCheckProfile(decoder,
			VDP_DECODER_PROFILE_H264_MAIN);
		}
		if (profile == VDP_INVALID_HANDLE) {
		    profile =
			VdpauCheckProfile(decoder,
			VDP_DECODER_PROFILE_H264_HIGH);
		}
	    } else if (video_ctx->profile == FF_PROFILE_H264_MAIN) {
		profile =
		    VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_H264_MAIN);
		if (profile == VDP_INVALID_HANDLE) {
		    profile =
			VdpauCheckProfile(decoder,
			VDP_DECODER_PROFILE_H264_HIGH);
		}
	    } else {
		profile =
		    VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_H264_MAIN);
	    }
	    break;
	case CODEC_ID_WMV3:
	    /*
	       p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Main);
	     */
	    goto slow_path;
	case CODEC_ID_VC1:
	    /*
	       p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Advanced);
	     */
	    goto slow_path;
	default:
	    goto slow_path;
    }

    if (profile == VDP_INVALID_HANDLE) {
	Error(_("video/vdpau: no valid profile found\n"));
	goto slow_path;
    }

    Debug(3, "video/vdpau: create decoder profile=%d %dx%d #%d refs\n",
	profile, video_ctx->width, video_ctx->height, max_refs);

    decoder->Profile = profile;
    decoder->SurfacesNeeded = max_refs + VIDEO_SURFACES_MAX + 1;
    decoder->PixFmt = *fmt_idx;
    decoder->InputWidth = 0;
    decoder->InputHeight = 0;

#if 0
    if (video_ctx->width && video_ctx->height) {
	VdpStatus status;

	VdpauCleanup(decoder);

	status =
	    VdpauDecoderCreate(VdpauDevice, profile, video_ctx->width,
	    video_ctx->height, max_refs, &decoder->VideoDecoder);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't create decoder: %s\n"),
		VdpauGetErrorString(status));
	    goto slow_path;
	}

	decoder->InputWidth = video_ctx->width;
	decoder->InputHeight = video_ctx->height;
	decoder->InputAspect = video_ctx->sample_aspect_ratio;

	VdpauSetupOutput(decoder);
    }
#endif

    Debug(3, "\t%#010x %s\n", fmt_idx[0], av_get_pix_fmt_name(fmt_idx[0]));
    return *fmt_idx;

  slow_path:
    // no accelerated format found
    decoder->Profile = VDP_INVALID_HANDLE;
    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX + 2;
    decoder->PixFmt = PIX_FMT_NONE;

    decoder->InputWidth = 0;
    decoder->InputHeight = 0;
    video_ctx->hwaccel_context = NULL;

    return avcodec_default_get_format(video_ctx, fmt);
}

#ifdef USE_GRAB

#ifdef DEBUG				// function not used

///
///	Grab video surface.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauGrabVideoSurface(VdpauDecoder * decoder)
{
    VdpVideoSurface surface;
    VdpStatus status;
    VdpChromaType chroma_type;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[3];
    uint32_t pitches[3];
    VdpYCbCrFormat format;

    // FIXME: test function to grab output surface content
    // for screen shots, atom light and auto crop.

    surface = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
	% VIDEO_SURFACES_MAX];

    //	get real surface size
    status =
	VdpauVideoSurfaceGetParameters(surface, &chroma_type, &width, &height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface parameters: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }
    switch (chroma_type) {
	case VDP_CHROMA_TYPE_420:
	case VDP_CHROMA_TYPE_422:
	case VDP_CHROMA_TYPE_444:
	    size = width * height + ((width + 1) / 2) * ((height + 1) / 2)
		+ ((width + 1) / 2) * ((height + 1) / 2);
	    base = malloc(size);
	    if (!base) {
		Error(_("video/vdpau: out of memory\n"));
		return;
	    }
	    pitches[0] = width;
	    pitches[1] = width / 2;
	    pitches[2] = width / 2;
	    data[0] = base;
	    data[1] = base + width * height;
	    data[2] = base + width * height + width * height / 4;
	    format = VDP_YCBCR_FORMAT_YV12;
	    break;
	default:
	    Error(_("video/vdpau: unsupported chroma type %d\n"), chroma_type);
	    return;
    }
    status = VdpauVideoSurfaceGetBitsYCbCr(surface, format, data, pitches);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface bits: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }

    free(base);
}

#endif

///
///	Grab output surface already locked.
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *VdpauGrabOutputSurfaceLocked(int *ret_size, int *ret_width,
    int *ret_height)
{
    VdpOutputSurface surface;
    VdpStatus status;
    VdpRGBAFormat rgba_format;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[1];
    uint32_t pitches[1];
    VdpRect source_rect;
    VdpRect output_rect;

    surface = VdpauSurfacesRb[VdpauSurfaceIndex];

    //	get real surface size
    status =
	VdpauOutputSurfaceGetParameters(surface, &rgba_format, &width,
	&height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get output surface parameters: %s\n"),
	    VdpauGetErrorString(status));
	return NULL;
    }

    Debug(3, "video/vdpau: grab %dx%d format %d\n", width, height,
	rgba_format);

    source_rect.x0 = 0;
    source_rect.y0 = 0;
    source_rect.x1 = width;
    source_rect.y1 = height;

    if (ret_width && ret_height) {
	if (*ret_width <= -64) {	// this is a Atmo grab service request
	    int overscan;

	    // calculate aspect correct size of analyze image
	    width = *ret_width * -1;
	    height = (width * source_rect.y1) / source_rect.x1;

	    // calculate size of grab (sub) window
	    overscan = *ret_height;

	    if (overscan > 0 && overscan <= 200) {
		source_rect.x0 = source_rect.x1 * overscan / 1000;
		source_rect.x1 -= source_rect.x0;
		source_rect.y0 = source_rect.y1 * overscan / 1000;
		source_rect.y1 -= source_rect.y0;
	    }
	} else {
	    if (*ret_width > 0 && (unsigned)*ret_width < width) {
		width = *ret_width;
	    }
	    if (*ret_height > 0 && (unsigned)*ret_height < height) {
		height = *ret_height;
	    }
	}

	Debug(3, "video/vdpau: grab source rect %d,%d:%d,%d dest dim %dx%d\n",
	    source_rect.x0, source_rect.y0, source_rect.x1, source_rect.y1,
	    width, height);

	if ((source_rect.x1 - source_rect.x0) != width
	    || (source_rect.y1 - source_rect.y0) != height) {
	    output_rect.x0 = 0;
	    output_rect.y0 = 0;
	    output_rect.x1 = width;
	    output_rect.y1 = height;

	    status =
		VdpauOutputSurfaceRenderOutputSurface(VdpauGrabRenderSurface,
		&output_rect, surface, &source_rect, NULL, NULL,
		VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't render output surface: %s\n"),
		    VdpauGetErrorString(status));
		return NULL;
	    }

	    surface = VdpauGrabRenderSurface;
	    source_rect = output_rect;
	}
    }

    switch (rgba_format) {
	case VDP_RGBA_FORMAT_B8G8R8A8:
	case VDP_RGBA_FORMAT_R8G8B8A8:
	    size = width * height * sizeof(uint32_t);
	    base = malloc(size);
	    if (!base) {
		Error(_("video/vdpau: out of memory\n"));
		return NULL;
	    }
	    pitches[0] = width * sizeof(uint32_t);
	    data[0] = base;
	    break;
	case VDP_RGBA_FORMAT_R10G10B10A2:
	case VDP_RGBA_FORMAT_B10G10R10A2:
	case VDP_RGBA_FORMAT_A8:
	default:
	    Error(_("video/vdpau: unsupported rgba format %d\n"), rgba_format);
	    return NULL;
    }

    status =
	VdpauOutputSurfaceGetBitsNative(surface, &source_rect, data, pitches);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface bits native: %s\n"),
	    VdpauGetErrorString(status));
	free(base);
	return NULL;
    }

    if (ret_size) {
	*ret_size = size;
    }
    if (ret_width) {
	*ret_width = width;
    }
    if (ret_height) {
	*ret_height = height;
    }

    return base;
}

///
///	Grab output surface.
///
///	@param ret_size[out]		size of allocated surface copy
///	@param ret_width[in,out]	width of output
///	@param ret_height[in,out]	height of output
///
static uint8_t *VdpauGrabOutputSurface(int *ret_size, int *ret_width,
    int *ret_height)
{
    uint8_t *img;

    if (VdpauGrabRenderSurface == VDP_INVALID_HANDLE) {
	return NULL;			// vdpau video module not yet initialized
    }

    pthread_mutex_lock(&VdpauGrabMutex);
    img = VdpauGrabOutputSurfaceLocked(ret_size, ret_width, ret_height);
    pthread_mutex_unlock(&VdpauGrabMutex);
    return img;
}

#endif

#ifdef USE_AUTOCROP

///
///	VDPAU auto-crop support.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauAutoCrop(VdpauDecoder * decoder)
{
    VdpVideoSurface surface;
    VdpStatus status;
    VdpChromaType chroma_type;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    void *base;
    void *data[3];
    uint32_t pitches[3];
    int crop14;
    int crop16;
    int next_state;
    VdpYCbCrFormat format;

    surface = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
	% VIDEO_SURFACES_MAX];

    //	get real surface size
    status =
	VdpauVideoSurfaceGetParameters(surface, &chroma_type, &width, &height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface parameters: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }
    switch (chroma_type) {
	case VDP_CHROMA_TYPE_420:
	case VDP_CHROMA_TYPE_422:
	case VDP_CHROMA_TYPE_444:
	    size = width * height + ((width + 1) / 2) * ((height + 1) / 2)
		+ ((width + 1) / 2) * ((height + 1) / 2);
	    base = malloc(size);
	    if (!base) {
		Error(_("video/vdpau: out of memory\n"));
		return;
	    }
	    pitches[0] = width;
	    pitches[1] = width / 2;
	    pitches[2] = width / 2;
	    data[0] = base;
	    data[1] = base + width * height;
	    data[2] = base + width * height + width * height / 4;
	    format = VDP_YCBCR_FORMAT_YV12;
	    break;
	default:
	    Error(_("video/vdpau: unsupported chroma type %d\n"), chroma_type);
	    return;
    }
    status = VdpauVideoSurfaceGetBitsYCbCr(surface, format, data, pitches);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't get video surface bits: %s\n"),
	    VdpauGetErrorString(status));
	return;
    }

    AutoCropDetect(decoder->AutoCrop, width, height, data, pitches);
    free(base);

    // ignore black frames
    if (decoder->AutoCrop->Y1 >= decoder->AutoCrop->Y2) {
	return;
    }

    crop14 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 14);
    crop14 = (decoder->InputHeight - crop14) / 2;
    crop16 =
	(decoder->InputWidth * decoder->InputAspect.num * 9) /
	(decoder->InputAspect.den * 16);
    crop16 = (decoder->InputHeight - crop16) / 2;

    if (decoder->AutoCrop->Y1 >= crop16 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop16 - AutoCropTolerance) {
	next_state = 16;
    } else if (decoder->AutoCrop->Y1 >= crop14 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >=
	crop14 - AutoCropTolerance) {
	next_state = 14;
    } else {
	next_state = 0;
    }

    if (decoder->AutoCrop->State == next_state) {
	return;
    }

    Debug(3, "video: crop aspect %d:%d %d/%d %d%+d\n",
	decoder->InputAspect.num, decoder->InputAspect.den, crop14, crop16,
	decoder->AutoCrop->Y1, decoder->InputHeight - decoder->AutoCrop->Y2);

    Debug(3, "video: crop aspect %d -> %d\n", decoder->AutoCrop->State,
	next_state);

    switch (decoder->AutoCrop->State) {
	case 16:
	case 14:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay / 2) {
		return;
	    }
	    break;
	case 0:
	    if (decoder->AutoCrop->Count++ < AutoCropDelay) {
		return;
	    }
	    break;
    }

    decoder->AutoCrop->State = next_state;
    if (next_state) {
	decoder->CropX = VideoCutLeftRight[decoder->Resolution];
	decoder->CropY =
	    (next_state ==
	    16 ? crop16 : crop14) + VideoCutTopBottom[decoder->Resolution];
	decoder->CropWidth = decoder->InputWidth - decoder->CropX * 2;
	decoder->CropHeight = decoder->InputHeight - decoder->CropY * 2;

	// FIXME: this overwrites user choosen output position
	// FIXME: resize kills the auto crop values
	// FIXME: support other 4:3 zoom modes
	decoder->OutputX = decoder->VideoX;
	decoder->OutputY = decoder->VideoY;
	decoder->OutputWidth = (decoder->VideoHeight * next_state) / 9;
	decoder->OutputHeight = (decoder->VideoWidth * 9) / next_state;
	if (decoder->OutputWidth > decoder->VideoWidth) {
	    decoder->OutputWidth = decoder->VideoWidth;
	    decoder->OutputY =
		(decoder->VideoHeight - decoder->OutputHeight) / 2;
	} else if (decoder->OutputHeight > decoder->VideoHeight) {
	    decoder->OutputHeight = decoder->VideoHeight;
	    decoder->OutputX =
		(decoder->VideoWidth - decoder->OutputWidth) / 2;
	}
	Debug(3, "video: aspect output %dx%d %dx%d%+d%+d\n",
	    decoder->InputWidth, decoder->InputHeight, decoder->OutputWidth,
	    decoder->OutputHeight, decoder->OutputX, decoder->OutputY);
    } else {
	// sets AutoCrop->Count
	VdpauUpdateOutput(decoder);
    }
    decoder->AutoCrop->Count = 0;
}

///
///	VDPAU check if auto-crop todo.
///
///	@param decoder	VDPAU hw decoder
///
///	@note a copy of VaapiCheckAutoCrop
///	@note auto-crop only supported with normal 4:3 display mode
///
static void VdpauCheckAutoCrop(VdpauDecoder * decoder)
{
    // reduce load, check only n frames
    if (Video4to3ZoomMode == VideoNormal && AutoCropInterval
	&& !(decoder->FrameCounter % AutoCropInterval)) {
	AVRational input_aspect_ratio;
	AVRational tmp_ratio;

	av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den,
	    decoder->InputWidth * decoder->InputAspect.num,
	    decoder->InputHeight * decoder->InputAspect.den, 1024 * 1024);

	tmp_ratio.num = 4;
	tmp_ratio.den = 3;
	// only 4:3 with 16:9/14:9 inside supported
	if (!av_cmp_q(input_aspect_ratio, tmp_ratio)) {
	    VdpauAutoCrop(decoder);
	} else {
	    decoder->AutoCrop->Count = 0;
	    decoder->AutoCrop->State = 0;
	}
    }
}

///
///	VDPAU reset auto-crop.
///
static void VdpauResetAutoCrop(void)
{
    int i;

    for (i = 0; i < VdpauDecoderN; ++i) {
	VdpauDecoders[i]->AutoCrop->State = 0;
	VdpauDecoders[i]->AutoCrop->Count = 0;
    }
}

#endif

///
///	Queue output surface.
///
///	@param decoder	VDPAU hw decoder
///	@param surface	output surface
///	@param softdec	software decoder
///
///	@note we can't mix software and hardware decoder surfaces
///
static void VdpauQueueSurface(VdpauDecoder * decoder, VdpVideoSurface surface,
    int softdec)
{
    VdpVideoSurface old;

    ++decoder->FrameCounter;

    if (1) {				// can't wait for output queue empty
	if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    Warning(_
		("video/vdpau: output buffer full, dropping frame (%d/%d)\n"),
		++decoder->FramesDropped, decoder->FrameCounter);
	    if (!(decoder->FramesDisplayed % 300)) {
		VdpauPrintFrames(decoder);
	    }
	    // software surfaces only
	    if (softdec) {
		VdpauReleaseSurface(decoder, surface);
	    }
	    return;
	}
#if 0
    } else {				// wait for output queue empty
	while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    VideoDisplayHandler();
	}
#endif
    }

    //
    //	    Check and release, old surface
    //
    if ((old = decoder->SurfacesRb[decoder->SurfaceWrite])
	!= VDP_INVALID_HANDLE) {

	// now we can release the surface, software surfaces only
	if (softdec) {
	    VdpauReleaseSurface(decoder, old);
	}
    }

    Debug(4, "video/vdpau: yy video surface %#08x@%d ready\n", surface,
	decoder->SurfaceWrite);

    decoder->SurfacesRb[decoder->SurfaceWrite] = surface;
    decoder->SurfaceWrite = (decoder->SurfaceWrite + 1)
	% VIDEO_SURFACES_MAX;
    atomic_inc(&decoder->SurfacesFilled);
}

///
///	Render a ffmpeg frame.
///
///	@param decoder		VDPAU hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VdpauRenderFrame(VdpauDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    VdpStatus status;
    VdpVideoSurface surface;
    int interlaced;

    // FIXME: some tv-stations toggle interlace on/off
    // frame->interlaced_frame isn't always correct set
    interlaced = frame->interlaced_frame;
    if (video_ctx->height == 720) {
	if (interlaced && !decoder->WrongInterlacedWarned) {
	    Debug(3, "video/vdpau: wrong interlace flag fixed\n");
	    decoder->WrongInterlacedWarned = 1;
	}
	interlaced = 0;
    } else {
	if (!interlaced && !decoder->WrongInterlacedWarned) {
	    Debug(3, "video/vdpau: wrong interlace flag fixed\n");
	    decoder->WrongInterlacedWarned = 1;
	}
	interlaced = 1;
    }

    // FIXME: should be done by init video_ctx->field_order
    if (decoder->Interlaced != interlaced
	|| decoder->TopFieldFirst != frame->top_field_first) {

	Debug(3, "video/vdpau: interlaced %d top-field-first %d\n", interlaced,
	    frame->top_field_first);

	decoder->Interlaced = interlaced;
	decoder->TopFieldFirst = frame->top_field_first;
	decoder->SurfaceField = 0;
    }
    // update aspect ratio changes
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(53,60,100)
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, frame->sample_aspect_ratio)) {
	Debug(3, "video/vdpau: aspect ratio changed\n");

	decoder->InputAspect = frame->sample_aspect_ratio;
	VdpauUpdateOutput(decoder);
    }
#else
    if (decoder->InputWidth && decoder->InputHeight
	&& av_cmp_q(decoder->InputAspect, video_ctx->sample_aspect_ratio)) {
	Debug(3, "video/vdpau: aspect ratio changed\n");

	decoder->InputAspect = video_ctx->sample_aspect_ratio;
	VdpauUpdateOutput(decoder);
    }
#endif

    //
    // Hardware render
    //
    // VDPAU: PIX_FMT_VDPAU_H264 .. PIX_FMT_VDPAU_VC1 PIX_FMT_VDPAU_MPEG4
    if ((PIX_FMT_VDPAU_H264 <= video_ctx->pix_fmt
	    && video_ctx->pix_fmt <= PIX_FMT_VDPAU_VC1)
	|| video_ctx->pix_fmt == PIX_FMT_VDPAU_MPEG4) {
	struct vdpau_render_state *vrs;

	vrs = (struct vdpau_render_state *)frame->data[0];
	surface = vrs->surface;
	Debug(4, "video/vdpau: hw render hw surface %#08x\n", surface);

	if (interlaced
	    && VideoDeinterlace[decoder->Resolution] >=
	    VideoDeinterlaceSoftBob) {
	    // FIXME: software deinterlace avpicture_deinterlace
	    // FIXME: VdpauCpuDeinterlace(decoder, surface);
	    VdpauQueueSurface(decoder, surface, 0);
	} else {
	    VdpauQueueSurface(decoder, surface, 0);
	}

	//
	// PutBitsYCbCr render
	//
    } else {
	void const *data[3];
	uint32_t pitches[3];

	//
	//	Check image, format, size
	//
	if (decoder->PixFmt != video_ctx->pix_fmt
	    || video_ctx->width != decoder->InputWidth
	    || video_ctx->height != decoder->InputHeight) {

	    decoder->PixFmt = video_ctx->pix_fmt;
	    decoder->InputWidth = video_ctx->width;
	    decoder->InputHeight = video_ctx->height;

	    VdpauCleanup(decoder);
	    decoder->SurfacesNeeded = VIDEO_SURFACES_MAX + 2;
	    VdpauSetupOutput(decoder);
	}
	//
	//	Copy data from frame to image
	//
	switch (video_ctx->pix_fmt) {
	    case PIX_FMT_YUV420P:
		break;
	    case PIX_FMT_YUV422P:
	    case PIX_FMT_YUV444P:
	    default:
		Fatal(_("video/vdpau: pixel format %d not supported\n"),
		    video_ctx->pix_fmt);
		// FIXME: no fatals!
	}

	// convert ffmpeg order to vdpau
	data[0] = frame->data[0];
	data[1] = frame->data[2];
	data[2] = frame->data[1];
	pitches[0] = frame->linesize[0];
	pitches[1] = frame->linesize[2];
	pitches[2] = frame->linesize[1];

	surface = VdpauGetSurface0(decoder);
	status =
	    VdpauVideoSurfacePutBitsYCbCr(surface, VDP_YCBCR_FORMAT_YV12, data,
	    pitches);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't put video surface bits: %s\n"),
		VdpauGetErrorString(status));
	}

	Debug(4, "video/vdpau: sw render hw surface %#08x\n", surface);

	VdpauQueueSurface(decoder, surface, 1);
    }

    if (frame->interlaced_frame) {
	++decoder->FrameCounter;
    }
}

///
///	Render osd surface to output surface.
///
static void VdpauMixOsd(void)
{
    VdpOutputSurfaceRenderBlendState blend_state;
    VdpRect source_rect;
    VdpRect output_rect;
    VdpRect output_double_rect;
    VdpStatus status;

    //uint32_t start;
    //uint32_t end;

    //
    //	blend overlay over output
    //
    blend_state.struct_version = VDP_OUTPUT_SURFACE_RENDER_BLEND_STATE_VERSION;
    blend_state.blend_factor_source_color =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_SRC_ALPHA;
    blend_state.blend_factor_source_alpha =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE;
    blend_state.blend_factor_destination_color =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_state.blend_factor_destination_alpha =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_state.blend_equation_color =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD;
    blend_state.blend_equation_alpha =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD;

    // use dirty area
    if (OsdDirtyWidth && OsdDirtyHeight) {
	source_rect.x0 = OsdDirtyX;
	source_rect.y0 = OsdDirtyY;
	source_rect.x1 = source_rect.x0 + OsdDirtyWidth;
	source_rect.y1 = source_rect.y0 + OsdDirtyHeight;

	output_rect.x0 = (OsdDirtyX * VideoWindowWidth) / OsdWidth;
	output_rect.y0 = (OsdDirtyY * VideoWindowHeight) / OsdHeight;
	output_rect.x1 =
	    output_rect.x0 + (OsdDirtyWidth * VideoWindowWidth) / OsdWidth;
	output_rect.y1 =
	    output_rect.y0 + (OsdDirtyHeight * VideoWindowHeight) / OsdHeight;
    } else {
	source_rect.x0 = 0;
	source_rect.y0 = 0;
	source_rect.x1 = OsdWidth;
	source_rect.y1 = OsdHeight;

	output_rect.x0 = 0;
	output_rect.y0 = 0;
	output_rect.x1 = VideoWindowWidth;
	output_rect.y1 = VideoWindowHeight;
    }

    output_double_rect = output_rect;

    switch (Osd3DMode) {
	case 1:
	    output_rect.x0 = output_rect.x0 / 2;
	    output_rect.x1 = output_rect.x1 / 2;
	    output_double_rect.x0 = output_rect.x0 + (VideoWindowWidth / 2);
	    output_double_rect.x1 = output_rect.x1 + (VideoWindowWidth / 2);
	    break;
	case 2:
	    output_rect.y0 = output_rect.y0 / 2;
	    output_rect.y1 = output_rect.y1 / 2;
	    output_double_rect.y0 = output_rect.y0 + (VideoWindowHeight / 2);
	    output_double_rect.y1 = output_rect.y1 + (VideoWindowHeight / 2);
	    break;
	default:
	    break;
    }

    //start = GetMsTicks();

    // FIXME: double buffered osd disabled
    VdpauOsdSurfaceIndex = 1;
#ifdef USE_BITMAP
    status =
	VdpauOutputSurfaceRenderBitmapSurface(VdpauSurfacesRb
	[VdpauSurfaceIndex], &output_rect,
	VdpauOsdBitmapSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL,
	VideoTransparentOsd ? &blend_state : NULL,
	VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render bitmap surface: %s\n"),
	    VdpauGetErrorString(status));
    }

    if (Osd3DMode > 0) {
	status =
	    VdpauOutputSurfaceRenderBitmapSurface(VdpauSurfacesRb
	    [VdpauSurfaceIndex], &output_double_rect,
	    VdpauOsdOutputSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL,
	    VideoTransparentOsd ? &blend_state : NULL,
	    VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't render output surface: %s\n"),
		VdpauGetErrorString(status));
	}
    }
#else
    status =
	VdpauOutputSurfaceRenderOutputSurface(VdpauSurfacesRb
	[VdpauSurfaceIndex], &output_rect,
	VdpauOsdOutputSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL,
	VideoTransparentOsd ? &blend_state : NULL,
	VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render output surface: %s\n"),
	    VdpauGetErrorString(status));
    }

    if (Osd3DMode > 0) {
	status =
	    VdpauOutputSurfaceRenderOutputSurface(VdpauSurfacesRb
	    [VdpauSurfaceIndex], &output_double_rect,
	    VdpauOsdOutputSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL,
	    VideoTransparentOsd ? &blend_state : NULL,
	    VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't render output surface: %s\n"),
		VdpauGetErrorString(status));
	}
    }
#endif
    //end = GetMsTicks();
    /*
       Debug(4, "video:/vdpau: osd render %d %dms\n", VdpauOsdSurfaceIndex,
       end - start);
     */

    VdpauOsdSurfaceIndex = !VdpauOsdSurfaceIndex;
}

///
///	Render video surface to output surface.
///
///	@param decoder	VDPAU hw decoder
///	@param level	video surface level 0 = bottom
///
static void VdpauMixVideo(VdpauDecoder * decoder, int level)
{
    VdpVideoSurface current;
    VdpRect video_src_rect;
    VdpRect dst_rect;
    VdpRect dst_video_rect;
    VdpStatus status;

#ifdef USE_AUTOCROP
    // FIXME: can move to render frame
    VdpauCheckAutoCrop(decoder);
#endif

    if (level) {
	dst_rect.x0 = decoder->VideoX;	// video window output (clip)
	dst_rect.y0 = decoder->VideoY;
	dst_rect.x1 = decoder->VideoX + decoder->VideoWidth;
	dst_rect.y1 = decoder->VideoY + decoder->VideoHeight;
    } else {
	dst_rect.x0 = 0;		// complete window (clip)
	dst_rect.y0 = 0;
	dst_rect.x1 = VideoWindowWidth;
	dst_rect.y1 = VideoWindowHeight;
    }

    video_src_rect.x0 = decoder->CropX;	// video source (crop)
    video_src_rect.y0 = decoder->CropY;
    video_src_rect.x1 = decoder->CropX + decoder->CropWidth;
    video_src_rect.y1 = decoder->CropY + decoder->CropHeight;

    dst_video_rect.x0 = decoder->OutputX;	// video output (scale)
    dst_video_rect.y0 = decoder->OutputY;
    dst_video_rect.x1 = decoder->OutputX + decoder->OutputWidth;
    dst_video_rect.y1 = decoder->OutputY + decoder->OutputHeight;

    if (decoder->Interlaced
	&& VideoDeinterlace[decoder->Resolution] != VideoDeinterlaceWeave) {
	//
	//	Build deinterlace structures
	//
	VdpVideoMixerPictureStructure cps;
	VdpVideoSurface past[3];
	int past_n;
	VdpVideoSurface future[3];
	int future_n;

#ifdef DEBUG
	if (atomic_read(&decoder->SurfacesFilled) < 3) {
	    Debug(3, "only %d\n", atomic_read(&decoder->SurfacesFilled));
	}
#endif
	// FIXME: can use VDP_INVALID_HANDLE to support less surface on start

	if (VideoDeinterlaceSurfaces == 5) {
	    past_n = 2;
	    future_n = 2;

	    // FIXME: wrong for bottom-field first
	    // read: past: B0 T0 current T1 future B1 T2 (0 1 2)
	    // read: past: T1 B0 current B1 future T2 B2 (0 1 2)
	    if (decoder->TopFieldFirst != decoder->SurfaceField) {
		cps = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD;

		past[1] = decoder->SurfacesRb[decoder->SurfaceRead];
		past[0] = past[1];
		current = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX];
		future[0] = current;
		future[1] = decoder->SurfacesRb[(decoder->SurfaceRead + 2)
		    % VIDEO_SURFACES_MAX];
		// FIXME: can support 1 future more
	    } else {
		cps = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_BOTTOM_FIELD;

		// FIXME: can support 1 past more
		past[1] = decoder->SurfacesRb[decoder->SurfaceRead];
		past[0] = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX];
		current = past[0];
		future[0] = decoder->SurfacesRb[(decoder->SurfaceRead + 2)
		    % VIDEO_SURFACES_MAX];
		future[1] = future[0];
	    }

	} else if (VideoDeinterlaceSurfaces == 4) {
	    past_n = 2;
	    future_n = 1;

	    // FIXME: wrong for bottom-field first
	    // read: past: B0 T0 current T1 future B1 (0 1 2)
	    // read: past: T1 B0 current B1 future T2 (0 1 2)
	    if (decoder->TopFieldFirst != decoder->SurfaceField) {
		cps = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD;

		past[1] = decoder->SurfacesRb[decoder->SurfaceRead];
		past[0] = past[1];
		current = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX];
		future[0] = current;
	    } else {
		cps = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_BOTTOM_FIELD;

		past[1] = decoder->SurfacesRb[decoder->SurfaceRead];
		past[0] = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX];
		current = past[0];
		future[0] = decoder->SurfacesRb[(decoder->SurfaceRead + 2)
		    % VIDEO_SURFACES_MAX];
	    }

	} else {
	    Error(_("video/vdpau: %d surface deinterlace unsupported\n"),
		VideoDeinterlaceSurfaces);
	}

	// FIXME: past_n, future_n here:
	Debug(4, " %02d	 %02d(%c%02d) %02d  %02d\n", past[1], past[0],
	    cps == VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD ? 'T' : 'B',
	    current, future[0], future[1]);

	status =
	    VdpauVideoMixerRender(decoder->VideoMixer, VDP_INVALID_HANDLE,
	    NULL, cps, past_n, past, current, future_n, future,
	    &video_src_rect, VdpauSurfacesRb[VdpauSurfaceIndex], &dst_rect,
	    &dst_video_rect, 0, NULL);
    } else {
	if (decoder->Interlaced) {
	    current = decoder->SurfacesRb[(decoder->SurfaceRead + 1)
		% VIDEO_SURFACES_MAX];
	} else {
	    current = decoder->SurfacesRb[decoder->SurfaceRead];
	}

	status =
	    VdpauVideoMixerRender(decoder->VideoMixer, VDP_INVALID_HANDLE,
	    NULL, VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME, 0, NULL, current, 0,
	    NULL, &video_src_rect, VdpauSurfacesRb[VdpauSurfaceIndex],
	    &dst_rect, &dst_video_rect, 0, NULL);
    }
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render mixer: %s\n"),
	    VdpauGetErrorString(status));
    }

    Debug(4, "video/vdpau: yy video surface %#08x@%d displayed\n", current,
	decoder->SurfaceRead);
}

///
///	Create and display a black empty surface.
///
///	@param decoder	VDPAU hw decoder
///
///	@FIXME: render only video area, not fullscreen!
///	decoder->Output.. isn't correct setup for radio stations
///
static void VdpauBlackSurface(VdpauDecoder * decoder)
{
    VdpStatus status;
    VdpRect source_rect;
    VdpRect output_rect;

    source_rect.x0 = 0;
    source_rect.y0 = 0;
    source_rect.x1 = 0;
    source_rect.y1 = 0;

    // FIXME: what happens with PIP?
    if (0) {
	// FIXME: wrong for radio channels
	output_rect.x0 = decoder->OutputX;	// video output (scale)
	output_rect.y0 = decoder->OutputY;
	output_rect.x1 = decoder->OutputX + decoder->OutputWidth;
	output_rect.y1 = decoder->OutputY + decoder->OutputHeight;
    } else {
	output_rect.x0 = decoder->VideoX;
	output_rect.y0 = decoder->VideoY;
	output_rect.x1 = decoder->VideoWidth;
	output_rect.y1 = decoder->VideoHeight;
    }

    status =
	VdpauOutputSurfaceRenderOutputSurface(VdpauSurfacesRb
	[VdpauSurfaceIndex], &output_rect,
	VdpauOsdOutputSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL, NULL,
	VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
}

///
///	Advance displayed frame of decoder.
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauAdvanceDecoderFrame(VdpauDecoder * decoder)
{
    // next surface, if complete frame is displayed (1 -> 0)
    if (decoder->SurfaceField) {
	int filled;

	// FIXME: this should check the caller
	// check decoder, if new surface is available
	// need 2 frames for progressive
	// need 4 frames for interlaced
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled <= 1 + 2 * decoder->Interlaced) {
	    // keep use of last surface
	    ++decoder->FramesDuped;
	    // FIXME: don't warn after stream start, don't warn during pause
	    Error(_("video: display buffer empty, duping frame (%d/%d) %d\n"),
		decoder->FramesDuped, decoder->FrameCounter,
		VideoGetBuffers(decoder->Stream));
	    return;
	}
	decoder->SurfaceRead = (decoder->SurfaceRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&decoder->SurfacesFilled);
	decoder->SurfaceField = !decoder->Interlaced;
	return;
    }
    // next field
    decoder->SurfaceField = 1;
}

///
///	Display a video frame.
///
static void VdpauDisplayFrame(void)
{
    VdpStatus status;
    VdpTime first_time;
    static VdpTime last_time;
    int i;

    if (VideoSurfaceModesChanged) {	// handle changed modes
	VideoSurfaceModesChanged = 0;
	for (i = 0; i < VdpauDecoderN; ++i) {
	    if (VdpauDecoders[i]->VideoMixer != VDP_INVALID_HANDLE) {
		VdpauMixerSetup(VdpauDecoders[i]);
	    }
	}
    }
    //
    //	wait for surface visible (blocks max ~5ms)
    //
    status =
	VdpauPresentationQueueBlockUntilSurfaceIdle(VdpauQueue,
	VdpauSurfacesRb[VdpauSurfaceIndex], &first_time);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't block queue: %s\n"),
	    VdpauGetErrorString(status));
    }
    // check if surface was displayed for more than 1 frame
    // FIXME: 21 only correct for 50Hz
    if (last_time && first_time > last_time + 21 * 1000 * 1000) {
	// FIXME: ignore still-frame, trick-speed
	Debug(3, "video/vdpau: %" PRId64 " display time %" PRId64 "\n",
	    first_time / 1000, (first_time - last_time) / 1000);
	// FIXME: can be more than 1 frame long shown
	for (i = 0; i < VdpauDecoderN; ++i) {
	    VdpauDecoders[i]->FramesMissed++;
	    VdpauMessage(2, _("video/vdpau: missed frame (%d/%d)\n"),
		VdpauDecoders[i]->FramesMissed,
		VdpauDecoders[i]->FrameCounter);
	}
    }
    last_time = first_time;

    //
    //	Render videos into output
    //
    for (i = 0; i < VdpauDecoderN; ++i) {
	int filled;
	VdpauDecoder *decoder;

	decoder = VdpauDecoders[i];
	decoder->FramesDisplayed++;
	decoder->StartCounter++;

	filled = atomic_read(&decoder->SurfacesFilled);
	// need 1 frame for progressive, 3 frames for interlaced
	if (filled < 1 + 2 * decoder->Interlaced) {
	    // FIXME: rewrite MixVideo to support less surfaces
	    if ((VideoShowBlackPicture && !decoder->TrickSpeed)
		|| decoder->Closing < -300) {
		VdpauBlackSurface(decoder);
		VdpauMessage(3, "video/vdpau: black surface displayed\n");
	    }
	    continue;
	}

	VdpauMixVideo(decoder, i);
    }

    //
    //	add osd to surface
    //
    if (OsdShown) {			// showing costs performance
	VdpauMixOsd();
    }
    //
    //	place surface in presentation queue
    //
    status =
	VdpauPresentationQueueDisplay(VdpauQueue,
	VdpauSurfacesRb[VdpauSurfaceIndex], 0, 0, 0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't queue display: %s\n"),
	    VdpauGetErrorString(status));
    }
    // FIXME: CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_REALTIME, &VdpauFrameTime);
    for (i = 0; i < VdpauDecoderN; ++i) {
	// remember time of last shown surface
	VdpauDecoders[i]->FrameTime = VdpauFrameTime;
    }

    VdpauSurfaceIndex = (VdpauSurfaceIndex + 1) % OUTPUT_SURFACES_MAX;

    xcb_flush(Connection);
}

///
///	Set VDPAU decoder video clock.
///
///	@param decoder	VDPAU hardware decoder
///	@param pts	audio presentation timestamp
///
void VdpauSetClock(VdpauDecoder * decoder, int64_t pts)
{
    decoder->PTS = pts;
}

///
///	Get VDPAU decoder video clock.
///
///	@param decoder	VDPAU hw decoder
///
///	FIXME: 20 wrong for 60hz dvb streams
///
static int64_t VdpauGetClock(const VdpauDecoder * decoder)
{
    // pts is the timestamp of the latest decoded frame
    if (decoder->PTS == (int64_t) AV_NOPTS_VALUE) {
	return AV_NOPTS_VALUE;
    }
    // subtract buffered decoded frames
    if (decoder->Interlaced) {
	/*
	   Info("video: %s =pts field%d #%d\n",
	   Timestamp2String(decoder->PTS),
	   decoder->SurfaceField,
	   atomic_read(&decoder->SurfacesFilled));
	 */
	// 1 field is future, 2 fields are past, + 2 in driver queue
	return decoder->PTS -
	    20 * 90 * (2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField - 2 + 2);
    }
    // + 2 in driver queue
    return decoder->PTS - 20 * 90 * (atomic_read(&decoder->SurfacesFilled) +
	2);
}

///
///	Set trick play speed.
///
///	@param decoder		VDPAU decoder
///	@param speed		trick speed (0 = normal)
///
static void VdpauSetTrickSpeed(VdpauDecoder * decoder, int speed)
{
    decoder->TrickSpeed = speed;
    decoder->TrickCounter = speed;
    if (speed) {
	decoder->Closing = 0;
    }
}

///
///	Sync decoder output to audio.
///
///	trick-speed	show frame <n> times
///	still-picture	show frame until new frame arrives
///	60hz-mode	repeat every 5th picture
///	video>audio	slow down video by duplicating frames
///	video<audio	speed up video by skipping frames
///	soft-start	show every second frame
///
///	@param decoder	VDPAU hw decoder
///
static void VdpauSyncDecoder(VdpauDecoder * decoder)
{
    int err;
    int filled;
    int64_t audio_clock;
    int64_t video_clock;

    err = 0;
    video_clock = VdpauGetClock(decoder);
    filled = atomic_read(&decoder->SurfacesFilled);

    if (!decoder->SyncOnAudio) {
	audio_clock = AV_NOPTS_VALUE;
	// FIXME: 60Hz Mode
	goto skip_sync;
    }
    audio_clock = AudioGetClock();

    // 60Hz: repeat every 5th field
    if (Video60HzMode && !(decoder->FramesDisplayed % 6)) {
	if (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock == (int64_t) AV_NOPTS_VALUE) {
	    goto out;
	}
	// both clocks are known
	if (audio_clock + VideoAudioDelay <= video_clock + 15 * 90) {
	    goto out;
	}
	// out of sync: audio before video
	if (!decoder->TrickSpeed) {
	    goto skip_sync;
	}
    }
    // TrickSpeed
    if (decoder->TrickSpeed) {
	if (decoder->TrickCounter--) {
	    goto out;
	}
	decoder->TrickCounter = decoder->TrickSpeed;
	goto skip_sync;
    }
    // at start of new video stream, soft or hard sync video to audio
    if (!VideoSoftStartSync && decoder->StartCounter < VideoSoftStartFrames
	&& video_clock != (int64_t) AV_NOPTS_VALUE
	&& (audio_clock == (int64_t) AV_NOPTS_VALUE
	    || video_clock > audio_clock + VideoAudioDelay + 120 * 90)) {
	err =
	    VdpauMessage(3, "video: initial slow down video, frame %d\n",
	    decoder->StartCounter);
	goto out;
    }

    if (decoder->SyncCounter && decoder->SyncCounter--) {
	goto skip_sync;
    }

    if (audio_clock != (int64_t) AV_NOPTS_VALUE
	&& video_clock != (int64_t) AV_NOPTS_VALUE) {
	// both clocks are known

	if (abs(video_clock - audio_clock + VideoAudioDelay) > 5000 * 90) {
	    err = VdpauMessage(2, "video: audio/video difference too big\n");
	} else if (video_clock > audio_clock + VideoAudioDelay + 100 * 90) {
	    // FIXME: this quicker sync step, did not work with new code!
	    err = VdpauMessage(2, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    decoder->SyncCounter = 1;
	    goto out;
	} else if (video_clock > audio_clock + VideoAudioDelay + 45 * 90) {
	    err = VdpauMessage(2, "video: slow down video, duping frame\n");
	    ++decoder->FramesDuped;
	    decoder->SyncCounter = 1;
	    goto out;
	} else if (audio_clock + VideoAudioDelay > video_clock + 15 * 90
	    && filled > 1 + 2 * decoder->Interlaced) {
	    err = VdpauMessage(2, "video: speed up video, droping frame\n");
	    ++decoder->FramesDropped;
	    VdpauAdvanceDecoderFrame(decoder);
	    decoder->SyncCounter = 1;
	}
#if defined(DEBUG) || defined(AV_INFO)
	if (!decoder->SyncCounter && decoder->StartCounter < 1000) {
	    Debug(3, "video/vdpau: synced after %d frames\n",
		decoder->StartCounter);
	    decoder->StartCounter += 1000;
	}
#endif
    }

  skip_sync:
    // check if next field is available
    if (decoder->SurfaceField && filled <= 1 + 2 * decoder->Interlaced) {
	if (filled == 1 + 2 * decoder->Interlaced) {
	    ++decoder->FramesDuped;
	    // FIXME: don't warn after stream start, don't warn during pause
	    err =
		VdpauMessage(1,
		_("video: decoder buffer empty, "
		    "duping frame (%d/%d) %d v-buf\n"), decoder->FramesDuped,
		decoder->FrameCounter, VideoGetBuffers(decoder->Stream));
	    if (decoder->Closing < -300) {
		atomic_set(&decoder->SurfacesFilled, 0);
	    }
	}
	goto out;
    }

    VdpauAdvanceDecoderFrame(decoder);
  out:
#if defined(DEBUG) || defined(AV_INFO)
    // debug audio/video sync
    if (err || !(decoder->FramesDisplayed % AV_INFO_TIME)) {
	if (!err) {
	    VdpauMessage(0, NULL);
	}
	Info("video: %s%+5" PRId64 " %4" PRId64 " %3d/\\ms %3d%+d v-buf\n",
	    Timestamp2String(video_clock),
	    abs((video_clock - audio_clock) / 90) <
	    8888 ? ((video_clock - audio_clock) / 90) : 8888,
	    AudioGetDelay() / 90, (int)VideoDeltaPTS / 90,
	    VideoGetBuffers(decoder->Stream),
	    (1 + decoder->Interlaced) * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField);
	if (!(decoder->FramesDisplayed % (5 * 60 * 60))) {
	    VdpauPrintFrames(decoder);
	}
    }
#endif
    return;				// fix gcc bug!
}

///
///	Sync a video frame.
///
static void VdpauSyncFrame(void)
{
    int i;

    //
    //	Sync video decoder to audio
    //
    for (i = 0; i < VdpauDecoderN; ++i) {
	VdpauSyncDecoder(VdpauDecoders[i]);
    }
}

///
///	Sync and display surface.
///
static void VdpauSyncDisplayFrame(void)
{
    VdpauDisplayFrame();
    VdpauSyncFrame();
}

///
///	Sync and render a ffmpeg frame
///
///	@param decoder		VDPAU hw decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VdpauSyncRenderFrame(VdpauDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    // FIXME: temp debug
    if (0 && frame->pkt_pts != (int64_t) AV_NOPTS_VALUE) {
	Debug(3, "video: render frame pts %s\n",
	    Timestamp2String(frame->pkt_pts));
    }
#ifdef DEBUG
    if (!atomic_read(&decoder->SurfacesFilled)) {
	Debug(3, "video: new stream frame %dms\n", GetMsTicks() - VideoSwitch);
    }
#endif

    if (VdpauPreemption) {		// display preempted
	if (!decoder->Closing) {
	    VideoSetPts(&decoder->PTS, decoder->Interlaced, frame);
	}
	return;
    }
    // if video output buffer is full, wait and display surface.
    // loop for interlace
    // FIXME: wrong for multiple streams
#ifdef DEBUG
    if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	Debug(3, "video/vdpau: this code part shouldn't be used\n");
    }
#endif

    while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	struct timespec abstime;

	pthread_mutex_unlock(&VideoLockMutex);

	abstime = decoder->FrameTime;
	abstime.tv_nsec += 14 * 1000 * 1000;
	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
	    // avoid overflow
	    abstime.tv_sec++;
	    abstime.tv_nsec -= 1000 * 1000 * 1000;
	}

	VideoPollEvent();

	// fix dead-lock with VdpauExit
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_mutex_lock(&VideoLockMutex);
	// give osd some time slot
	while (pthread_cond_timedwait(&VideoWakeupCond, &VideoLockMutex,
		&abstime) != ETIMEDOUT) {
	    // SIGUSR1
	    Debug(3, "video/vdpau: pthread_cond_timedwait error\n");
	}
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	if (VdpauPreemption) {		// display become preempted
	    return;
	}
	VdpauSyncDisplayFrame();
    }

    if (!decoder->Closing) {
	VideoSetPts(&decoder->PTS, decoder->Interlaced, frame);
    }
    VdpauRenderFrame(decoder, video_ctx, frame);
}

///
///	Recover from preemption.
///
static int VdpauPreemptionRecover(void)
{
    VdpStatus status;
    int i;

    status =
	vdp_device_create_x11(XlibDisplay, DefaultScreen(XlibDisplay),
	&VdpauDevice, &VdpauGetProcAddress);
    if (status != VDP_STATUS_OK) {
	VdpauPreemption = 1;
	return -1;
    }
    // VDPAU seems to loose the callback during preemption
    status =
	VdpauPreemptionCallbackRegister(VdpauDevice, VdpauPreemptionCallback,
	NULL);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't register preemption callback: %s\n"),
	    VdpauGetErrorString(status));
    }

    VdpauPreemption = 0;
    Debug(3, "video/vdpau: display preemption recovery\n");

    VdpauInitOutputQueue();

    // mixer
    for (i = 0; i < VdpauDecoderN; ++i) {
	VdpauDecoders[i]->VideoDecoder = VDP_INVALID_HANDLE;
	VdpauDecoders[i]->VideoMixer = VDP_INVALID_HANDLE;
	VdpauDecoders[i]->SurfaceFreeN = 0;
	VdpauDecoders[i]->SurfaceUsedN = 0;
    }

    // FIXME: codec has still some surfaces used

    //
    //	invalid osd bitmap/output surfaces
    //
    for (i = 0; i < 1; ++i) {
#ifdef USE_BITMAP
	VdpauOsdBitmapSurface[i] = VDP_INVALID_HANDLE;
#else
	VdpauOsdOutputSurface[i] = VDP_INVALID_HANDLE;
#endif
    }

    VdpauOsdInit(OsdWidth, OsdHeight);

    return 1;
}

///
///	Set VDPAU background color.
///
///	@param rgba	32 bit RGBA color.
///
static void VdpauSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
}

///
///	Set VDPAU video mode.
///
static void VdpauSetVideoMode(void)
{
    int i;

    VdpauExitOutputQueue();

    VdpauInitOutputQueue();
    for (i = 0; i < VdpauDecoderN; ++i) {
	// reset video window, upper level needs to fix the positions
	VdpauDecoders[i]->VideoX = 0;
	VdpauDecoders[i]->VideoY = 0;
	VdpauDecoders[i]->VideoWidth = VideoWindowWidth;
	VdpauDecoders[i]->VideoHeight = VideoWindowHeight;
	VdpauUpdateOutput(VdpauDecoders[i]);
    }
}

#ifdef USE_VIDEO_THREAD2

#else

#ifdef USE_VIDEO_THREAD

///
///	Handle a VDPAU display.
///
///	@todo FIXME: only a single decoder supported.
///
static void VdpauDisplayHandlerThread(void)
{
    int i;
    int err;
    int decoded;
    struct timespec nowtime;
    VdpauDecoder *decoder;

    if (VdpauPreemption) {		// display preempted
	if (VdpauPreemptionRecover()) {
	    usleep(15 * 1000);
	    return;
	}
    }

    decoded = 0;
    pthread_mutex_lock(&VideoLockMutex);
    for (i = 0; i < VdpauDecoderN; ++i) {
	int filled;

	decoder = VdpauDecoders[i];

	//
	// fill frame output ring buffer
	//
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled < VIDEO_SURFACES_MAX) {
	    // FIXME: hot polling
	    // fetch+decode or reopen
	    err = VideoDecodeInput(decoder->Stream);
	} else {
	    err = VideoPollInput(decoder->Stream);
	}
	if (err) {
	    // nothing buffered?
	    if (err == -1 && decoder->Closing) {
		decoder->Closing--;
		if (!decoder->Closing) {
		    Debug(3, "video/vdpau: closing eof\n");
		    decoder->Closing = -1;
		}
	    }
	    continue;
	}
	decoded = 1;
    }
    pthread_mutex_unlock(&VideoLockMutex);

    if (!decoded) {			// nothing decoded, sleep
	// FIXME: sleep on wakeup
	usleep(5 * 1000);
    }

    clock_gettime(CLOCK_REALTIME, &nowtime);
    // time for one frame over?
    if ((nowtime.tv_sec - VdpauFrameTime.tv_sec) * 1000 * 1000 * 1000 +
	(nowtime.tv_nsec - VdpauFrameTime.tv_nsec) < 15 * 1000 * 1000) {
	return;
    }

    pthread_mutex_lock(&VideoLockMutex);
    VdpauSyncDisplayFrame();
    pthread_mutex_unlock(&VideoLockMutex);
}

#else

#define VdpauDisplayHandlerThread	NULL

#endif

#endif

///
///	Set video output position.
///
///	@param decoder	VDPAU hw decoder
///	@param x	video output x coordinate inside the window
///	@param y	video output y coordinate inside the window
///	@param width	video output width
///	@param height	video output height
///
///	@note FIXME: need to know which stream.
///
static void VdpauSetOutputPosition(VdpauDecoder * decoder, int x, int y,
    int width, int height)
{
    Debug(3, "video/vdapu: output %dx%d%+d%+d\n", width, height, x, y);

    decoder->VideoX = x;
    decoder->VideoY = y;
    decoder->VideoWidth = width;
    decoder->VideoHeight = height;

    // next video pictures are automatic rendered to correct position
}

//----------------------------------------------------------------------------
//	VDPAU OSD
//----------------------------------------------------------------------------

static const uint8_t OsdZeros[1920 * 1200 * 4];	///< 0 for clear osd

///
///	Clear subpicture image.
///
///	@note looked by caller
///
static void VdpauOsdClear(void)
{
    VdpStatus status;
    void const *data[1];
    uint32_t pitches[1];
    VdpRect dst_rect;

    if (VdpauPreemption) {		// display preempted
	return;
    }
    // osd image available?
#ifdef USE_BITMAP
    if (VdpauOsdBitmapSurface[VdpauOsdSurfaceIndex] == VDP_INVALID_HANDLE) {
	return;
    }
#else
    if (VdpauOsdOutputSurface[VdpauOsdSurfaceIndex] == VDP_INVALID_HANDLE) {
	return;
    }
#endif

    if (OsdWidth * OsdHeight > 1920 * 1200) {
	Error(_("video/vdpau: osd too big: unsupported\n"));
	return;
    }
    // have dirty area.
    if (OsdDirtyWidth && OsdDirtyHeight) {
	Debug(3, "video/vdpau: osd clear dirty %dx%d%+d%+d\n", OsdDirtyWidth,
	    OsdDirtyHeight, OsdDirtyX, OsdDirtyY);
	dst_rect.x0 = OsdDirtyX;
	dst_rect.y0 = OsdDirtyY;
	dst_rect.x1 = dst_rect.x0 + OsdDirtyWidth;
	dst_rect.y1 = dst_rect.y0 + OsdDirtyHeight;
    } else {
	Debug(3, "video/vdpau: osd clear image\n");
	dst_rect.x0 = 0;
	dst_rect.y0 = 0;
	dst_rect.x1 = dst_rect.x0 + OsdWidth;
	dst_rect.y1 = dst_rect.y0 + OsdHeight;
    }
    data[0] = OsdZeros;
    pitches[0] = OsdWidth * 4;

#ifdef USE_BITMAP
    status =
	VdpauBitmapSurfacePutBitsNative(VdpauOsdBitmapSurface
	[VdpauOsdSurfaceIndex], data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: bitmap surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }
#else
    status =
	VdpauOutputSurfacePutBitsNative(VdpauOsdOutputSurface
	[VdpauOsdSurfaceIndex], data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: output surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }
#endif
}

///
///	Upload ARGB to subpicture image.
///
///	@param x	x position of image in osd
///	@param y	y position of image in osd
///	@param width	width of image
///	@param height	height of image
///	@param argb	argb image
///
///	@note looked by caller
///
static void VdpauOsdDrawARGB(int x, int y, int width, int height,
    const uint8_t * argb)
{
    VdpStatus status;
    void const *data[1];
    uint32_t pitches[1];
    VdpRect dst_rect;

#ifdef DEBUG
    uint32_t start;
    uint32_t end;
#endif

    if (VdpauPreemption) {		// display preempted
	return;
    }
    // osd image available?
#ifdef USE_BITMAP
    if (VdpauOsdBitmapSurface[VdpauOsdSurfaceIndex] == VDP_INVALID_HANDLE) {
	return;
    }
#else
    if (VdpauOsdOutputSurface[VdpauOsdSurfaceIndex] == VDP_INVALID_HANDLE) {
	return;
    }
#endif

#ifdef DEBUG
    start = GetMsTicks();
#endif

    dst_rect.x0 = x;
    dst_rect.y0 = y;
    dst_rect.x1 = dst_rect.x0 + width;
    dst_rect.y1 = dst_rect.y0 + height;
    data[0] = argb;
    pitches[0] = width * 4;

#ifdef USE_BITMAP
    status =
	VdpauBitmapSurfacePutBitsNative(VdpauOsdBitmapSurface
	[VdpauOsdSurfaceIndex], data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: bitmap surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }
#else
    status =
	VdpauOutputSurfacePutBitsNative(VdpauOsdOutputSurface
	[VdpauOsdSurfaceIndex], data, pitches, &dst_rect);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: output surface put bits failed: %s\n"),
	    VdpauGetErrorString(status));
    }
#endif
#ifdef DEBUG
    end = GetMsTicks();

    Debug(3, "video/vdpau: osd upload %dx%d%+d%+d %dms %d\n", width, height, x,
	y, end - start, width * height * 4);
#endif
}

///
///	VDPAU initialize OSD.
///
///	@param width	osd width
///	@param height	osd height
///
static void VdpauOsdInit(int width, int height)
{
    int i;
    VdpStatus status;

    if (!VdpauDevice) {
	Debug(3, "video/vdpau: vdpau not setup\n");
	return;
    }
    //
    //	create bitmap/surface for osd
    //
#ifdef USE_BITMAP
    if (VdpauOsdBitmapSurface[0] == VDP_INVALID_HANDLE) {
	for (i = 0; i < 1; ++i) {
	    status =
		VdpauBitmapSurfaceCreate(VdpauDevice, VDP_RGBA_FORMAT_B8G8R8A8,
		width, height, VDP_TRUE, VdpauOsdBitmapSurface + i);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't create bitmap surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    Debug(4,
		"video/vdpau: created bitmap surface %dx%d with id 0x%08x\n",
		width, height, VdpauOsdBitmapSurface[i]);
	}
    }
#else
    if (VdpauOsdOutputSurface[0] == VDP_INVALID_HANDLE) {
	for (i = 0; i < 1; ++i) {
	    status =
		VdpauOutputSurfaceCreate(VdpauDevice, VDP_RGBA_FORMAT_B8G8R8A8,
		width, height, VdpauOsdOutputSurface + i);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't create output surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    Debug(4,
		"video/vdpau: created osd output surface %dx%d with id 0x%08x\n",
		width, height, VdpauOsdOutputSurface[i]);
	}
    }
#endif
    Debug(3, "video/vdpau: osd surfaces created\n");
}

///
///	Cleanup osd.
///
static void VdpauOsdExit(void)
{
    int i;

    //
    //	destroy osd bitmap/output surfaces
    //
#ifdef USE_BITMAP
    for (i = 0; i < 1; ++i) {
	VdpStatus status;

	if (VdpauOsdBitmapSurface[i] != VDP_INVALID_HANDLE) {
	    status = VdpauBitmapSurfaceDestroy(VdpauOsdBitmapSurface[i]);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't destroy bitmap surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    VdpauOsdBitmapSurface[i] = VDP_INVALID_HANDLE;
	}
    }
#else
    for (i = 0; i < 1; ++i) {
	VdpStatus status;

	if (VdpauOsdOutputSurface[i] != VDP_INVALID_HANDLE) {
	    status = VdpauOutputSurfaceDestroy(VdpauOsdOutputSurface[i]);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't destroy output surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	    VdpauOsdOutputSurface[i] = VDP_INVALID_HANDLE;
	}
    }
#endif
}

///
///	VDPAU module.
///
static const VideoModule VdpauModule = {
    .Name = "vdpau",
    .Enabled = 1,
    .NewHwDecoder =
	(VideoHwDecoder * (*const)(VideoStream *)) VdpauNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))VdpauDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))VdpauGetSurface,
    .ReleaseSurface =
	(void (*const) (VideoHwDecoder *, unsigned))VdpauReleaseSurface,
    .get_format = (enum PixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum PixelFormat *))Vdpau_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))VdpauSyncRenderFrame,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))VdpauSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))VdpauGetClock,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))VdpauSetTrickSpeed,
    .GrabOutput = VdpauGrabOutputSurface,
    .SetBackground = VdpauSetBackground,
    .SetVideoMode = VdpauSetVideoMode,
    .ResetAutoCrop = VdpauResetAutoCrop,
    .DisplayHandlerThread = VdpauDisplayHandlerThread,
    .OsdClear = VdpauOsdClear,
    .OsdDrawARGB = VdpauOsdDrawARGB,
    .OsdInit = VdpauOsdInit,
    .OsdExit = VdpauOsdExit,
    .Init = VdpauInit,
    .Exit = VdpauExit,
};

#endif

//----------------------------------------------------------------------------
//	NOOP
//----------------------------------------------------------------------------

///
///	Allocate new noop decoder.
///
///	@param stream	video stream
///
///	@returns always NULL.
///
static VideoHwDecoder *NoopNewHwDecoder(
    __attribute__ ((unused)) VideoStream * stream)
{
    return NULL;
}

///
///	Release a surface.
///
///	Can be called while exit.
///
///	@param decoder	noop hw decoder
///	@param surface	surface no longer used
///
static void NoopReleaseSurface(
    __attribute__ ((unused)) VideoHwDecoder * decoder, __attribute__ ((unused))
    unsigned surface)
{
}

///
///	Set noop background color.
///
///	@param rgba	32 bit RGBA color.
///
static void NoopSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
}

///
///	Noop initialize OSD.
///
///	@param width	osd width
///	@param height	osd height
///
static void NoopOsdInit( __attribute__ ((unused))
    int width, __attribute__ ((unused))
    int height)
{
}

///
///	Draw OSD ARGB image.
///
///	@param x	x position of image in osd
///	@param y	y position of image in osd
///	@param width	width of image
///	@param height	height of image
///	@param argb	argb image
///
///	@note looked by caller
///
static void NoopOsdDrawARGB( __attribute__ ((unused))
    int x, __attribute__ ((unused))
    int y, __attribute__ ((unused))
    int width, __attribute__ ((unused))
    int height, __attribute__ ((unused))
    const uint8_t * argb)
{
}

///
///	Noop setup.
///
///	@param display_name	x11/xcb display name
///
///	@returns always true.
///
static int NoopInit(const char *display_name)
{
    Info("video/noop: noop driver running on display '%s'\n", display_name);
    return 1;
}

#ifdef USE_VIDEO_THREAD

///
///	Handle a noop display.
///
static void NoopDisplayHandlerThread(void)
{
    // avoid 100% cpu use
    usleep(20 * 1000);
#if 0
    // this can't be canceled
    if (XlibDisplay) {
	XEvent event;

	XPeekEvent(XlibDisplay, &event);
    }
#endif
}

#else

#define NoopDisplayHandlerThread	NULL

#endif

///
///	Noop void function.
///
static void NoopVoid(void)
{
}

///
///	Noop video module.
///
static const VideoModule NoopModule = {
    .Name = "noop",
    .Enabled = 1,
    .NewHwDecoder = NoopNewHwDecoder,
#if 0
    // can't be called:
    .DelHwDecoder = NoopDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))NoopGetSurface,
#endif
    .ReleaseSurface = NoopReleaseSurface,
#if 0
    .get_format = (enum PixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum PixelFormat *))Noop_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))NoopSyncRenderFrame,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))NoopSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))NoopGetClock,
    .SetTrickSpeed =
	(void (*const) (const VideoHwDecoder *, int))NoopSetTrickSpeed,
    .GrabOutput = NoopGrabOutputSurface,
#endif
    .SetBackground = NoopSetBackground,
    .SetVideoMode = NoopVoid,
    .ResetAutoCrop = NoopVoid,
    .DisplayHandlerThread = NoopDisplayHandlerThread,
    .OsdClear = NoopVoid,
    .OsdDrawARGB = NoopOsdDrawARGB,
    .OsdInit = NoopOsdInit,
    .OsdExit = NoopVoid,
    .Init = NoopInit,
    .Exit = NoopVoid,
};

//----------------------------------------------------------------------------
//	OSD
//----------------------------------------------------------------------------

///
///	Clear the OSD.
///
///	@todo I use glTexImage2D to clear the texture, are there faster and
///	better ways to clear a texture?
///
void VideoOsdClear(void)
{
    VideoThreadLock();
#ifdef USE_GLX
    if (GlxEnabled) {
	void *texbuf;

	texbuf = calloc(OsdWidth * OsdHeight, 4);
	glEnable(GL_TEXTURE_2D);	// 2d texture
	glBindTexture(GL_TEXTURE_2D, OsdGlTextures[OsdIndex]);
	// upload no image data, clears texture (on some drivers only)
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, OsdWidth, OsdHeight, 0,
	    GL_BGRA, GL_UNSIGNED_BYTE, texbuf);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);
	GlxCheck();
	free(texbuf);
    }
#endif

    VideoUsedModule->OsdClear();

    OsdDirtyX = OsdWidth;
    OsdDirtyY = OsdHeight;
    OsdDirtyWidth = 0;
    OsdDirtyHeight = 0;
    OsdShown = 0;

    VideoThreadUnlock();
}

///
///	Draw an OSD ARGB image.
///
///	@param x	x position of image in osd
///	@param y	y position of image in osd
///	@param width	width of image
///	@param height	height of image
///	@param argb	argb image
///
void VideoOsdDrawARGB(int x, int y, int width, int height,
    const uint8_t * argb)
{
    VideoThreadLock();
    // update dirty area
    if (x < OsdDirtyX) {
	if (OsdDirtyWidth) {
	    OsdDirtyWidth += OsdDirtyX - x;
	}
	OsdDirtyX = x;
    }
    if (y < OsdDirtyY) {
	if (OsdDirtyHeight) {
	    OsdDirtyHeight += OsdDirtyY - y;
	}
	OsdDirtyY = y;
    }
    if (x + width > OsdDirtyX + OsdDirtyWidth) {
	OsdDirtyWidth = x + width - OsdDirtyX;
    }
    if (y + height > OsdDirtyY + OsdDirtyHeight) {
	OsdDirtyHeight = y + height - OsdDirtyY;
    }
    Debug(4, "video: osd dirty %dx%d%+d%+d -> %dx%d%+d%+d\n", width, height, x,
	y, OsdDirtyWidth, OsdDirtyHeight, OsdDirtyX, OsdDirtyY);

#ifdef USE_GLX
    if (GlxEnabled) {
	Debug(3, "video: %p <-> %p\n", glXGetCurrentContext(), GlxContext);
	GlxUploadTexture(x, y, height, width, argb);
	VideoThreadUnlock();
	return;
    }
#endif
    VideoUsedModule->OsdDrawARGB(x, y, width, height, argb);
    OsdShown = 1;

    VideoThreadUnlock();
}

///
///	Get OSD size.
///
///	@param[out] width	OSD width
///	@param[out] height	OSD height
///
void VideoGetOsdSize(int *width, int *height)
{
    *width = 1920;
    *height = 1080;			// unknown default
    if (OsdWidth && OsdHeight) {
	*width = OsdWidth;
	*height = OsdHeight;
    }
}

///	Set OSD Size.
///
///	@param width	OSD width
///	@param height	OSD height
///
void VideoSetOsdSize(int width, int height)
{
    if (OsdConfigWidth != width || OsdConfigHeight != height) {
	VideoOsdExit();
	OsdConfigWidth = width;
	OsdConfigHeight = height;
	VideoOsdInit();
    }
}

///
///	Set the 3d OSD mode.
///
///	@param mode	OSD mode (0=off, 1=SBS, 2=Top Bottom)
///
void VideoSetOsd3DMode(int mode)
{
    Osd3DMode = mode;
}

///
///	Setup osd.
///
///	FIXME: looking for BGRA, but this fourcc isn't supported by the
///	drawing functions yet.
///
void VideoOsdInit(void)
{
    if (OsdConfigWidth && OsdConfigHeight) {
	OsdWidth = OsdConfigWidth;
	OsdHeight = OsdConfigHeight;
    } else {
	OsdWidth = VideoWindowWidth;
	OsdHeight = VideoWindowHeight;
    }

#ifdef USE_GLX
    // FIXME: make an extra function for this
    if (GlxEnabled) {
	int i;

	Debug(3, "video/glx: %p <-> %p\n", glXGetCurrentContext(), GlxContext);

	//
	//  create a RGBA texture.
	//
	glEnable(GL_TEXTURE_2D);	// create 2d texture(s)

	glGenTextures(2, OsdGlTextures);
	for (i = 0; i < 2; ++i) {
	    glBindTexture(GL_TEXTURE_2D, OsdGlTextures[i]);
	    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
		GL_CLAMP_TO_EDGE);
	    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
		GL_CLAMP_TO_EDGE);
	    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, OsdWidth, OsdHeight, 0,
		GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	}
	glBindTexture(GL_TEXTURE_2D, 0);

	glDisable(GL_TEXTURE_2D);
	return;
    }
#endif

    VideoThreadLock();
    VideoUsedModule->OsdInit(OsdWidth, OsdHeight);
    VideoThreadUnlock();
    VideoOsdClear();
}

///
///	Cleanup OSD.
///
void VideoOsdExit(void)
{
    VideoThreadLock();
    VideoUsedModule->OsdExit();
    VideoThreadUnlock();
    OsdDirtyWidth = 0;
    OsdDirtyHeight = 0;
}

#if 0

//----------------------------------------------------------------------------
//	Overlay
//----------------------------------------------------------------------------

///
///	Render osd surface.
///
void VideoRenderOverlay(void)
{
#ifdef USE_GLX
    if (GlxEnabled) {
	GlxRender(OsdWidth, OsdHeight);
    } else
#endif
    {
    }
}

///
///	Display overlay surface.
///
void VideoDisplayOverlay(void)
{
#ifdef USE_GLX
    if (GlxEnabled) {
	int osd_x1;
	int osd_y1;

	osd_x1 = 0;
	osd_y1 = 0;
#ifdef noDEBUG
	osd_x1 = 100;
	osd_y1 = 100;
#endif
	GlxRenderTexture(OsdGlTextures[OsdIndex], osd_x1, osd_y1,
	    VideoWindowWidth, VideoWindowHeight);
	return;
    }
#endif
#ifdef USE_VAAPI
    {
	void *image_buffer;
	static int counter;

	// upload needs long time
	if (counter == 5) {
	    //return;
	}
	// osd image available?
	if (VaOsdImage.image_id == VA_INVALID_ID) {
	    return;
	}
	// FIXME: this version hangups
	//return;

	// map osd surface/image into memory.
	if (vaMapBuffer(VaDisplay, VaOsdImage.buf,
		&image_buffer) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't map osd image buffer\n"));
	    return;
	}
	// 100% transparent
	memset(image_buffer, 0x80 | counter++, VaOsdImage.data_size);

	// convert internal osd to VA-API image
	//GfxConvert(image_buffer, VaOsdImage.offsets[0], VaOsdImage.pitches[0]);

	if (vaUnmapBuffer(VaDisplay, VaOsdImage.buf) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't unmap osd image buffer\n"));
	}
    }
#endif
}

#endif

//----------------------------------------------------------------------------
//	Events
//----------------------------------------------------------------------------

/// C callback feed key press
extern void FeedKeyPress(const char *, const char *, int, int);

///
///	Handle XLib I/O Errors.
///
///	@param display	display with i/o error
///
static int VideoIOErrorHandler( __attribute__ ((unused)) Display * display)
{

    Error(_("video: fatal i/o error\n"));
    // should be called from VideoThread
    if (VideoThread && VideoThread == pthread_self()) {
	Debug(3, "video: called from video thread\n");
	VideoUsedModule = &NoopModule;
	XlibDisplay = NULL;
	VideoWindow = XCB_NONE;
#ifdef USE_VIDEO_THREAD
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_cond_destroy(&VideoWakeupCond);
	pthread_mutex_destroy(&VideoLockMutex);
	pthread_mutex_destroy(&VideoMutex);
	VideoThread = 0;
	pthread_exit("video thread exit");
#endif
    }
    do {
	sleep(1000);
    } while (1);			// let other threads running

    return -1;
}

///
///	Handle X11 events.
///
///	@todo	Signal WmDeleteMessage to application.
///
static void VideoEvent(void)
{
    XEvent event;
    KeySym keysym;
    char buf[32];
    uint32_t values[1];

    XNextEvent(XlibDisplay, &event);
    switch (event.type) {
	case ClientMessage:
	    Debug(3, "video/event: ClientMessage\n");
	    if (event.xclient.data.l[0] == (long)WmDeleteWindowAtom) {
		Debug(3, "video/event: wm-delete-message\n");
		FeedKeyPress("XKeySym", "Close", 0, 0);
	    }
	    break;

	case MapNotify:
	    Debug(3, "video/event: MapNotify\n");
	    // µwm workaround
	    xcb_change_window_attributes(Connection, VideoWindow,
		XCB_CW_CURSOR, &VideoBlankCursor);
	    VideoBlankTick = 0;
	    break;
	case Expose:
	    //Debug(3, "video/event: Expose\n");
	    break;
	case ReparentNotify:
	    Debug(3, "video/event: ReparentNotify\n");
	    break;
	case ConfigureNotify:
	    //Debug(3, "video/event: ConfigureNotify\n");
	    VideoSetVideoMode(event.xconfigure.x, event.xconfigure.y,
		event.xconfigure.width, event.xconfigure.height);
	    break;
	case ButtonPress:
	    VideoSetFullscreen(-1);
	    break;
	case KeyPress:
	    XLookupString(&event.xkey, buf, sizeof(buf), &keysym, NULL);
	    if (keysym == NoSymbol) {
		Warning(_("video/event: No symbol for %d\n"),
		    event.xkey.keycode);
		break;
	    }
	    FeedKeyPress("XKeySym", XKeysymToString(keysym), 0, 0);
	    break;
	case KeyRelease:
	    break;
	case MotionNotify:
	    values[0] = XCB_NONE;
	    xcb_change_window_attributes(Connection, VideoWindow,
		XCB_CW_CURSOR, values);
	    VideoBlankTick = GetMsTicks();
	    break;
	default:
#if 0
	    if (XShmGetEventBase(XlibDisplay) + ShmCompletion == event.type) {
		// printf("ShmCompletion\n");
	    }
#endif
	    Debug(3, "Unsupported event type %d\n", event.type);
	    break;
    }
}

///
///	Poll all x11 events.
///
void VideoPollEvent(void)
{
    // hide cursor, after xx ms
    if (VideoBlankTick && VideoWindow != XCB_NONE
	&& VideoBlankTick + 200 < GetMsTicks()) {
	xcb_change_window_attributes(Connection, VideoWindow, XCB_CW_CURSOR,
	    &VideoBlankCursor);
	VideoBlankTick = 0;
    }
    while (XlibDisplay && XPending(XlibDisplay)) {
	VideoEvent();
    }
}

//----------------------------------------------------------------------------
//	Thread
//----------------------------------------------------------------------------

#ifdef USE_VIDEO_THREAD

#ifdef USE_GLX
static GLXContext GlxThreadContext;	///< our gl context for the thread
#endif

///
///	Lock video thread.
///
static void VideoThreadLock(void)
{
    if (VideoThread) {
	if (pthread_mutex_lock(&VideoLockMutex)) {
	    Error(_("video: can't lock thread\n"));
	}
    }
}

///
///	Unlock video thread.
///
static void VideoThreadUnlock(void)
{
    if (VideoThread) {
	if (pthread_mutex_unlock(&VideoLockMutex)) {
	    Error(_("video: can't unlock thread\n"));
	}
    }
}

///
///	Video render thread.
///
static void *VideoDisplayHandlerThread(void *dummy)
{
    Debug(3, "video: display thread started\n");

#ifdef USE_GLX
    if (GlxEnabled) {
	Debug(3, "video: %p <-> %p\n", glXGetCurrentContext(),
	    GlxThreadContext);
	GlxThreadContext =
	    glXCreateContext(XlibDisplay, GlxVisualInfo, GlxContext, GL_TRUE);
	if (!GlxThreadContext) {
	    Error(_("video/glx: can't create glx context\n"));
	    return NULL;
	}
	// set glx context
	if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext)) {
	    GlxCheck();
	    Error(_("video/glx: can't make glx context current\n"));
	    return NULL;
	}
    }
#endif

    for (;;) {
	// fix dead-lock with VdpauExit
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	VideoPollEvent();

	VideoUsedModule->DisplayHandlerThread();
    }

    return dummy;
}

///
///	Initialize video threads.
///
static void VideoThreadInit(void)
{
    pthread_mutex_init(&VideoMutex, NULL);
    pthread_mutex_init(&VideoLockMutex, NULL);
    pthread_cond_init(&VideoWakeupCond, NULL);
    pthread_create(&VideoThread, NULL, VideoDisplayHandlerThread, NULL);
    pthread_setname_np(VideoThread, "softhddev video");
}

///
///	Exit and cleanup video threads.
///
static void VideoThreadExit(void)
{
    if (VideoThread) {
	void *retval;

	Debug(3, "video: video thread canceled\n");
	//VideoThreadLock();
	// FIXME: can't cancel locked
	if (pthread_cancel(VideoThread)) {
	    Error(_("video: can't queue cancel video display thread\n"));
	}
	//VideoThreadUnlock();
	if (pthread_join(VideoThread, &retval) || retval != PTHREAD_CANCELED) {
	    Error(_("video: can't cancel video display thread\n"));
	}
	VideoThread = 0;
	pthread_cond_destroy(&VideoWakeupCond);
	pthread_mutex_destroy(&VideoLockMutex);
	pthread_mutex_destroy(&VideoMutex);
    }
}

///
///	Video display wakeup.
///
///	New video arrived, wakeup video thread.
///
void VideoDisplayWakeup(void)
{
    if (!XlibDisplay) {			// not yet started
	return;
    }

    if (!VideoThread) {			// start video thread, if needed
	VideoThreadInit();
    }
}

#endif

//----------------------------------------------------------------------------
//	Video API
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

///
///	Table of all audio modules.
///
static const VideoModule *VideoModules[] = {
#ifdef USE_VDPAU
    &VdpauModule,
#endif
#ifdef USE_VAAPI
    &VaapiModule,
#endif
    &NoopModule
};

///
///	Video hardware decoder
///
struct _video_hw_decoder_
{
    union
    {
#ifdef USE_VAAPI
	VaapiDecoder Vaapi;		///< VA-API decoder structure
#endif
#ifdef USE_VDPAU
	VdpauDecoder Vdpau;		///< vdpau decoder structure
#endif
    };
};

///
///	Allocate new video hw decoder.
///
///	@param stream	video stream
///
///	@returns a new initialized video hardware decoder.
///
VideoHwDecoder *VideoNewHwDecoder(VideoStream * stream)
{
    VideoHwDecoder *hw;

    VideoThreadLock();
    hw = VideoUsedModule->NewHwDecoder(stream);
    VideoThreadUnlock();

    return hw;
}

///
///	Destroy a video hw decoder.
///
///	@param hw_decoder	video hardware decoder
///
void VideoDelHwDecoder(VideoHwDecoder * hw_decoder)
{
    if (hw_decoder) {
	VideoThreadLock();
	VideoUsedModule->DelHwDecoder(hw_decoder);
	VideoThreadUnlock();
    }
}

///
///	Get a free hardware decoder surface.
///
///	@param hw_decoder	video hardware decoder
///	@param video_ctx	ffmpeg video codec context
///
///	@returns the oldest free surface or invalid surface
///
unsigned VideoGetSurface(VideoHwDecoder * hw_decoder,
    const AVCodecContext * video_ctx)
{
    return VideoUsedModule->GetSurface(hw_decoder, video_ctx);
}

///
///	Release a hardware decoder surface.
///
///	@param hw_decoder	video hardware decoder
///	@param surface		surface no longer used
///
void VideoReleaseSurface(VideoHwDecoder * hw_decoder, unsigned surface)
{
    // FIXME: must be guarded against calls, after VideoExit
    VideoUsedModule->ReleaseSurface(hw_decoder, surface);
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param hw_decoder	video hardware decoder
///	@param video_ctx	ffmpeg video codec context
///	@param fmt		is the list of formats which are supported by
///				the codec, it is terminated by -1 as 0 is a
///				valid format, the formats are ordered by
///				quality.
///
enum PixelFormat Video_get_format(VideoHwDecoder * hw_decoder,
    AVCodecContext * video_ctx, const enum PixelFormat *fmt)
{
#ifdef DEBUG
    int ms_delay;

    // FIXME: use frame time
    ms_delay = (1000 * video_ctx->time_base.num * video_ctx->ticks_per_frame)
	/ video_ctx->time_base.den;

    Debug(3, "video: ready %s %dms/frame\n",
	Timestamp2String(VideoGetClock(hw_decoder)), ms_delay);
#endif

    return VideoUsedModule->get_format(hw_decoder, video_ctx, fmt);
}

///
///	Display a ffmpeg frame
///
///	@param hw_decoder	video hardware decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
void VideoRenderFrame(VideoHwDecoder * hw_decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
#if 0
    fprintf(stderr, "video: render frame pts %s closing %d\n",
	Timestamp2String(frame->pkt_pts), hw_decoder->Vdpau.Closing);
#endif
    if (frame->repeat_pict && !VideoIgnoreRepeatPict) {
	Warning(_("video: repeated pict %d found, but not handled\n"),
	    frame->repeat_pict);
    }
    VideoUsedModule->RenderFrame(hw_decoder, video_ctx, frame);
}

///
///	Get VA-API ffmpeg context
///
///	@param hw_decoder	video hardware decoder (must be VA-API)
///
struct vaapi_context *VideoGetVaapiContext(VideoHwDecoder * hw_decoder)
{
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule) {
	return hw_decoder->Vaapi.VaapiContext;
    }
#endif
    (void)hw_decoder;
    Error(_("video/vaapi: get vaapi context, without vaapi enabled\n"));
    return NULL;
}

#ifdef USE_VDPAU

///
///	Draw ffmpeg vdpau render state.
///
///	@param hw_decoder	video hardware decoder
///	@param vrs		vdpau render state
///
void VideoDrawRenderState(VideoHwDecoder * hw_decoder,
    struct vdpau_render_state *vrs)
{
    if (VideoUsedModule == &VdpauModule) {
	VdpStatus status;
	uint32_t start;
	uint32_t end;
	VdpauDecoder *decoder;

	decoder = &hw_decoder->Vdpau;
	if (decoder->VideoDecoder == VDP_INVALID_HANDLE) {
	    // must be hardware decoder!
	    Debug(3, "video/vdpau: recover preemption\n");
	    status =
		VdpauDecoderCreate(VdpauDevice, decoder->Profile,
		decoder->InputWidth, decoder->InputHeight,
		decoder->SurfacesNeeded - VIDEO_SURFACES_MAX - 1,
		&decoder->VideoDecoder);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't create decoder: %s\n"),
		    VdpauGetErrorString(status));
	    }

	    VdpauSetupOutput(decoder);
	    return;
	}

	Debug(4, "video/vdpau: decoder render to %#010x\n", vrs->surface);
	start = GetMsTicks();
	status =
	    VdpauDecoderRender(decoder->VideoDecoder, vrs->surface,
	    (VdpPictureInfo const *)&vrs->info, vrs->bitstream_buffers_used,
	    vrs->bitstream_buffers);
	end = GetMsTicks();
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: decoder rendering failed: %s\n"),
		VdpauGetErrorString(status));
	}
	if (end - start > 35) {
	    // report this
	    Info(_("video/vdpau: %s: decoder render too slow %ums\n"),
		Timestamp2String(decoder->PTS), end - start);
	}
	return;
    }
    Error(_("video/vdpau: draw render state, without vdpau enabled\n"));
}

#endif

///
///	Set video clock.
///
///	@param hw_decoder	video hardware decoder
///	@param pts		audio presentation timestamp
///
void VideoSetClock(VideoHwDecoder * hw_decoder, int64_t pts)
{
    Debug(3, "video: set clock %s\n", Timestamp2String(pts));
    if (hw_decoder) {
	VideoUsedModule->SetClock(hw_decoder, pts);
    }
}

///
///	Get video clock.
///
///	@param hw_decoder	video hardware decoder
///
///	@note this isn't monoton, decoding reorders frames, setter keeps it
///	monotonic
///
int64_t VideoGetClock(const VideoHwDecoder * hw_decoder)
{
    if (hw_decoder) {
	return VideoUsedModule->GetClock(hw_decoder);
    }
    return AV_NOPTS_VALUE;
}

///
///	Set closing stream flag.
///
///	@param hw_decoder	video hardware decoder
///
void VideoSetClosing(VideoHwDecoder * hw_decoder)
{
    Debug(3, "video: set closing\n");
    // FIXME: test to check if working, than make module function
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule) {
	hw_decoder->Vdpau.Closing = 1;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule) {
	hw_decoder->Vaapi.Closing = 1;
    }
#endif
    // clear clock to avoid further sync
    VideoSetClock(hw_decoder, AV_NOPTS_VALUE);
}

///
///	Reset start of frame counter.
///
///	@param hw_decoder	video hardware decoder
///
void VideoResetStart(VideoHwDecoder * hw_decoder)
{
    Debug(3, "video: reset start\n");
    // FIXME: test to check if working, than make module function
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule) {
	hw_decoder->Vdpau.StartCounter = 0;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule) {
	hw_decoder->Vaapi.StartCounter = 0;
    }
#endif
    // clear clock to trigger new video stream
    VideoSetClock(hw_decoder, AV_NOPTS_VALUE);
}

///
///	Set trick play speed.
///
///	@param hw_decoder	video hardware decoder
///	@param speed		trick speed (0 = normal)
///
void VideoSetTrickSpeed(VideoHwDecoder * hw_decoder, int speed)
{
    return VideoUsedModule->SetTrickSpeed(hw_decoder, speed);
}

///
///	Grab full screen image.
///
///	@param size[out]	size of allocated image
///	@param width[in,out]	width of image
///	@param height[in,out]	height of image
///
uint8_t *VideoGrab(int *size, int *width, int *height, int write_header)
{
    Debug(3, "video: grab\n");

#ifdef USE_GRAB
    if (VideoUsedModule->GrabOutput) {
	uint8_t *data;
	uint8_t *rgb;
	char buf[64];
	int i;
	int n;
	int scale_width;
	int scale_height;
	int x;
	int y;
	double src_x;
	double src_y;
	double scale_x;
	double scale_y;

	scale_width = *width;
	scale_height = *height;
	n = 0;
	data = VideoUsedModule->GrabOutput(size, width, height);
	if (data == NULL)
	    return NULL;

	if (scale_width <= 0) {
	    scale_width = *width;
	}
	if (scale_height <= 0) {
	    scale_height = *height;
	}
	// hardware didn't scale for us, use simple software scaler
	if (scale_width != *width && scale_height != *height) {
	    if (write_header) {
		n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n",
		    scale_width, scale_height);
	    }
	    rgb = malloc(scale_width * scale_height * 3 + n);
	    if (!rgb) {
		Error(_("video: out of memory\n"));
		free(data);
		return NULL;
	    }
	    *size = scale_width * scale_height * 3 + n;
	    memcpy(rgb, buf, n);	// header

	    scale_x = (double)*width / scale_width;
	    scale_y = (double)*height / scale_height;

	    src_y = 0.0;
	    for (y = 0; y < scale_height; y++) {
		int o;

		src_x = 0.0;
		o = (int)src_y **width;

		for (x = 0; x < scale_width; x++) {
		    i = 4 * (o + (int)src_x);

		    rgb[n + (x + y * scale_width) * 3 + 0] = data[i + 2];
		    rgb[n + (x + y * scale_width) * 3 + 1] = data[i + 1];
		    rgb[n + (x + y * scale_width) * 3 + 2] = data[i + 0];

		    src_x += scale_x;
		}

		src_y += scale_y;
	    }

	    *width = scale_width;
	    *height = scale_height;

	    // grabed image of correct size convert BGRA -> RGB
	} else {
	    if (write_header) {
		n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n", *width,
		    *height);
	    }
	    rgb = malloc(*width * *height * 3 + n);
	    if (!rgb) {
		Error(_("video: out of memory\n"));
		free(data);
		return NULL;
	    }
	    memcpy(rgb, buf, n);	// header

	    for (i = 0; i < *size / 4; ++i) {	// convert bgra -> rgb
		rgb[n + i * 3 + 0] = data[i * 4 + 2];
		rgb[n + i * 3 + 1] = data[i * 4 + 1];
		rgb[n + i * 3 + 2] = data[i * 4 + 0];
	    }

	    *size = *width * *height * 3 + n;
	}
	free(data);

	return rgb;
    } else
#endif
    {
	Warning(_("softhddev: grab unsupported\n"));
    }

    (void)size;
    (void)width;
    (void)height;
    (void)write_header;
    return NULL;
}

///
///	Grab image service.
///
///	@param size[out]	size of allocated image
///	@param width[in,out]	width of image
///	@param height[in,out]	height of image
///
uint8_t *VideoGrabService(int *size, int *width, int *height)
{
    Debug(3, "video: grab service\n");

#ifdef USE_GRAB
    if (VideoUsedModule->GrabOutput) {
	return VideoUsedModule->GrabOutput(size, width, height);
    } else
#endif
    {
	Warning(_("softhddev: grab unsupported\n"));
    }

    (void)size;
    (void)width;
    (void)height;
    return NULL;
}

///
///	Get decoder statistics.
///
///	@param hw_decoder	video hardware decoder
///	@param[out] missed	missed frames
///	@param[out] duped	duped frames
///	@param[out] dropped	dropped frames
///	@param[out] count	number of decoded frames
///
void VideoGetStats(VideoHwDecoder * hw_decoder, int *missed, int *duped,
    int *dropped, int *counter)
{
    // FIXME: test to check if working, than make module function
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule) {
	*missed = hw_decoder->Vdpau.FramesMissed;
	*duped = hw_decoder->Vdpau.FramesDuped;
	*dropped = hw_decoder->Vdpau.FramesDropped;
	*counter = hw_decoder->Vdpau.FrameCounter;
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule) {
	*missed = hw_decoder->Vaapi.FramesMissed;
	*duped = hw_decoder->Vaapi.FramesDuped;
	*dropped = hw_decoder->Vaapi.FramesDropped;
	*counter = hw_decoder->Vaapi.FrameCounter;
    }
#endif
}

///
///	Get decoder video stream size.
///
///	@param hw_decoder	video hardware decoder
///	@param[out] width	video stream width
///	@param[out] height	video stream height
///	@param[out] aspect_num	video stream aspect numerator
///	@param[out] aspect_den	video stream aspect denominator
///
void VideoGetVideoSize(VideoHwDecoder * hw_decoder, int *width, int *height,
    int *aspect_num, int *aspect_den)
{
    *width = 1920;
    *height = 1080;
    *aspect_num = 16;
    *aspect_den = 9;
    // FIXME: test to check if working, than make module function
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule) {
	*width = hw_decoder->Vdpau.InputWidth;
	*height = hw_decoder->Vdpau.InputHeight;
	av_reduce(aspect_num, aspect_den,
	    hw_decoder->Vdpau.InputWidth * hw_decoder->Vdpau.InputAspect.num,
	    hw_decoder->Vdpau.InputHeight * hw_decoder->Vdpau.InputAspect.den,
	    1024 * 1024);
    }
#endif
#ifdef USE_VAAPI
    if (VideoUsedModule == &VaapiModule) {
	*width = hw_decoder->Vaapi.InputWidth;
	*height = hw_decoder->Vaapi.InputHeight;
	av_reduce(aspect_num, aspect_den,
	    hw_decoder->Vaapi.InputWidth * hw_decoder->Vaapi.InputAspect.num,
	    hw_decoder->Vaapi.InputHeight * hw_decoder->Vaapi.InputAspect.den,
	    1024 * 1024);
    }
#endif

}

#ifdef USE_SCREENSAVER

//----------------------------------------------------------------------------
//	DPMS / Screensaver
//----------------------------------------------------------------------------

static char DPMSDisabled;		///< flag we have disabled dpms

///
///	Suspend X11 screen saver.
///
///	@param connection	X11 connection to enable/disable screensaver
///	@param suspend		True suspend screensaver,
///				false enable screensaver
///
static void X11SuspendScreenSaver(xcb_connection_t * connection, int suspend)
{
    const xcb_query_extension_reply_t *query_extension_reply;

    query_extension_reply =
	xcb_get_extension_data(connection, &xcb_screensaver_id);
    if (query_extension_reply && query_extension_reply->present) {
	xcb_screensaver_query_version_cookie_t cookie;
	xcb_screensaver_query_version_reply_t *reply;

	Debug(3, "video: screen saver extension present\n");

	cookie =
	    xcb_screensaver_query_version_unchecked(connection,
	    XCB_SCREENSAVER_MAJOR_VERSION, XCB_SCREENSAVER_MINOR_VERSION);
	reply = xcb_screensaver_query_version_reply(connection, cookie, NULL);
	if (reply
	    && (reply->server_major_version >= XCB_SCREENSAVER_MAJOR_VERSION)
	    && (reply->server_minor_version >= XCB_SCREENSAVER_MINOR_VERSION)
	    ) {
	    xcb_screensaver_suspend(connection, suspend);
	}
	free(reply);
    }
}

///
///	DPMS (Display Power Management Signaling) extension available.
///
///	@param connection	X11 connection to check for DPMS
///
static int X11HaveDPMS(xcb_connection_t * connection)
{
    static int have_dpms = -1;
    const xcb_query_extension_reply_t *query_extension_reply;

    if (have_dpms != -1) {		// already checked
	return have_dpms;
    }

    have_dpms = 0;
    query_extension_reply = xcb_get_extension_data(connection, &xcb_dpms_id);
    if (query_extension_reply && query_extension_reply->present) {
	xcb_dpms_get_version_cookie_t cookie;
	xcb_dpms_get_version_reply_t *reply;
	int major;
	int minor;

	Debug(3, "video: dpms extension present\n");

	cookie =
	    xcb_dpms_get_version_unchecked(connection, XCB_DPMS_MAJOR_VERSION,
	    XCB_DPMS_MINOR_VERSION);
	reply = xcb_dpms_get_version_reply(connection, cookie, NULL);
	// use locals to avoid gcc warning
	major = XCB_DPMS_MAJOR_VERSION;
	minor = XCB_DPMS_MINOR_VERSION;
	if (reply && (reply->server_major_version >= major)
	    && (reply->server_minor_version >= minor)
	    ) {
	    have_dpms = 1;
	}
	free(reply);
    }
    return have_dpms;
}

///
///	Disable DPMS (Display Power Management Signaling)
///
///	@param connection	X11 connection to disable DPMS
///
static void X11DPMSDisable(xcb_connection_t * connection)
{
    if (X11HaveDPMS(connection)) {
	xcb_dpms_info_cookie_t cookie;
	xcb_dpms_info_reply_t *reply;

	cookie = xcb_dpms_info_unchecked(connection);
	reply = xcb_dpms_info_reply(connection, cookie, NULL);
	if (reply) {
	    if (reply->state) {
		Debug(3, "video: dpms was enabled\n");
		DPMSDisabled = 1;
		xcb_dpms_disable(connection);	// monitor powersave off
	    }
	    free(reply);
	}
    }
}

///
///	Reenable DPMS (Display Power Management Signaling)
///
///	@param connection	X11 connection to enable DPMS
///
static void X11DPMSReenable(xcb_connection_t * connection)
{
    if (DPMSDisabled && X11HaveDPMS(connection)) {
	xcb_dpms_enable(connection);	// monitor powersave on
	xcb_dpms_force_level(connection, XCB_DPMS_DPMS_MODE_ON);
	DPMSDisabled = 0;
    }
}

#else

    /// dummy function: Suspend X11 screen saver.
#define X11SuspendScreenSaver(connection, suspend)
    /// dummy function: Disable X11 DPMS.
#define X11DPMSDisable(connection)
    /// dummy function: Reenable X11 DPMS.
#define X11DPMSReenable(connection)

#endif

//----------------------------------------------------------------------------
//	Setup
//----------------------------------------------------------------------------

///
///	Create main window.
///
///	@param parent	parent of new window
///	@param visual	visual of parent
///	@param depth	depth of parent
///
static void VideoCreateWindow(xcb_window_t parent, xcb_visualid_t visual,
    uint8_t depth)
{
    uint32_t values[4];
    xcb_intern_atom_reply_t *reply;
    xcb_pixmap_t pixmap;
    xcb_cursor_t cursor;

    Debug(3, "video: visual %#0x depth %d\n", visual, depth);

    // Color map
    VideoColormap = xcb_generate_id(Connection);
    xcb_create_colormap(Connection, XCB_COLORMAP_ALLOC_NONE, VideoColormap,
	parent, visual);

    values[0] = 0;
    values[1] = 0;
    values[2] =
	XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
	XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
	XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_EXPOSURE |
	XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    values[3] = VideoColormap;
    VideoWindow = xcb_generate_id(Connection);
    xcb_create_window(Connection, depth, VideoWindow, parent, VideoWindowX,
	VideoWindowY, VideoWindowWidth, VideoWindowHeight, 0,
	XCB_WINDOW_CLASS_INPUT_OUTPUT, visual,
	XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK |
	XCB_CW_COLORMAP, values);

    // define only available with xcb-utils-0.3.8
#ifdef XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS
    // FIXME: utf _NET_WM_NAME
    xcb_icccm_set_wm_name(Connection, VideoWindow, XCB_ATOM_STRING, 8,
	sizeof("softhddevice") - 1, "softhddevice");
    xcb_icccm_set_wm_icon_name(Connection, VideoWindow, XCB_ATOM_STRING, 8,
	sizeof("softhddevice") - 1, "softhddevice");
#endif
    // define only available with xcb-utils-0.3.6
#ifdef XCB_NUM_WM_HINTS_ELEMENTS
    // FIXME: utf _NET_WM_NAME
    xcb_set_wm_name(Connection, VideoWindow, XCB_ATOM_STRING,
	sizeof("softhddevice") - 1, "softhddevice");
    xcb_set_wm_icon_name(Connection, VideoWindow, XCB_ATOM_STRING,
	sizeof("softhddevice") - 1, "softhddevice");
#endif

    // FIXME: size hints

    // register interest in the delete window message
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0,
		    sizeof("WM_DELETE_WINDOW") - 1, "WM_DELETE_WINDOW"),
		NULL))) {
	WmDeleteWindowAtom = reply->atom;
	free(reply);
	if ((reply =
		xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection,
			0, sizeof("WM_PROTOCOLS") - 1, "WM_PROTOCOLS"),
		    NULL))) {
#ifdef XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS
	    xcb_icccm_set_wm_protocols(Connection, VideoWindow, reply->atom, 1,
		&WmDeleteWindowAtom);
#endif
#ifdef XCB_NUM_WM_HINTS_ELEMENTS
	    xcb_set_wm_protocols(Connection, reply->atom, VideoWindow, 1,
		&WmDeleteWindowAtom);
#endif
	    free(reply);
	}
    }
    //
    //	prepare fullscreen.
    //
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0,
		    sizeof("_NET_WM_STATE") - 1, "_NET_WM_STATE"), NULL))) {
	NetWmState = reply->atom;
	free(reply);
    }
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0,
		    sizeof("_NET_WM_STATE_FULLSCREEN") - 1,
		    "_NET_WM_STATE_FULLSCREEN"), NULL))) {
	NetWmStateFullscreen = reply->atom;
	free(reply);
    }

    xcb_map_window(Connection, VideoWindow);

    //
    //	hide cursor
    //
    pixmap = xcb_generate_id(Connection);
    xcb_create_pixmap(Connection, 1, pixmap, parent, 1, 1);
    cursor = xcb_generate_id(Connection);
    xcb_create_cursor(Connection, cursor, pixmap, pixmap, 0, 0, 0, 0, 0, 0, 1,
	1);

    values[0] = cursor;
    xcb_change_window_attributes(Connection, VideoWindow, XCB_CW_CURSOR,
	values);
    VideoCursorPixmap = pixmap;
    VideoBlankCursor = cursor;
    VideoBlankTick = 0;
}

///
///	Set video device.
///
///	Currently this only choose the driver.
///
void VideoSetDevice(const char *device)
{
    VideoDriverName = device;
}

///
///	Get video driver name.
///
///	@returns name of current video driver.
///
const char *VideoGetDriverName(void)
{
    if (VideoUsedModule) {
	return VideoUsedModule->Name;
    }
    return "";
}

///
///	Set video geometry.
///
///	@param geometry	 [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]
///
int VideoSetGeometry(const char *geometry)
{
    XParseGeometry(geometry, &VideoWindowX, &VideoWindowY, &VideoWindowWidth,
	&VideoWindowHeight);

    return 0;
}

///
///	Set 60hz display mode.
///
///	Pull up 50 Hz video for 60 Hz display.
///
///	@param onoff	enable / disable the 60 Hz mode.
///
void VideoSet60HzMode(int onoff)
{
    Video60HzMode = onoff;
}

///
///	Set soft start audio/video sync.
///
///	@param onoff	enable / disable the soft start sync.
///
void VideoSetSoftStartSync(int onoff)
{
    VideoSoftStartSync = onoff;
}

///
///	Set show black picture during channel switch.
///
///	@param onoff	enable / disable black picture.
///
void VideoSetBlackPicture(int onoff)
{
    VideoShowBlackPicture = onoff;
}

///
///	Set brightness adjustment.
///
///	@param brightness	between -1000 and 1000.
///				0 represents no modification
///
void VideoSetBrightness(int brightness)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule) {
	VdpauDecoders[0]->Procamp.brightness = brightness / 1000;
    }
#endif
    // FIXME: VA-API support
}

///
///	Set contrast adjustment.
///
///	@param contrast		between 0 and 10000.
///				1000 represents no modification
///
void VideoSetContrast(int contrast)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule) {
	VdpauDecoders[0]->Procamp.contrast = contrast / 1000;
    }
#endif
    // FIXME: VA-API support
}

///
///	Set saturation adjustment.
///
///	@param saturation	between 0 and 10000.
///				1000 represents no modification
///
void VideoSetSaturation(int saturation)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule) {
	VdpauDecoders[0]->Procamp.saturation = saturation / 1000;
    }
#endif
    // FIXME: VA-API support
}

///
///	Set hue adjustment.
///
///	@param hue	between -PI*1000 and PI*1000.
///			0 represents no modification
///
void VideoSetHue(int hue)
{
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule) {
	VdpauDecoders[0]->Procamp.hue = hue / 1000;
    }
#endif
    // FIXME: VA-API support
}

///
///	Set video output position.
///
///	@param hw_decoder	video hardware decoder
///	@param x		video output x coordinate OSD relative
///	@param y		video output y coordinate OSD relative
///	@param width		video output width
///	@param height		video output height
///
void VideoSetOutputPosition(VideoHwDecoder * hw_decoder, int x, int y,
    int width, int height)
{
    if (!OsdWidth || !OsdHeight) {
	return;
    }
    if (!width || !height) {
	// restore full size
	width = VideoWindowWidth;
	height = VideoWindowHeight;
    } else {
	// convert OSD coordinates to window coordinates
	x = (x * VideoWindowWidth) / OsdWidth;
	width = (width * VideoWindowWidth) / OsdWidth;
	y = (y * VideoWindowHeight) / OsdHeight;
	height = (height * VideoWindowHeight) / OsdHeight;
    }

    // FIXME: add function to module class
#ifdef USE_VDPAU
    if (VideoUsedModule == &VdpauModule) {
	// check values to be able to avoid
	// interfering with the video thread if possible

	if (x == hw_decoder->Vdpau.VideoX && y == hw_decoder->Vdpau.VideoY
	    && width == hw_decoder->Vdpau.VideoWidth
	    && height == hw_decoder->Vdpau.VideoHeight) {
	    // not necessary...
	    return;
	}
	VideoThreadLock();
	VdpauSetOutputPosition(&hw_decoder->Vdpau, x, y, width, height);
	VdpauUpdateOutput(&hw_decoder->Vdpau);
	VideoThreadUnlock();
    }
#endif
#ifdef USE_VAAPI
    // FIXME: not supported by vaapi without unscaled OSD,
    // FIXME: if used to position video inside osd
#endif
}

///
///	Set video window position.
///
///	@param x	window x coordinate
///	@param y	window y coordinate
///	@param width	window width
///	@param height	window height
///
///	@note no need to lock, only called from inside the video thread
///
void VideoSetVideoMode( __attribute__ ((unused))
    int x, __attribute__ ((unused))
    int y, int width, int height)
{
    Debug(4, "video: %s %dx%d%+d%+d\n", __FUNCTION__, width, height, x, y);

    if ((unsigned)width == VideoWindowWidth
	&& (unsigned)height == VideoWindowHeight) {
	return;				// same size nothing todo
    }

    VideoOsdExit();
    // FIXME: must tell VDR that the OsdSize has been changed!

    VideoThreadLock();
    VideoWindowWidth = width;
    VideoWindowHeight = height;
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();
    VideoOsdInit();
}

///
///	Set 4:3 video display format.
///
///	@param format	video format (stretch, normal, center cut-out)
///
void VideoSet4to3DisplayFormat(int format)
{
    // convert api to internal format
    switch (format) {
	case -1:			// rotate settings
	    format = (Video4to3ZoomMode + 1) % (VideoCenterCutOut + 1);
	    break;
	case 0:			// pan&scan (we have no pan&scan)
	    format = VideoStretch;
	    break;
	case 1:			// letter box
	    format = VideoNormal;
	    break;
	case 2:			// center cut-out
	    format = VideoCenterCutOut;
	    break;
    }

    if ((unsigned)format == Video4to3ZoomMode) {
	return;				// no change, no need to lock
    }

    VideoOsdExit();
    // FIXME: must tell VDR that the OsdSize has been changed!

    VideoThreadLock();
    Video4to3ZoomMode = format;
    // FIXME: need only VideoUsedModule->UpdateOutput();
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();

    VideoOsdInit();
}

///
///	Set other video display format.
///
///	@param format	video format (stretch, normal, center cut-out)
///
void VideoSetOtherDisplayFormat(int format)
{
    // convert api to internal format
    switch (format) {
	case -1:			// rotate settings
	    format = (VideoOtherZoomMode + 1) % (VideoCenterCutOut + 1);
	    break;
	case 0:			// pan&scan (we have no pan&scan)
	    format = VideoStretch;
	    break;
	case 1:			// letter box
	    format = VideoNormal;
	    break;
	case 2:			// center cut-out
	    format = VideoCenterCutOut;
	    break;
    }

    if ((unsigned)format == VideoOtherZoomMode) {
	return;				// no change, no need to lock
    }

    VideoOsdExit();
    // FIXME: must tell VDR that the OsdSize has been changed!

    VideoThreadLock();
    VideoOtherZoomMode = format;
    // FIXME: need only VideoUsedModule->UpdateOutput();
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();

    VideoOsdInit();
}

///
///	Send fullscreen message to window.
///
///	@param onoff	-1 toggle, true turn on, false turn off
///
void VideoSetFullscreen(int onoff)
{
    if (XlibDisplay) {			// needs running connection
	xcb_client_message_event_t event;

	memset(&event, 0, sizeof(event));
	event.response_type = XCB_CLIENT_MESSAGE;
	event.format = 32;
	event.window = VideoWindow;
	event.type = NetWmState;
	if (onoff < 0) {
	    event.data.data32[0] = XCB_EWMH_WM_STATE_TOGGLE;
	} else if (onoff) {
	    event.data.data32[0] = XCB_EWMH_WM_STATE_ADD;
	} else {
	    event.data.data32[0] = XCB_EWMH_WM_STATE_REMOVE;
	}
	event.data.data32[1] = NetWmStateFullscreen;

	xcb_send_event(Connection, XCB_SEND_EVENT_DEST_POINTER_WINDOW,
	    DefaultRootWindow(XlibDisplay),
	    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
	    XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (void *)&event);
	Debug(3, "video/x11: send fullscreen message %x %x\n",
	    event.data.data32[0], event.data.data32[1]);
    }
}

///
///	Set deinterlace mode.
///
void VideoSetDeinterlace(int mode[VideoResolutionMax])
{
    VideoDeinterlace[0] = mode[0];
    VideoDeinterlace[1] = mode[1];
    VideoDeinterlace[2] = mode[2];
    VideoDeinterlace[3] = mode[3];
    VideoSurfaceModesChanged = 1;
}

///
///	Set skip chroma deinterlace on/off.
///
void VideoSetSkipChromaDeinterlace(int onoff[VideoResolutionMax])
{
    VideoSkipChromaDeinterlace[0] = onoff[0];
    VideoSkipChromaDeinterlace[1] = onoff[1];
    VideoSkipChromaDeinterlace[2] = onoff[2];
    VideoSkipChromaDeinterlace[3] = onoff[3];
    VideoSurfaceModesChanged = 1;
}

///
///	Set inverse telecine on/off.
///
void VideoSetInverseTelecine(int onoff[VideoResolutionMax])
{
    VideoInverseTelecine[0] = onoff[0];
    VideoInverseTelecine[1] = onoff[1];
    VideoInverseTelecine[2] = onoff[2];
    VideoInverseTelecine[3] = onoff[3];
    VideoSurfaceModesChanged = 1;
}

///
///	Set denoise level (0 .. 1000).
///
void VideoSetDenoise(int level[VideoResolutionMax])
{
    VideoDenoise[0] = level[0];
    VideoDenoise[1] = level[1];
    VideoDenoise[2] = level[2];
    VideoDenoise[3] = level[3];
    VideoSurfaceModesChanged = 1;
}

///
///	Set sharpness level (-1000 .. 1000).
///
void VideoSetSharpen(int level[VideoResolutionMax])
{
    VideoSharpen[0] = level[0];
    VideoSharpen[1] = level[1];
    VideoSharpen[2] = level[2];
    VideoSharpen[3] = level[3];
    VideoSurfaceModesChanged = 1;
}

///
///	Set scaling mode.
///
///	@param mode	table with VideoResolutionMax values
///
void VideoSetScaling(int mode[VideoResolutionMax])
{
    VideoScaling[0] = mode[0];
    VideoScaling[1] = mode[1];
    VideoScaling[2] = mode[2];
    VideoScaling[3] = mode[3];
    VideoSurfaceModesChanged = 1;
}

///
///	Set cut top and bottom.
///
///	@param pixels table with VideoResolutionMax values
///
void VideoSetCutTopBottom(int pixels[VideoResolutionMax])
{
    VideoCutTopBottom[0] = pixels[0];
    VideoCutTopBottom[1] = pixels[1];
    VideoCutTopBottom[2] = pixels[2];
    VideoCutTopBottom[3] = pixels[3];
    // FIXME: update output
}

///
///	Set cut left and right.
///
///	@param pixels	table with VideoResolutionMax values
///
void VideoSetCutLeftRight(int pixels[VideoResolutionMax])
{
    VideoCutLeftRight[0] = pixels[0];
    VideoCutLeftRight[1] = pixels[1];
    VideoCutLeftRight[2] = pixels[2];
    VideoCutLeftRight[3] = pixels[3];
    // FIXME: update output
}

///
///	Set studio levels.
///
///	@param onoff	flag on/off
///
void VideoSetStudioLevels(int onoff)
{
    VideoStudioLevels = onoff;
}

///
///	Set background color.
///
///	@param rgba	32 bit RGBA color.
///
void VideoSetBackground(uint32_t rgba)
{
    VideoBackground = rgba;		// saved for later start
    VideoUsedModule->SetBackground(rgba);
}

///
///	Set audio delay.
///
///	@param ms	delay in ms
///
void VideoSetAudioDelay(int ms)
{
    VideoAudioDelay = ms * 90;
}

///
///	Set auto-crop parameters.
///
void VideoSetAutoCrop(int interval, int delay, int tolerance)
{
#ifdef USE_AUTOCROP
    AutoCropInterval = interval;
    AutoCropDelay = delay;
    AutoCropTolerance = tolerance;

    VideoThreadLock();
    VideoUsedModule->ResetAutoCrop();
    VideoThreadUnlock();
#else
    (void)interval;
    (void)delay;
    (void)tolerance;
#endif
}

///
///	Initialize video output module.
///
///	@param display_name	X11 display name
///
void VideoInit(const char *display_name)
{
    int screen_nr;
    int i;
    xcb_screen_iterator_t screen_iter;
    xcb_screen_t const *screen;

    if (XlibDisplay) {			// allow multiple calls
	Debug(3, "video: x11 already setup\n");
	return;
    }
    // Open the connection to the X server.
    // use the DISPLAY environment variable as the default display name
    if (!display_name && !(display_name = getenv("DISPLAY"))) {
	// if no environment variable, use :0.0 as default display name
	display_name = ":0.0";
    }
    if (!(XlibDisplay = XOpenDisplay(display_name))) {
	Error(_("video: Can't connect to X11 server on '%s'\n"), display_name);
	// FIXME: we need to retry connection
	return;
    }
    XInitThreads();
    // Register error handler
    XSetIOErrorHandler(VideoIOErrorHandler);

    // Convert XLIB display to XCB connection
    if (!(Connection = XGetXCBConnection(XlibDisplay))) {
	Error(_("video: Can't convert XLIB display to XCB connection\n"));
	VideoExit();
	return;
    }
    // prefetch extensions
    //xcb_prefetch_extension_data(Connection, &xcb_big_requests_id);
    //xcb_prefetch_extension_data(Connection, &xcb_glx_id);
    //xcb_prefetch_extension_data(Connection, &xcb_randr_id);
#ifdef USE_SCREENSAVER
    xcb_prefetch_extension_data(Connection, &xcb_screensaver_id);
    xcb_prefetch_extension_data(Connection, &xcb_dpms_id);
#endif
    //xcb_prefetch_extension_data(Connection, &xcb_shm_id);
    //xcb_prefetch_extension_data(Connection, &xcb_xv_id);

    // Get the requested screen number
    screen_nr = DefaultScreen(XlibDisplay);
    screen_iter = xcb_setup_roots_iterator(xcb_get_setup(Connection));
    for (i = 0; i < screen_nr; ++i) {
	xcb_screen_next(&screen_iter);
    }
    screen = screen_iter.data;
    VideoScreen = screen;

    //
    //	Default window size
    //
    if (!VideoWindowHeight) {
	if (VideoWindowWidth) {
	    VideoWindowHeight = (VideoWindowWidth * 9) / 16;
	} else {			// default to fullscreen
	    VideoWindowHeight = screen->height_in_pixels;
	    VideoWindowWidth = screen->width_in_pixels;
	}
    }
    if (!VideoWindowWidth) {
	VideoWindowWidth = (VideoWindowHeight * 16) / 9;
    }
    //
    //	prepare opengl
    //
#ifdef USE_GLX
    if (GlxEnabled) {

	GlxInit();
	// FIXME: use root window?
	VideoCreateWindow(screen->root, GlxVisualInfo->visualid,
	    GlxVisualInfo->depth);
	GlxSetupWindow(VideoWindow, VideoWindowWidth, VideoWindowHeight);
    } else
#endif

	//
	// Create output window
	//
    if (1) {				// FIXME: use window mode

	VideoCreateWindow(screen->root, screen->root_visual,
	    screen->root_depth);

    } else {
	// FIXME: support embedded mode
	VideoWindow = screen->root;

	// FIXME: VideoWindowHeight VideoWindowWidth
    }

    Debug(3, "video: window prepared\n");

    //
    //	prepare hardware decoder VA-API/VDPAU
    //
    for (i = 0; i < (int)(sizeof(VideoModules) / sizeof(*VideoModules)); ++i) {
	// FIXME: support list of drivers and include display name
	// use user device or first working enabled device driver
	if ((VideoDriverName
		&& !strcasecmp(VideoDriverName, VideoModules[i]->Name))
	    || (!VideoDriverName && VideoModules[i]->Enabled)) {
	    if (VideoModules[i]->Init(display_name)) {
		VideoUsedModule = VideoModules[i];
		goto found;
	    }
	}
    }
    Error(_("video: '%s' output module isn't supported\n"), VideoDriverName);
    VideoUsedModule = &NoopModule;

  found:
    // FIXME: make it configurable from gui
    if (getenv("NO_MPEG_HW")) {
	VideoHardwareDecoder = 1;
    }
    if (getenv("NO_HW")) {
	VideoHardwareDecoder = 0;
    }
    if (getenv("STUDIO_LEVELS")) {
	VideoStudioLevels = 1;
    }
    // disable x11 screensaver
    X11SuspendScreenSaver(Connection, 1);
    X11DPMSDisable(Connection);

    //xcb_prefetch_maximum_request_length(Connection);
    xcb_flush(Connection);

    // I would like to start threads here, but this produces:
    // [xcb] Unknown sequence number while processing queue
    // [xcb] Most likely this is a multi-threaded client and XInitThreads
    // has not been called
    //VideoPollEvent();
    //VideoThreadInit();
}

///
///	Cleanup video output module.
///
void VideoExit(void)
{
    if (!XlibDisplay) {			// no init or failed
	return;
    }
    //
    //	Reenable screensaver / DPMS.
    //
    X11DPMSReenable(Connection);
    X11SuspendScreenSaver(Connection, 0);

#ifdef USE_VIDEO_THREAD
    VideoThreadExit();
    // VDPAU cleanup hangs in XLockDisplay every 100 exits
    // XUnlockDisplay(XlibDisplay);
    // xcb_flush(Connection);
#endif
    VideoUsedModule->Exit();
    VideoUsedModule = &NoopModule;
#ifdef USE_GLX
    if (GlxEnabled) {
	GlxExit();
    }
#endif

    //
    //	FIXME: cleanup.
    //
    //RandrExit();

    //
    //	X11/xcb cleanup
    //
    if (VideoWindow != XCB_NONE) {
	xcb_destroy_window(Connection, VideoWindow);
	VideoWindow = XCB_NONE;
    }
    if (VideoColormap != XCB_NONE) {
	xcb_free_colormap(Connection, VideoColormap);
	VideoColormap = XCB_NONE;
    }
    if (VideoBlankCursor != XCB_NONE) {
	xcb_free_cursor(Connection, VideoBlankCursor);
	VideoBlankCursor = XCB_NONE;
    }
    if (VideoCursorPixmap != XCB_NONE) {
	xcb_free_pixmap(Connection, VideoCursorPixmap);
	VideoCursorPixmap = XCB_NONE;
    }
    xcb_flush(Connection);
    if (XlibDisplay) {
	if (XCloseDisplay(XlibDisplay)) {
	    Error(_("video: error closing display\n"));
	}
	XlibDisplay = NULL;
	Connection = 0;
    }
}

#endif

#ifdef VIDEO_TEST

#include <getopt.h>

int SysLogLevel;			///< show additional debug informations
uint32_t VideoSwitch;			///< required

int64_t AudioGetDelay(void)		///< required
{
    return 0L;
}

int64_t AudioGetClock(void)		///< required
{
    return AV_NOPTS_VALUE;
}

void FeedKeyPress( __attribute__ ((unused))
    const char *x, __attribute__ ((unused))
    const char *y, __attribute__ ((unused))
    int a, __attribute__ ((unused))
    int b)
{
}

int VideoDecodeInput( __attribute__ ((unused)) VideoStream * stream)
{
    return -1;
}

///
///	Print version.
///
static void PrintVersion(void)
{
    printf("video_test: video tester Version " VERSION
#ifdef GIT_REV
	"(GIT-" GIT_REV ")"
#endif
	",\n\t(c) 2009 - 2012 by Johns\n"
	"\tLicense AGPLv3: GNU Affero General Public License version 3\n");
}

///
///	Print usage.
///
static void PrintUsage(void)
{
    printf("Usage: video_test [-?dhv]\n"
	"\t-d\tenable debug, more -d increase the verbosity\n"
	"\t-? -h\tdisplay this message\n" "\t-v\tdisplay version information\n"
	"Only idiots print usage on stderr!\n");
}

///
///	Main entry point.
///
///	@param argc	number of arguments
///	@param argv	arguments vector
///
///	@returns -1 on failures, 0 clean exit.
///
int main(int argc, char *const argv[])
{
    uint32_t start_tick;
    uint32_t tick;
    int n;
    VideoHwDecoder *video_hw_decoder;

    SysLogLevel = 0;

    //
    //	Parse command line arguments
    //
    for (;;) {
	switch (getopt(argc, argv, "hv?-c:dg:")) {
	    case 'd':			// enabled debug
		++SysLogLevel;
		continue;
	    case 'g':			// geometry
		if (VideoSetGeometry(optarg) < 0) {
		    fprintf(stderr,
			_
			("Bad formated geometry please use: [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]\n"));
		    return 0;
		}
		continue;

	    case EOF:
		break;
	    case 'v':			// print version
		PrintVersion();
		return 0;
	    case '?':
	    case 'h':			// help usage
		PrintVersion();
		PrintUsage();
		return 0;
	    case '-':
		PrintVersion();
		PrintUsage();
		fprintf(stderr, "\nWe need no long options\n");
		return -1;
	    case ':':
		PrintVersion();
		fprintf(stderr, "Missing argument for option '%c'\n", optopt);
		return -1;
	    default:
		PrintVersion();
		fprintf(stderr, "Unkown option '%c'\n", optopt);
		return -1;
	}
	break;
    }
    if (optind < argc) {
	PrintVersion();
	while (optind < argc) {
	    fprintf(stderr, "Unhandled argument '%s'\n", argv[optind++]);
	}
	return -1;
    }
    //
    //	  main loop
    //
    VideoInit(NULL);
    VideoOsdInit();
    video_hw_decoder = VideoNewHwDecoder(NULL);
    start_tick = GetMsTicks();
    n = 0;
    for (;;) {
#if 0
	VideoRenderOverlay();
	VideoDisplayOverlay();
	glXSwapBuffers(XlibDisplay, VideoWindow);
	GlxCheck();
	glClear(GL_COLOR_BUFFER_BIT);

	XSync(XlibDisplay, False);
	XFlush(XlibDisplay);
#endif
#ifdef USE_VAAPI
	if (VideoVaapiEnabled) {
	    VaapiDisplayFrame();
	}
#endif
#ifdef USE_VDPAU
	if (VideoVdpauEnabled) {
	    VdpauDisplayFrame();
	}
#endif
	tick = GetMsTicks();
	n++;
	if (!(n % 100)) {
	    printf("%dms / frame\n", (tick - start_tick) / n);
	}
	usleep(2 * 1000);
    }
    VideoExit();

    return 0;
}

#endif
