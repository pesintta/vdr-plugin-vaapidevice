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

#define DEBUG
#define USE_XLIB_XCB
#define noUSE_GLX
#define noUSE_DOUBLEBUFFER

#define USE_VAAPI
#define noUSE_VDPAU
#define noUSE_BITMAP

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

#define VIDEO_SURFACES_MAX	3	///< video output surfaces for queue
#define OUTPUT_SURFACES_MAX	4	///< output surfaces for flip page

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

static Display *XlibDisplay;		///< Xlib X11 display
static xcb_connection_t *Connection;	///< xcb connection
static xcb_colormap_t VideoColormap;	///< video colormap
static xcb_window_t VideoWindow;	///< video window

static int VideoWindowX;		///< video output x
static int VideoWindowY;		///< video outout y
static unsigned VideoWindowWidth;	///< video output width
static unsigned VideoWindowHeight;	///< video output height

    /// Default deinterlace mode
static VideoDeinterlaceModes VideoDeinterlace;

    /// Default scaling mode
static VideoScalingModes VideoScaling;

static xcb_atom_t WmDeleteWindowAtom;	///< WM delete message

extern uint32_t VideoSwitch;

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

static VADisplay *VaDisplay;		///< VA-API display

static VAImage VaOsdImage = {
    .image_id = VA_INVALID_ID
};					///< osd VA-API image

