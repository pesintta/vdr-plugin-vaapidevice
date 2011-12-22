///
///	@file video.c	@brief Video module
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

///
///	@defgroup Video The video module.
///
///	This module contains all video rendering functions.
///
///	@todo hide mouse cursor support
///
///	Uses Xlib where it is needed for VA-API or vdpau.  XCB is used for
///	everything else.
///
///	- X11
///	- OpenGL rendering
///	- OpenGL rendering with GLX texture-from-pixmap
///	- Xrender rendering
///

#define USE_XLIB_XCB
#define noUSE_GLX
#define noUSE_DOUBLEBUFFER

//#define USE_VAAPI				///< enable vaapi support
//#define USE_VDPAU				///< enable vdpau support
#define noUSE_BITMAP			///< use vdpau bitmap surface

#define USE_VIDEO_THREAD

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
#endif

#ifdef USE_XLIB_XCB
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <xcb/xcb.h>
#include <xcb/bigreq.h>
#include <xcb/dpms.h>
#include <xcb/glx.h>
#include <xcb/randr.h>
#include <xcb/screensaver.h>
#include <xcb/shm.h>
#include <xcb/xv.h>

#include <xcb/xcb_image.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#endif

#ifdef USE_GLX
#include <GL/glx.h>
// only for gluErrorString
#include <GL/glu.h>
#endif

#ifdef USE_VAAPI
#include <va/va_x11.h>
#ifdef USE_GLX
#include <va/va_glx.h>
#endif
#endif

#ifdef USE_VDPAU
#include <vdpau/vdpau_x11.h>
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
///	Video deinterlace modes.
///
typedef enum _video_deinterlace_modes_
{
    VideoDeinterlaceBob,		///< bob deinterlace
    VideoDeinterlaceWeave,		///< weave deinterlace
    VideoDeinterlaceTemporal,		///< temporal deinterlace
    VideoDeinterlaceTemporalSpatial,	///< temporal spatial deinterlace
    VideoDeinterlaceSoftware,		///< software deinterlace
} VideoDeinterlaceModes;

///
///	Video scalinng modes.
///
typedef enum _video_scaling_modes_
{
    VideoScalingNormal,			///< normal scaling
    VideoScalingFast,			///< fastest scaling
    VideoScalingHQ,			///< high quality scaling
    VideoScalingAnamorphic,		///< anamorphic scaling
} VideoScalingModes;

//----------------------------------------------------------------------------
//	Defines
//----------------------------------------------------------------------------

#define CODEC_SURFACES_MAX	31	///< maximal of surfaces
#define CODEC_SURFACES_DEFAULT	(21+4)	///< default of surfaces
#define CODEC_SURFACES_MPEG2	3	///< 1 decode, up to  2 references
#define CODEC_SURFACES_MPEG4	3	///< 1 decode, up to  2 references
#define CODEC_SURFACES_H264	21	///< 1 decode, up to 20 references
#define CODEC_SURFACES_VC1	3	///< 1 decode, up to  2 references

#define VIDEO_SURFACES_MAX	4	///< video output surfaces for queue
#define OUTPUT_SURFACES_MAX	4	///< output surfaces for flip page

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

static Display *XlibDisplay;		///< Xlib X11 display
static xcb_connection_t *Connection;	///< xcb connection
static xcb_colormap_t VideoColormap;	///< video colormap
static xcb_window_t VideoWindow;	///< video window

static int VideoWindowX;		///< video output window x coordinate
static int VideoWindowY;		///< video outout window y coordinate
static unsigned VideoWindowWidth;	///< video output window width
static unsigned VideoWindowHeight;	///< video output window height

    /// Default deinterlace mode
static VideoDeinterlaceModes VideoDeinterlace;

    /// Default scaling mode
static VideoScalingModes VideoScaling;

    /// Default audio/video delay
static int VideoAudioDelay;

//static char VideoSoftStartSync;		///< soft start sync audio/video

static char Video60HzMode;		///< handle 60hz displays

static xcb_atom_t WmDeleteWindowAtom;	///< WM delete message

extern uint32_t VideoSwitch;		///< ticks for channel switch

#ifdef USE_VIDEO_THREAD

static pthread_t VideoThread;		///< video decode thread
static pthread_cond_t VideoWakeupCond;	///< wakeup condition variable
static pthread_mutex_t VideoMutex;	///< video condition mutex
static pthread_mutex_t VideoLockMutex;	///< video lock mutex

#endif

//----------------------------------------------------------------------------
//	Functions
//----------------------------------------------------------------------------

static void VideoThreadLock(void);	///< lock video thread
static void VideoThreadUnlock(void);	///< unlock video thread

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

/**
**	Render texture.
**
**	@param texture	2d texture
*/
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

/**
**	Upload texture.
*/
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

/**
**	Render to glx texture.
*/
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
	Debug(3, "video/glx: %5d frame rate %d ms\n", count, end - start);
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
//	VA-API
//----------------------------------------------------------------------------

#ifdef USE_VAAPI

static int VideoVaapiEnabled = 1;	///< use VA-API decoder
static int VaapiBuggyVdpau;		///< fix libva-driver-vdpau bugs
static int VaapiBuggyIntel;		///< fix libva-driver-intel bugs

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
    unsigned SurfaceFlags;		///< flags for put surface

    xcb_window_t Window;		///< output window
    int OutputX;			///< output window x
    int OutputY;			///< output window y
    int OutputWidth;			///< output window width
    int OutputHeight;			///< output window height

    enum PixelFormat PixFmt;		///< ffmpeg frame pixfmt
    int WrongInterlacedWarned;		///< warning about interlace flag issued
    int Interlaced;			///< ffmpeg interlaced flag
    int TopFieldFirst;			///< ffmpeg top field displayed first

    VAImage DeintImages[3];		///< deinterlace image buffers

    VAImage Image[1];			///< image buffer to update surface

    struct vaapi_context VaapiContext[1];	///< ffmpeg VA-API context

    int SurfaceUsedN;			///< number of used surfaces
    /// used surface ids
    VASurfaceID SurfacesUsed[CODEC_SURFACES_MAX];
    int SurfaceFreeN;			///< number of free surfaces
    /// free surface ids
    VASurfaceID SurfacesFree[CODEC_SURFACES_MAX];

    int InputX;				///< input x
    int InputY;				///< input y
    int InputWidth;			///< input width
    int InputHeight;			///< input height
    AVRational InputAspect;		///< input aspect ratio

#ifdef USE_GLX
    GLuint GlTexture[2];		///< gl texture for VA-API
    void *GlxSurface[2];		///< VA-API/GLX surface
#endif
    VASurfaceID BlackSurface;		///< empty black surface

    /// video surface ring buffer
    VASurfaceID SurfacesRb[VIDEO_SURFACES_MAX];
    int SurfaceWrite;			///< write pointer
    int SurfaceRead;			///< read pointer
    atomic_t SurfacesFilled;		///< how many of the buffer is used

    int SurfaceField;			///< current displayed field
    int DropNextFrame;			///< flag drop next frame
    int DupNextFrame;			///< flag duplicate next frame
    struct timespec FrameTime;		///< time of last display
    struct timespec StartTime;		///< decoder start time
    int64_t PTS;			///< video PTS clock

    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
};

static VaapiDecoder *VaapiDecoders[1];	///< open decoder streams
static int VaapiDecoderN;		///< number of decoder streams

    /// forward display back surface
static void VaapiBlackSurface(VaapiDecoder * decoder);

//----------------------------------------------------------------------------
//	VA-API Functions
//----------------------------------------------------------------------------

//	Surfaces -------------------------------------------------------------

///
///	Create surfaces for VA-API decoder.
///
///	@param decoder	VA-API decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void VaapiCreateSurfaces(VaapiDecoder * decoder, int width, int height)
{
    Debug(3, "video/vaapi: %s: %dx%d * %d\n", __FUNCTION__, width, height,
	CODEC_SURFACES_DEFAULT);

    // FIXME: allocate only the number of needed surfaces
    decoder->SurfaceFreeN = CODEC_SURFACES_DEFAULT;
    // VA_RT_FORMAT_YUV420 VA_RT_FORMAT_YUV422 VA_RT_FORMAT_YUV444
    if (vaCreateSurfaces(decoder->VaDisplay, width, height,
	    VA_RT_FORMAT_YUV420, decoder->SurfaceFreeN,
	    decoder->SurfacesFree) != VA_STATUS_SUCCESS) {
	Fatal(_("video/vaapi: can't create %d surfaces\n"),
	    decoder->SurfaceFreeN);
	// FIXME: write error handler / fallback
    }
    //
    //	update OSD associate
    //
    if (VaOsdSubpicture == VA_INVALID_ID) {
	Warning(_("video/vaapi: no osd subpicture yet\n"));
	return;
    }
#if 0
    // FIXME: try to fix intel osd bugs
    if (vaDestroySubpicture(VaDisplay, VaOsdSubpicture)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't destroy subpicture\n"));
    }
    VaOsdSubpicture = VA_INVALID_ID;

    if (vaCreateSubpicture(VaDisplay, VaOsdImage.image_id,
	    &VaOsdSubpicture) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create subpicture\n"));
	return;
    }
#endif

    if (VaapiUnscaledOsd) {
	if (vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN, 0, 0,
		VaOsdImage.width, VaOsdImage.height, 0, 0, VideoWindowWidth,
		VideoWindowHeight, VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
    } else {
	int i;

	if (vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN, 0, 0,
		VaOsdImage.width, VaOsdImage.height, 0, 0, width, height, 0)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
	for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	    Debug(3, "video/vaapi: associate %08x\n",
		decoder->SurfacesFree[i]);
	}
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

    if (vaDestroySurfaces(decoder->VaDisplay, decoder->SurfacesFree,
	    decoder->SurfaceFreeN)
	!= VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't destroy %d surfaces\n",
	    decoder->SurfaceFreeN);
    }
    decoder->SurfaceFreeN = 0;
    if (vaDestroySurfaces(decoder->VaDisplay, decoder->SurfacesUsed,
	    decoder->SurfaceUsedN)
	!= VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't destroy %d surfaces\n",
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
static VASurfaceID VaapiGetSurface(VaapiDecoder * decoder)
{
    VASurfaceID surface;
    int i;

    if (!decoder->SurfaceFreeN) {
	Error(_("video/vaapi: out of surfaces\n"));
	return VA_INVALID_ID;
    }
    // use oldest surface
    surface = decoder->SurfacesFree[0];

    decoder->SurfaceFreeN--;
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	decoder->SurfacesFree[i] = decoder->SurfacesFree[i + 1];
    }

    // save as used
    decoder->SurfacesUsed[decoder->SurfaceUsedN++] = surface;

    return surface;
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
    Error(_("video/vaapi: release surface %#x, which is not in use\n"),
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
    Debug(3, "video/vaapi: %d missed, %d duped, %d dropped frames of %d\n",
	decoder->FramesMissed, decoder->FramesDuped, decoder->FramesDropped,
	decoder->FrameCounter);
}

///
///	Allocate new VA-API decoder.
///
///	@returns a new prepared va-api hardware decoder.
///
static VaapiDecoder *VaapiNewDecoder(void)
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

    decoder->SurfaceFlags = VA_CLEAR_DRAWABLE;
    // color space conversion none, ITU-R BT.601, ITU-R BT.709
    decoder->SurfaceFlags |= VA_SRC_BT601;

    // scaling flags FAST, HQ, NL_ANAMORPHIC
    // FIXME: need to detect the backend to choose the parameter
    switch (VideoScaling) {
	case VideoScalingNormal:
	    decoder->SurfaceFlags |= VA_FILTER_SCALING_DEFAULT;
	    break;
	case VideoScalingFast:
	    decoder->SurfaceFlags |= VA_FILTER_SCALING_FAST;
	    break;
	case VideoScalingHQ:
	    // vdpau backend supports only VA_FILTER_SCALING_HQ
	    // vdpau backend with advanced deinterlacer and my GT-210
	    // is too slow
	    decoder->SurfaceFlags |= VA_FILTER_SCALING_HQ;
	    break;
	case VideoScalingAnamorphic:
	    // intel backend supports only VA_FILTER_SCALING_NL_ANAMORPHIC;
	    // don't use it, its for 4:3 -> 16:9 scaling
	    decoder->SurfaceFlags |= VA_FILTER_SCALING_NL_ANAMORPHIC;
	    break;
    }

    // deinterlace flags (not yet supported by libva)
    switch (VideoDeinterlace) {
	case VideoDeinterlaceBob:
	    break;
	case VideoDeinterlaceWeave:
	    break;
	case VideoDeinterlaceTemporal:
	    //FIXME: private hack
	    //decoder->SurfaceFlags |= 0x00002000;
	    break;
	case VideoDeinterlaceTemporalSpatial:
	    //FIXME: private hack
	    //decoder->SurfaceFlags |= 0x00006000;
	    break;
	case VideoDeinterlaceSoftware:
	    break;
    }

    decoder->DeintImages[0].image_id = VA_INVALID_ID;
    decoder->DeintImages[1].image_id = VA_INVALID_ID;
    decoder->DeintImages[2].image_id = VA_INVALID_ID;

    decoder->Image->image_id = VA_INVALID_ID;

    // setup video surface ring buffer
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VA_INVALID_ID;
    }

    decoder->BlackSurface = VA_INVALID_ID;

    //
    //	Setup ffmpeg vaapi context
    //
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

#ifdef noDEBUG
    // FIXME: for play
    decoder->OutputX = 40;
    decoder->OutputY = 40;
    decoder->OutputWidth = VideoWindowWidth - 40 * 2;
    decoder->OutputHeight = VideoWindowHeight - 40 * 2;
#endif

    VaapiDecoders[VaapiDecoderN++] = decoder;

    return decoder;
}

/**
**	Cleanup VA-API.
**
**	@param decoder	va-api hw decoder
*/
static void VaapiCleanup(VaapiDecoder * decoder)
{
    int filled;
    VASurfaceID surface;

    // flush output queue, only 1-2 frames buffered, no big loss
    while ((filled = atomic_read(&decoder->SurfacesFilled))) {
	decoder->SurfaceRead = (decoder->SurfaceRead + 1) % VIDEO_SURFACES_MAX;
	atomic_dec(&decoder->SurfacesFilled);

	surface = decoder->SurfacesRb[decoder->SurfaceRead];
	if (surface == VA_INVALID_ID) {
	    printf(_("video/vaapi: invalid surface in ringbuffer\n"));
	    Error(_("video/vaapi: invalid surface in ringbuffer\n"));
	    continue;
	}
	// can crash and hang
	if (0 && vaSyncSurface(decoder->VaDisplay, surface)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
    }

    if (decoder->SurfaceRead != decoder->SurfaceWrite) {
	abort();
    }

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

    decoder->PTS = AV_NOPTS_VALUE;
    clock_gettime(CLOCK_REALTIME, &decoder->StartTime);
}

///
///	Destroy a VA-API decoder.
///
///	@param decoder	VA-API decoder
///
static void VaapiDelDecoder(VaapiDecoder * decoder)
{
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
    // FIXME: decoder->DeintImages
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

/**
**	VA-API setup.
**
**	@param display_name	x11/xcb display name
*/
static void VideoVaapiInit(const char *display_name)
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
	Error(_("video/vaapi: Can't connect VA-API to X11 server on '%s'"),
	    display_name);
	// FIXME: no fatal for plugin
	return;
    }

    if (vaInitialize(VaDisplay, &major, &minor) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: Can't inititialize VA-API on '%s'"),
	    display_name);
	vaTerminate(VaDisplay);
	VaDisplay = NULL;
	return;
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
    if (strstr(s, "Intel i965")) {
	VaapiBuggyIntel = 1;
    }
    //
    //	check if driver makes a copy of the VA surface for display.
    //
    attr.type = VADisplayAttribDirectSurface;
    attr.flags = VA_DISPLAY_ATTRIB_GETTABLE;
    if (vaGetDisplayAttributes(VaDisplay, &attr, 1) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: Can't get direct-surface attribute\n"));
	attr.value = 1;
    }
    Info(_("video/vaapi: VA surface is %s\n"),
	attr.value ? _("direct mapped") : _("copied"));
    // FIXME: handle the cases: new liba: Don't use it.

#if 0
    //
    //	check the chroma format
    //
    attr.type = VAConfigAttribRTFormat attr.flags = VA_DISPLAY_ATTRIB_GETTABLE;
#endif
}

///
///	VA-API cleanup
///
static void VideoVaapiExit(void)
{
    int i;

    // FIXME: more VA-API cleanups...
    // FIXME: can hang with vdpau in pthread_rwlock_wrlock

    for (i = 0; i < VaapiDecoderN; ++i) {
	if (VaapiDecoders[i]) {
	    VaapiDelDecoder(VaapiDecoders[i]);
	    VaapiDecoders[i] = NULL;
	}
    }
    VaapiDecoderN = 0;

    if (VaOsdImage.image_id != VA_INVALID_ID) {
	if (vaDestroyImage(VaDisplay,
		VaOsdImage.image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	VaOsdImage.image_id = VA_INVALID_ID;
    }

    if (VaOsdSubpicture != VA_INVALID_ID) {
	// still has 35 surfaces associated to it
	if (vaDestroySubpicture(VaDisplay, VaOsdSubpicture)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy subpicture\n"));
	}
	VaOsdSubpicture = VA_INVALID_ID;
    }

    if (!VaDisplay) {
	vaTerminate(VaDisplay);
	VaDisplay = NULL;
    }
}

/**
**	Update output for new size or aspect ratio.
**
**	@param decoder	VA-API decoder
*/
static void VaapiUpdateOutput(VaapiDecoder * decoder)
{
    AVRational input_aspect_ratio;
    AVRational display_aspect_ratio;

    input_aspect_ratio = decoder->InputAspect;
    if (!input_aspect_ratio.num || !input_aspect_ratio.den) {
	input_aspect_ratio.num = 1;
	input_aspect_ratio.den = 1;
	Debug(3, "video: aspect defaults to %d:%d\n", input_aspect_ratio.num,
	    input_aspect_ratio.den);
    }

    av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
	decoder->InputWidth * input_aspect_ratio.num,
	decoder->InputHeight * input_aspect_ratio.den, 1024 * 1024);

    Debug(3, "video: aspect %d : %d\n", display_aspect_ratio.num,
	display_aspect_ratio.den);

    // FIXME: store different positions for the ratios

    decoder->OutputX = 0;
    decoder->OutputY = 0;
    decoder->OutputWidth = (VideoWindowHeight * display_aspect_ratio.num)
	/ display_aspect_ratio.den;
    decoder->OutputHeight = (VideoWindowWidth * display_aspect_ratio.num)
	/ display_aspect_ratio.den;
    if ((unsigned)decoder->OutputWidth > VideoWindowWidth) {
	decoder->OutputWidth = VideoWindowWidth;
	decoder->OutputY = (VideoWindowHeight - decoder->OutputHeight) / 2;
    } else {
	decoder->OutputHeight = VideoWindowHeight;
	decoder->OutputX = (VideoWindowWidth - decoder->OutputWidth) / 2;
    }
}

/**
**	Find VA-API profile.
**
**	Check if the requested profile is supported by VA-API.
**
**	@param profiles	a table of all supported profiles
**	@param n	number of supported profiles
**	@param profile	requested profile
**
**	@returns the profile if supported, -1 if unsupported.
*/
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

/**
**	Find VA-API entry point.
**
**	Check if the requested entry point is supported by VA-API.
**
**	@param entrypoints	a table of all supported entrypoints
**	@param n		number of supported entrypoints
**	@param entrypoint	requested entrypoint
**
**	@returns the entry point if supported, -1 if unsupported.
*/
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

/**
**	Callback to negotiate the PixelFormat.
**
**	@param fmt	is the list of formats which are supported by the codec,
**			it is terminated by -1 as 0 is a valid format, the
**			formats are ordered by quality.
*/
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

    Debug(3, "video: new stream format %d\n", GetMsTicks() - VideoSwitch);

    // create initial black surface and display
    VaapiBlackSurface(decoder);
    VaapiCleanup(decoder);

    if (getenv("NO_HW")) {		// FIXME: make config option
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
	    p = VaapiFindProfile(profiles, profile_n, VAProfileMPEG2Main);
	    break;
	case CODEC_ID_MPEG4:
	case CODEC_ID_H263:
	    p = VaapiFindProfile(profiles, profile_n,
		VAProfileMPEG4AdvancedSimple);
	    break;
	case CODEC_ID_H264:
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
	    p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Main);
	    break;
	case CODEC_ID_VC1:
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
    //	prepare decoder
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
    // create a configuration for the decode pipeline
    if (vaCreateConfig(decoder->VaDisplay, p, e, &attrib, 1,
	    &decoder->VaapiContext->config_id)) {
	Error(_("codec: can't create config"));
	goto slow_path;
    }
    // FIXME: need only to create and destroy surfaces for size changes!
    VaapiCreateSurfaces(decoder, video_ctx->width, video_ctx->height);

    // bind surfaces to context
    if (vaCreateContext(decoder->VaDisplay, decoder->VaapiContext->config_id,
	    video_ctx->width, video_ctx->height, VA_PROGRESSIVE,
	    decoder->SurfacesFree, decoder->SurfaceFreeN,
	    &decoder->VaapiContext->context_id)) {
	Error(_("codec: can't create context"));
	goto slow_path;
    }

    decoder->InputX = 0;
    decoder->InputY = 0;
    decoder->InputWidth = video_ctx->width;
    decoder->InputHeight = video_ctx->height;
    decoder->InputAspect = video_ctx->sample_aspect_ratio;
    VaapiUpdateOutput(decoder);

#ifdef USE_GLX
    if (GlxEnabled) {
	GlxSetupDecoder(decoder);
	// FIXME: try two textures, but vdpau-backend supports only 1 surface
	if (vaCreateSurfaceGLX(decoder->VaDisplay, GL_TEXTURE_2D,
		decoder->GlTexture[0], &decoder->GlxSurface[0])
	    != VA_STATUS_SUCCESS) {
	    Fatal(_("video: can't create glx surfaces"));
	}
	// FIXME: this isn't usable with vdpau-backend
	/*
	   if (vaCreateSurfaceGLX(decoder->VaDisplay, GL_TEXTURE_2D,
	   decoder->GlTexture[1], &decoder->GlxSurface[1])
	   != VA_STATUS_SUCCESS) {
	   Fatal(_("video: can't create glx surfaces"));
	   }
	 */
    }
#endif

    Debug(3, "\tpixel format %#010x\n", *fmt_idx);
    return *fmt_idx;

  slow_path:
    // no accelerated format found
    video_ctx->hwaccel_context = NULL;
    return avcodec_default_get_format(video_ctx, fmt);
}

/**
**	Draw surface of the VA-API decoder with x11.
**
**	vaPutSurface with intel backend does sync on v-sync.
**
**	@param decoder	VA-API decoder
**	@param surface		VA-API surface id
**	@param interlaced	flag interlaced source
**	@param top_field_first	flag top_field_first for interlaced source
**	@param field		interlaced draw: 0 first field, 1 second field
*/
static void VaapiPutSurfaceX11(VaapiDecoder * decoder, VASurfaceID surface,
    int interlaced, int top_field_first, int field)
{
    unsigned type;
    VAStatus status;

    // deinterlace
    if (interlaced && VideoDeinterlace != VideoDeinterlaceWeave) {
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

    xcb_flush(Connection);
    if ((status = vaPutSurface(decoder->VaDisplay, surface, decoder->Window,
		// decoder src
		decoder->InputX, decoder->InputY, decoder->InputWidth,
		decoder->InputHeight,
		// video dst
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth,
		decoder->OutputHeight, NULL, 0,
		type | decoder->SurfaceFlags)) != VA_STATUS_SUCCESS) {
	// switching video kills VdpPresentationQueueBlockUntilSurfaceIdle
	Error(_("video/vaapi: vaPutSurface failed %d\n"), status);
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
		("video/vaapi: surface %#x not ready: still displayed %d\n"),
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
		Error(_("video: vaQuerySurface failed\n"));
	    }
	    Debug(3, "video/vaapi: %2d %d\n", i, status);
	    usleep(1 * 1000);
	}
    }

}

#ifdef USE_GLX

/**
**	Render texture.
**
**	@param texture	2d texture
*/
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

/**
**	Draw surface of the VA-API decoder with glx.
**
**	@param decoder	VA-API decoder
**	@param surface		VA-API surface id
**	@param interlaced	flag interlaced source
**	@param top_field_first	flag top_field_first for interlaced source
**	@param field		interlaced draw: 0 first field, 1 second field
*/
static void VaapiPutSurfaceGLX(VaapiDecoder * decoder, VASurfaceID surface,
    int interlaced, int top_field_first, int field)
{
    unsigned type;
    uint32_t start;
    uint32_t copy;
    uint32_t end;

    // deinterlace
    if (interlaced && VideoDeinterlace != VideoDeinterlaceWeave) {
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
	    type | decoder->SurfaceFlags) != VA_STATUS_SUCCESS) {
	Error(_("video: vaCopySurfaceGLX failed\n"));
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

/**
**	Find VA-API image format.
**
**	@param decoder	VA-API decoder
**	@param pix_fmt		ffmpeg pixel format
**	@param[out] format	image format
**
**	FIXME: can fallback from I420 to YV12, if not supported
**	FIXME: must check if put/get with this format is supported (see intel)
*/
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
	    fourcc = VA_FOURCC_YV12;	// YVU
	    fourcc = VA_FOURCC('I', '4', '2', '0');	// YUV
	    // FIXME: intel deinterlace ... only supported with nv12
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

    return 0;
}

/**
**	Configure VA-API for new video format.
**
**	@param decoder	VA-API decoder
**
**	@note called only for software decoder.
*/
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
    if (decoder->Image->image_id != VA_INVALID_ID) {
	if (vaDestroyImage(VaDisplay, decoder->Image->image_id)
	    != VA_STATUS_SUCCESS) {
	    Error("video: can't destroy image!\n");
	}
    }
    VaapiFindImageFormat(decoder, video_ctx->pix_fmt, format);

    if (vaCreateImage(VaDisplay, format, width, height,
	    decoder->Image) != VA_STATUS_SUCCESS) {
	Fatal("video: can't create image!\n");
    }
    Debug(3,
	"video/vaapi: created image %dx%d with id 0x%08x and buffer id 0x%08x\n",
	width, height, decoder->Image->image_id, decoder->Image->buf);

    VaapiCreateSurfaces(decoder, width, height);

#ifdef USE_GLX
    if (GlxEnabled) {
	// FIXME: destroy old context

	GlxSetupDecoder(decoder);
	// FIXME: try two textures
	if (vaCreateSurfaceGLX(decoder->VaDisplay, GL_TEXTURE_2D,
		decoder->GlTexture[0], &decoder->GlxSurface[0])
	    != VA_STATUS_SUCCESS) {
	    Fatal(_("video: can't create glx surfaces"));
	}
	/*
	   if (vaCreateSurfaceGLX(decoder->VaDisplay, GL_TEXTURE_2D,
	   decoder->GlTexture[1], &decoder->GlxSurface[1])
	   != VA_STATUS_SUCCESS) {
	   Fatal(_("video: can't create glx surfaces"));
	   }
	 */
    }