static VASubpictureID VaOsdSubpicture;	///< osd VA-API subpicture
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
    struct timespec FrameTime;		///< time of last display
    struct timespec StartTime;		///< decoder start time

    int FramesDuped;			///< frames duplicated
    int FramesDropped;			///< frames dropped
    int FrameCounter;			///< number of frames decoded
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

    if (VaapiUnscaledOsd) {
	if (vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN, 0, 0,
		VaOsdImage.width, VaOsdImage.height, 0, 0, VideoWindowWidth,
		VideoWindowHeight, VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
	}
    } else {
	if (vaAssociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN, 0, 0,
		VaOsdImage.width, VaOsdImage.height, 0, 0, width, height, 0)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't associate subpicture\n"));
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
	if (vaDeassociateSubpicture(VaDisplay, VaOsdSubpicture,
		decoder->SurfacesFree, decoder->SurfaceFreeN)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't deassociate %d surfaces\n"),
		decoder->SurfaceFreeN);
	}

	if (vaDeassociateSubpicture(VaDisplay, VaOsdSubpicture,
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
static VASurfaceID VaapiGetSurface(VaapiDecoder * decoder)
{
    VASurfaceID surface;
    int i;

    if (!decoder->SurfaceFreeN) {
	Error("video/vaapi: out of surfaces\n");
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
    Debug(3, "video/vaapi: %d duped, %d dropped frames of %d\n",
	decoder->FramesDuped, decoder->FramesDropped, decoder->FrameCounter);
}

///
///	Allocate new VA-API decoder.
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
	if (vaSyncSurface(decoder->VaDisplay, surface)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: vaSyncSurface failed\n"));
	}
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

    if (decoder->BlackSurface) {
	if (vaDestroySurfaces(decoder->VaDisplay, &decoder->BlackSurface, 1)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy a surface\n"));
	}
    }
    // FIXME: decoder->DeintImages
#ifdef USE_GLX
    if (decoder->GlxSurface[0]) {
	if (vaDestroySurfaceGLX(VaDisplay, decoder->GlxSurface[0])
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy glx surface!\n"));
	}
    }
    if (decoder->GlxSurface[1]) {
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

    // FIXME: make configurable
    // FIXME: intel get hangups with bob
    // VideoDeinterlace = VideoDeinterlaceWeave;

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
	Fatal(_("video/vaapi: Can't connect VA-API to X11 server on '%s'"),
	    display_name);
	// FIXME: no fatal for plugin
    }

    if (vaInitialize(VaDisplay, &major, &minor) != VA_STATUS_SUCCESS) {
	Fatal(_("video/vaapi: Can't inititialize VA-API on '%s'"),
	    display_name);
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

    if (VaOsdImage.image_id != VA_INVALID_ID) {
	if (vaDestroyImage(VaDisplay,
		VaOsdImage.image_id) != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy image!\n"));
	}
	VaOsdImage.image_id = VA_INVALID_ID;
    }

    if (VaOsdSubpicture != VA_INVALID_ID) {
	if (vaDestroySubpicture(VaDisplay, VaOsdSubpicture)
	    != VA_STATUS_SUCCESS) {
	    Error(_("video/vaapi: can't destroy subpicture\n"));
	}
	VaOsdSubpicture = VA_INVALID_ID;
    }

    for (i = 0; i < VaapiDecoderN; ++i) {
	if (VaapiDecoders[i]) {
	    VaapiDelDecoder(VaapiDecoders[i]);
	    VaapiDecoders[i] = NULL;
	}
    }
    VaapiDecoderN = 0;

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

    // fixes: [drm:i915_hangcheck_elapsed] *ERROR* Hangcheck
    //	  timer elapsed... GPU hung
    usleep(1 * 1000);

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
	if (vaSyncSurface(decoder->VaDisplay, surface) != VA_STATUS_SUCCESS) {
	    Error(_("video: vaSyncSurface failed\n"));
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
**	@note called only for software decoder.
*/
static void VaapiSetup(VaapiDecoder * decoder, AVCodecContext * video_ctx)
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
	    if (!(decoder->FrameCounter % 100)) {
		VaapiPrintFrames(decoder);
	    }
	    if (softdec) {		// software surfaces only
		VaapiReleaseSurface(decoder, surface);
	    }
	    return;
	}
    } else {				// wait for output queue empty
	while (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	    VideoDisplayHandler();
	}
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
	    if (vaSyncSurface(decoder->VaDisplay, old) != VA_STATUS_SUCCESS) {
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
    //
    //	associate the OSD with surface
    //
    if (vaAssociateSubpicture(VaDisplay, VaOsdSubpicture, &surface, 1, 0, 0,
	    VaOsdImage.width, VaOsdImage.height, 0, 0, decoder->InputWidth,
	    decoder->InputHeight, 0) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't associate subpicture\n"));
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
    }

    if (vaAssociateSubpicture(decoder->VaDisplay, VaOsdSubpicture,
	    &decoder->BlackSurface, 1, 0, 0, VaOsdImage.width,
	    VaOsdImage.height, 0, 0, VideoWindowWidth, VideoWindowHeight,
	    0) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't associate subpicture\n"));
    }

    if (vaSyncSurface(decoder->VaDisplay,
	    decoder->BlackSurface) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }

    Debug(4, "video/vaapi: yy black video surface %#x displayed\n",
	decoder->BlackSurface);
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

    if (vaSyncSurface(decoder->VaDisplay,
	    decoder->BlackSurface) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: vaSyncSurface failed\n"));
    }

    usleep(1 * 1000);
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

    if (vaSyncSurface(decoder->VaDisplay, surface) != VA_STATUS_SUCCESS) {
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
    if (vaSyncSurface(decoder->VaDisplay, out1) != VA_STATUS_SUCCESS) {
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
    if (vaSyncSurface(decoder->VaDisplay, out2) != VA_STATUS_SUCCESS) {
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
    AVCodecContext * video_ctx, AVFrame * frame)
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
	if (av_cmp_q(decoder->InputAspect, frame->sample_aspect_ratio)) {
	    Debug(3, "video/vaapi: aspect ratio changed\n");

	    //decoder->InputWidth = video_ctx->width;
	    //decoder->InputHeight = video_ctx->height;
	    decoder->InputAspect = frame->sample_aspect_ratio;
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
**	Video render frame.
**
**	FIXME: no locks for multi-thread
**	FIXME: frame delay for 50hz hardcoded
**
*/
void VaapiDisplayFrame(void)
{
    uint32_t start;
    uint32_t sync;
    uint32_t put1;
    uint32_t put2;
    int i;
    VaapiDecoder *decoder;
    VASurfaceID surface;

    // look if any stream have a new surface available
    for (i = 0; i < VaapiDecoderN; ++i) {
	int filled;

	decoder = VaapiDecoders[i];
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled) {
	    // show any frame as fast as possible
	    // we keep always the last frame in the ring buffer
	    if (filled > 1) {
		decoder->SurfaceRead = (decoder->SurfaceRead + 1)
		    % VIDEO_SURFACES_MAX;
		atomic_dec(&decoder->SurfacesFilled);
	    }

	    start = GetMsTicks();
	    surface = decoder->SurfacesRb[decoder->SurfaceRead];
	    Debug(4, "video/vaapi: yy video surface %#x displayed\n", surface);

	    if (vaSyncSurface(decoder->VaDisplay, surface)
		!= VA_STATUS_SUCCESS) {
		Error(_("video/vaapi: vaSyncSurface failed\n"));
	    }

	    sync = GetMsTicks();
	    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		decoder->TopFieldFirst, 0);
	    put1 = GetMsTicks();
	    put2 = put1;
	    // deinterlace and full frame rate
	    if (decoder->Interlaced) {
		VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
		    decoder->TopFieldFirst, 1);
		// FIXME: buggy libva-driver-vdpau.
		if (VaapiBuggyVdpau
		    && VideoDeinterlace != VideoDeinterlaceWeave) {
		    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
			decoder->TopFieldFirst, 0);
		    VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced,
			decoder->TopFieldFirst, 1);
		}
		put2 = GetMsTicks();
	    }
	    xcb_flush(Connection);
	    Debug(4, "video/vaapi: sync %2u put1 %2u put2 %2u\n", sync - start,
		put1 - sync, put2 - put1);
	    clock_gettime(CLOCK_REALTIME, &decoder->FrameTime);
	} else {
	    Debug(3, "video/vaapi: no video surface ready\n");
	}
    }
}

/**
**	Clear subpicture image.
**
**	@note it is possible, that we need a lock here
*/
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

/**
**	Upload ARGB to subpicture image.
**
**	@note it is possible, that we need a lock here
*/
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

/**
**	VA-API initialize OSD.
**
**	Subpicture is unusable, its scaled with the video image.
*/
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
    // FIXME:
    VaapiUnscaledOsd = 0;
    Info(_("video/vaapi: unscaled osd disabled\n"));

    if (vaCreateImage(VaDisplay, &formats[u], width, height,
	    &VaOsdImage) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create osd image\n"));
	return;
    }
    if (vaCreateSubpicture(VaDisplay, VaOsdImage.image_id,
	    &VaOsdSubpicture) != VA_STATUS_SUCCESS) {
	Error(_("video/vaapi: can't create subpicture\n"));
	return;
    }
    // FIXME: must store format, to convert ARGB to it.

    VaapiOsdClear();
}

#endif

//----------------------------------------------------------------------------
//	OSD
//----------------------------------------------------------------------------

//static int OsdShow;			///< flag show osd
static int OsdWidth;			///< osd width
static int OsdHeight;			///< osd height

/**
**	Clear the OSD.
**
**	@todo I use glTexImage2D to clear the texture, are there faster and
**	better ways to clear a texture?
*/
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
    VideoThreadUnlock();
}

/**
**	Draw an OSD ARGB image.
*/
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

static pthread_t VideoThread;		///< video decode thread
static pthread_cond_t VideoWakeupCond;	///< wakeup condition variable
static pthread_mutex_t VideoMutex;	///< video condition mutex
static pthread_mutex_t VideoLockMutex;	///< video lock mutex

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
	int err;
	int filled;
	struct timespec nowtime;
	struct timespec abstime;
	VaapiDecoder *decoder;
	uint64_t delay;

	decoder = VaapiDecoders[0];

	VideoPollEvent();

	// initial delay
	delay = AudioGetDelay();
	if (delay < 100 * 90) {		// no audio delay known
	    delay = 760 * 1000 * 1000;
	} else {
	    delay = (delay * 1000 * 1000) / 90 + 60 * 1000 * 1000;
	}
	clock_gettime(CLOCK_REALTIME, &nowtime);
	if (!atomic_read(&decoder->SurfacesFilled)
	    || (uint64_t) ((nowtime.tv_sec - decoder->StartTime.tv_sec)
		* 1000 * 1000 * 1000 + (nowtime.tv_nsec -
		    decoder->StartTime.tv_nsec)) > delay) {

	    if ((nowtime.tv_sec - decoder->StartTime.tv_sec)
		* 1000 * 1000 * 1000 + (nowtime.tv_nsec -
		    decoder->StartTime.tv_nsec)
		< 2000 * 1000 * 1000) {
		Debug(3, "video: audio delay %lu ms\n", delay / (1000 * 1000));
	    }
	    // FIXME: hot polling
	    pthread_mutex_lock(&VideoLockMutex);
	    // fetch or reopen
	    err = VideoDecode();
	    pthread_mutex_unlock(&VideoLockMutex);
	    if (err) {
		// FIXME: sleep on wakeup
		usleep(5 * 1000);	// nothing buffered
	    }
	} else {
	    Debug(3, "video/vaapi: waiting %9lu ms\n",
		((nowtime.tv_sec - decoder->StartTime.tv_sec)
		    * 1000 * 1000 * 1000 + (nowtime.tv_nsec -
			decoder->StartTime.tv_nsec)) / (1000 * 1000));

	    abstime = nowtime;
	    abstime.tv_nsec += 18 * 1000 * 1000;
	    if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
		// avoid overflow
		abstime.tv_sec++;
		abstime.tv_nsec -= 1000 * 1000 * 1000;
	    }

	    pthread_mutex_lock(&VideoLockMutex);
	    // give osd some time slot
	    while (pthread_cond_timedwait(&VideoWakeupCond, &VideoLockMutex,
		    &abstime) != ETIMEDOUT) {
		// SIGUSR1
		Debug(3, "video/vaapi: pthread_cond_timedwait error\n");
	    }
	    pthread_mutex_unlock(&VideoLockMutex);
	}

	clock_gettime(CLOCK_REALTIME, &nowtime);
	// time for one frame over, buggy for vaapi-vdpau
	if ((nowtime.tv_sec - decoder->FrameTime.tv_sec) * 1000 * 1000 * 1000 +
	    (nowtime.tv_nsec - decoder->FrameTime.tv_nsec) <
	    (decoder->Interlaced ? 17 : 17) * 1000 * 1000) {
	    continue;
	}

	filled = atomic_read(&decoder->SurfacesFilled);
	if (!filled) {
	    pthread_mutex_lock(&VideoLockMutex);
	    VaapiBlackSurface(decoder);
	    pthread_mutex_unlock(&VideoLockMutex);
	} else if (filled == 1) {
	    decoder->FramesDuped++;
	    ++decoder->FrameCounter;
	    if (!(decoder->FrameCounter % 333)) {
		Warning(_
		    ("video: display buffer empty, duping frame (%d/%d)\n"),
		    decoder->FramesDuped, decoder->FrameCounter);
		VaapiPrintFrames(decoder);
	    }
	}

	if (filled) {
	    pthread_mutex_lock(&VideoLockMutex);
	    VideoDisplayFrame();
	    pthread_mutex_unlock(&VideoLockMutex);
	}
    }