#endif
}

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
	    if (!(decoder->FramesDisplayed % 100)) {
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

	if (vaSyncSurface(decoder->VaDisplay, old) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
#if 0
	VASurfaceStatus status;

	if (vaQuerySurfaceStatus(decoder->VaDisplay, old, &status)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaQuerySurface failed\n"));
	    status = VASurfaceReady;
	}
	if (status != VASurfaceReady) {
	    Warning(_
		("video/vaapi: surface %#x not ready: still displayed %d\n"),
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
#if 1
    // FIXME: intel seems to forget this, nvidia GT 210 has speed problems here
    if (VaapiBuggyIntel && VaOsdSubpicture != VA_INVALID_ID) {

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

    Debug(4, "video/vaapi: yy video surface %#x ready\n", surface);
}

#if 0

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
static void FilterLine(const uint8_t * past, const uint8_t * cur,
    const uint8_t * future, int width, int above, int below)
{
    int a, b, c, d, e, f, g, h, i, j, k, l, m, n;
}

#endif

///
///	Create and display a black empty surface.
///
///	@param decoder	VA-API decoder
///
static void VaapiBlackSurface(VaapiDecoder * decoder)
{
    VAStatus status;
    uint32_t start;
    uint32_t sync;
    uint32_t put1;

    // wait until we have osd subpicture
    if (VaOsdSubpicture == VA_INVALID_ID) {
	Warning(_("video/vaapi: no osd subpicture yet\n"));
	return;
    }

    if (decoder->BlackSurface == VA_INVALID_ID) {
	if (vaCreateSurfaces(decoder->VaDisplay, VideoWindowWidth,
		VideoWindowHeight, VA_RT_FORMAT_YUV420, 1,
		&decoder->BlackSurface) != VA_STATUS_SUCCESS) {
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

	start = GetMsTicks();
	if (vaSyncSurface(decoder->VaDisplay,
		decoder->BlackSurface) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
    } else {
	start = GetMsTicks();
    }

    Debug(4, "video/vaapi: yy black video surface %#x displayed\n",
	decoder->BlackSurface);
    sync = GetMsTicks();
    xcb_flush(Connection);
    if ((status =
	    vaPutSurface(decoder->VaDisplay, decoder->BlackSurface,
		decoder->Window,
		// decoder src
		0, 0, VideoWindowWidth, VideoWindowHeight,
		// video dst
		0, 0, VideoWindowWidth, VideoWindowHeight, NULL, 0,
		VA_FRAME_PICTURE)) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaPutSurface failed %d\n"), status);
    }
    clock_gettime(CLOCK_REALTIME, &decoder->FrameTime);

    put1 = GetMsTicks();
    Debug(4, "video/vaapi: sync %2u put1 %2u\n", sync - start, put1 - sync);

    if (0
	&& vaSyncSurface(decoder->VaDisplay,
	    decoder->BlackSurface) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }

    usleep(500);
}

///
///	Vaapi bob deinterlace.
///
static void VaapiBob(VaapiDecoder * decoder, VAImage * src, VAImage * dst1,
    VAImage * dst2)
{
    void *src_base;
    void *dst1_base;
    void *dst2_base;
    unsigned y;
    unsigned p;

    if (vaMapBuffer(decoder->VaDisplay, src->buf,
	    &src_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
    if (vaMapBuffer(decoder->VaDisplay, dst1->buf,
	    &dst1_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }
    if (vaMapBuffer(decoder->VaDisplay, dst2->buf,
	    &dst2_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }

    if (1) {
	memset(dst1_base, 0x00, dst1->data_size);
	memset(dst2_base, 0x00, dst2->data_size);
    }
    for (p = 0; p < src->num_planes; ++p) {
	for (y = 0; y < (unsigned)(src->height >> (p != 0)); y += 2) {
	    memcpy(dst1_base + src->offsets[p] + y * src->pitches[p],
		src_base + src->offsets[p] + y * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst1_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + y * src->pitches[p],
		src->pitches[p]);

	    memcpy(dst2_base + src->offsets[p] + y * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	    memcpy(dst2_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src_base + src->offsets[p] + (y + 1) * src->pitches[p],
		src->pitches[p]);
	}
    }

    if (vaUnmapBuffer(decoder->VaDisplay, dst2->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
    if (vaUnmapBuffer(decoder->VaDisplay, dst1->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
    if (vaUnmapBuffer(decoder->VaDisplay, src->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }
}

///
///	Vaapi software deinterlace.
///
static void VaapiCpuDeinterlace(VaapiDecoder * decoder, VASurfaceID surface)
{
#if 0
    VAImage image[1];
    VAStatus status;
    VAImageFormat format[1];
    void *image_base;
    int image_derived;

    // release old frame
    // get new frame
    // deinterlace

    image_derived = 1;
    if ((status =
	    vaDeriveImage(decoder->VaDisplay, surface,
		image)) != VA_STATUS_SUCCESS) {
	image_derived = 0;
	Warning(_("video/vaapi: vaDeriveImage failed %d\n"), status);
	// NV12, YV12, I420, BGRA
	VaapiFindImageFormat(decoder, PIX_FMT_YUV420P, format);
	if (vaCreateImage(decoder->VaDisplay, format, decoder->InputWidth,
		decoder->InputHeight, image) != VA_STATUS_SUCCESS) {
	    Fatal(_("video/vaapi: can't create image!\n"));
	}
	if (vaGetImage(decoder->VaDisplay, surface, 0, 0, decoder->InputWidth,
		decoder->InputHeight, image->image_id) != VA_STATUS_SUCCESS) {
	    Fatal(_("video/vaapi: can't get image!\n"));
	}
    }
    Debug(3, "video/vaapi: %c%c%c%c %dx%d*%d\n", image->format.fourcc,
	image->format.fourcc >> 8, image->format.fourcc >> 16,
	image->format.fourcc >> 24, image->width, image->height,
	image->num_planes);

    if (vaMapBuffer(decoder->VaDisplay, image->buf,
	    &image_base) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't map the image!\n");
    }

    memset(image_base, 0xff, image->width * image->height);

    if (vaUnmapBuffer(decoder->VaDisplay, image->buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap image buffer\n"));
    }

    if (!image_derived) {
	if ((status =
		vaPutImage(decoder->VaDisplay, surface, image->image_id, 0, 0,
		    image->width, image->height, 0, 0, image->width,
		    image->height)) != VA_STATUS_SUCCESS) {
	    Fatal("video/vaapi: can't put image %d!\n", status);
	}
    }

    vaDestroyImage(decoder->VaDisplay, image->image_id);
#endif
    VAImage *img1;
    VAImage *img2;
    VAImage *img3;
    VASurfaceID out1;
    VASurfaceID out2;

    //
    //	Create deinterlace images.
    //
    if (decoder->DeintImages[0].image_id == VA_INVALID_ID) {
	VAImageFormat format[1];
	int i;

	// NV12, YV12, I420, BGRA
	// VaapiFindImageFormat(decoder, PIX_FMT_YUV420P, format);

	// Intel needs NV12
	VaapiFindImageFormat(decoder, PIX_FMT_NV12, format);
	for (i = 0; i < 3; ++i) {
	    if (vaCreateImage(decoder->VaDisplay, format, decoder->InputWidth,
		    decoder->InputHeight,
		    decoder->DeintImages + i) != VA_STATUS_SUCCESS) {
		Fatal(_("video/vaapi: can't create image!\n"));
	    }
	}
	img1 = decoder->DeintImages;
	Debug(3, "video/vaapi: %c%c%c%c %dx%d*%d\n", img1->format.fourcc,
	    img1->format.fourcc >> 8, img1->format.fourcc >> 16,
	    img1->format.fourcc >> 24, img1->width, img1->height,
	    img1->num_planes);
    }

    if (0 && vaSyncSurface(decoder->VaDisplay, surface) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }

    img1 = decoder->DeintImages;
    img2 = decoder->DeintImages + 1;
    img3 = decoder->DeintImages + 2;

    if (vaGetImage(decoder->VaDisplay, surface, 0, 0, decoder->InputWidth,
	    decoder->InputHeight, img1->image_id) != VA_STATUS_SUCCESS) {
	Fatal(_("video/vaapi: can't get img1!\n"));
    }

    VaapiBob(decoder, img1, img2, img3);

    // get a free surface and upload the image
    out1 = VaapiGetSurface(decoder);
    if (vaPutImage(VaDisplay, out1, img2->image_id, 0, 0, img2->width,
	    img2->height, 0, 0, img2->width,
	    img2->height) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't put image!\n");
    }
    VaapiQueueSurface(decoder, out1, 1);
    if (0 && vaSyncSurface(decoder->VaDisplay, out1) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }
    // get a free surface and upload the image
    out2 = VaapiGetSurface(decoder);
    if (vaPutImage(VaDisplay, out2, img3->image_id, 0, 0, img3->width,
	    img3->height, 0, 0, img3->width,
	    img3->height) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't put image!\n");
    }
    VaapiQueueSurface(decoder, out2, 1);
    if (0 && vaSyncSurface(decoder->VaDisplay, out2) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
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

    if (video_ctx->height != decoder->InputHeight
	|| video_ctx->width != decoder->InputWidth) {
	Debug(3, "video/vaapi: stream <-> surface size mismatch\n");
    }
    //
    // Hardware render
    //
    if (video_ctx->hwaccel_context) {
	int interlaced;

	surface = (unsigned)(size_t) frame->data[3];
	Debug(4, "video/vaapi: hw render hw surface %#x\n", surface);

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

	// update aspect ratio changes
#ifdef still_to_detect_define
	if (av_cmp_q(decoder->InputAspect, frame->sample_aspect_ratio)) {
	    Debug(3, "video/vaapi: aspect ratio changed\n");

	    //decoder->InputWidth = video_ctx->width;
	    //decoder->InputHeight = video_ctx->height;
	    decoder->InputAspect = frame->sample_aspect_ratio;
	    VaapiUpdateOutput(decoder);
	}
#else
	if (av_cmp_q(decoder->InputAspect, video_ctx->sample_aspect_ratio)) {
	    Debug(3, "video/vaapi: aspect ratio changed\n");

	    //decoder->InputWidth = video_ctx->width;
	    //decoder->InputHeight = video_ctx->height;
	    decoder->InputAspect = video_ctx->sample_aspect_ratio;
	    VaapiUpdateOutput(decoder);
	}
#endif

	if (VideoDeinterlace == VideoDeinterlaceSoftware && interlaced) {
	    // FIXME: software deinterlace avpicture_deinterlace
	    VaapiCpuDeinterlace(decoder, surface);
	} else {
	    // FIXME: should be done by init
	    if (decoder->Interlaced != interlaced
		|| decoder->TopFieldFirst != frame->top_field_first) {

		Debug(3, "video/vaapi: interlaced %d top-field-first %d\n",
		    interlaced, frame->top_field_first);

		decoder->Interlaced = interlaced;
		decoder->TopFieldFirst = frame->top_field_first;
		decoder->SurfaceField = 1;

	    }
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
	if (decoder->Image->image_id == VA_INVALID_ID
	    || decoder->PixFmt != video_ctx->pix_fmt
	    || width != decoder->InputWidth
	    || height != decoder->InputHeight) {

	    decoder->PixFmt = video_ctx->pix_fmt;
	    decoder->InputX = 0;
	    decoder->InputY = 0;
	    decoder->InputWidth = width;
	    decoder->InputHeight = height;

	    VaapiSetup(decoder, video_ctx);

	    //
	    //	detect interlaced input
	    //
	    Debug(3, "video/vaapi: interlaced %d top-field-first %d\n",
		frame->interlaced_frame, frame->top_field_first);

	    decoder->Interlaced = frame->interlaced_frame;
	    decoder->TopFieldFirst = frame->top_field_first;
	    decoder->SurfaceField = 1;
	    // FIXME: I hope this didn't change in the middle of the stream
	}
	// FIXME: Need to insert software deinterlace here

	//
	//	Copy data from frame to image
	//
	if (vaMapBuffer(VaDisplay, decoder->Image->buf, &va_image_data)
	    != VA_STATUS_SUCCESS) {
	    Fatal("video/vaapi: can't map the image!\n");
	}
	for (i = 0; (unsigned)i < decoder->Image->num_planes; ++i) {
	    picture->data[i] = va_image_data + decoder->Image->offsets[i];
	    picture->linesize[i] = decoder->Image->pitches[i];
	}

	av_picture_copy(picture, (AVPicture *) frame, video_ctx->pix_fmt,
	    width, height);

	if (vaUnmapBuffer(VaDisplay, decoder->Image->buf) != VA_STATUS_SUCCESS) {
	    Fatal("video/vaapi: can't unmap the image!\n");
	}
	// get a free surface and upload the image
	surface = VaapiGetSurface(decoder);

	// FIXME: intel didn't support put image.
	if ((i = vaPutImage(VaDisplay, surface, decoder->Image->image_id, 0, 0,
		    width, height, 0, 0, width, height)
	    ) != VA_STATUS_SUCCESS) {
	    Fatal("video/vaapi: can't put image %d!\n", i);
	}

	VaapiQueueSurface(decoder, surface, 1);
    }

    if (decoder->Interlaced) {
	++decoder->FrameCounter;
    }
}

/**
**	Advance displayed frame.
*/
static void VaapiAdvanceFrame(void)
{
    int i;

    // show any frame as fast as possible
    // we keep always the last frame in the ring buffer

    for (i = 0; i < VaapiDecoderN; ++i) {
	VaapiDecoder *decoder;
	VASurfaceID surface;
	int filled;

	decoder = VaapiDecoders[i];
	filled = atomic_read(&decoder->SurfacesFilled);

	// 0 -> 1
	// 1 -> 0 + advance
	if (decoder->Interlaced) {
	    // FIXME: first frame is never shown
	    if (decoder->SurfaceField) {
		if (filled > 1) {
		    decoder->SurfaceField = 0;
		}
	    } else {
		decoder->SurfaceField = 1;
		return;
	    }
	}

	if (filled > 1) {
	    decoder->SurfaceRead = (decoder->SurfaceRead + 1)
		% VIDEO_SURFACES_MAX;
	    atomic_dec(&decoder->SurfacesFilled);

	    // wait for rendering finished
	    surface = decoder->SurfacesRb[decoder->SurfaceRead];
	    if (vaSyncSurface(decoder->VaDisplay, surface)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaSyncSurface failed\n"));
	    }
	}
    }
}

/**
**	Display a video frame.
**
**	@todo FIXME: add detection of missed frames
*/
static void VaapiDisplayFrame(void)
{
    struct timespec nowtime;
    uint32_t start;
    uint32_t put1;
    uint32_t put2;
    int i;

    // look if any stream have a new surface available
    for (i = 0; i < VaapiDecoderN; ++i) {
	VaapiDecoder *decoder;
	VASurfaceID surface;
	int filled;

	decoder = VaapiDecoders[i];
	decoder->FramesDisplayed++;

	filled = atomic_read(&decoder->SurfacesFilled);
	// no surface availble show black with possible osd
	if (!filled) {
	    VaapiBlackSurface(decoder);
	    continue;
	}

	surface = decoder->SurfacesRb[decoder->SurfaceRead];
#ifdef DEBUG
	if (surface == VA_INVALID_ID) {
	    printf(_("video/vaapi: invalid surface in ringbuffer\n"));
	}
	Debug(4, "video/vaapi: yy video surface %#x displayed\n", surface);
#endif

	start = GetMsTicks();

	// deinterlace and full frame rate
	// VDPAU driver only display a frame, if a full frame is put
	// INTEL driver does the same, but only with 1080i
	if (0 && decoder->Interlaced
	    // FIXME: buggy libva-driver-vdpau, buggy libva-driver-intel
	    && (VaapiBuggyVdpau || (0 && VaapiBuggyIntel
		    && decoder->InputHeight == 1080))
	    && VideoDeinterlace != VideoDeinterlaceWeave) {
	    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		decoder->TopFieldFirst, 0);
	    put1 = GetMsTicks();

	    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		decoder->TopFieldFirst, 1);
	    put2 = GetMsTicks();
	} else {
	    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		decoder->TopFieldFirst, decoder->SurfaceField);
	    put1 = GetMsTicks();
	    put2 = put1;
	}
	clock_gettime(CLOCK_REALTIME, &nowtime);
#ifdef noDEBUG
	if ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
	    * 1000 * 1000 * 1000 + (nowtime.tv_nsec -
		decoder->FrameTime.tv_nsec) > 21 * 1000 * 1000) {
	    Debug(3, "video/vaapi: time/frame too long %ld ms\n",
		((nowtime.tv_sec - decoder->FrameTime.tv_sec)
		    * 1000 * 1000 * 1000 + (nowtime.tv_nsec -
			decoder->FrameTime.tv_nsec)) / (1000 * 1000));
	    Debug(4, "video/vaapi: put1 %2u put2 %2u\n", put1 - start,
		put2 - put1);
	}
	if (put2 > start + 20) {
	    Debug(3, "video/vaapi: putsurface too long %u ms\n", put2 - start);
	}
	Debug(4, "video/vaapi: put1 %2u put2 %2u\n", put1 - start,
	    put2 - put1);
#endif

	decoder->FrameTime = nowtime;

	// fixes: [drm:i915_hangcheck_elapsed] *ERROR* Hangcheck
	//	  timer elapsed... GPU hung
	usleep(1 * 1000);
    }
}

///
///	Sync and display surface.
///
///	@param decoder	VA-API decoder
///
static void VaapiSyncDisplayFrame(VaapiDecoder * decoder)
{
    int filled;
    int64_t audio_clock;
    int64_t video_clock;

    if (!decoder->DupNextFrame && (!Video60HzMode
	    || decoder->FramesDisplayed % 6)) {
	VaapiAdvanceFrame();
    }
    // debug duplicate frames
    filled = atomic_read(&decoder->SurfacesFilled);
    if (filled == 1) {
	decoder->FramesDuped++;
	Warning(_("video: display buffer empty, duping frame (%d/%d)\n"),
	    decoder->FramesDuped, decoder->FrameCounter);
	if (!(decoder->FramesDisplayed % 333)) {
	    VaapiPrintFrames(decoder);
	}
    }

    VaapiDisplayFrame();

    //
    //	audio/video sync
    //
    audio_clock = AudioGetClock();
    video_clock = VideoGetClock();
    // FIXME: audio not known assume 333ms delay

    if (decoder->DupNextFrame) {
	decoder->DupNextFrame = 0;
    } else if ((uint64_t) audio_clock != AV_NOPTS_VALUE
	&& (uint64_t) video_clock != AV_NOPTS_VALUE) {
	// both clocks are known

	if (abs(video_clock - audio_clock) > 5000 * 90) {
	    Debug(3, "video: pts difference too big\n");
	} else if (video_clock > audio_clock + VideoAudioDelay + 30 * 90) {
	    Debug(3, "video: slow down video\n");
	    decoder->DupNextFrame = 1;
	} else if (audio_clock + VideoAudioDelay > video_clock + 50 * 90
	    && filled > 1) {
	    Debug(3, "video: speed up video\n");
	    decoder->DropNextFrame = 1;
	}
    }

    if (decoder->DupNextFrame || decoder->DropNextFrame
	|| !(decoder->FramesDisplayed % (50 * 10))) {
	static int64_t last_video_clock;

	Debug(3,
	    "video: %09" PRIx64 "-%09" PRIx64 " %4" PRId64 " pts %+dms %"
	    PRId64 "\n", audio_clock, video_clock,
	    video_clock - last_video_clock,
	    (int)(audio_clock - video_clock) / 90, AudioGetDelay() / 90);

	last_video_clock = video_clock;
    }
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
    if (!atomic_read(&decoder->SurfacesFilled)) {
	Debug(3, "video: new stream frame %d\n", GetMsTicks() - VideoSwitch);
    }

    if (decoder->DropNextFrame) {	// drop frame requested
	++decoder->FramesDropped;
	Warning(_("video: dropping frame (%d/%d)\n"), decoder->FramesDropped,
	    decoder->FrameCounter);
	if (!(decoder->FramesDisplayed % 100)) {
	    VaapiPrintFrames(decoder);
	}
	decoder->DropNextFrame = 0;
	return;
    }
    // if video output buffer is full, wait and display surface.
    // loop for interlace
    while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	struct timespec abstime;

	abstime = decoder->FrameTime;
	abstime.tv_nsec += 14 * 1000 * 1000;
	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
	    // avoid overflow
	    abstime.tv_sec++;
	    abstime.tv_nsec -= 1000 * 1000 * 1000;
	}

	VideoPollEvent();

	// give osd some time slot
	while (pthread_cond_timedwait(&VideoWakeupCond, &VideoLockMutex,
		&abstime) != ETIMEDOUT) {
	    // SIGUSR1
	    Debug(3, "video/vaapi: pthread_cond_timedwait error\n");
	}

	VaapiSyncDisplayFrame(decoder);
    }

    VaapiRenderFrame(decoder, video_ctx, frame);
}

#if 0

/**
**	Update video pts.
**
**	@param decoder	VA-API decoder
**	@param frame		frame to display
*/
static void VaapiSetPts(VaapiDecoder * decoder, const AVFrame * frame)
{
    int64_t pts;

    // update video clock
    if ((uint64_t) decoder->PTS != AV_NOPTS_VALUE) {
	decoder->PTS += decoder->Interlaced ? 40 * 90 : 20 * 90;
    }
    //pts = frame->best_effort_timestamp;
    pts = frame->pkt_pts;
    if ((uint64_t) pts == AV_NOPTS_VALUE || !pts) {
	// libav: 0.8pre didn't set pts
	pts = frame->pkt_dts;
    }
    if (!pts) {
	pts = AV_NOPTS_VALUE;
    }
    // build a monotonic pts
    if ((uint64_t) decoder->PTS != AV_NOPTS_VALUE) {
	if (pts - decoder->PTS < -10 * 90) {
	    pts = AV_NOPTS_VALUE;
	}
    }
    // libav: sets only pkt_dts which can be 0
    if ((uint64_t) pts != AV_NOPTS_VALUE) {
	if (decoder->PTS != pts) {
	    Debug(3,
		"video: %#012" PRIx64 "->%#012" PRIx64 " %4" PRId64 " pts\n",
		decoder->PTS, pts, pts - decoder->PTS);
	    decoder->PTS = pts;
	}
    }

}

#endif

#ifdef USE_VIDEO_THREAD

/**
**	Handle a va-api display.
**
**	@todo FIXME: only a single decoder supported.
*/
static void VaapiDisplayHandlerThread(void)
{
    int err;
    int filled;
    struct timespec nowtime;
    VaapiDecoder *decoder;

    decoder = VaapiDecoders[0];

    //
    // fill frame output ring buffer
    //
    filled = atomic_read(&decoder->SurfacesFilled);
    err = 1;
    if (filled <= 2) {
	// FIXME: hot polling
	pthread_mutex_lock(&VideoLockMutex);
	// fetch+decode or reopen
	err = VideoDecode();
	pthread_mutex_unlock(&VideoLockMutex);
    }
    if (err) {
	// FIXME: sleep on wakeup
	usleep(5 * 1000);		// nothing buffered
    }

    filled = atomic_read(&decoder->SurfacesFilled);
    clock_gettime(CLOCK_REALTIME, &nowtime);
    // time for one frame over?
    if ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
	* 1000 * 1000 * 1000 + (nowtime.tv_nsec - decoder->FrameTime.tv_nsec) <
	15 * 1000 * 1000) {
	return;
    }

    pthread_mutex_lock(&VideoLockMutex);
    VaapiSyncDisplayFrame(decoder);
    pthread_mutex_unlock(&VideoLockMutex);
}

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
    // 100% transparent
    memset(image_buffer, 0x00, VaOsdImage.data_size);

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
static void VaapiUploadImage(int x, int y, int width, int height,
    const uint8_t * argb)
{
    void *image_buffer;
    int o;

    // osd image available?
    if (VaOsdImage.image_id == VA_INVALID_ID) {
	return;
    }

    Debug(3, "video/vaapi: upload image\n");

    // map osd surface/image into memory.
    if (vaMapBuffer(VaDisplay, VaOsdImage.buf, &image_buffer)
	!= VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't map osd image buffer\n"));
	return;
    }
    // 100% transparent
    //memset(image_buffer, 0x00, VaOsdImage.data_size);

    // FIXME: convert image from ARGB to subpicture format, if not argb

    // copy argb to image
    for (o = 0; o < height; ++o) {
	memcpy(image_buffer + (x + (y + o) * VaOsdImage.width) * 4,
	    argb + o * width * 4, width * 4);
    }

    if (vaUnmapBuffer(VaDisplay, VaOsdImage.buf) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't unmap osd image buffer\n"));
    }
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

    // FIXME: lock
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

    VaapiOsdClear();
    // FIXME: unlock
}

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
    int OutputX;			///< output window x
    int OutputY;			///< output window y
    int OutputWidth;			///< output window width
    int OutputHeight;			///< output window height

    enum PixelFormat PixFmt;		///< ffmpeg frame pixfmt
    int Interlaced;			///< ffmpeg interlaced flag
    int TopFieldFirst;			///< ffmpeg top field displayed first

    int InputX;				///< input x
    int InputY;				///< input y
    int InputWidth;			///< input width
    int InputHeight;			///< input height

#ifdef noyetUSE_GLX
    GLuint GlTexture[2];		///< gl texture for VDPAU
    void *GlxSurface[2];		///< VDPAU/GLX surface
#endif

    VdpVideoMixer VideoMixer;		///< vdp video mixer
    VdpChromaType ChromaType;		///< vdp video surface chroma format

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
    int DropNextFrame;			///< flag drop next frame
    int DupNextFrame;			///< flag duplicate next frame
    struct timespec FrameTime;		///< time of last display
    struct timespec StartTime;		///< decoder start time
    int64_t PTS;			///< video PTS clock

    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
} VdpauDecoder;

static int VideoVdpauEnabled = 1;	///< use VDPAU decoder

static VdpauDecoder *VdpauDecoders[1];	///< open decoder streams
static int VdpauDecoderN;		///< number of decoder streams

static VdpDevice VdpauDevice;		///< VDPAU device
static VdpGetProcAddress *VdpauGetProcAddress;	///< entry point to use

    /// presentation queue target
static VdpPresentationQueueTarget VdpauQueueTarget;
static VdpPresentationQueue VdpauQueue;	///< presentation queue
static VdpColor VdpauBackgroundColor[1];	///< queue background color

static int VdpauHqScalingMax;		///< highest supported scaling level
static int VdpauTemporal;		///< temporal deinterlacer supported
static int VdpauTemporalSpatial;	///< temporal spatial deint. supported
static int VdpauInverseTelecine;	///< inverse telecine deint. supported
static int VdpauSkipChroma;		///< skip chroma deint. supported

    /// display surface ring buffer
static VdpOutputSurface VdpauSurfacesRb[OUTPUT_SURFACES_MAX];
static int VdpauSurfaceIndex;		///< current display surface
static int VdpauSurfaceNotStart;	///< not the first surface

static int VdpauOsdWidth;		///< width of osd surface
static int VdpauOsdHeight;		///< height of osd surface

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

///
///	Function pointer of the VDPAU device.
///
///@{
static VdpGetErrorString *VdpauGetErrorString;
static VdpDeviceDestroy *VdpauDeviceDestroy;
static VdpGenerateCSCMatrix *VdpauGenerateCSCMatrix;
static VdpVideoSurfaceQueryCapabilities *VdpauVideoSurfaceQueryCapabilities;
static VdpVideoSurfaceQueryGetPutBitsYCbCrCapabilities
    *VdpauVideoSurfaceQueryGetPutBitsYCbCrCapabilities;
static VdpVideoSurfaceCreate *VdpauVideoSurfaceCreate;
static VdpVideoSurfaceDestroy *VdpauVideoSurfaceDestroy;
static VdpVideoSurfaceGetParameters *VdpauVideoSurfaceGetParameters;

//static VdpVideoSurfaceGetBitsYCbCr * VdpauVideoSurfaceGetBitsYCbCr;
static VdpVideoSurfacePutBitsYCbCr *VdpauVideoSurfacePutBitsYCbCr;

static VdpOutputSurfaceCreate *VdpauOutputSurfaceCreate;
static VdpOutputSurfaceDestroy *VdpauOutputSurfaceDestroy;

static VdpOutputSurfacePutBitsNative *VdpauOutputSurfacePutBitsNative;

static VdpBitmapSurfaceQueryCapabilities *VdpauBitmapSurfaceQueryCapabilities;
static VdpBitmapSurfaceCreate *VdpauBitmapSurfaceCreate;
static VdpBitmapSurfaceDestroy *VdpauBitmapSurfaceDestroy;

static VdpBitmapSurfacePutBitsNative *VdpauBitmapSurfacePutBitsNative;

static VdpOutputSurfaceRenderOutputSurface *
    VdpauOutputSurfaceRenderOutputSurface;
static VdpOutputSurfaceRenderBitmapSurface *
    VdpauOutputSurfaceRenderBitmapSurface;

static VdpDecoderQueryCapabilities *VdpauDecoderQueryCapabilities;
static VdpDecoderCreate *VdpauDecoderCreate;
static VdpDecoderDestroy *VdpauDecoderDestroy;

static VdpVideoMixerQueryFeatureSupport *VdpauVideoMixerQueryFeatureSupport;
static VdpVideoMixerCreate *VdpauVideoMixerCreate;
static VdpVideoMixerSetFeatureEnables *VdpauVideoMixerSetFeatureEnables;
static VdpVideoMixerSetAttributeValues *VdpauVideoMixerSetAttributeValues;

static VdpVideoMixerDestroy *VdpauVideoMixerDestroy;
static VdpVideoMixerRender *VdpauVideoMixerRender;
static VdpPresentationQueueTargetDestroy *VdpauPresentationQueueTargetDestroy;
static VdpPresentationQueueCreate *VdpauPresentationQueueCreate;
static VdpPresentationQueueDestroy *VdpauPresentationQueueDestroy;
static VdpPresentationQueueSetBackgroundColor
    *VdpauPresentationQueueSetBackgroundColor;

static VdpPresentationQueueGetTime *VdpauPresentationQueueGetTime;
static VdpPresentationQueueDisplay *VdpauPresentationQueueDisplay;
static VdpPresentationQueueBlockUntilSurfaceIdle *
    VdpauPresentationQueueBlockUntilSurfaceIdle;
static VdpPresentationQueueQuerySurfaceStatus *
    VdpauPresentationQueueQuerySurfaceStatus;

static VdpPresentationQueueTargetCreateX11
    *VdpauPresentationQueueTargetCreateX11;
///@}

///
///	Create surfaces for VDPAU decoder.
///
///	@param decoder	VDPAU decoder
///	@param width	surface source/video width
///	@param height	surface source/video height
///
static void VdpauCreateSurfaces(VdpauDecoder * decoder, int width, int height)
{
    int i;

    Debug(3, "video/vdpau: %s %dx%d\n", __FUNCTION__, width, height);

    // FIXME: allocate only the number of needed surfaces
    decoder->SurfaceFreeN = CODEC_SURFACES_DEFAULT;
    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	VdpStatus status;

	status =
	    VdpauVideoSurfaceCreate(decoder->Device, decoder->ChromaType,
	    width, height, decoder->SurfacesFree + i);
	if (status != VDP_STATUS_OK) {
	    Fatal(_("video/vdpau: can't create video surface: %s\n"),
		VdpauGetErrorString(status));
	    // FIXME: no fatal
	}
	Debug(4, "video/vdpau: created video surface %dx%d with id 0x%08x\n",
	    width, height, decoder->SurfacesFree[i]);
    }
}

///
///	Destroy surfaces of VDPAU decoder.
///
///	@param decoder	VDPAU decoder
///
static void VdpauDestroySurfaces(VdpauDecoder * decoder)
{
    int i;
    VdpStatus status;

    Debug(3, "video/vdpau: %s\n", __FUNCTION__);

    for (i = 0; i < decoder->SurfaceFreeN; ++i) {
	status = VdpauVideoSurfaceDestroy(decoder->SurfacesFree[i]);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video surface: %s\n"),
		VdpauGetErrorString(status));
	}
    }
    for (i = 0; i < decoder->SurfaceUsedN; ++i) {
	status = VdpauVideoSurfaceDestroy(decoder->SurfacesUsed[i]);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't destroy video surface: %s\n"),
		VdpauGetErrorString(status));
	}
    }
    decoder->SurfaceFreeN = 0;
    decoder->SurfaceUsedN = 0;
}

///
///	Get a free surface.
///
///	@param decoder	VDPAU decoder
///
///	@returns the oldest free surface
///
static unsigned VdpauGetSurface(VdpauDecoder * decoder)
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

    // save as used
    decoder->SurfacesUsed[decoder->SurfaceUsedN++] = surface;

    return surface;
}

///
///	Release a surface.
///
///	@param decoder	VDPAU decoder
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
    Error(_("video/vdpau: release surface %#x, which is not in use\n"),
	surface);
}

///
///	Debug VDPAU decoder frames drop...
///
///	@param decoder	VDPAU decoder
///
static void VdpauPrintFrames(const VdpauDecoder * decoder)
{
    Debug(3, "video/vdpau: %d missed, %d duped, %d dropped frames of %d\n",
	decoder->FramesMissed, decoder->FramesDuped, decoder->FramesDropped,
	decoder->FrameCounter);
}

///
///	Create and setup VDPAU mixer.
///
///	@param decoder	VDPAU decoder
///
static void VdpauMixerSetup(VdpauDecoder * decoder)
{
    VdpStatus status;
    int i;
    VdpVideoMixerFeature features[13];
    VdpBool enables[13];
    int feature_n;
    VdpVideoMixerParameter paramaters[10];
    void const *values[10];
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
    // FIXME:
    // VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION
    // VDP_VIDEO_MIXER_FEATURE_SHARPNESS
    for (i = VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1;
	i <= VdpauHqScalingMax; ++i) {
	features[feature_n++] = i;
    }

    decoder->ChromaType = chroma_type = VDP_CHROMA_TYPE_420;

    //
    //	Setup parameter/value tables
    //
    paramaters[0] = VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH;
    values[0] = &decoder->InputWidth;
    paramaters[1] = VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT;
    values[1] = &decoder->InputHeight;
    paramaters[2] = VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE;
    values[2] = &chroma_type;
    layers = 0;
    paramaters[3] = VDP_VIDEO_MIXER_PARAMETER_LAYERS;
    values[3] = &layers;
    parameter_n = 4;

    status =
	VdpauVideoMixerCreate(VdpauDevice, feature_n, features, parameter_n,
	paramaters, values, &decoder->VideoMixer);
    if (status != VDP_STATUS_OK) {
	Fatal(_("video/vdpau: can't create video mixer: %s\n"),
	    VdpauGetErrorString(status));
	// FIXME: no fatal errors
    }
    //
    //	Build default enables table
    //
    feature_n = 0;
    if (VdpauTemporal) {
	enables[feature_n] = (VideoDeinterlace == VideoDeinterlaceTemporal
	    || VideoDeinterlace ==
	    VideoDeinterlaceTemporalSpatial) ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL;
	Debug(3, "video/vdpau: temporal deinterlace %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauTemporalSpatial) {
	enables[feature_n] =
	    VideoDeinterlace ==
	    VideoDeinterlaceTemporalSpatial ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] =
	    VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL;
	Debug(3, "video/vdpau: temporal spatial deinterlace %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    if (VdpauInverseTelecine) {
	enables[feature_n] = VDP_FALSE;
	features[feature_n++] = VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE;
	Debug(3, "video/vdpau: inverse telecine %s\n",
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    for (i = VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1;
	i <= VdpauHqScalingMax; ++i) {
	enables[feature_n] =
	    VideoScaling == VideoScalingHQ ? VDP_TRUE : VDP_FALSE;
	features[feature_n++] = i;
	Debug(3, "video/vdpau: high quality scaling %d %s\n",
	    1 + i - VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1,
	    enables[feature_n - 1] ? "enabled" : "disabled");
    }
    VdpauVideoMixerSetFeatureEnables(decoder->VideoMixer, feature_n, features,
	enables);

    /*
       FIXME:
       VdpVideoMixerSetAttributeValues(decoder->Mixer, attribute_n,
       attributes, values);
     */

    //VdpColorStandard color_standard;
    //color_standard = VDP_COLOR_STANDARD_ITUR_BT_601;
    //VdpGenerateCSCMatrix(procamp, standard, &csc_matrix);
}

///
///	Allocate new VDPAU decoder.
///
///	@returns a new prepared vdpau hardware decoder.
///
static VdpauDecoder *VdpauNewDecoder(void)
{
    VdpauDecoder *decoder;
    int i;

    if (VdpauDecoderN == 1) {
	Fatal(_("video/vdpau: out of decoders\n"));
    }

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
	Fatal(_("video/vdpau: out of memory\n"));
    }
    decoder->Device = VdpauDevice;
    decoder->Window = VideoWindow;

    decoder->VideoMixer = VDP_INVALID_HANDLE;

    //
    // setup video surface ring buffer
    //
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VDP_INVALID_HANDLE;
    }
    // we advance before display, to loose no surface, we set it before
    //decoder->SurfaceRead = VIDEO_SURFACES_MAX - 1;
    //decoder->SurfaceField = 1;

#ifdef DEBUG
    if (VIDEO_SURFACES_MAX < 1 + 1 + 1 + 1) {
	Fatal(_
	    ("video/vdpau: need 1 future, 1 current, 1 back and 1 work surface\n"));
    }
#endif

    decoder->OutputWidth = VideoWindowWidth;
    decoder->OutputHeight = VideoWindowHeight;

#ifdef noDEBUG
    // FIXME: for play
    decoder->OutputX = 40;
    decoder->OutputY = 40;
    decoder->OutputWidth -= 40 * 2;
    decoder->OutputHeight -= 40 * 2;
#endif

    // FIXME: hack
    VdpauDecoderN = 1;
    VdpauDecoders[0] = decoder;

    return decoder;
}

///
///	Destroy a VDPAU decoder.
///
///	@param decoder	VDPAU decoder
///
static void VdpauDelDecoder(VdpauDecoder * decoder)
{
    // FIXME: more cleanup
    VdpauDestroySurfaces(decoder);

    VdpauPrintFrames(decoder);

    free(decoder);
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
    }
}

///
///	VDPAU setup.
///
///	@param display_name	x11/xcb display name
///
static void VideoVdpauInit(const char *display_name)
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

    status =
	vdp_device_create_x11(XlibDisplay, DefaultScreen(XlibDisplay),
	&VdpauDevice, &VdpauGetProcAddress);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: Can't create vdp device on display '%s'\n"),
	    display_name);
	VideoVdpauEnabled = 0;
	return;
    }
    // get error function first, for better error messages
    status =
	VdpauGetProcAddress(VdpauDevice, VDP_FUNC_ID_GET_ERROR_STRING,
	(void **)&VdpauGetErrorString);
    if (status != VDP_STATUS_OK) {
	Error(_
	    ("video/vdpau: Can't get function address of 'GetErrorString'\n"));
	VideoVdpauEnabled = 0;
	// FIXME: destroy_x11 VdpauDeviceDestroy
	return;
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
    // VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_GET_BITS_Y_CB_CR, &VdpauVideoSurfaceGetBitsYCbCr, "VideoSurfaceGetBitsYCbCr");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_SURFACE_PUT_BITS_Y_CB_CR,
	&VdpauVideoSurfacePutBitsYCbCr, "VideoSurfacePutBitsYCbCr");
#if 0
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_CAPABILITIES, &, "");
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
#if 0
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_GET_PARAMETERS, &, "");
    VdpauGetProc(VDP_FUNC_ID_OUTPUT_SURFACE_GET_BITS_NATIVE, &, "");