#if 0
    for (;;) {
	int err;
	int filled;
	struct timespec nowtime;
	struct timespec abstime;
	VaapiDecoder *decoder;

	clock_gettime(CLOCK_REALTIME, &abstime);

	VideoPollEvent();

	// fill surface buffer
	for (;;) {
	    static int max_filled;
	    uint32_t delay;

	    clock_gettime(CLOCK_REALTIME, &nowtime);
	    // time to receive and decode over
	    if ((nowtime.tv_sec - abstime.tv_sec) * 1000 * 1000 * 1000 +
		(nowtime.tv_nsec - abstime.tv_nsec) >
		(decoder->Interlaced + 1) * 15 * 1000 * 1000) {
		break;
	    }

	    delay = 700 * 1000 * 1000;
	    // initial delay get decode only 1 frame
	    if ((nowtime.tv_sec - decoder->StartTime.tv_sec)
		* 1000 * 1000 * 1000 + (nowtime.tv_nsec -
		    decoder->StartTime.tv_nsec) < delay) {
		Debug(3, "video/vaapi: waiting %9lu ms\n",
		    ((nowtime.tv_sec - decoder->StartTime.tv_sec)
			* 1000 * 1000 * 1000 + (nowtime.tv_nsec -
			    decoder->StartTime.tv_nsec)) / (1000 * 1000));

		if (atomic_read(&decoder->SurfacesFilled)) {
		    break;
		}
	    }

	    if (atomic_read(&decoder->SurfacesFilled) >= 3) {
		break;
	    }
	    // FIXME: hot polling
	    pthread_mutex_lock(&VideoLockMutex);
	    err = VideoDecode();
	    pthread_mutex_unlock(&VideoLockMutex);
	    if (atomic_read(&decoder->SurfacesFilled) > 3) {
		Debug(3, "video: %d filled\n",
		    atomic_read(&decoder->SurfacesFilled));
		if (atomic_read(&decoder->SurfacesFilled) > max_filled) {
		    max_filled = atomic_read(&decoder->SurfacesFilled);
		}
	    }
	    if (err) {
		usleep(1 * 1000);	// nothing buffered
	    }

	}

	// wait up to 20ms
	// FIXME: 50hz video frame rate hardcoded
	abstime.tv_nsec += (decoder->Interlaced + 1) * 16 * 1000 * 1000;
	if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
	    // avoid overflow
	    abstime.tv_sec++;
	    abstime.tv_nsec -= 1000 * 1000 * 1000;
	}
	pthread_mutex_lock(&VideoMutex);
	while ((err =
		pthread_cond_timedwait(&VideoWakeupCond, &VideoMutex,
		    &abstime)) != ETIMEDOUT) {
	    Debug(3, "video/vaapi: pthread_cond_timedwait timeout\n");
	}
	pthread_mutex_unlock(&VideoMutex);
	if (err != ETIMEDOUT) {
	    Debug(3, "video/vaapi: pthread_cond_timedwait failed: %d\n", err);
	}