#endif
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
    VdpauGetProc(VDP_FUNC_ID_DECODER_RENDER, &VdpauDecoderRender,
	"DecoderRender");
#endif
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_FEATURE_SUPPORT,
	&VdpauVideoMixerQueryFeatureSupport, "VideoMixerQueryFeatureSupport");
#if 0
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_PARAMETER_SUPPORT, &, "");
    VdpauGetProc(VDP_FUNC_ID_VIDEO_MIXER_QUERY_ATTRIBUTE_SUPPORT, &, "");
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
#if 0
    VdpauGetProc(VDP_FUNC_ID_PREEMPTION_CALLBACK_REGISTER,
	&VdpauPreemptionCallback, "PreemptionCallback");
#endif
    VdpauGetProc(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_CREATE_X11,
	&VdpauPresentationQueueTargetCreateX11,
	"PresentationQueueTargetCreateX11");

    // vdp_preemption_callback_register

    //
    //	Create presentation queue, only one queue pro window
    //

    status =
	VdpauPresentationQueueTargetCreateX11(VdpauDevice, VideoWindow,
	&VdpauQueueTarget);
    if (status != VDP_STATUS_OK) {
	Fatal(_("video/vdpau: can't create presentation queue target: %s\n"),
	    VdpauGetErrorString(status));
	// FIXME: no fatal errors
    }

    status =
	VdpauPresentationQueueCreate(VdpauDevice, VdpauQueueTarget,
	&VdpauQueue);
    if (status != VDP_STATUS_OK) {
	Fatal(_("video/vdpau: can't create presentation queue: %s\n"),
	    VdpauGetErrorString(status));
    }

    VdpauBackgroundColor->red = 0.01;
    VdpauBackgroundColor->green = 0.02;
    VdpauBackgroundColor->blue = 0.03;
    VdpauBackgroundColor->alpha = 1.00;

    VdpauPresentationQueueSetBackgroundColor(VdpauQueue, VdpauBackgroundColor);

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
	VDP_VIDEO_MIXER_ATTRIBUTE_SKIP_CHROMA_DEINTERLACE, &flag);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't query feature '%s': %s\n"),
	    "skip-chroma-deinterlace", VdpauGetErrorString(status));
    } else {
	VdpauSkipChroma = flag;
    }

    // VDP_VIDEO_MIXER_ATTRIBUTE_BACKGROUND_COLOR

    Info(_("video/vdpau: highest supported high quality scaling %d\n"),
	VdpauHqScalingMax - VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 +
	1);
    Info(_("video/vdpau: feature deinterlace temporal %s\n"),
	VdpauTemporal ? _("supported") : _("unsupported"));
    Info(_("video/vdpau: feature deinterlace temporal spatial %s\n"),
	VdpauTemporal ? _("supported") : _("unsupported"));
    Info(_("video/vdpau: attribute skip chroma deinterlace %s\n"),
	VdpauTemporal ? _("supported") : _("unsupported"));

    //
    //	video formats
    //
    flag = VDP_FALSE;
    status =
	VdpauVideoSurfaceQueryCapabilities(VdpauDevice, VDP_CHROMA_TYPE_420,
	&flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't create output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 4:2:0 chroma format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauVideoSurfaceQueryCapabilities(VdpauDevice, VDP_CHROMA_TYPE_422,
	&flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't create output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 4:2:2 chroma format with %dx%d supported\n"),
	    max_width, max_height);
    }
    flag = VDP_FALSE;
    status =
	VdpauVideoSurfaceQueryCapabilities(VdpauDevice, VDP_CHROMA_TYPE_444,
	&flag, &max_width, &max_height);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't create output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
    if (flag) {
	Info(_("video/vdpau: 4:4:4 chroma format with %dx%d supported\n"),
	    max_width, max_height);
    }
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
    //FIXME: format support and size support
    //VdpOutputSurfaceQueryCapabilities

    //
    //	Create display output surfaces
    //
    for (i = 0; i < OUTPUT_SURFACES_MAX; ++i) {
	status =
	    VdpauOutputSurfaceCreate(VdpauDevice, VDP_RGBA_FORMAT_B8G8R8A8,
	    VideoWindowWidth, VideoWindowHeight, VdpauSurfacesRb + i);
	if (status != VDP_STATUS_OK) {
	    Fatal(_("video/vdpau: can't create output surface: %s\n"),
		VdpauGetErrorString(status));
	}
    }
}

///
///	VDPAU cleanup.
///
static void VideoVdpauExit(void)
{
    if (VdpauDecoders[0]) {
	VdpauDelDecoder(VdpauDecoders[0]);
    }

    if (VdpauDevice) {
	if (VdpauQueue) {
	    VdpauPresentationQueueDestroy(VdpauQueue);
	    VdpauQueue = 0;
	}
	if (VdpauQueueTarget) {
	    VdpauPresentationQueueTargetDestroy(VdpauQueueTarget);
	    VdpauQueueTarget = 0;
	}
	// FIXME: more VDPAU cleanups...
	if (VdpauDeviceDestroy) {
	    VdpauDeviceDestroy(VdpauDevice);
	    VdpauDevice = 0;
	}
    }
}

///
///	Check profile supported.
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
	Error(_("video/vdpau: can't queey decoder capabilities: %s\n"),
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
    VdpDecoderProfile profile;
    int i;

    Debug(3, "%s: %18p\n", __FUNCTION__, decoder);
    if (getenv("NO_HW")) {
	goto slow_path;
    }
#ifdef DEBUG
#ifndef FF_API_GET_PIX_FMT_NAME
    Debug(3, "%s: codec %d fmts:\n", __FUNCTION__, video_ctx->codec_id);
    for (i = 0; fmt[i] != PIX_FMT_NONE; ++i) {
	Debug(3, "\t%#010x %s\n", fmt[i], avcodec_get_pix_fmt_name(fmt[i]));
    }
    Debug(3, "\n");
#else
    Debug(3, "%s: codec %d fmts:\n", __FUNCTION__, video_ctx->codec_id);
    for (i = 0; fmt[i] != PIX_FMT_NONE; ++i) {
	Debug(3, "\t%#010x %s\n", fmt[i], av_get_pix_fmt_name(fmt[i]));
    }
    Debug(3, "\n");
#endif
#endif

    // check profile
    switch (video_ctx->codec_id) {
	case CODEC_ID_MPEG2VIDEO:
	    profile =
		VdpauCheckProfile(decoder, VDP_DECODER_PROFILE_MPEG2_MAIN);
	    break;
	case CODEC_ID_MPEG4:
	case CODEC_ID_H263:
	    /*
	       p = VaapiFindProfile(profiles, profile_n,
	       VAProfileMPEG4AdvancedSimple);
	     */
	    break;
	case CODEC_ID_H264:
	    /*
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
	     */
	    break;
	case CODEC_ID_WMV3:
	    /*
	       p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Main);
	     */
	    break;
	case CODEC_ID_VC1:
	    /*
	       p = VaapiFindProfile(profiles, profile_n, VAProfileVC1Advanced);
	     */
	    break;
	default:
	    goto slow_path;
    }
#if 0
    //
    //	prepare decoder
    //
    memset(&attrib, 0, sizeof(attrib));
    attrib.type = VAConfigAttribRTFormat;
    if (vaGetConfigAttributes(decoder->VaDisplay, p, e, &attrib, 1)) {
	Error("codec: can't get attributes");
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
    // only YUV420 supported
    if (!(attrib.value & VA_RT_FORMAT_YUV420)) {
	Warning("codec: YUV 420 not supported");
	goto slow_path;
    }
    // create a configuration for the decode pipeline
    if (vaCreateConfig(decoder->VaDisplay, p, e, &attrib, 1,
	    &decoder->VaapiContext->config_id)) {
	Error("codec: can't create config");
	goto slow_path;
    }

    VaapiCreateSurfaces(decoder, video_ctx->width, video_ctx->height);

    // bind surfaces to context
    if (vaCreateContext(decoder->VaDisplay, decoder->VaapiContext->config_id,
	    video_ctx->width, video_ctx->height, VA_PROGRESSIVE,
	    decoder->SurfacesFree, decoder->SurfaceFreeN,
	    &decoder->VaapiContext->context_id)) {
	Error("codec: can't create context");
	// FIXME: must cleanup
	goto slow_path;
    }

    decoder->InputX = 0;
    decoder->InputY = 0;
    decoder->InputWidth = video_ctx->width;
    decoder->InputHeight = video_ctx->height;

    Debug(3, "\tpixel format %#010x\n", *fmt_idx);
    return *fmt_idx;
#endif
    return *fmt;

  slow_path:
    // no accelerated format found
    video_ctx->hwaccel_context = NULL;
    return avcodec_default_get_format(video_ctx, fmt);
}

///
///	Configure VDPAU for new video format.
///
///	@param decoder		vdpau decoder
///	@param video_ctx	ffmpeg video codec context
///
static void VdpauSetup(VdpauDecoder * decoder,
    const AVCodecContext * video_ctx)
{
    VdpStatus status;
    VdpChromaType chroma_type;
    uint32_t width;
    uint32_t height;

    // decoder->Input... already setup by caller

    if (decoder->VideoMixer != VDP_INVALID_HANDLE) {
	VdpauVideoMixerDestroy(decoder->VideoMixer);
	decoder->VideoMixer = VDP_INVALID_HANDLE;
    }
    VdpauMixerSetup(decoder);

    if (decoder->SurfaceFreeN || decoder->SurfaceUsedN) {
	VdpauDestroySurfaces(decoder);
    }
    VdpauCreateSurfaces(decoder, video_ctx->width, video_ctx->height);

    //	get real surface size
    status =
	VdpauVideoSurfaceGetParameters(decoder->SurfacesFree[0], &chroma_type,
	&width, &height);
    if (status != VDP_STATUS_OK) {
	Fatal(_("video/vdpau: can't get video surface parameters: %s\n"),
	    VdpauGetErrorString(status));
    }
    // vdpau can choose different sizes, must use them for putbits
    if (chroma_type != decoder->ChromaType
	|| width != (uint32_t) video_ctx->width
	|| height != (uint32_t) video_ctx->height) {
	// FIXME: must rewrite the code to support this case
	Fatal(_("video/vdpau: video surface type/size mismatch\n"));
    }
    // FIXME: reset output ring buffer
}

///
///	Render a ffmpeg frame.
///
///	@param decoder		VDPAU decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VdpauRenderFrame(VdpauDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    VdpStatus status;
    VdpVideoSurface surface;
    VdpVideoSurface old;

    //
    // Hardware render
    //
    if (video_ctx->hwaccel_context) {
	surface = (size_t) frame->data[3];

	Debug(2, "video/vdpau: display surface %#x\n", surface);

	// FIXME: should be done by init
	if (decoder->Interlaced != frame->interlaced_frame
	    || decoder->TopFieldFirst != frame->top_field_first) {
	    Debug(3, "video/vdpau: interlaced %d top-field-first %d\n",
		frame->interlaced_frame, frame->top_field_first);
	    decoder->Interlaced = frame->interlaced_frame;
	    decoder->TopFieldFirst = frame->top_field_first;
	}
	//
	// VAImage render
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
	    decoder->InputX = 0;
	    decoder->InputY = 0;
	    decoder->InputWidth = video_ctx->width;
	    decoder->InputHeight = video_ctx->height;

	    //
	    //	detect interlaced input
	    //
	    Debug(3, "video/vdpau: interlaced %d top-field-first %d\n",
		frame->interlaced_frame, frame->top_field_first);

	    decoder->Interlaced = frame->interlaced_frame;
	    decoder->TopFieldFirst = frame->top_field_first;
	    // FIXME: I hope this didn't change in the middle of the stream

	    VdpauSetup(decoder, video_ctx);
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
	}

	// convert ffmpeg order to vdpau
	data[0] = frame->data[0];
	data[1] = frame->data[2];
	data[2] = frame->data[1];
	pitches[0] = frame->linesize[0];
	pitches[1] = frame->linesize[2];
	pitches[2] = frame->linesize[1];

	surface = VdpauGetSurface(decoder);
	status =
	    VdpauVideoSurfacePutBitsYCbCr(surface, VDP_YCBCR_FORMAT_YV12, data,
	    pitches);
	if (status != VDP_STATUS_OK) {
	    Error(_("video/vdpau: can't put video surface bits: %s\n"),
		VdpauGetErrorString(status));
	}
    }

    if (frame->interlaced_frame) {
	++decoder->FrameCounter;
    }
    ++decoder->FrameCounter;

    // place in output queue
    // I place it here, for later thread support

    if (0) {				// can't wait for output queue empty
	if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    Warning(_
		("video/vdpau: output buffer full, dropping frame (%d/%d)\n"),
		++decoder->FramesDropped, decoder->FrameCounter);
	    VdpauPrintFrames(decoder);
	    // software surfaces only
	    if (!video_ctx->hwaccel_context) {
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
	if (!video_ctx->hwaccel_context) {
	    VdpauReleaseSurface(decoder, old);
	}
    }

    Debug(4, "video: yy video surface %#x@%d ready\n", surface,
	decoder->SurfaceWrite);

    decoder->SurfacesRb[decoder->SurfaceWrite] = surface;
    decoder->SurfaceWrite = (decoder->SurfaceWrite + 1)
	% VIDEO_SURFACES_MAX;
    atomic_inc(&decoder->SurfacesFilled);
}