#ifdef USE_GLX
	//printf("video %p <-> %p\n", glXGetCurrentContext(), GlxThreadContext);
	if (!glXMakeCurrent(XlibDisplay, VideoWindow, GlxThreadContext)) {
	    GlxCheck();
	    Error(_("video/glx: can't make glx context current\n"));
	    return NULL;
	}
#endif

	filled = atomic_read(&decoder->SurfacesFilled);
	if (!filled) {
	    pthread_mutex_lock(&VideoLockMutex);
	    VaapiBlackSurface(decoder);
	    pthread_mutex_unlock(&VideoLockMutex);
	} else if (filled == 1) {
	    decoder->FramesDuped++;
	    ++decoder->FrameCounter;
	    Warning(_("video: display buffer empty, duping frame (%d/%d)\n"),
		decoder->FramesDuped, decoder->FrameCounter);
	    if (!(decoder->FrameCounter % 333)) {
		VaapiPrintFrames(decoder);
	    }
	}

	if (filled) {
	    pthread_mutex_lock(&VideoLockMutex);
	    VideoDisplayFrame();
	    pthread_mutex_unlock(&VideoLockMutex);
	}

	if (0) {
	    clock_gettime(CLOCK_REALTIME, &nowtime);
	    Debug(3, "video/vaapi: ticks %9lu ms\n",
		((nowtime.tv_sec - abstime.tv_sec) * 1000 * 1000 * 1000 +
		    (nowtime.tv_nsec - abstime.tv_nsec)) / (1000 * 1000));
	}
    }