///
///	Render osd surface to output surface.
///
static void VdpauMixOsd(void)
{
    VdpOutputSurfaceRenderBlendState blend_state;
    VdpRect source_rect;
    VdpRect output_rect;
    VdpStatus status;
    uint32_t start;
    uint32_t end;

    //
    //	blend overlay over output
    //
    blend_state.struct_version = VDP_OUTPUT_SURFACE_RENDER_BLEND_STATE_VERSION;
    blend_state.blend_factor_source_color =
	VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE;
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

    source_rect.x0 = 0;
    source_rect.y0 = 0;
    source_rect.x1 = VdpauOsdWidth;
    source_rect.y1 = VdpauOsdHeight;

    output_rect.x0 = 0;
    output_rect.y0 = 0;
    output_rect.x1 = VideoWindowWidth;
    output_rect.y1 = VideoWindowHeight;

    start = GetMsTicks();

    VdpauOsdSurfaceIndex = 1;
#ifdef USE_BITMAP
    status =
	VdpauOutputSurfaceRenderBitmapSurface(dpauSurfacesRb
	[VdpauSurfaceIndex], &output_rect,
	VdpauOsdBitmapSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL,
	&blend_state, VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render bitmap surface: %s\n"),
	    VdpauGetErrorString(status));
    }
#else
    status =
	VdpauOutputSurfaceRenderOutputSurface(VdpauSurfacesRb
	[VdpauSurfaceIndex], &output_rect,
	VdpauOsdOutputSurface[!VdpauOsdSurfaceIndex], &source_rect, NULL,
	&blend_state, VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't render output surface: %s\n"),
	    VdpauGetErrorString(status));
    }
#endif
    end = GetMsTicks();

    //Debug(3, "video:/vdpau: osd render %d ms\n", end - start);

    VdpauOsdSurfaceIndex = !VdpauOsdSurfaceIndex;
}

///
///	Render video surface to output surface.
///
///	@param decoder		VDPAU decoder
///
static void VdpauMixVideo(VdpauDecoder * decoder)
{
    VdpVideoSurface current;
    VdpRect video_src_rect;
    VdpRect dst_rect;
    VdpRect dst_video_rect;
    VdpStatus status;

    dst_rect.x0 = 0;			// window output (clip)
    dst_rect.y0 = 0;
    dst_rect.x1 = VideoWindowWidth;
    dst_rect.y1 = VideoWindowHeight;

    video_src_rect.x0 = decoder->InputX;	// video source (crop)
    video_src_rect.y0 = decoder->InputY;
    video_src_rect.x1 = decoder->InputX + decoder->InputWidth;
    video_src_rect.y1 = decoder->InputY + decoder->InputHeight;

    dst_video_rect.x0 = decoder->OutputX;	// video output (scale)
    dst_video_rect.y0 = decoder->OutputY;
    dst_video_rect.x1 = decoder->OutputX + decoder->OutputWidth;
    dst_video_rect.y1 = decoder->OutputY + decoder->OutputHeight;

    if (decoder->Interlaced && VideoDeinterlace != VideoDeinterlaceWeave) {
	//
	//	Build deinterlace structures
	//
	VdpVideoMixerPictureStructure cps;
	VdpVideoSurface past[2];
	VdpVideoSurface future[2];

#ifdef DEBUG
	if (atomic_read(&decoder->SurfacesFilled) < 3) {
	    Debug(3, "only %d\n", atomic_read(&decoder->SurfacesFilled));
	    abort();
	}
#endif

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

	Debug(4, " %02d	 %02d(%c%02d) %02d  %02d\n", past[1], past[0],
	    cps == VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD ? 'T' : 'B',
	    current, future[0], future[1]);

	status =
	    VdpauVideoMixerRender(decoder->VideoMixer, VDP_INVALID_HANDLE,
	    NULL, cps, 2, past, current, 2, future, &video_src_rect,
	    VdpauSurfacesRb[VdpauSurfaceIndex], &dst_rect, &dst_video_rect, 0,
	    NULL);
    } else {
	current = decoder->SurfacesRb[decoder->SurfaceRead];

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

    Debug(4, "video: yy video surface %#x@%d displayed\n", current,
	decoder->SurfaceRead);
}

///
///	Display a video frame.
///
static void VdpauDisplayFrame(void)
{
    uint32_t now;
    uint32_t end;
    static uint32_t last_frame_tick;
    VdpStatus status;
    VdpTime first_time;
    static VdpTime last_time;
    int i;

    now = GetMsTicks();
    //Debug(3, "video/vdpau: tick %d\n", now - last_frame_tick);

    //
    //	wait for surface visible (blocks max ~5ms)
    //
    status =
	VdpauPresentationQueueBlockUntilSurfaceIdle(VdpauQueue,
	VdpauSurfacesRb[VdpauSurfaceIndex], &first_time);
    end = GetMsTicks();
    if (status != VDP_STATUS_OK) {
	Error(_("video/vdpau: can't block queue: %s\n"),
	    VdpauGetErrorString(status));
    }
    // check if surface was displayed for more than 1 frame
    if (last_time && first_time > last_time + 21 * 1000 * 1000) {
	Debug(3, "video/vdpau: %ld display time %ld - %d ms\n", first_time,
	    (first_time - last_time) / 1000, end - now);
	// FIXME: can be more than 1 frame long shown
	for (i = 0; i < VdpauDecoderN; ++i) {
	    VdpauDecoders[i]->FramesMissed++;
	    VdpauPrintFrames(VdpauDecoders[i]);
	}
    }
    last_time = first_time;
    last_frame_tick = now;

    //
    //	Render videos into output
    //
    for (i = 0; i < VdpauDecoderN; ++i) {
	int filled;
	VdpauDecoder *decoder;

	decoder = VdpauDecoders[i];

	filled = atomic_read(&decoder->SurfacesFilled);
	// need 1 frame for progressive, 3 frames for interlaced
	if (filled < 1 + 2 * decoder->Interlaced) {
	    // FIXME: render black surface
	    continue;
	}

	VdpauMixVideo(decoder);

	// next field
	if (decoder->Interlaced) {
	    decoder->SurfaceField ^= 1;
	}
	// next surface, if complete frame is displayed
	if (!decoder->SurfaceField) {
	    // check decoder, if new surface is available
	    // need 2 frames for progressive
	    // need 4 frames for interlaced
	    if (filled <= 1 + 2 * decoder->Interlaced) {
		// keep use of last surface
		++decoder->FramesDuped;
		VdpauPrintFrames(decoder);
		decoder->SurfaceField = decoder->Interlaced;
	    } else {
		decoder->SurfaceRead = (decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX;
		atomic_dec(&decoder->SurfacesFilled);
	    }
	}
    }

    //
    //	add osd to surface
    //
    VdpauMixOsd();

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

    VdpauSurfaceIndex = (VdpauSurfaceIndex + 1) % OUTPUT_SURFACES_MAX;

    xcb_flush(Connection);
}

///
///	Sync and display surface.
///
///	@param decoder	vdpau decoder
///
static void VdpauSyncDisplayFrame(VdpauDecoder * decoder)
{
    VdpauDisplayFrame();
#if 0
    int filled;
    int64_t audio_clock;
    int64_t video_clock;

    if (!decoder->DupNextFrame && (!Video60HzMode
	    || decoder->FramesDisplayed % 6)) {
	VaapiAdvanceFrame();
    }
    // debug duplicate frames
    filled = atomic_read(&decoder->SurfacesFilled);
    if (filled == 1) {
	decoder->FramesDuped++;
	Warning(_("video: display buffer empty, duping frame (%d/%d)\n"),
	    decoder->FramesDuped, decoder->FrameCounter);
	if (!(decoder->FramesDisplayed % 333)) {
	    VaapiPrintFrames(decoder);
	}
    }

    VaapiDisplayFrame();

    //
    //	audio/video sync
    //
    audio_clock = AudioGetClock();
    video_clock = VideoGetClock();
    // FIXME: audio not known assume 333ms delay

    if (decoder->DupNextFrame) {
	decoder->DupNextFrame = 0;
    } else if ((uint64_t) audio_clock != AV_NOPTS_VALUE
	&& (uint64_t) video_clock != AV_NOPTS_VALUE) {
	// both clocks are known

	if (abs(video_clock - audio_clock) > 5000 * 90) {
	    Debug(3, "video: pts difference too big\n");
	} else if (video_clock > audio_clock + VideoAudioDelay + 30 * 90) {
	    Debug(3, "video: slow down video\n");
	    decoder->DupNextFrame = 1;
	} else if (audio_clock + VideoAudioDelay > video_clock + 50 * 90
	    && filled > 1) {
	    Debug(3, "video: speed up video\n");
	    decoder->DropNextFrame = 1;
	}
    }

    if (decoder->DupNextFrame || decoder->DropNextFrame
	|| !(decoder->FramesDisplayed % (50 * 10))) {
	static int64_t last_video_clock;

	Debug(3,
	    "video: %09" PRIx64 "-%09" PRIx64 " %4" PRId64 " pts %+dms %"
	    PRId64 "\n", audio_clock, video_clock,
	    video_clock - last_video_clock,
	    (int)(audio_clock - video_clock) / 90, AudioGetDelay() / 90);

	last_video_clock = video_clock;
    }
#endif
}

///
///	Sync and render a ffmpeg frame
///
///	@param decoder		vdpau decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
static void VdpauSyncRenderFrame(VdpauDecoder * decoder,
    const AVCodecContext * video_ctx, const AVFrame * frame)
{
    if (!atomic_read(&decoder->SurfacesFilled)) {
	Debug(3, "video: new stream frame %d\n", GetMsTicks() - VideoSwitch);
    }

    if (decoder->DropNextFrame) {	// drop frame requested
	++decoder->FramesDropped;
	Warning(_("video: dropping frame (%d/%d)\n"), decoder->FramesDropped,
	    decoder->FrameCounter);
	if (!(decoder->FramesDisplayed % 100)) {
	    VdpauPrintFrames(decoder);
	}
	decoder->DropNextFrame = 0;
	return;
    }
    // if video output buffer is full, wait and display surface.
    // loop for interlace
    while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	struct timespec abstime;

	abstime = decoder->FrameTime;
	abstime.tv_nsec += 14 * 1000 * 1000;
	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
	    // avoid overflow
	    abstime.tv_sec++;
	    abstime.tv_nsec -= 1000 * 1000 * 1000;
	}

	VideoPollEvent();

	// give osd some time slot
	while (pthread_cond_timedwait(&VideoWakeupCond, &VideoLockMutex,
		&abstime) != ETIMEDOUT) {
	    // SIGUSR1
	    Debug(3, "video/vdpau: pthread_cond_timedwait error\n");
	}

	VdpauSyncDisplayFrame(decoder);
    }

    VdpauRenderFrame(decoder, video_ctx, frame);
}

#ifdef USE_VIDEO_THREAD

/**
**	Handle a VDPAU display.
**
**	@todo FIXME: only a single decoder supported.
*/
static void VdpauDisplayHandlerThread(void)
{
    int err;
    int filled;
    struct timespec nowtime;
    VdpauDecoder *decoder;

    decoder = VdpauDecoders[0];

    //
    // fill frame output ring buffer
    //
    filled = atomic_read(&decoder->SurfacesFilled);
    err = 1;
    if (filled < VIDEO_SURFACES_MAX) {
	// FIXME: hot polling
	pthread_mutex_lock(&VideoLockMutex);
	// fetch+decode or reopen
	err = VideoDecode();
	pthread_mutex_unlock(&VideoLockMutex);
    }
    if (err) {
	// FIXME: sleep on wakeup
	usleep(5 * 1000);		// nothing buffered
    }

    filled = atomic_read(&decoder->SurfacesFilled);
    clock_gettime(CLOCK_REALTIME, &nowtime);
    // time for one frame over?
    if ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
	* 1000 * 1000 * 1000 + (nowtime.tv_nsec - decoder->FrameTime.tv_nsec) <
	15 * 1000 * 1000) {
	return;
    }

    pthread_mutex_lock(&VideoLockMutex);
    VdpauSyncDisplayFrame(decoder);
    pthread_mutex_unlock(&VideoLockMutex);
}

#endif

//----------------------------------------------------------------------------
//	VDPAU OSD
//----------------------------------------------------------------------------

///
///	Clear subpicture image.
///
///	@note looked by caller
///
static void VdpauOsdClear(void)
{
    VdpStatus status;
    void *image;
    void const *data[1];
    uint32_t pitches[1];
    VdpRect dst_rect;

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

    Debug(3, "video/vdpau: clear image\n");

    image = calloc(4, VdpauOsdWidth * VdpauOsdHeight);

    dst_rect.x0 = 0;
    dst_rect.y0 = 0;
    dst_rect.x1 = dst_rect.x0 + VdpauOsdWidth;
    dst_rect.y1 = dst_rect.y0 + VdpauOsdHeight;
    data[0] = image;
    pitches[0] = VdpauOsdWidth * 4;

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

    free(image);
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
static void VdpauUploadImage(int x, int y, int width, int height,
    const uint8_t * argb)
{
    VdpStatus status;
    void const *data[1];
    uint32_t pitches[1];
    VdpRect dst_rect;

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

    Debug(3, "video/vdpau: upload image\n");

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
    }

    VdpauOsdWidth = width;
    VdpauOsdHeight = height;

    //
    //	create bitmap/surface for osd
    //
#ifdef USE_BITMAP
    if (VdpauOsdBitmapSurface[0] == VDP_INVALID_HANDLE) {
	for (i = 0; i < 2; ++i) {
	    status =
		VdpauBitmapSurfaceCreate(VdpauDevice, VDP_RGBA_FORMAT_B8G8R8A8,
		width, height, VDP_TRUE, VdpauOsdBitmapSurface + i);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't create bitmap surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	}
    }
#else
    if (VdpauOsdOutputSurface[0] == VDP_INVALID_HANDLE) {
	for (i = 0; i < 2; ++i) {
	    status =
		VdpauOutputSurfaceCreate(VdpauDevice, VDP_RGBA_FORMAT_B8G8R8A8,
		width, height, VdpauOsdOutputSurface + i);
	    if (status != VDP_STATUS_OK) {
		Error(_("video/vdpau: can't create output surface: %s\n"),
		    VdpauGetErrorString(status));
	    }
	}
    }
#endif

    Debug(3, "video/vdpau: osd surfaces created\n");

    VdpauOsdClear();
}

#endif

//----------------------------------------------------------------------------
//	OSD
//----------------------------------------------------------------------------

//static int OsdShow;			///< flag show osd
static int OsdWidth;			///< osd width
static int OsdHeight;			///< osd height

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
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	VaapiOsdClear();
	VideoThreadUnlock();
	return;
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	VdpauOsdClear();
	VideoThreadUnlock();
	return;
    }
#endif
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
void VideoOsdDrawARGB(int x, int y, int height, int width,
    const uint8_t * argb)
{
    VideoThreadLock();
#ifdef USE_GLX
    if (GlxEnabled) {
	Debug(3, "video: %p <-> %p\n", glXGetCurrentContext(), GlxContext);
	GlxUploadTexture(x, y, height, width, argb);
	VideoThreadUnlock();
	return;
    }
#endif
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	VaapiUploadImage(x, y, height, width, argb);
	VideoThreadUnlock();
	return;
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	VdpauUploadImage(x, y, height, width, argb);
	VideoThreadUnlock();
	return;
    }
#endif
    (void)x;
    (void)y;
    (void)height;
    (void)width;
    (void)argb;
    VideoThreadUnlock();
}

/**
**	Setup osd.
**
**	FIXME: looking for BGRA, but this fourcc isn't supported by the
**	drawing functions yet.
*/
void VideoOsdInit(void)
{
    OsdWidth = 1920 / 1;
    OsdHeight = 1080 / 1;		// worst-case

    //OsdWidth = 768;
    //OsdHeight = VideoWindowHeight;		// FIXME: must be configured

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
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	VaapiOsdInit(OsdWidth, OsdHeight);
	return;
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	VdpauOsdInit(OsdWidth, OsdHeight);
	return;
    }
#endif
}

#if 0

//----------------------------------------------------------------------------
//	Overlay
//----------------------------------------------------------------------------

/**
**	Render osd surface.
*/
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

/**
**	Display overlay surface.
*/
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
//	Frame
//----------------------------------------------------------------------------

#if 0

/**
**	Display a single frame.
*/
static void VideoDisplayFrame(void)
{
#ifdef USE_GLX
    if (GlxEnabled) {
	VideoDisplayOverlay();

#ifdef USE_DOUBLEBUFFER
	glXSwapBuffers(XlibDisplay, VideoWindow);
#else
	glFinish();			// wait for all execution finished
#endif
	GlxCheck();

	glClear(GL_COLOR_BUFFER_BIT);
    }
#endif
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	VaapiDisplayFrame();
	return;
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	return;
    }
#endif
}

#endif

//----------------------------------------------------------------------------
//	Events
//----------------------------------------------------------------------------

/// C callback feed key press
extern void FeedKeyPress(const char *, const char *, int, int);

/**
**	Handle X11 events.
**
**	@todo	Signal WmDeleteMessage to application.
*/
static void VideoEvent(void)
{
    XEvent event;
    KeySym keysym;

    //char buf[32];

    XNextEvent(XlibDisplay, &event);
    switch (event.type) {
	case ClientMessage:
	    Debug(3, "video/event: ClientMessage\n");
	    if (event.xclient.data.l[0] == (long)WmDeleteWindowAtom) {
		// FIXME: wrong, kills recordings ...
		Error(_("video: FIXME: wm-delete-message\n"));
	    }
	    break;

	case MapNotify:
	    Debug(3, "video/event: MapNotify\n");
	    break;
	case Expose:
	    Debug(3, "video/event: Expose\n");
	    break;
	case ReparentNotify:
	    Debug(3, "video/event: ReparentNotify\n");
	    break;
	case ConfigureNotify:
	    Debug(3, "video/event: ConfigureNotify\n");
	    break;
	case KeyPress:
	    keysym = XLookupKeysym(&event.xkey, 0);
#if 0
	    switch (keysym) {
		case XK_d:
		    break;
		case XK_S:
		    break;
	    }
#endif
	    if (keysym == NoSymbol) {
		Warning(_("video: No symbol for %d\n"), event.xkey.keycode);
	    }
	    FeedKeyPress("XKeySym", XKeysymToString(keysym), 0, 0);
	    /*
	       if (XLookupString(&event.xkey, buf, sizeof(buf), &keysym, NULL)) {
	       FeedKeyPress("XKeySym", buf, 0, 0);
	       } else {
	       FeedKeyPress("XKeySym", XKeysymToString(keysym), 0, 0);
	       }
	     */
	case KeyRelease:
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

/**
**	Poll all x11 events.
*/
void VideoPollEvent(void)
{
    while (XPending(XlibDisplay)) {
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

/**
**	Lock video thread.
*/
static void VideoThreadLock(void)
{
    if (pthread_mutex_lock(&VideoLockMutex)) {
	Error(_("video: can't lock thread\n"));
    }
}

/**
**	Unlock video thread.
*/
static void VideoThreadUnlock(void)
{
    if (pthread_mutex_unlock(&VideoLockMutex)) {
	Error(_("video: can't unlock thread\n"));
    }
}

/**
**	Video render thread.
*/
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
	VideoPollEvent();

#ifdef USE_VAAPI
	if (VideoVaapiEnabled) {
	    VaapiDisplayHandlerThread();
	}
#endif
#ifdef USE_VDPAU
	if (VideoVdpauEnabled) {
	    VdpauDisplayHandlerThread();
	}
#endif
#if !defined(USE_VAAPI) && !defined(USE_VDPAU)
	// avoid 100% cpu use
	if (1) {
	    XEvent event;

	    XPeekEvent(XlibDisplay, &event);
	} else {
	    usleep(10 * 1000);
	}
#endif
    }

    return dummy;
}

/**
**	Video render.
*/
void VideoDisplayHandler(void)
{
    if (!XlibDisplay) {			// not yet started
	return;
    }
#ifdef USE_GLX
    glFinish();				// wait for all execution finished
    Debug(3, "video: %p <-> %p\n", glXGetCurrentContext(), GlxContext);
#endif

    if (!VideoThread) {
#ifdef USE_GLX
	if (GlxEnabled) {		// other thread renders
	    // glXMakeCurrent(XlibDisplay, None, NULL);
	}
#endif

	pthread_mutex_init(&VideoMutex, NULL);
	pthread_mutex_init(&VideoLockMutex, NULL);
	pthread_cond_init(&VideoWakeupCond, NULL);
	pthread_create(&VideoThread, NULL, VideoDisplayHandlerThread, NULL);
	//pthread_detach(VideoThread);
    }
}

/**
**	Exit and cleanup video threads.
*/
static void VideoThreadExit(void)
{
    void *retval;

    Debug(3, "video: video thread canceled\n");
    if (VideoThread) {
	if (pthread_cancel(VideoThread)) {
	    Error(_("video: can't queue cancel video display thread\n"));
	}
	if (pthread_join(VideoThread, &retval) || retval != PTHREAD_CANCELED) {
	    Error(_("video: can't cancel video display thread\n"));
	}
	pthread_cond_destroy(&VideoWakeupCond);
	pthread_mutex_destroy(&VideoLockMutex);
	pthread_mutex_destroy(&VideoMutex);
	VideoThread = 0;
    }
}

#endif

//----------------------------------------------------------------------------
//	Video API
//----------------------------------------------------------------------------

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
VideoHwDecoder *VideoNewHwDecoder(void)
{
    if (!XlibDisplay) {			// waiting for x11 start
	return NULL;
    }
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	return (VideoHwDecoder *) VaapiNewDecoder();
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	return (VideoHwDecoder *) VdpauNewDecoder();
    }
#endif
    return NULL;
}

///
///	Get a free hardware decoder surface.
///
///	@param decoder	video hardware decoder
///
unsigned VideoGetSurface(VideoHwDecoder * decoder)
{
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	return VaapiGetSurface(&decoder->Vaapi);
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	return VdpauGetSurface(&decoder->Vdpau);
    }
#endif
    (void)decoder;
    return -1;
}