#endif

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

    if (VideoThread) {
	if (pthread_cancel(VideoThread)) {
	    Error(_("video: can't cancel video display thread\n"));
	}
	if (pthread_join(VideoThread, &retval) || retval != PTHREAD_CANCELED) {
	    Error(_("video: can't cancel video display thread\n"));
	}
	pthread_cond_destroy(&VideoWakeupCond);
	pthread_mutex_destroy(&VideoLockMutex);
	pthread_mutex_destroy(&VideoMutex);
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
    return fmt[0];
}

/**
**	Test
*/
void VaapiTest(void)
{
    static int state;
    static uint32_t clock;
    static uint32_t last_tick;
    int i;

    //XLockDisplay(XlibDisplay);
    VideoPollEvent();
    for (i = 0; i < VaapiDecoderN; ++i) {
	int filled;
	VaapiDecoder *decoder;
	uint32_t start;
	uint32_t end;

	decoder = VaapiDecoders[i];
	filled = atomic_read(&decoder->SurfacesFilled);
	if (!filled) {			// trick to reset for new streams
	    state = 0;
	}
	switch (state) {
	    case 0:
		// new stream, wait until enough frames are buffered
		Debug(3, "video/state: wait on full\n");
		if (filled == 1) {
		    VaapiDisplayFrame();
		}
		if (filled < VIDEO_SURFACES_MAX - 1) {
		    continue;
		}
		state++;
	    case 1:
		// we have enough frames buffered, fill driver buffer
		Debug(3, "video/state: ringbuffer full\n");
		// intel has 0 buffers
		//VaapiDisplayFrame();
		state++;
	    case 2:
		// normal run, just play a buffered frame
		start = GetMsTicks();
		// intel 20ms / 40ms
		VaapiDisplayFrame();
		end = GetMsTicks();
		last_tick = end;
		if (start + (decoder->Interlaced + 1) * 20 < end) {
		    Debug(3, "video/state: display %u ms\n", end - start);
		}
		clock += (decoder->Interlaced + 1) * 20;
		if (last_tick < clock - 1000) {
		    clock = last_tick;
		}
		if (last_tick > clock + 1000) {
		    clock = last_tick;
		}
		//Debug(3, "video/state: %+4d ms\n", clock - last_tick);
		break;
	}
    }
    //XUnlockDisplay(XlibDisplay);
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
    if (!atomic_read(&decoder->Vaapi.SurfacesFilled)) {
	Debug(3, "video: new stream frame %d\n", GetMsTicks() - VideoSwitch);
    }
    //	if video output buffer is full, wait and display surface.
    if (atomic_read(&decoder->Vaapi.SurfacesFilled) >= VIDEO_SURFACES_MAX) {
	struct timespec abstime;

	abstime = decoder->Vaapi.FrameTime;
	abstime.tv_nsec += 16 * 1000 * 1000;
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

	VideoDisplayFrame();
    }
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	VaapiRenderFrame(&decoder->Vaapi, video_ctx, frame);
	return;
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	VdpauRenderFrame(&decoder->Vdpau, video_ctx, frame);
	return;
    }
#endif
    (void)decoder;
    (void)video_ctx;
    (void)frame;
    //Error(_("video: unsupported %p %p %p\n"), decoder, video_ctx, frame);
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

    // FIXME: utf _NET_WM_NAME
    xcb_icccm_set_wm_name(Connection, VideoWindow, XCB_ATOM_STRING, 8,
	sizeof("softhddevice") - 1, "softhddevice");
    xcb_icccm_set_wm_icon_name(Connection, VideoWindow, XCB_ATOM_STRING, 8,
	sizeof("softhddevice") - 1, "softhddevice");

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
	    xcb_icccm_set_wm_protocols(Connection, VideoWindow, reply->atom, 1,
		&WmDeleteWindowAtom);
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
#ifdef USE_VAAPI
    if (VideoVaapiEnabled) {
	VideoVaapiInit(display_name);
    }
#endif
#ifdef USE_VDPAU
    if (VideoVdpauEnabled) {
	VideoVdpauInit(display_name);
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