///
///	Release a hardware decoder surface.
///
///	@param decoder	video hardware decoder
///	@param surface	surface no longer used
///
void VideoReleaseSurface(VideoHwDecoder * decoder, unsigned surface)
{
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	VaapiReleaseSurface(&decoder->Vaapi, surface);
	return;
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	return VdpauReleaseSurface(&decoder->Vdpau, surface);
    }
#endif
    (void)decoder;
    (void)surface;
}

///
///	Callback to negotiate the PixelFormat.
///
///	@param fmt	is the list of formats which are supported by the codec,
///			it is terminated by -1 as 0 is a valid format, the
///			formats are ordered by quality.
///
enum PixelFormat Video_get_format(VideoHwDecoder * decoder,
    AVCodecContext * video_ctx, const enum PixelFormat *fmt)
{
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	return Vaapi_get_format(&decoder->Vaapi, video_ctx, fmt);
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	return Vdpau_get_format(&decoder->Vdpau, video_ctx, fmt);
    }
#endif
    (void)decoder;
    (void)video_ctx;
    (void)fmt;
    return fmt[0];
}

///
///	Update video pts.
///
///	@param pts_p		pointer to pts
///	@param interlaced	interlaced flag (frame isn't right)
///	@param frame		frame to display
///
static void VideoSetPts(int64_t * pts_p, int interlaced, const AVFrame * frame)
{
    int64_t pts;

    if (interlaced != frame->interlaced_frame) {
	Debug(3, "video: can't use frame->interlaced_frame\n");
    }
    // update video clock
    if ((uint64_t) * pts_p != AV_NOPTS_VALUE) {
	*pts_p += interlaced ? 40 * 90 : 20 * 90;
    }
    //pts = frame->best_effort_timestamp;
    pts = frame->pkt_pts;
    if ((uint64_t) pts == AV_NOPTS_VALUE || !pts) {
	// libav: 0.8pre didn't set pts
	pts = frame->pkt_dts;
    }
    if (!pts) {
	pts = AV_NOPTS_VALUE;
    }
    // build a monotonic pts
    if ((uint64_t) * pts_p != AV_NOPTS_VALUE) {
	if (pts - *pts_p < -10 * 90) {
	    pts = AV_NOPTS_VALUE;
	}
    }
    // libav: sets only pkt_dts which can be 0
    if ((uint64_t) pts != AV_NOPTS_VALUE) {
	if (*pts_p != pts) {
	    Debug(3,
		"video: %#012" PRIx64 "->%#012" PRIx64 " %4" PRId64 " pts\n",
		*pts_p, pts, pts - *pts_p);
	    *pts_p = pts;
	}
    }
}

///
///	Display a ffmpeg frame
///
///	@param decoder		video hardware decoder
///	@param video_ctx	ffmpeg video codec context
///	@param frame		frame to display
///
void VideoRenderFrame(VideoHwDecoder * decoder, AVCodecContext * video_ctx,
    AVFrame * frame)
{
    if (frame->repeat_pict) {
	Warning("video: repeated pict found, but not handled\n");
    }
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	VideoSetPts(&decoder->Vaapi.PTS, decoder->Vaapi.Interlaced, frame);
	VaapiSyncRenderFrame(&decoder->Vaapi, video_ctx, frame);
	return;
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	VideoSetPts(&decoder->Vdpau.PTS, decoder->Vdpau.Interlaced, frame);
	VdpauSyncRenderFrame(&decoder->Vdpau, video_ctx, frame);
	return;
    }
#endif
    (void)decoder;
    (void)video_ctx;
    (void)frame;
}

///
///	Get VA-API ffmpeg context
///
///	@param decoder	VA-API decoder
///
struct vaapi_context *VideoGetVaapiContext(VideoHwDecoder * decoder)
{
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	return decoder->Vaapi.VaapiContext;
    }
#endif
    (void)decoder;
    Error(_("video/vaapi: get vaapi context, without vaapi enabled\n"));
    return NULL;
}

#ifndef USE_VIDEO_THREAD

/**
**	Video render.
*/
void VideoDisplayHandler(void)
{
    uint32_t now;

    if (!XlibDisplay) {			// not yet started
	return;
    }

    now = GetMsTicks();
    if (now < VaapiDecoders[0]->LastFrameTick) {
	return;
    }
    if (now - VaapiDecoders[0]->LastFrameTick < 500) {
	return;
    }
    VideoPollEvent();
    VaapiBlackSurface(VaapiDecoders[0]);

    return;
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	VaapiDisplayFrame();
	return;
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	return;
    }
#endif
    VideoDisplayFrame();
}

#endif

/**
**	Get video clock.
**
**	@note this isn't monoton, decoding reorders frames,
**	setter keeps it monotonic
*/
int64_t VideoGetClock(void)
{
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	// FIXME: VaapiGetClock();

	// pts is the timestamp of the latest decoded frame
	if ((uint64_t) VaapiDecoders[0]->PTS == AV_NOPTS_VALUE) {
	    return AV_NOPTS_VALUE;
	}
	if (VaapiDecoders[0]->Interlaced) {
	    return VaapiDecoders[0]->PTS -
		20 * 90 * (2 * atomic_read(&VaapiDecoders[0]->SurfacesFilled)
		- VaapiDecoders[0]->SurfaceField);
	}
	return VaapiDecoders[0]->PTS -
	    20 * 90 * (atomic_read(&VaapiDecoders[0]->SurfacesFilled) - 1);
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	return 0L;
    }
#endif
    return 0L;
}

//----------------------------------------------------------------------------
//	Setup
//----------------------------------------------------------------------------

/**
**	Create main window.
*/
static void VideoCreateWindow(xcb_window_t parent, xcb_visualid_t visual,
    uint8_t depth)
{
    uint32_t values[4];
    xcb_intern_atom_reply_t *reply;

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
	XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
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

    values[0] = XCB_NONE;
    xcb_change_window_attributes(Connection, VideoWindow, XCB_CW_CURSOR,
	values);

    xcb_map_window(Connection, VideoWindow);
}

/**
**	Set video geometry.
**
**	@param geometry	 [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]
*/
int VideoSetGeometry(const char *geometry)
{
    int flags;

    flags =
	XParseGeometry(geometry, &VideoWindowX, &VideoWindowY,
	&VideoWindowWidth, &VideoWindowHeight);

    return 0;
}

/**
**	Set deinterlace mode.
*/
void VideoSetDeinterlace(int mode)
{
    VideoDeinterlace = mode;
}

/**
**	Set scaling mode.
*/
void VideoSetScaling(int mode)
{
    VideoScaling = mode;
}

/**
**	Set audio delay.
**
**	@param ms	delay in ms
*/
void VideoSetAudioDelay(int ms)
{
    VideoAudioDelay = ms * 90;
}

/**
**	Initialize video output module.
**
**	@param display_name	X11 display name
*/
void VideoInit(const char *display_name)
{
    int screen_nr;
    int i;
    xcb_screen_iterator_t screen_iter;
    xcb_screen_t *screen;

    if (XlibDisplay) {			// allow multiple calls
	Debug(3, "video: x11 already setup\n");
	return;
    }
    // Open the connection to the X server.
    // use the DISPLAY environment variable as the default display name
    if (!display_name) {
	display_name = getenv("DISPLAY");
	if (!display_name) {
	    // use :0.0 as default display name
	    display_name = ":0.0";
	}
    }
    if (!(XlibDisplay = XOpenDisplay(display_name))) {
	Fatal(_("video: Can't connect to X11 server on '%s'"), display_name);
	// FIXME: we need to retry connection
    }
    XInitThreads();
    // Convert XLIB display to XCB connection
    if (!(Connection = XGetXCBConnection(XlibDisplay))) {
	Fatal(_("video: Can't convert XLIB display to XCB connection"));
    }
    // prefetch extensions
    //xcb_prefetch_extension_data(Connection, &xcb_big_requests_id);
    //xcb_prefetch_extension_data(Connection, &xcb_dpms_id);
    //xcb_prefetch_extension_data(Connection, &xcb_glx_id);
    //xcb_prefetch_extension_data(Connection, &xcb_randr_id);
    //xcb_prefetch_extension_data(Connection, &xcb_screensaver_id);
    //xcb_prefetch_extension_data(Connection, &xcb_shm_id);
    //xcb_prefetch_extension_data(Connection, &xcb_xv_id);

    // Get the requested screen number
    screen_nr = DefaultScreen(XlibDisplay);
    screen_iter = xcb_setup_roots_iterator(xcb_get_setup(Connection));
    for (i = 0; i < screen_nr; ++i) {
	xcb_screen_next(&screen_iter);
    }
    screen = screen_iter.data;

    //
    //	Default window size
    //
    if (!VideoWindowHeight) {
	if (VideoWindowWidth) {
	    VideoWindowHeight = (VideoWindowWidth * 9) / 16;
	}
	VideoWindowHeight = 576;
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
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	VideoVdpauInit(display_name);
	// disable va-api, if vdpau succeeded
	if (VideoVdpauEnabled) {
	    VideoVaapiEnabled = 0;
	}
    }
#endif
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	VideoVaapiInit(display_name);
    }
#endif

    //xcb_prefetch_maximum_request_length(Connection);
    xcb_flush(Connection);
}

/**
**	Cleanup video output module.
*/
void VideoExit(void)
{
    if (!XlibDisplay) {			// no init or failed
	return;
    }
#ifdef USE_VIDEO_THREAD
    VideoThreadExit();
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	VideoVdpauExit();
    }
#endif
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	VideoVaapiExit();
    }
#endif
#ifdef USE_GLX
    if (GlxEnabled) {
	GlxExit();
    }
#endif

    //
    //	Reenable screensaver / DPMS.
    //
    //X11SuspendScreenSaver(XlibDisplay, False);
    //X11DPMSEnable(XlibDisplay);

    //
    //	FIXME: cleanup.
    //
    //RandrExit();
}

#endif

#ifdef VIDEO_TEST

#include <getopt.h>

int SysLogLevel;			///< show additional debug informations

/**
**	Print version.
*/
static void PrintVersion(void)
{
    printf("video_test: video tester Version " VERSION
#ifdef GIT_REV
	"(GIT-" GIT_REV ")"
#endif
	",\n\t(c) 2009 - 2011 by Johns\n"
	"\tLicense AGPLv3: GNU Affero General Public License version 3\n");
}

/**
**	Print usage.
*/
static void PrintUsage(void)
{
    printf("Usage: video_test [-?dhv]\n"
	"\t-d\tenable debug, more -d increase the verbosity\n"
	"\t-? -h\tdisplay this message\n" "\t-v\tdisplay version information\n"
	"Only idiots print usage on stderr!\n");
}

/**
**	Main entry point.
**
**	@param argc	number of arguments
**	@param argv	arguments vector
**
**	@returns -1 on failures, 0 clean exit.
*/
int main(int argc, char *const argv[])
{
    SysLogLevel = 0;

    //
    //	Parse command line arguments
    //
    for (;;) {
	switch (getopt(argc, argv, "hv?-c:d")) {
	    case 'd':			// enabled debug
		++SysLogLevel;
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
    VideoInit();
    VideoOsdInit();
    for (;;) {
	VideoRenderOverlay();
	VideoDisplayOverlay();
	glXSwapBuffers(XlibDisplay, VideoWindow);
	GlxCheck();
	glClear(GL_COLOR_BUFFER_BIT);

	XFlush(XlibDisplay);
	XSync(XlibDisplay, False);
	XFlush(XlibDisplay);
	XSync(XlibDisplay, False);
	XFlush(XlibDisplay);
	XSync(XlibDisplay, False);
	XFlush(XlibDisplay);
	XSync(XlibDisplay, False);
	XFlush(XlibDisplay);
	XSync(XlibDisplay, False);
	XFlush(XlibDisplay);
	usleep(20 * 1000);
    }
    VideoExit();

    return 0;
}

#endif
