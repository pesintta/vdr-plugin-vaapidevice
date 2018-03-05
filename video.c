/// Copyright (C) 2009 - 2015 by Johns. All Rights Reserved.
/// Copyright (C) 2018 by pesintta, rofafor.
///
/// SPDX-License-Identifier: AGPL-3.0-only

///
/// This module contains all video rendering functions.
///
/// Uses Xlib where it is needed for VA-API.  XCB is used for
/// everything else.
///

#ifndef AV_INFO_TIME
#define AV_INFO_TIME (50 * 60)		///< a/v info every minute
#endif

#include <sys/time.h>
#include <sys/shm.h>
#include <sys/ipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#ifndef __USE_GNU
#define __USE_GNU
#include <pthread.h>
#include <time.h>
#include <signal.h>
#endif

#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>
#include <xcb/screensaver.h>
#include <xcb/dpms.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_ewmh.h>

#include <va/va_x11.h>
#if !VA_CHECK_VERSION(1,0,0)
#warning "libva is too old - please, upgrade!"
#define VA_FOURCC_I420 VA_FOURCC('I', '4', '2', '0')
#endif
#include <va/va_vpp.h>

#include <libavcodec/avcodec.h>
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,64,100)
#error "libavcodec is too old - please, upgrade!"
#endif
#include <libswscale/swscale.h>
#include <libavcodec/vaapi.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>

#include "iatomic.h"			// portable atomic_t
#include "misc.h"
#include "video.h"
#include "audio.h"
#include "codec.h"

#define ARRAY_ELEMS(array) (sizeof(array)/sizeof(array[0]))

#define TO_AVHW_DEVICE_CTX(x) ((AVHWDeviceContext*)x->data)
#define TO_AVHW_FRAMES_CTX(x) ((AVHWFramesContext*)x->data)

#define TO_VAAPI_DEVICE_CTX(x) ((AVVAAPIDeviceContext*)TO_AVHW_DEVICE_CTX(x)->hwctx)
#define TO_VAAPI_FRAMES_CTX(x) ((AVVAAPIFramesContext*)TO_AVHW_FRAMES_CTX(x)->hwctx)

//----------------------------------------------------------------------------
//  Declarations
//----------------------------------------------------------------------------

///
/// Video resolutions selector.
///
typedef enum _video_resolutions_
{
    VideoResolution576i,		///< ...x576 interlaced
    VideoResolution720p,		///< ...x720 progressive
    VideoResolutionFake1080i,		///< 1280x1080 1440x1080 interlaced
    VideoResolution1080i,		///< 1920x1080 interlaced
    VideoResolutionUHD,			///< UHD progressive
    VideoResolutionMax			///< number of resolution indexs
} VideoResolutions;

///
/// Video scaling modes.
///
typedef enum _video_scaling_modes_
{
    VideoScalingNormal,			///< normal scaling
    VideoScalingFast,			///< fastest scaling
    VideoScalingHQ,			///< high quality scaling
    VideoScalingAnamorphic,		///< anamorphic scaling
} VideoScalingModes;

///
/// Video zoom modes.
///
typedef enum _video_zoom_modes_
{
    VideoNormal,			///< normal
    VideoStretch,			///< stretch to all edges
    VideoCenterCutOut,			///< center and cut out
    VideoAnamorphic,			///< anamorphic scaled (unsupported)
} VideoZoomModes;

///
/// Video color space conversions.
///
typedef enum _video_color_space_
{
    VideoColorSpaceNone,		///< no conversion
    VideoColorSpaceBt601,		///< ITU.BT-601 Y'CbCr
    VideoColorSpaceBt709,		///< ITU.BT-709 HDTV Y'CbCr
    VideoColorSpaceSmpte240		///< SMPTE-240M Y'PbPr
} VideoColorSpace;

///
/// Video output module structure and typedef.
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
    enum AVPixelFormat (*const get_format) (VideoHwDecoder *, AVCodecContext *, const enum AVPixelFormat *);
    void (*const RenderFrame) (VideoHwDecoder *, const AVCodecContext *, const AVFrame *);
    void (*const SetClock) (VideoHwDecoder *, int64_t);
     int64_t(*const GetClock) (const VideoHwDecoder *);
    void (*const SetClosing) (const VideoHwDecoder *);
    void (*const ResetStart) (const VideoHwDecoder *);
    void (*const SetTrickSpeed) (const VideoHwDecoder *, int);
    uint8_t *(*const GrabOutput)(int *, int *, int *);
    void (*const GetStats) (VideoHwDecoder *, int *, int *, int *, int *);
    void (*const SetBackground) (uint32_t);
    void (*const SetVideoMode) (void);
    void (*const ResetAutoCrop) (void);

    /// module display handler thread
    void (*const DisplayHandlerThread) (void);

    void (*const OsdClear) (void);	///< clear OSD
    /// draw OSD ARGB area
    void (*const OsdDrawARGB) (int, int, int, int, int, const uint8_t *, int, int);
    void (*const OsdInit) (int, int);	///< initialize OSD
    void (*const OsdExit) (void);	///< cleanup OSD

    int (*const Init) (const char *);	///< initialize video output module
    void (*const Exit) (void);		///< cleanup video output module
} VideoModule;

///
/// Video configuration values typedef.
///
typedef struct _video_config_values_
{
    int active;
    float min_value;
    float max_value;
    float def_value;
    float step;
    float scale;			// scale is normalized to match UI requirements
    float drv_scale;			// re-normalizing requires the original scale required for latching data to the driver
} VideoConfigValues;

//----------------------------------------------------------------------------
//  Defines
//----------------------------------------------------------------------------

#define VIDEO_SURFACES_MAX	4	    ///< video output surfaces for queue
#define POSTPROC_SURFACES_MAX	8	///< video postprocessing surfaces for queue

//----------------------------------------------------------------------------
//  Variables
//----------------------------------------------------------------------------

// Brightness (-100.00 - 100.00 ++ 1.00 = 0.00)
static VideoConfigValues VaapiConfigBrightness = {.active = 0,.min_value = -100.0,.max_value = 100.0,.def_value =
	0.0,.step = 1.0,.scale = 1.0,.drv_scale = 1.0
};

// Contrast (0.00 - 10.00 ++ 0.10 = 1.00)
static VideoConfigValues VaapiConfigContrast = {.active = 0,.min_value = 0.0,.max_value = 10.0,.def_value = 1.0,.step =
	0.1,.scale = 1.0,.drv_scale = 1.0
};

// Saturation (0.00 - 10.00 ++ 0.10 = 1.00)
static VideoConfigValues VaapiConfigSaturation = {.active = 0,.min_value = 0.0,.max_value = 10.0,.def_value =
	1.0,.step = 0.1,.scale = 1.0,.drv_scale = 1.0
};

// Hue (-180.00 - 180.00 ++ 1.00 = 0.00)
static VideoConfigValues VaapiConfigHue = {.active = 0,.min_value = -180.0,.max_value = 180.0,.def_value = 0.0,.step =
	1.0,.scale = 1.0,.drv_scale = 1.0
};

// Denoise (0.00 - 1.00 ++ 0.03 = 0.50)
static VideoConfigValues VaapiConfigDenoise = {.active = 0,.min_value = 0.0,.max_value = 1.0,.def_value = 0.5,.step =
	0.03,.scale = 1.0,.drv_scale = 1.0
};

// Sharpen (0.00 - 1.00 ++ 0.03 = 0.50)
static VideoConfigValues VaapiConfigSharpen = {.active = 0,.min_value = 0.0,.max_value = 1.0,.def_value = 0.5,.step =
	0.03,.scale = 1.0,.drv_scale = 1.0
};

static VideoConfigValues VaapiConfigStde = {.active = 1,.min_value = 0.0,.max_value = 4.0,.def_value = 0.0,.step =
	1.0,.scale = 1.0,.drv_scale = 1.0
};

char VideoIgnoreRepeatPict;		///< disable repeat pict warning

static const char *VideoDriverName = "va-api";	///< video output device - default to va-api

static Display *XlibDisplay;		///< Xlib X11 display
static xcb_connection_t *Connection;	///< xcb connection
static xcb_colormap_t VideoColormap;	///< video colormap
static xcb_window_t VideoWindow;	///< video window
static xcb_screen_t const *VideoScreen; ///< video screen
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

static char VideoSurfaceModesChanged;	///< flag surface modes changed

static uint32_t VideoBackground;	///< video background color
static char VideoStudioLevels;		///< flag use studio levels

    /// Default skin tone enhancement mode.
static int VideoSkinToneEnhancement = 0;

    /// Default deinterlace mode.
static VAProcDeinterlacingType VideoDeinterlace[VideoResolutionMax];

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
    VideoColorSpaceBt709, VideoColorSpaceBt709
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

static xcb_atom_t WmDeleteWindowAtom;	///< WM delete message atom
static xcb_atom_t NetWmState;		///< wm-state message atom
static xcb_atom_t NetWmStateFullscreen; ///< fullscreen wm-state message atom

#ifdef DEBUG
extern uint32_t VideoSwitch;		///< ticks for channel switch
#endif
extern void AudioVideoReady(int64_t);	///< tell audio video is ready
extern int IsReplay(void);

static pthread_t VideoThread;		///< video decode thread
static pthread_cond_t VideoWakeupCond;	///< wakeup condition variable
static pthread_mutex_t VideoMutex;	///< video condition mutex
static pthread_mutex_t VideoLockMutex;	///< video lock mutex
extern pthread_mutex_t PTS_mutex;	///< PTS mutex
extern pthread_mutex_t ReadAdvance_mutex;   ///< PTS mutex

static char OsdShown;			///< flag show osd
static int OsdDirtyX;			///< osd dirty area x
static int OsdDirtyY;			///< osd dirty area y
static int OsdDirtyWidth;		///< osd dirty area width
static int OsdDirtyHeight;		///< osd dirty area height

static int64_t VideoDeltaPTS;		///< FIXME: fix pts

static char DPMSDisabled;		///< flag we have disabled dpms

uint32_t mutex_start_time;
uint32_t max_mutex_delay = 1;

//----------------------------------------------------------------------------
//  Common Functions
//----------------------------------------------------------------------------

static void VideoThreadLock(void);	///< lock video thread
static void VideoThreadUnlock(void);	///< unlock video thread
static void VideoThreadExit(void);	///< exit/kill video thread

static void X11SuspendScreenSaver(xcb_connection_t *, int);
static int X11HaveDPMS(xcb_connection_t *);
static void X11DPMSReenable(xcb_connection_t *);
static void X11DPMSDisable(xcb_connection_t *);

///
/// Update video pts.
///
/// @param pts_p    pointer to pts
/// @param interlaced	interlaced flag (frame isn't right)
/// @param frame    frame to display
///
/// @note frame->interlaced_frame can't be used for interlace detection
///
static void VideoSetPts(int64_t * pts_p, int interlaced, const AVCodecContext * video_ctx, const AVFrame * frame)
{
    int64_t pts;
    int duration;

    //
    //	Get duration for this frame.
    //	FIXME: using framerate as workaround for av_frame_get_pkt_duration
    //
    if (video_ctx->framerate.num && video_ctx->framerate.den) {
	duration = 1000 * video_ctx->framerate.den / video_ctx->framerate.num;
    } else {
	duration = interlaced ? 40 : 20;    // 50Hz -> 20ms default
    }
    Debug8("video: %d/%d %" PRIx64 " -> %d", video_ctx->framerate.den, video_ctx->framerate.num,
	av_frame_get_pkt_duration(frame), duration);

    // update video clock
    if (*pts_p != (int64_t) AV_NOPTS_VALUE) {
	*pts_p += duration * 90;
	//Info("video: %s +pts", Timestamp2String(*pts_p));
    }
    //av_opt_ptr(avcodec_get_frame_class(), frame, "best_effort_timestamp");
    //pts = frame->best_effort_timestamp;
    pts = frame->pts;
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
		    Debug8("video: %#012" PRIx64 "->%#012" PRIx64 " delta%+4" PRId64 " pts", *pts_p, pts,
			pts - *pts_p);
		}
		return;
	    }
	} else {			// first new clock value
	    AudioVideoReady(pts);
	}
	if (*pts_p != pts) {
	    Debug8("video: %#012" PRIx64 "->%#012" PRIx64 " delta=%4" PRId64 " pts", *pts_p, pts, pts - *pts_p);
	    *pts_p = pts;
	}
    }
}

///
/// Update output for new size or aspect ratio.
///
/// @param input_aspect_ratio	video stream aspect
///
static void VideoUpdateOutput(AVRational input_aspect_ratio, int input_width, int input_height,
    VideoResolutions resolution, int video_x, int video_y, int video_width, int video_height, int *output_x,
    int *output_y, int *output_width, int *output_height, int *crop_x, int *crop_y, int *crop_width, int *crop_height)
{
    AVRational display_aspect_ratio;
    AVRational tmp_ratio;

    if (!input_aspect_ratio.num || !input_aspect_ratio.den) {
	input_aspect_ratio.num = 1;
	input_aspect_ratio.den = 1;
	Debug7("video: aspect defaults to %d:%d", input_aspect_ratio.num, input_aspect_ratio.den);
    }

    av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den, input_width * input_aspect_ratio.num,
	input_height * input_aspect_ratio.den, 1024 * 1024);

    // InputWidth/Height can be zero = uninitialized
    if (!input_aspect_ratio.num || !input_aspect_ratio.den) {
	input_aspect_ratio.num = 1;
	input_aspect_ratio.den = 1;
    }

    display_aspect_ratio.num = VideoScreen->width_in_pixels * VideoScreen->height_in_millimeters;
    display_aspect_ratio.den = VideoScreen->height_in_pixels * VideoScreen->width_in_millimeters;

    display_aspect_ratio = av_mul_q(input_aspect_ratio, display_aspect_ratio);
    Debug7("video: aspect %d:%d", display_aspect_ratio.num, display_aspect_ratio.den);

    *crop_x = VideoCutLeftRight[resolution];
    *crop_y = VideoCutTopBottom[resolution];
    *crop_width = VideoWindowWidth - VideoCutLeftRight[resolution] * 2;
    *crop_height = VideoWindowHeight - VideoCutTopBottom[resolution] * 2;

    // FIXME: store different positions for the ratios
    tmp_ratio.num = 4;
    tmp_ratio.den = 3;
#ifdef DEBUG
    Debug8("video: ratio %d:%d %d:%d", input_aspect_ratio.num, input_aspect_ratio.den, display_aspect_ratio.num,
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
	(video_height * display_aspect_ratio.num + display_aspect_ratio.den - 1) / display_aspect_ratio.den;
    *output_height =
	(video_width * display_aspect_ratio.den + display_aspect_ratio.num - 1) / display_aspect_ratio.num;
    if (*output_width > video_width) {
	*output_width = video_width;
	*output_y += (video_height - *output_height) / 2;
    } else if (*output_height > video_height) {
	*output_height = video_height;
	*output_x += (video_width - *output_width) / 2;
    }
    Debug7("video: aspect output %dx%d%+d%+d", *output_width, *output_height, *output_x, *output_y);
    return;

  stretch:
    *output_x = video_x;
    *output_y = video_y;
    *output_width = video_width;
    *output_height = video_height;
    Debug7("video: stretch output %dx%d%+d%+d", *output_width, *output_height, *output_x, *output_y);
    return;

  center_cut_out:
    *output_x = video_x;
    *output_y = video_y;
    *output_height = video_height;
    *output_width = video_width;

    *crop_width = (video_height * display_aspect_ratio.num + display_aspect_ratio.den - 1) / display_aspect_ratio.den;
    *crop_height = (video_width * display_aspect_ratio.den + display_aspect_ratio.num - 1) / display_aspect_ratio.num;

    // look which side must be cut
    if (*crop_width > video_width) {
	int tmp;

	*crop_height = input_height - VideoCutTopBottom[resolution] * 2;

	// adjust scaling
	tmp = ((*crop_width - video_width) * input_width) / (2 * video_width);
	// FIXME: round failure?
	if (tmp > *crop_x) {
	    *crop_x = tmp;
	}
	*crop_width = input_width - *crop_x * 2;
    } else if (*crop_height > video_height) {
	int tmp;

	*crop_width = input_width - VideoCutLeftRight[resolution] * 2;

	// adjust scaling
	tmp = ((*crop_height - video_height) * input_height)
	    / (2 * video_height);
	// FIXME: round failure?
	if (tmp > *crop_y) {
	    *crop_y = tmp;
	}
	*crop_height = input_height - *crop_y * 2;
    } else {
	*crop_width = input_width - VideoCutLeftRight[resolution] * 2;
	*crop_height = input_height - VideoCutTopBottom[resolution] * 2;
    }
    Debug7("video: aspect crop %dx%d%+d%+d", *crop_width, *crop_height, *crop_x, *crop_y);
    return;
}

//----------------------------------------------------------------------------
//  common functions
//----------------------------------------------------------------------------

///
/// Calculate resolution group.
///
/// @param width    video picture raw width
/// @param height   video picture raw height
/// @param interlace	flag interlaced video picture
///
/// @note interlace isn't used yet and probably wrong set by caller.
///
static VideoResolutions VideoResolutionGroup(int width, int height, __attribute__ ((unused))
    int interlace)
{
    if (height == 2160) {
	return VideoResolutionUHD;
    }
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

///
/// Clamp given value against config limits
///
/// @param config   config struct
/// @param valueIn  sample value
/// @return clamped value
///
static inline int VideoConfigClamp(VideoConfigValues * config, float valueIn)
{
    if (valueIn < config->min_value)
	return config->min_value;
    else if (valueIn > config->max_value)
	return config->def_value;
    return valueIn;
}

//----------------------------------------------------------------------------
//  auto-crop
//----------------------------------------------------------------------------

///
/// auto-crop context structure and typedef.
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

#define YBLACK 0x20			///< below is black
#define UVBLACK 0x80			///< around is black
#define M64 UINT64_C(0x0101010101010101)    ///< 64bit multiplicator

    /// auto-crop percent of video width to ignore logos
static const int AutoCropLogoIgnore = 24;
static int AutoCropInterval;		///< auto-crop check interval
static int AutoCropDelay;		///< auto-crop switch delay
static int AutoCropTolerance;		///< auto-crop tolerance

///
/// Detect black line Y.
///
/// @param data Y plane pixel data
/// @param length   number of pixel to check
/// @param pitch    offset of pixels
///
/// @note 8 pixel are checked at once, all values must be 8 aligned
///
static int AutoCropIsBlackLineY(const uint8_t * data, int length, int pitch)
{
    int n;
    int o;
    uint64_t r;
    const uint64_t *p;

#ifdef DEBUG
    if ((size_t) data & 0x7 || pitch & 0x7) {
	abort();
    }
#endif
    p = (const uint64_t *)data;
    n = length;				// FIXME: can remove n
    o = pitch / 8;

    r = 0UL;
    while (--n >= 0) {
	r |= *p;
	p += o;
    }

    // below YBLACK(0x20) is black
    return !(r & ~((YBLACK - 1) * M64));
}

///
/// Auto detect black borders and crop them.
///
/// @param autocrop auto-crop variables
/// @param width    frame width in pixel
/// @param height   frame height in pixel
/// @param data frame planes data (Y, U, V)
/// @param pitches  frame planes pitches (Y, U, V)
///
/// @note FIXME: can reduce the checked range, left, right crop isn't
/// used yet.
///
/// @note FIXME: only Y is checked, for black.
///
static void AutoCropDetect(AutoCropCtx * autocrop, int width, int height, void *data[3], uint32_t pitches[3])
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
	if (!AutoCropIsBlackLineY(data_y + logo_skip + y * length_y, (width - 2 * logo_skip) / 8, 8)) {
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
	if (!AutoCropIsBlackLineY(data_y + logo_skip + y * length_y, (width - 2 * logo_skip) / 8, 8)) {
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
	if (!AutoCropIsBlackLineY(data_y + x + SKIP_Y * length_y, height - 2 * SKIP_Y, length_y)) {
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
	if (!AutoCropIsBlackLineY(data_y + x + SKIP_Y * length_y, height - 2 * SKIP_Y * 8, length_y)) {
	    if (x == width - SKIP_X - 8) {
		x = width - 1;
	    }
	    x2 = x;
	    break;
	}
    }

    autocrop->X1 = x1;
    autocrop->X2 = x2;
    autocrop->Y1 = y1;
    autocrop->Y2 = y2;
}

//----------------------------------------------------------------------------
//  VA-API
//----------------------------------------------------------------------------

AVBufferRef *HwDeviceContext;		///< ffmpeg HW device context

static VADisplay *VaDisplay;		///< VA-API display

static VAImage VaOsdImage = {
    .image_id = VA_INVALID_ID
};					///< osd VA-API image

static VASubpictureID VaOsdSubpicture = VA_INVALID_ID;	///< osd VA-API subpicture
static char VaapiUnscaledOsd;		///< unscaled osd supported

static char VaapiVideoProcessing;	///< supports video processing

    /// VA-API decoder typedef
typedef struct _vaapi_decoder_ VaapiDecoder;

///
/// VA-API decoder
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
    unsigned SurfaceDeintTable[VideoResolutionMax];

    enum AVPixelFormat PixFmt;		///< ffmpeg frame pixfmt
    int WrongInterlacedWarned;		///< warning about interlace flag issued
    int Interlaced;			///< ffmpeg interlaced flag
    int Deinterlaced;			///< vpp deinterlace was run / not run
    int TopFieldFirst;			///< ffmpeg top field displayed first

    int GetPutImage;			///< flag get/put image can be used
    VAImage Image[1];			///< image buffer to update surface

    VAEntrypoint VppEntrypoint;		///< VA-API postprocessing entrypoint

    VAConfigID VppConfig;		///< VPP Config
    VAContextID vpp_ctx;		///< VPP Context

    int InputWidth;			///< video input width
    int InputHeight;			///< video input height
    AVRational InputAspect;		///< video input aspect ratio
    VideoResolutions Resolution;	///< resolution group

    int CropX;				///< video crop x
    int CropY;				///< video crop y
    int CropWidth;			///< video crop width
    int CropHeight;			///< video crop height
    AutoCropCtx AutoCrop[1];		///< auto-crop variables
    VASurfaceID BlackSurface;		///< empty black surface

    /// video surface ring buffer
    VASurfaceID SurfacesRb[VIDEO_SURFACES_MAX];
    VASurfaceID PostProcSurfacesRb[POSTPROC_SURFACES_MAX];  ///< Posprocessing result surfaces

    VASurfaceID *ForwardRefSurfaces;	///< Forward referencing surfaces for post processing
    VASurfaceID *BackwardRefSurfaces;	///< Backward referencing surfaces for post processing

    unsigned int ForwardRefCount;	///< Current number of forward references
    unsigned int BackwardRefCount;	///< Current number of backward references

    VASurfaceID PlaybackSurface;	///< Currently playing surface

    int SurfaceWrite;			///< write pointer
    int SurfaceRead;			///< read pointer
    atomic_t SurfacesFilled;		///< how many of the buffer is used

    int PostProcSurfaceWrite;		///< postprocessing write pointer

    int SurfaceField;			///< current displayed field
    int TrickSpeed;			///< current trick speed
    int TrickCounter;			///< current trick speed counter
    struct timespec FrameTime;		///< time of last display
    VideoStream *Stream;		///< video stream
    int Closing;			///< flag about closing current stream
    int SyncOnAudio;			///< flag sync to audio
    int64_t PTS;			///< video PTS clock

    int LastAVDiff;			///< last audio - video difference
    int SyncCounter;			///< counter to sync frames
    int StartCounter;			///< counter for video start
    int FramesDuped;			///< number of frames duplicated
    int FramesMissed;			///< number of frames missed
    int FramesDropped;			///< number of frames dropped
    int FrameCounter;			///< number of frames decoded
    int FramesDisplayed;		///< number of frames displayed
    VABufferID filters[VAProcFilterCount];  ///< video postprocessing filters via vpp
    VABufferID gpe_filters[VAProcFilterCount];	///< video postprocessing filters via gpe
    unsigned filter_n;			///< number of postprocessing filters
    unsigned gpe_filter_n;		///< number of gpe postprocessing filters
    unsigned SupportedDeinterlacers[VAProcDeinterlacingCount];	///< supported deinterlacing methods
    VABufferID *vpp_deinterlace_buf;	///< video postprocessing deinterlace buffer
    VABufferID *vpp_denoise_buf;	///< video postprocessing denoise buffer
    VABufferID *vpp_cbal_buf;		///< video color balance filters via vpp
    VABufferID *vpp_sharpen_buf;	///< video postprocessing sharpen buffer
    VABufferID *vpp_stde_buf;		///< video postprocessing skin tone enhancement buffer
    int vpp_brightness_idx;		///< video postprocessing brightness buffer index
    int vpp_contrast_idx;		///< video postprocessing contrast buffer index
    int vpp_hue_idx;			///< video postprocessing hue buffer index
    int vpp_saturation_idx;		///< video postprocessing saturation buffer index
};

static VaapiDecoder *VaapiDecoders[1];	///< open decoder streams
static int VaapiDecoderN;		///< number of decoder streams

    /// forward display back surface
static void VaapiBlackSurface(VaapiDecoder *);

    /// forward definition release surface
static void VaapiReleaseSurface(VaapiDecoder *, VASurfaceID);

//----------------------------------------------------------------------------
//  VA-API Functions
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

extern int SysLogLevel;			///< VDR's global log level

///
/// Output video messages.
///
/// Reduce output.
///
/// @param level    message level (Error, Info, Debug, ...)
/// @param format   printf format string (NULL to flush messages)
/// @param ...	printf arguments
///
/// @returns true, if message shown
///
static int VaapiMessage(int level, const char *format, ...)
{
    if (SysLogLevel > level) {
	static const char *last_format;
	static char buf[256];
	va_list ap;

	va_start(ap, format);
	if (format != last_format) {	// don't repeat same message
	    if (buf[0]) {		// print last repeated message
		switch (level) {
		    case 0:
			Error("%s", buf);
			break;
		    case 1:
			Info("%s", buf);
			break;
		    default:
			Debug("%s", buf);
			break;
		}
		buf[0] = '\0';
	    }

	    if (format) {
		last_format = format;
		switch (level) {
		    case 0:
			Error(format, ap);
			break;
		    case 1:
			Info(format, ap);
			break;
		    default:
			Debug(format, ap);
			break;
		}
	    }
	    va_end(ap);
	    return 1;
	}
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
    }
    return 0;
}

//  Surfaces -------------------------------------------------------------

///
/// Associate OSD with surface.
///
/// @param decoder  VA-API decoder
///
static void VaapiAssociate(VaapiDecoder * decoder)
{

}

///
/// Deassociate OSD with surface.
///
/// @param decoder  VA-API decoder
///
static void VaapiDeassociate(VaapiDecoder * decoder)
{
    if (VaOsdSubpicture != VA_INVALID_ID) {
	vaDeassociateSubpicture(decoder->VaDisplay, VaOsdSubpicture, decoder->PostProcSurfacesRb,
	    POSTPROC_SURFACES_MAX);
    }
}

///
/// Create surfaces for VA-API decoder.
///
/// @param decoder  VA-API decoder
/// @param width    surface source/video width
/// @param height   surface source/video height
///
static void VaapiCreateSurfaces(VaapiDecoder * decoder, int width, int height)
{
    if (vaCreateSurfaces(decoder->VaDisplay, VA_RT_FORMAT_YUV420, width, height, decoder->PostProcSurfacesRb,
	    POSTPROC_SURFACES_MAX, NULL, 0) != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't create %d postproc surfaces", POSTPROC_SURFACES_MAX);
    }
}

///
/// Destroy surfaces of VA-API decoder.
///
/// @param decoder  VA-API decoder
///
static void VaapiDestroySurfaces(VaapiDecoder * decoder)
{
    Debug7("video/vaapi: %s:", __FUNCTION__);

    //
    //	update OSD associate
    //
    VaapiDeassociate(decoder);

    if (vaDestroySurfaces(decoder->VaDisplay, decoder->PostProcSurfacesRb, POSTPROC_SURFACES_MAX) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't destroy %d surfaces", POSTPROC_SURFACES_MAX);
    }
}

///
/// Get a free surface.
///
/// @param decoder  VA-API decoder
///
/// @returns the oldest free surface
///
static VASurfaceID VaapiGetPluginSurface(VaapiDecoder * decoder)
{
    VAStatus va_status;
    VASurfaceID surface;
    VASurfaceStatus status;

    /* Get next postproc surface to write from ring buffer */
    decoder->PostProcSurfaceWrite = (decoder->PostProcSurfaceWrite + 1) % POSTPROC_SURFACES_MAX;
    surface = decoder->PostProcSurfacesRb[decoder->PostProcSurfaceWrite];
    if (surface == VA_INVALID_ID) {
	Debug8("video/vaapi: surface at %d (%#010x) not initialized", decoder->PostProcSurfaceWrite, surface);
	return VA_INVALID_ID;
    }
    va_status = vaQuerySurfaceStatus(decoder->VaDisplay, surface, &status);
    if (va_status != VA_STATUS_SUCCESS) {
	// this fails with mpeg softdecoder
	Error("video/vaapi: vaQuerySurface failed: %s", vaErrorStr(va_status));
	status = VASurfaceReady;
    }
    if (status != VASurfaceReady) {
	Debug8("video/vaapi: surface %#010x not ready: %d", surface, status);
	return VA_INVALID_ID;
    }

    return surface;
}

///
/// Release a surface.
///
/// @param decoder  VA-API decoder
/// @param surface  surface no longer used
///
static void VaapiReleaseSurface(VaapiDecoder * decoder, VASurfaceID surface)
{

}

//  Init/Exit ------------------------------------------------------------

///
/// Debug VA-API decoder frames drop...
///
/// @param decoder  video hardware decoder
///
static void VaapiPrintFrames(const VaapiDecoder * decoder)
{
    Debug7("video/vaapi: %d missed, %d duped, %d dropped frames of %d,%d", decoder->FramesMissed, decoder->FramesDuped,
	decoder->FramesDropped, decoder->FrameCounter, decoder->FramesDisplayed);
}

///
/// Normalize config values for UI
///
/// @param config   config struct to normalize
/// @param valueMin range min value
/// @param valueMax range max value
/// @param valueDef range default value
/// @param step range step
///
static inline void VaapiNormalizeConfig(VideoConfigValues * config, float valueMin, float valueMax, float valueDef,
    float step)
{
    config->min_value = valueMin;
    config->max_value = valueMax;
    config->def_value = valueDef;
    config->step = step;
    config->scale = config->drv_scale;
    // normalize values for UI
    while (config && config->step < 1) {
	config->min_value *= 10;
	config->max_value *= 10;
	config->def_value *= 10;
	config->step *= 10;
	config->scale /= 10;
    }
}

///
/// Initialize surface flags.
///
/// @param decoder  video hardware decoder
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
		decoder->SurfaceFlagsTable[i] |= VA_FILTER_SCALING_HQ;
		break;
	    case VideoScalingAnamorphic:
		// intel backend supports only VA_FILTER_SCALING_NL_ANAMORPHIC;
		// FIXME: Highlevel should display 4:3 as 16:9 to support this
		decoder->SurfaceFlagsTable[i] |= VA_FILTER_SCALING_NL_ANAMORPHIC;
		break;
	}

	// deinterlace flags
	switch (VideoDeinterlace[i]) {
	    case VAProcDeinterlacingNone:
	    case VAProcDeinterlacingBob:
	    case VAProcDeinterlacingWeave:
	    case VAProcDeinterlacingMotionAdaptive:
	    case VAProcDeinterlacingMotionCompensated:
		decoder->SurfaceDeintTable[i] = VideoDeinterlace[i];
		break;
	    default:
		Error("Selected deinterlacer for resolution %d is not supported by HW", i);
		decoder->SurfaceDeintTable[i] = VAProcDeinterlacingNone;
		break;
	}
    }
    if (decoder->vpp_denoise_buf) {
	VAProcFilterParameterBuffer *denoise_param;
	VAStatus va_status = vaMapBuffer(decoder->VaDisplay, *decoder->vpp_denoise_buf, (void **)&denoise_param);

	if (va_status == VA_STATUS_SUCCESS) {

	    /* Assuming here that the type is set before and does not need to be modified */
	    denoise_param->value = VideoDenoise[decoder->Resolution] * VaapiConfigDenoise.scale;
	    vaUnmapBuffer(VaDisplay, *decoder->vpp_denoise_buf);
	}
    }
    if (decoder->vpp_sharpen_buf) {
	VAProcFilterParameterBuffer *sharpen_param;
	VAStatus va_status = vaMapBuffer(VaDisplay, *decoder->vpp_sharpen_buf, (void **)&sharpen_param);

	if (va_status == VA_STATUS_SUCCESS) {
	    /* Assuming here that the type is set before and does not need to be modified */
	    sharpen_param->value = VideoSharpen[decoder->Resolution] * VaapiConfigSharpen.scale;
	    vaUnmapBuffer(VaDisplay, *decoder->vpp_sharpen_buf);
	}
    }
    if (decoder->vpp_stde_buf) {
	VAProcFilterParameterBuffer *stde_param;
	VAStatus va_status = vaMapBuffer(VaDisplay, *decoder->vpp_stde_buf, (void **)&stde_param);

	if (va_status == VA_STATUS_SUCCESS) {
	    /* Assuming here that the type is set before and does not need to be modified */
	    stde_param->value = VideoSkinToneEnhancement * VaapiConfigStde.scale;
	    vaUnmapBuffer(VaDisplay, *decoder->vpp_stde_buf);
	}
    }
}

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,107,100) // FFMPEG 3.4
#if VA_CHECK_VERSION(1,0,0)
static void VaapiInfoCallback(void *context, const char *msg)
{
    Debug13("libva: %s", msg);
}

static void VaapiErrorCallback(void *context, const char *msg)
{
    Debug13("libva/error: %s", msg);
}
#endif
#endif

///
/// Allocate new VA-API decoder.
///
/// @returns a new prepared VA-API hardware decoder.
///
static VaapiDecoder *VaapiNewHwDecoder(VideoStream * stream)
{
    VaapiDecoder *decoder;
    int i;
    AVBufferRef *hw_device_ctx;

    if (VaapiDecoderN == 1) {
	Fatal("video/vaapi: out of decoders");
    }

    if (!(decoder = calloc(1, sizeof(*decoder)))) {
	Fatal("video/vaapi: out of memory");
    }

    if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, X11DisplayName, NULL, 0)) {
	Fatal("codec: can't allocate HW video codec context");
    }
    HwDeviceContext = av_buffer_ref(hw_device_ctx);

    VaDisplay = TO_VAAPI_DEVICE_CTX(HwDeviceContext)->display;
    decoder->VaDisplay = VaDisplay;
    decoder->Window = VideoWindow;
    decoder->VideoX = 0;
    decoder->VideoY = 0;
    decoder->VideoWidth = VideoWindowWidth;
    decoder->VideoHeight = VideoWindowHeight;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57,107,100) // FFMPEG 3.4
#if VA_CHECK_VERSION(1,0,0)
    vaSetInfoCallback(decoder->VaDisplay, VaapiInfoCallback, NULL);
    vaSetErrorCallback(decoder->VaDisplay, VaapiErrorCallback, NULL);
#endif
#endif

    VaapiInitSurfaceFlags(decoder);

    decoder->Image->image_id = VA_INVALID_ID;

    // setup video surface ring buffer
    atomic_set(&decoder->SurfacesFilled, 0);

    for (i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VA_INVALID_ID;
    }
    for (i = 0; i < POSTPROC_SURFACES_MAX; ++i) {
	decoder->PostProcSurfacesRb[i] = VA_INVALID_ID;
    }

    // Initialize postprocessing surfaces to 0
    // They are allocated on-demand
    if (decoder->ForwardRefSurfaces)
	free(decoder->ForwardRefSurfaces);
    decoder->ForwardRefSurfaces = NULL;
    decoder->ForwardRefCount = 0;

    decoder->PlaybackSurface = VA_INVALID_ID;

    if (decoder->BackwardRefSurfaces)
	free(decoder->BackwardRefSurfaces);
    decoder->BackwardRefSurfaces = NULL;
    decoder->BackwardRefCount = 0;

    // Initialize vpp filter chain
    for (i = 0; i < VAProcFilterCount; ++i) {
	decoder->filters[i] = VA_INVALID_ID;
	decoder->gpe_filters[i] = VA_INVALID_ID;
    }
    decoder->filter_n = 0;
    decoder->gpe_filter_n = 0;

    memset(&decoder->SupportedDeinterlacers, 0, sizeof(decoder->SupportedDeinterlacers));

    decoder->vpp_deinterlace_buf = NULL;
    decoder->vpp_denoise_buf = NULL;
    decoder->vpp_sharpen_buf = NULL;
    decoder->vpp_stde_buf = NULL;
    decoder->vpp_brightness_idx = -1;
    decoder->vpp_contrast_idx = -1;
    decoder->vpp_saturation_idx = -1;
    decoder->vpp_hue_idx = -1;

    decoder->BlackSurface = VA_INVALID_ID;

    //
    //	Setup ffmpeg vaapi context
    //
    decoder->VppEntrypoint = VA_INVALID_ID;
    decoder->VppConfig = VA_INVALID_ID;
    decoder->vpp_ctx = VA_INVALID_ID;

    decoder->OutputWidth = VideoWindowWidth;
    decoder->OutputHeight = VideoWindowHeight;

    decoder->PixFmt = AV_PIX_FMT_NONE;

    decoder->Stream = stream;
    if (!VaapiDecoderN) {		// FIXME: hack sync on audio
	decoder->SyncOnAudio = 1;
    }
    decoder->Closing = -300 - 1;

    decoder->PTS = AV_NOPTS_VALUE;

    decoder->GetPutImage = 1;

    VaapiDecoders[VaapiDecoderN++] = decoder;

    return decoder;
}

///
/// Cleanup VA-API.
///
/// @param decoder  va-api hw decoder
///
static void VaapiCleanup(VaapiDecoder * decoder)
{
    pthread_mutex_lock(&VideoMutex);

#ifdef DEBUG
    if (decoder->SurfaceRead != decoder->SurfaceWrite) {
	Error("Surface queue mismatch. SurfaceRead = %d, SurfaceWrite = %d, SurfacesFilled = %d", decoder->SurfaceRead,
	    decoder->SurfaceWrite, atomic_read(&decoder->SurfacesFilled));
    }
#endif

    // clear ring buffer
    // the surfaces here are owned/destroyed by ffmpeg
    for (int i = 0; i < VIDEO_SURFACES_MAX; ++i) {
	decoder->SurfacesRb[i] = VA_INVALID_ID;
    }

    //	cleanup surfaces
    VaapiDestroySurfaces(decoder);

    // clear forward/backward references for vpp
    if (decoder->ForwardRefSurfaces)
	free(decoder->ForwardRefSurfaces);
    decoder->ForwardRefSurfaces = NULL;
    decoder->ForwardRefCount = 0;

    decoder->PlaybackSurface = VA_INVALID_ID;

    if (decoder->BackwardRefSurfaces)
	free(decoder->BackwardRefSurfaces);
    decoder->BackwardRefSurfaces = NULL;
    decoder->BackwardRefCount = 0;

    // Free & clear vpp filter chain
    for (unsigned int i = 0; i < decoder->filter_n; ++i) {
	vaDestroyBuffer(decoder->VaDisplay, decoder->filters[i]);
	vaDestroyBuffer(decoder->VaDisplay, decoder->gpe_filters[i]);
	decoder->filters[i] = VA_INVALID_ID;
	decoder->gpe_filters[i] = VA_INVALID_ID;
    }
    decoder->filter_n = 0;
    decoder->gpe_filter_n = 0;

    decoder->WrongInterlacedWarned = 0;

    //	cleanup image
    if (decoder->Image->image_id != VA_INVALID_ID) {
	if (vaDestroyImage(decoder->VaDisplay, decoder->Image->image_id) != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't destroy image!");
	}
	decoder->Image->image_id = VA_INVALID_ID;
    }

    if (vaDestroyContext(decoder->VaDisplay, decoder->vpp_ctx) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't destroy postproc context!");
    }
    decoder->vpp_ctx = VA_INVALID_ID;

    if (vaDestroyConfig(decoder->VaDisplay, decoder->VppConfig) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't destroy config!");
    }
    decoder->VppConfig = VA_INVALID_ID;

    decoder->InputWidth = 0;
    decoder->InputHeight = 0;

    decoder->SurfaceRead = 0;
    decoder->SurfaceWrite = 0;
    decoder->SurfaceField = 0;

    decoder->PostProcSurfaceWrite = 0;

    decoder->SyncCounter = 0;
    decoder->FrameCounter = 0;
    decoder->FramesDisplayed = 0;
    decoder->StartCounter = 0;
    decoder->Closing = 0;
    decoder->PTS = AV_NOPTS_VALUE;
    VideoDeltaPTS = 0;
    pthread_mutex_unlock(&VideoMutex);
}

///
/// Destroy a VA-API decoder.
///
/// @param decoder  VA-API decoder
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
	//  update OSD associate
	//
	if (VaOsdSubpicture != VA_INVALID_ID) {
	    if (vaDeassociateSubpicture(decoder->VaDisplay, VaOsdSubpicture, &decoder->BlackSurface,
		    1) != VA_STATUS_SUCCESS) {
		Error("video/vaapi: can't deassociate black surfaces");
	    }
	}
	if (vaDestroySurfaces(decoder->VaDisplay, &decoder->BlackSurface, 1)
	    != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't destroy a surface");
	}
    }

    VaapiPrintFrames(decoder);

    av_buffer_unref(&HwDeviceContext);
    free(decoder);
}

///
/// VA-API setup.
///
/// @param display_name x11/xcb display name
///
/// @returns true if VA-API could be initialized, false otherwise.
///
static int VaapiInit(const char *display_name)
{
    VaOsdImage.image_id = VA_INVALID_ID;
    VaOsdSubpicture = VA_INVALID_ID;

    return 1;
}

///
/// VA-API cleanup
///
static void VaapiExit(void)
{
    int i;

// FIXME: more VA-API cleanups...

    for (i = 0; i < VaapiDecoderN; ++i) {
	if (VaapiDecoders[i]) {
	    VaapiDelHwDecoder(VaapiDecoders[i]);
	    VaapiDecoders[i] = NULL;
	}
    }
    VaapiDecoderN = 0;
}

//----------------------------------------------------------------------------

///
/// Update output for new size or aspect ratio.
///
/// @param decoder  VA-API decoder
///
static void VaapiUpdateOutput(VaapiDecoder * decoder)
{
    VideoUpdateOutput(decoder->InputAspect, decoder->InputWidth, decoder->InputHeight, decoder->Resolution,
	decoder->VideoX, decoder->VideoY, decoder->VideoWidth, decoder->VideoHeight, &decoder->OutputX,
	&decoder->OutputY, &decoder->OutputWidth, &decoder->OutputHeight, &decoder->CropX, &decoder->CropY,
	&decoder->CropWidth, &decoder->CropHeight);
    decoder->AutoCrop->State = 0;
    decoder->AutoCrop->Count = AutoCropDelay;
}

///
/// Find VA-API image format.
///
/// @param decoder  VA-API decoder
/// @param pix_fmt  ffmpeg pixel format
/// @param[out] format	image format
///
/// FIXME: can fallback from I420 to YV12, if not supported
/// FIXME: must check if put/get with this format is supported (see intel)
///
static int VaapiFindImageFormat(VaapiDecoder * decoder, enum AVPixelFormat pix_fmt, VAImageFormat * format)
{
    VAImageFormat *imgfrmts;
    int imgfrmt_n;
    int i;
    unsigned fourcc;

    switch (pix_fmt) {			// convert ffmpeg to VA-API
	    // NV12, YV12, I420, BGRA
	    // intel: I420 is native format for MPEG-2 decoded surfaces
	    // intel: NV12 is native format for H.264 decoded surfaces
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
	    fourcc = VA_FOURCC_I420;	// aka. VA_FOURCC_IYUV
	    break;
	case AV_PIX_FMT_VAAPI_VLD:
	case AV_PIX_FMT_NV12:
	    fourcc = VA_FOURCC_NV12;
	    break;
	case AV_PIX_FMT_BGRA:
	    fourcc = VA_FOURCC_BGRX;
	    break;
	case AV_PIX_FMT_RGBA:
	    fourcc = VA_FOURCC_RGBX;
	    break;
	case AV_PIX_FMT_YUV420P10LE:
	    //fourcc = VA_FOURCC_P010;
	    fourcc = VA_FOURCC_NV12;	// FIXME: Do P010 on supported hardware
	    break;
	default:
	    Fatal("video/vaapi: unsupported pixel format %d (%s)", pix_fmt, av_get_pix_fmt_name(pix_fmt));
    }

    imgfrmt_n = vaMaxNumImageFormats(decoder->VaDisplay);
    imgfrmts = alloca(imgfrmt_n * sizeof(*imgfrmts));

    if (vaQueryImageFormats(decoder->VaDisplay, imgfrmts, &imgfrmt_n)
	!= VA_STATUS_SUCCESS) {
	Error("video/vaapi: vaQueryImageFormats failed");
	return 0;
    }
    Debug7("video/vaapi: search format %c%c%c%c in %d image formats", fourcc, fourcc >> 8, fourcc >> 16, fourcc >> 24,
	imgfrmt_n);
    Debug7("video/vaapi: supported image formats:");
    for (i = 0; i < imgfrmt_n; ++i) {
	Debug7("video/vaapi:\t%c%c%c%c\t%d", imgfrmts[i].fourcc, imgfrmts[i].fourcc >> 8, imgfrmts[i].fourcc >> 16,
	    imgfrmts[i].fourcc >> 24, imgfrmts[i].depth);
    }
    //
    //	search image format
    //
    for (i = 0; i < imgfrmt_n; ++i) {
	if (imgfrmts[i].fourcc == fourcc) {
	    *format = imgfrmts[i];
	    Debug7("video/vaapi: use\t%c%c%c%c\t%d", imgfrmts[i].fourcc, imgfrmts[i].fourcc >> 8,
		imgfrmts[i].fourcc >> 16, imgfrmts[i].fourcc >> 24, imgfrmts[i].depth);
	    return 1;
	}
    }

    Fatal("video/vaapi: pixel format %d unsupported by VA-API", pix_fmt);
    // FIXME: no fatal error!

    return 0;
}

///
/// Verify & Run arbitrary VPP processing on src/dst surface(s)
///
/// @param ctx[in]  VA-API postprocessing context
/// @param src[in]  source surface to scale
/// @param dst[in]  destination surface to put result in
/// @param filters[in]	    array of VABufferID filters to run
/// @param num_filters[in]  number of VABufferID filters supplied
/// @param filter_flags[in] filter flags to provide to postprocessing
/// @param pipeline_flags[in]	pipeline flags to provide to postprocessing
/// @param frefs[in]	    array of forward reference surface ids
/// @param num_frefs[in,out]	number of forward reference surface ids supplied/needed
/// @param brefs[in]	    array of backward reference surface ids
/// @param num_brefs[in,out]	number of backward reference surface ids supplied/needed
///
static VAStatus VaapiPostprocessSurface(VAContextID ctx, VASurfaceID src, VASurfaceID dst, VABufferID * filters,
    unsigned int num_filters, int filter_flags, int pipeline_flags, VASurfaceID * frefs, unsigned int *num_frefs,
    VASurfaceID * brefs, unsigned int *num_brefs)
{
    unsigned int i;
    unsigned int tmp_num_frefs = 0;
    unsigned int tmp_num_brefs = 0;
    VAStatus va_status;
    VASurfaceStatus va_surf_status;
    VABufferID pipeline_buf;
    VAProcPipelineCaps pipeline_caps;
    VAProcPipelineParameterBuffer pipeline_param;

    if (!num_frefs)
	num_frefs = &tmp_num_frefs;
    if (!num_brefs)
	num_brefs = &tmp_num_brefs;

    /* Make sure rendering is finished in earliest forward reference surface */
    if (*num_frefs)
	vaSyncSurface(VaDisplay, frefs[*num_frefs - 1]);

    /* Skip postprocessing if queue is not deinterlaceable */
    for (i = 0; i < *num_brefs; ++i) {
	// Trust the driver to "do the right thing" if invalid surfaces are provided as references
	if (brefs[i] == VA_INVALID_ID)
	    continue;
	va_status = vaQuerySurfaceStatus(VaDisplay, brefs[i], &va_surf_status);
	if (va_status != VA_STATUS_SUCCESS) {
	    Error("vaapi/vpp: Surface %d query status failed (0x%X): %s", i, va_status, vaErrorStr(va_status));
	    return va_status;
	}
	if (va_surf_status != VASurfaceReady) {
	    Info("Backward reference surface %d is not ready, surf_status = %d", i, va_surf_status);
	    return VA_STATUS_ERROR_SURFACE_BUSY;
	}
    }

    for (i = 0; i < *num_frefs; ++i) {
	// Trust the driver to "do the right thing" if invalid surfaces are provided as references
	if (frefs[i] == VA_INVALID_ID)
	    continue;
	va_status = vaQuerySurfaceStatus(VaDisplay, frefs[i], &va_surf_status);
	if (va_status != VA_STATUS_SUCCESS) {
	    Error("Surface %d query status = 0x%X: %s", i, va_status, vaErrorStr(va_status));
	    return va_status;
	}
	if (va_surf_status != VASurfaceReady) {
	    Info("Forward reference surface %d is not ready, surf_status = %d", i, va_surf_status);
	    return VA_STATUS_ERROR_SURFACE_BUSY;
	}
    }

    va_status = vaQueryVideoProcPipelineCaps(VaDisplay, ctx, filters, num_filters, &pipeline_caps);
    if (va_status != VA_STATUS_SUCCESS) {
	Error("vaapi/vpp: query pipeline caps failed (0x%x): %s", va_status, vaErrorStr(va_status));
	return va_status;
    }

    if (pipeline_caps.num_forward_references != *num_frefs) {
	Debug7("vaapi/vpp: Wrong number of forward references. Needed %d, got %d",
	    pipeline_caps.num_forward_references, *num_frefs);
	/* Fail operation when needing more references than currently have */
	if (pipeline_caps.num_forward_references > *num_frefs) {
	    *num_frefs = pipeline_caps.num_forward_references;
	    *num_brefs = pipeline_caps.num_backward_references;
	    return VA_STATUS_ERROR_INVALID_PARAMETER;
	}
    }

    if (pipeline_caps.num_backward_references != *num_brefs) {
	Debug7("vaapi/vpp: Wrong number of backward references. Needed %d, got %d",
	    pipeline_caps.num_forward_references, *num_brefs);
	/* Fail operation when needing more references than currently have */
	if (pipeline_caps.num_backward_references > *num_brefs) {
	    *num_frefs = pipeline_caps.num_forward_references;
	    *num_brefs = pipeline_caps.num_backward_references;
	    return VA_STATUS_ERROR_INVALID_PARAMETER;
	}
    }

    *num_frefs = pipeline_caps.num_forward_references;
    *num_brefs = pipeline_caps.num_backward_references;

    if (src == VA_INVALID_ID || dst == VA_INVALID_ID || src == dst)
	return VA_STATUS_ERROR_INVALID_PARAMETER;

    memset(&pipeline_param, '\0', sizeof(VAProcPipelineParameterBuffer));

    pipeline_param.surface = src;
    pipeline_param.surface_region = NULL;
    pipeline_param.surface_color_standard = VAProcColorStandardNone;
    pipeline_param.output_region = NULL;
    pipeline_param.output_background_color = 0xff000000;
    pipeline_param.output_color_standard = VAProcColorStandardNone;
    pipeline_param.pipeline_flags = pipeline_flags;
    pipeline_param.filter_flags = filter_flags;
    pipeline_param.filters = filters;
    pipeline_param.num_filters = num_filters;

    pipeline_param.forward_references = frefs;
    pipeline_param.num_forward_references = *num_frefs;
    pipeline_param.backward_references = brefs;
    pipeline_param.num_backward_references = *num_brefs;

    va_status =
	vaCreateBuffer(VaDisplay, ctx, VAProcPipelineParameterBufferType, sizeof(VAProcPipelineParameterBuffer), 1,
	&pipeline_param, &pipeline_buf);
    if (va_status != VA_STATUS_SUCCESS) {
	Error("vaapi/vpp: createbuffer failed (0x%x): %s", va_status, vaErrorStr(va_status));
	return va_status;
    }

    va_status = vaBeginPicture(VaDisplay, ctx, dst);
    if (va_status != VA_STATUS_SUCCESS) {
	Error("vaapi/vpp: begin picture failed (0x%x): %s", va_status, vaErrorStr(va_status));
	return va_status;
    }

    va_status = vaRenderPicture(VaDisplay, ctx, &pipeline_buf, 1);
    if (va_status != VA_STATUS_SUCCESS) {
	Error("vaapi/vpp: Postprocessing failed (0x%X): %s", va_status, vaErrorStr(va_status));
	return va_status;
    }
    vaEndPicture(VaDisplay, ctx);
    vaDestroyBuffer(VaDisplay, pipeline_buf);
    return VA_STATUS_SUCCESS;
}

///
/// Convert & Scale between source / destination surfaces
///
/// @param ctx[in]  VA-API postprocessing context
/// @param src[in]  source surface to scale
/// @param dst[in]  destination surface to put result in
static inline VAStatus VaapiRunScaling(VAContextID ctx, VASurfaceID src, VASurfaceID dst)
{
    return VaapiPostprocessSurface(ctx, src, dst, NULL, 0, VA_FILTER_SCALING_HQ, VA_PROC_PIPELINE_SUBPICTURES, NULL, 0,
	NULL, 0);
}

///
/// Construct and apply filters to a surface (should be called after queuing new surface)
///
/// @param decoder  VA-API decoder
/// @param top_field top field is first
/// @return Pointer to postprocessed surface or NULL if postprocessing failed
///
/// @note we can't mix software and hardware decoder surfaces
///
static VASurfaceID *VaapiApplyFilters(VaapiDecoder * decoder, int top_field)
{
    unsigned int filter_count = 0;
    unsigned int filter_flags = decoder->SurfaceFlagsTable[decoder->Resolution];
    unsigned int tmp_forwardRefCount = decoder->ForwardRefCount;
    unsigned int tmp_backwardRefCount = decoder->BackwardRefCount;
    VAStatus va_status;
    VABufferID filters_to_run[VAProcFilterCount];
    VAProcFilterParameterBufferDeinterlacing *deinterlace = NULL;
    VASurfaceID *surface = NULL;
    VASurfaceID *gpe_surface = NULL;

    /* No postprocessing filters enabled */
    if (!decoder->filter_n)
	return NULL;

    /* Get next postproc surface to write from ring buffer */
    decoder->PostProcSurfaceWrite = (decoder->PostProcSurfaceWrite + 1) % POSTPROC_SURFACES_MAX;
    surface = &decoder->PostProcSurfacesRb[decoder->PostProcSurfaceWrite];

    if (decoder->Deinterlaced || !decoder->Interlaced)
	filter_flags |= VA_FRAME_PICTURE;
    else if (decoder->Interlaced)
	filter_flags |= top_field ? VA_TOP_FIELD : VA_BOTTOM_FIELD;

    memcpy(filters_to_run, decoder->filters, VAProcFilterCount * sizeof(VABufferID));
    filter_count = decoder->filter_n;

    /* Map deinterlace buffer and handle field ordering */
    if (decoder->vpp_deinterlace_buf) {
	va_status = vaMapBuffer(decoder->VaDisplay, *decoder->vpp_deinterlace_buf, (void **)&deinterlace);
	if (va_status != VA_STATUS_SUCCESS) {
	    Error("deint map buffer va_status = 0x%X", va_status);
	    return NULL;
	}
	/* Change deint algorithm as set in plugin menu */
	deinterlace->algorithm = decoder->SurfaceDeintTable[decoder->Resolution];

	if (top_field)
	    deinterlace->flags = 0;
	else
	    deinterlace->flags = VA_DEINTERLACING_BOTTOM_FIELD;

	if (!decoder->TopFieldFirst)
	    deinterlace->flags |= VA_DEINTERLACING_BOTTOM_FIELD_FIRST;
	/* If non-interlaced then override flags with one field setup */
	if (!decoder->Interlaced)
	    deinterlace->flags = VA_DEINTERLACING_ONE_FIELD;

	/* This block of code skips various filters in-flight if source/settings
	   disallow running the filter in question */
	filter_count = 0;
	for (unsigned int i = 0; i < decoder->filter_n; ++i) {

	    /* Skip deinterlacer if disabled or source is not interlaced */
	    if (decoder->filters[i] == *decoder->vpp_deinterlace_buf) {
		if (!decoder->Interlaced)
		    continue;
		if (deinterlace->algorithm == VAProcDeinterlacingNone)
		    continue;
	    }

	    /* Skip denoise if value is set to 0 ("off") */
	    if (decoder->vpp_denoise_buf && decoder->filters[i] == *decoder->vpp_denoise_buf) {
		if (!VideoDenoise[decoder->Resolution])
		    continue;
	    }

	    /* Skip skin tone enhancement if value is set to 0 ("off") */
	    if (decoder->vpp_stde_buf && decoder->filters[i] == *decoder->vpp_stde_buf) {
		if (!VideoSkinToneEnhancement)
		    continue;
	    }

	    filters_to_run[filter_count++] = decoder->filters[i];
	}

	vaUnmapBuffer(decoder->VaDisplay, *decoder->vpp_deinterlace_buf);
    }

    if (!filter_count)
	return NULL;			/* no postprocessing if no filters applied */

    va_status =
	VaapiPostprocessSurface(decoder->vpp_ctx, decoder->PlaybackSurface, *surface, filters_to_run, filter_count,
	filter_flags, 0, decoder->ForwardRefSurfaces, &tmp_forwardRefCount, decoder->BackwardRefSurfaces,
	&tmp_backwardRefCount);

    if (tmp_forwardRefCount != decoder->ForwardRefCount) {
	Info("Changing to %d forward reference surfaces for postprocessing", tmp_forwardRefCount);
	decoder->ForwardRefSurfaces = realloc(decoder->ForwardRefSurfaces, tmp_forwardRefCount * sizeof(VASurfaceID));
	decoder->ForwardRefCount = tmp_forwardRefCount;
    }

    if (tmp_backwardRefCount != decoder->BackwardRefCount) {
	Info("Changing to %d backward reference surfaces for postprocessing", tmp_backwardRefCount);
	decoder->BackwardRefSurfaces =
	    realloc(decoder->BackwardRefSurfaces, tmp_backwardRefCount * sizeof(VASurfaceID));
	decoder->BackwardRefCount = tmp_backwardRefCount;
    }

    if (va_status != VA_STATUS_SUCCESS)
	return NULL;

    /* Skip sharpening if off */
    if (!decoder->vpp_sharpen_buf || !VideoSharpen[decoder->Resolution])
	return surface;

    vaSyncSurface(decoder->VaDisplay, *surface);

    /* Get postproc surface for gpe pipeline */
    decoder->PostProcSurfaceWrite = (decoder->PostProcSurfaceWrite + 1) % POSTPROC_SURFACES_MAX;
    gpe_surface = &decoder->PostProcSurfacesRb[decoder->PostProcSurfaceWrite];

    va_status =
	VaapiPostprocessSurface(decoder->vpp_ctx, *surface, *gpe_surface, decoder->gpe_filters, decoder->gpe_filter_n,
	VA_FRAME_PICTURE, 0, NULL, NULL, NULL, NULL);

    /* Failed to sharpen? Return previous surface */
    if (va_status != VA_STATUS_SUCCESS)
	return surface;

    return gpe_surface;
}

///
/// Clamp given value to range that fits in uint8_t
///
/// @param value[in]	input value to clamp
///
static inline uint8_t VaapiClampToUint8(const int value)
{
    if (value > 0xFF)
	return 0xFF;
    else if (value < 0)
	return 0;
    return value;
}

///
/// Grab output surface in YUV format and convert to bgra.
///
/// @param decoder[in]	    VA-API decoder
/// @param src[in]  Source VASurfaceID to grab
/// @param ret_size[out]    size of allocated surface copy
/// @param ret_width[in,out]	width of output
/// @param ret_height[in,out]	height of output
///
static uint8_t *VaapiGrabOutputSurfaceYUV(VaapiDecoder * decoder, VASurfaceID src, int *ret_size, int *ret_width,
    int *ret_height)
{
    int i, j;
    VAStatus status;
    VAImage image;
    VAImageFormat format[1];
    uint8_t *image_buffer = NULL;
    uint8_t *bgra = NULL;

    status = vaDeriveImage(decoder->VaDisplay, src, &image);
    if (status != VA_STATUS_SUCCESS) {
	Error("video/vaapi: Failed to derive image: %s\n Falling back to GetImage", vaErrorStr(status));

	if (!decoder->GetPutImage) {
	    Error("video/vaapi: Image grabbing not supported by HW");
	    return NULL;
	}

	if (!VaapiFindImageFormat(decoder, AV_PIX_FMT_NV12, format)) {
	    Error("video/vaapi: Image format suitable for grab not supported");
	    return NULL;
	}

	status = vaCreateImage(decoder->VaDisplay, format, *ret_width, *ret_height, &image);
	if (status != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: Failed to create image for grab: %s", vaErrorStr(status));
	    return NULL;
	}

	status = vaGetImage(decoder->VaDisplay, src, 0, 0, *ret_width, *ret_height, image.image_id);
	if (status != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: Failed to capture image: %s", vaErrorStr(status));
	    goto out_destroy;
	}
    }
    VaapiFindImageFormat(decoder, AV_PIX_FMT_NV12, format);

    // Sanity check for image format
    if (image.format.fourcc != VA_FOURCC_NV12 && image.format.fourcc != VA_FOURCC_I420) {
	Error("video/vaapi: Image format mismatch! (fourcc: 0x%x, planes: %d)", image.format.fourcc, image.num_planes);
	goto out_destroy;
    }

    status = vaMapBuffer(decoder->VaDisplay, image.buf, (void **)&image_buffer);
    if (status != VA_STATUS_SUCCESS) {
	Error("video/vaapi: Could not map grabbed image for access: %s", vaErrorStr(status));
	goto out_destroy;
    }

    bgra = malloc(*ret_size);
    if (!bgra) {
	Error("video/vaapi: Grab failed: Out of memory");
	goto out_unmap;
    }

    for (j = 0; j < *ret_height; ++j) {
	for (i = 0; i < *ret_width; ++i) {
	    uint8_t y = image_buffer[j * image.pitches[0] + i];
	    uint8_t u, v;
	    int b, g, r;

	    if (image.format.fourcc == VA_FOURCC_NV12) {
		unsigned int uv_index = image.offsets[1] + (image.pitches[1] * (j / 2)) + (i / 2) * 2;

		u = image_buffer[uv_index];
		v = image_buffer[uv_index + 1];
	    } else if (image.format.fourcc == VA_FOURCC_I420) {
		unsigned int u_index = image.offsets[1] + (image.pitches[1] * (j / 2) + (i / 2));
		unsigned int v_index = image.offsets[2] + (image.pitches[2] * (j / 2) + (i / 2));

		u = image_buffer[u_index];
		v = image_buffer[v_index];
	    } else {
		/* Use only y-plane if plane format is unknown */
		u = v = y;
	    }

	    b = 1.164 * (y - 16) + 2.018 * (u - 128);
	    g = 1.164 * (y - 16) - 0.813 * (v - 128) - 0.391 * (u - 128);
	    r = 1.164 * (y - 16) + 1.596 * (v - 128);

	    bgra[(i + j * *ret_width) * 4 + 0] = VaapiClampToUint8(b);
	    bgra[(i + j * *ret_width) * 4 + 1] = VaapiClampToUint8(g);
	    bgra[(i + j * *ret_width) * 4 + 2] = VaapiClampToUint8(r);
	    bgra[(i + j * *ret_width) * 4 + 3] = 0x00;
	}
    }

  out_unmap:
    vaUnmapBuffer(VaDisplay, image.buf);
  out_destroy:
    vaDestroyImage(VaDisplay, image.image_id);
    return bgra;
}

///
/// Grab output surface by utilizing VA-API surface color conversion HW.
///
/// @param decoder[in]	    VA-API decoder
/// @param src[in]  Source VASurfaceID to grab
/// @param ret_size[out]    size of allocated surface copy
/// @param ret_width[in,out]	width of output
/// @param ret_height[in,out]	height of output
///
static uint8_t *VaapiGrabOutputSurfaceHW(VaapiDecoder * decoder, VASurfaceID src, int *ret_size, int *ret_width,
    int *ret_height)
{
    int j;
    VAStatus status;
    VAImage image;
    VAImageFormat format[1];
    uint8_t *image_buffer = NULL;
    uint8_t *bgra = NULL;

    if (!decoder->GetPutImage) {
	Error("video/vaapi: Image grabbing not supported by HW");
	return NULL;
    }

    if (!VaapiFindImageFormat(decoder, AV_PIX_FMT_BGRA, format)) {
	Error("video/vaapi: Image format suitable for grab not supported");
	return NULL;
    }

    status = vaCreateImage(decoder->VaDisplay, format, *ret_width, *ret_height, &image);
    if (status != VA_STATUS_SUCCESS) {
	Error("video/vaapi: Failed to create image for grab: %s", vaErrorStr(status));
	return NULL;
    }

    status = vaGetImage(decoder->VaDisplay, src, 0, 0, *ret_width, *ret_height, image.image_id);
    if (status != VA_STATUS_SUCCESS) {
	Error("video/vaapi: Failed to capture image: %s", vaErrorStr(status));
	return NULL;
    }

    status = vaMapBuffer(decoder->VaDisplay, image.buf, (void **)&image_buffer);
    if (status != VA_STATUS_SUCCESS) {
	Error("video/vaapi: Could not map grabbed image for access: %s", vaErrorStr(status));
	goto out_destroy;
    }

    bgra = malloc(*ret_size);
    if (!bgra) {
	Error("video/vaapi: Grab failed: Out of memory");
	goto out_unmap;
    }

    for (j = 0; j < *ret_height; ++j) {
	memcpy(bgra + j * *ret_width * 4, image_buffer + j * image.pitches[0], *ret_width * 4);
    }

  out_unmap:
    vaUnmapBuffer(decoder->VaDisplay, image.buf);
  out_destroy:
    vaDestroyImage(decoder->VaDisplay, image.image_id);
    return bgra;
}

///
/// Grab output surface.
///
/// @param ret_size[out]    size of allocated surface copy
/// @param ret_width[in,out]	width of output
/// @param ret_height[in,out]	height of output
///
static uint8_t *VaapiGrabOutputSurface(int *ret_size, int *ret_width, int *ret_height)
{
    uint8_t *bgra = NULL;
    VAStatus status;
    VaapiDecoder *decoder = NULL;
    VASurfaceID scaled[1] = { VA_INVALID_ID };
    VASurfaceID grabbing = VA_INVALID_ID;
    VAContextID scaling_ctx;

    if (!(decoder = VaapiDecoders[0])) {
	Error("video/vaapi: Decoder not available for GRAB");
	return NULL;
    }

    grabbing = decoder->SurfacesRb[decoder->SurfaceRead];

    if (*ret_width <= 0)
	*ret_width = decoder->InputWidth;
    if (*ret_height <= 0)
	*ret_height = decoder->InputHeight;

    *ret_size = *ret_width * *ret_height * 4;

    status =
	vaCreateSurfaces(decoder->VaDisplay, VA_RT_FORMAT_YUV420, *ret_width, *ret_height, scaled, ARRAY_ELEMS(scaled),
	NULL, 0);
    if (status != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't create scaling surface for grab: %s", vaErrorStr(status));
    }

    status =
	vaCreateContext(decoder->VaDisplay, decoder->VppConfig, *ret_width, *ret_height, VA_PROGRESSIVE, scaled,
	ARRAY_ELEMS(scaled), &scaling_ctx);
    if (status != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't create scaling context for grab: %s", vaErrorStr(status));
	vaDestroySurfaces(decoder->VaDisplay, scaled, ARRAY_ELEMS(scaled));
	scaled[0] = VA_INVALID_ID;
    }

    status = VaapiRunScaling(scaling_ctx, grabbing, scaled[0]);
    if (status != VA_STATUS_SUCCESS) {
	vaDestroyContext(decoder->VaDisplay, scaling_ctx);
	vaDestroySurfaces(decoder->VaDisplay, scaled, ARRAY_ELEMS(scaled));
	scaled[0] = VA_INVALID_ID;
    } else {
	grabbing = scaled[0];
    }

    bgra = VaapiGrabOutputSurfaceHW(decoder, grabbing, ret_size, ret_width, ret_height);
    if (!bgra)
	bgra = VaapiGrabOutputSurfaceYUV(decoder, grabbing, ret_size, ret_width, ret_height);

    if (scaled[0] != VA_INVALID_ID) {
	vaDestroyContext(decoder->VaDisplay, scaling_ctx);
	vaDestroySurfaces(decoder->VaDisplay, scaled, ARRAY_ELEMS(scaled));
    }

    return bgra;
}

///
/// VA-API initialize OSD.
///
/// @param width    osd width
/// @param height   osd height
///
/// @note subpicture is unusable, it can be scaled with the video image.
///
static void VaapiOsdInit(int width, int height)
{
    VAImageFormat *formats;
    unsigned *flags;
    unsigned format_n;
    unsigned u;
    unsigned v;
    static uint32_t wanted_formats[] = { VA_FOURCC_BGRA, VA_FOURCC_RGBA };

    if (VaOsdImage.image_id != VA_INVALID_ID) {
	Debug7("video/vaapi: osd already setup");
	return;
    }
    if (!VaDisplay) {
	Debug7("video/vaapi: va-api not setup");
	return;
    }
    //
    //	look through subpicture formats
    //
    format_n = vaMaxNumSubpictureFormats(VaDisplay);
    formats = alloca(format_n * sizeof(*formats));
    flags = alloca(format_n * sizeof(*formats));
    if (vaQuerySubpictureFormats(VaDisplay, formats, flags, &format_n) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't get subpicture formats");
	return;
    }
#ifdef DEBUG
    Debug7("video/vaapi: supported subpicture formats:");
    for (u = 0; u < format_n; ++u) {
	Debug7("video/vaapi:\t%c%c%c%c flags %#x %s", formats[u].fourcc, formats[u].fourcc >> 8,
	    formats[u].fourcc >> 16, formats[u].fourcc >> 24, flags[u],
	    (flags[u] & VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD) ? "screen coord" : "");
    }
#endif
    for (v = 0; v < sizeof(wanted_formats) / sizeof(*wanted_formats); ++v) {
	for (u = 0; u < format_n; ++u) {
	    if (formats[u].fourcc == wanted_formats[v]) {
		goto found;
	    }
	}
    }
    Error("video/vaapi: can't find a supported subpicture format");
    return;

  found:
    Debug7("video/vaapi: use %c%c%c%c subpicture format with flags %#x", formats[u].fourcc, formats[u].fourcc >> 8,
	formats[u].fourcc >> 16, formats[u].fourcc >> 24, flags[u]);

    VaapiUnscaledOsd = 0;
    if (flags[u] & VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD) {
	Info("video/vaapi: supports unscaled osd");
	VaapiUnscaledOsd = 1;
    }

    if (vaCreateImage(VaDisplay, &formats[u], width, height, &VaOsdImage) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't create osd image");
	return;
    }
    if (vaCreateSubpicture(VaDisplay, VaOsdImage.image_id, &VaOsdSubpicture) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't create subpicture");

	if (vaDestroyImage(VaDisplay, VaOsdImage.image_id) != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't destroy image!");
	}
	VaOsdImage.image_id = VA_INVALID_ID;

	return;
    }
}

///
/// VA-API cleanup osd.
///
static void VaapiOsdExit(void)
{
    if (VaOsdImage.image_id != VA_INVALID_ID) {
	if (vaDestroyImage(VaDisplay, VaOsdImage.image_id) != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't destroy image!");
	}
	VaOsdImage.image_id = VA_INVALID_ID;
    }

    if (VaOsdSubpicture != VA_INVALID_ID) {
	for (int i = 0; i < VaapiDecoderN; ++i) {
	    VaapiDeassociate(VaapiDecoders[i]);
	}

	if (vaDestroySubpicture(VaDisplay, VaOsdSubpicture) != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't destroy subpicture");
	}
	VaOsdSubpicture = VA_INVALID_ID;
    }
}

///
/// Generic helper to set-up ParameterBuffer filters
/// (like NoiseReduction, SkinToneEnhancement, Sharpening...).
///
/// @param decoder  VA-API decoder
/// @param type Type of filter to set-up
/// @param value    Value of the filter to set-up to
/// @return Buffer ID for the filter or VA_INVALID_ID if unsuccessful
///
static VABufferID VaapiSetupParameterBufferProcessing(VaapiDecoder * decoder, VAProcFilterType type, float value)
{
    VAProcFilterParameterBuffer param_buf;
    VABufferID filter_buf_id;
    unsigned int cap_n = 1;
    VAProcFilterCap caps[cap_n];

    VAStatus va_status = vaQueryVideoProcFilterCaps(decoder->VaDisplay, decoder->vpp_ctx,
	type, caps, &cap_n);

    if (va_status != VA_STATUS_SUCCESS) {
	Error("Failed to query filter #%02x capabilities: %s", type, vaErrorStr(va_status));
	return VA_INVALID_ID;
    }
    if (type == VAProcFilterSkinToneEnhancement && cap_n == 0) {    // Intel driver doesn't return caps
	cap_n = 1;
	caps->range.min_value = 0.0;
	caps->range.max_value = 4.0;
	caps->range.default_value = 0.0;
	caps->range.step = 1.0;
	VaapiConfigStde.drv_scale = 3.0;
    }
    if (cap_n != 1) {
	Error("Wrong number of capabilities (%d) for filter %#010x", cap_n, type);
	return VA_INVALID_ID;
    }

    Info("video/vaapi: %.2f - %.2f ++ %.2f = %.2f", caps->range.min_value, caps->range.max_value, caps->range.step,
	caps->range.default_value);

    switch (type) {
	case VAProcFilterNoiseReduction:
	    VaapiNormalizeConfig(&VaapiConfigDenoise, caps->range.min_value, caps->range.max_value,
		caps->range.default_value, caps->range.step);
	    break;
	case VAProcFilterSharpening:
	    VaapiNormalizeConfig(&VaapiConfigSharpen, caps->range.min_value, caps->range.max_value,
		caps->range.default_value, caps->range.step);
	    break;
	case VAProcFilterSkinToneEnhancement:
	    VaapiNormalizeConfig(&VaapiConfigStde, caps->range.min_value, caps->range.max_value,
		caps->range.default_value, caps->range.step);
	    break;
	default:
	    break;
    }

    param_buf.type = type;
    param_buf.value = value;
    va_status =
	vaCreateBuffer(decoder->VaDisplay, decoder->vpp_ctx, VAProcFilterParameterBufferType, sizeof(param_buf), 1,
	&param_buf, &filter_buf_id);

    if (va_status != VA_STATUS_SUCCESS) {
	Error("Could not create buffer for filter #%02x: %s", type, vaErrorStr(va_status));
	return VA_INVALID_ID;
    }
    return filter_buf_id;
}

///
/// Configure VA-API for new video format.
///
/// @param decoder  VA-API decoder
///
static void VaapiSetupVideoProcessing(VaapiDecoder * decoder)
{
    VAStatus va_status;
    VAProcFilterType filtertypes[VAProcFilterCount];
    unsigned filtertype_n;
    unsigned u;
    unsigned v;
    VAProcFilterCapDeinterlacing deinterlacing_caps[VAProcDeinterlacingCount];
    VAProcFilterParameterBufferDeinterlacing deinterlace;
    unsigned deinterlacing_cap_n;
    VAProcFilterParameterBufferColorBalance cbal_param[VAProcColorBalanceCount];
    VAProcFilterCapColorBalance colorbalance_caps[VAProcColorBalanceCount];
    unsigned colorbalance_cap_n;

    VABufferID filter_buf_id;

    VAProcPipelineCaps pipeline_caps;
    VAProcColorStandardType in_color_standards[VAProcColorStandardCount];
    VAProcColorStandardType out_color_standards[VAProcColorStandardCount];

    if (!VaapiVideoProcessing) {
	return;
    }
    //
    //	display and filter infos.
    //
    filtertype_n = VAProcFilterCount;	// API break this must be done
    vaQueryVideoProcFilters(decoder->VaDisplay, decoder->vpp_ctx, filtertypes, &filtertype_n);

    for (u = 0; u < filtertype_n; ++u) {
	switch (filtertypes[u]) {
	    case VAProcFilterNoiseReduction:
		Info("video/vaapi: noise reduction supported");
		VaapiConfigDenoise.active = 1;
		filter_buf_id =
		    VaapiSetupParameterBufferProcessing(decoder, filtertypes[u],
		    VaapiConfigDenoise.def_value * VaapiConfigDenoise.scale);
		if (filter_buf_id != VA_INVALID_ID) {
		    Info("Enabling denoise filter (pos = %d)", decoder->filter_n);
		    decoder->vpp_denoise_buf = &decoder->filters[decoder->filter_n];
		    decoder->filters[decoder->filter_n++] = filter_buf_id;
		}
		break;
	    case VAProcFilterDeinterlacing:
		Info("video/vaapi: deinterlacing supported");

		deinterlacing_cap_n = VAProcDeinterlacingCount;
		vaQueryVideoProcFilterCaps(decoder->VaDisplay, decoder->vpp_ctx, VAProcFilterDeinterlacing,
		    deinterlacing_caps, &deinterlacing_cap_n);

		memset(&decoder->SupportedDeinterlacers, 0, sizeof(decoder->SupportedDeinterlacers));
		decoder->SupportedDeinterlacers[VAProcDeinterlacingNone] = 1;	// always enable none

		for (v = 0; v < deinterlacing_cap_n; ++v) {

		    /* Deinterlacing parameters */
		    deinterlace.type = VAProcFilterDeinterlacing;
		    deinterlace.flags = 0;

		    switch (deinterlacing_caps[v].type) {
			case VAProcDeinterlacingNone:
			    Info("video/vaapi: none deinterlace supported");
			    decoder->SupportedDeinterlacers[VAProcDeinterlacingNone] = 1;
			    deinterlace.algorithm = VAProcDeinterlacingNone;
			    break;
			case VAProcDeinterlacingBob:
			    Info("video/vaapi: bob deinterlace supported");
			    decoder->SupportedDeinterlacers[VAProcDeinterlacingBob] = 1;
			    deinterlace.algorithm = VAProcDeinterlacingBob;
			    break;
			case VAProcDeinterlacingWeave:
			    Info("video/vaapi: weave deinterlace supported");
			    decoder->SupportedDeinterlacers[VAProcDeinterlacingWeave] = 1;
			    deinterlace.algorithm = VAProcDeinterlacingWeave;
			    break;
			case VAProcDeinterlacingMotionAdaptive:
			    Info("video/vaapi: motion adaptive deinterlace supported");
			    decoder->SupportedDeinterlacers[VAProcDeinterlacingMotionAdaptive] = 1;
			    deinterlace.algorithm = VAProcDeinterlacingMotionAdaptive;
			    break;
			case VAProcDeinterlacingMotionCompensated:
			    Info("video/vaapi: motion compensated deinterlace supported");
			    decoder->SupportedDeinterlacers[VAProcDeinterlacingMotionCompensated] = 1;
			    deinterlace.algorithm = VAProcDeinterlacingMotionCompensated;
			    break;
			default:
			    Info("video/vaapi: unsupported deinterlace #%02x", deinterlacing_caps[v].type);
			    break;
		    }
		}
		/* Enabling the deint algorithm that was seen last */
		Info("Enabling Deint (pos = %d)", decoder->filter_n);
		va_status =
		    vaCreateBuffer(decoder->VaDisplay, decoder->vpp_ctx, VAProcFilterParameterBufferType,
		    sizeof(deinterlace), 1, &deinterlace, &filter_buf_id);
		decoder->vpp_deinterlace_buf = &decoder->filters[decoder->filter_n];
		decoder->filters[decoder->filter_n++] = filter_buf_id;
		break;
	    case VAProcFilterSharpening:
		Info("video/vaapi: sharpening supported");
		VaapiConfigSharpen.active = 1;
		// Sharpening needs to on a separated pipeline apart from vebox
		filter_buf_id =
		    VaapiSetupParameterBufferProcessing(decoder, filtertypes[u],
		    VaapiConfigSharpen.def_value * VaapiConfigSharpen.scale);
		if (filter_buf_id != VA_INVALID_ID) {
		    Info("Enabling sharpening filter (pos = %d)", decoder->gpe_filter_n);
		    decoder->vpp_sharpen_buf = &decoder->gpe_filters[decoder->gpe_filter_n];
		    decoder->gpe_filters[decoder->gpe_filter_n++] = filter_buf_id;
		}
		break;
	    case VAProcFilterColorBalance:
		Info("video/vaapi: enabling color balance filters");
		colorbalance_cap_n = VAProcColorBalanceCount;
		vaQueryVideoProcFilterCaps(decoder->VaDisplay, decoder->vpp_ctx, VAProcFilterColorBalance,
		    colorbalance_caps, &colorbalance_cap_n);

		Info("video/vaapi: Supported color balance filter count: %d", colorbalance_cap_n);

		if (!colorbalance_cap_n)
		    break;

		/* Set each color balance filter individually */
		for (v = 0; v < colorbalance_cap_n; ++v) {

		    switch (colorbalance_caps[v].type) {
			case VAProcColorBalanceNone:
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)", "None",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value,
				decoder->filter_n);
			    break;
			case VAProcColorBalanceHue:
			    VaapiConfigHue.active = 1;
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)", "Hue",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value,
				decoder->filter_n);
			    VaapiNormalizeConfig(&VaapiConfigHue, colorbalance_caps[v].range.min_value,
				colorbalance_caps[v].range.max_value, colorbalance_caps[v].range.default_value,
				colorbalance_caps[v].range.step);
			    decoder->vpp_hue_idx = v;
			    break;
			case VAProcColorBalanceSaturation:
			    VaapiConfigSaturation.active = 1;
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)", "Saturation",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value,
				decoder->filter_n);
			    VaapiNormalizeConfig(&VaapiConfigSaturation, colorbalance_caps[v].range.min_value,
				colorbalance_caps[v].range.max_value, colorbalance_caps[v].range.default_value,
				colorbalance_caps[v].range.step);
			    decoder->vpp_saturation_idx = v;
			    break;
			case VAProcColorBalanceBrightness:
			    VaapiConfigBrightness.active = 1;
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)", "Brightness",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value,
				decoder->filter_n);
			    VaapiNormalizeConfig(&VaapiConfigBrightness, colorbalance_caps[v].range.min_value,
				colorbalance_caps[v].range.max_value, colorbalance_caps[v].range.default_value,
				colorbalance_caps[v].range.step);
			    decoder->vpp_brightness_idx = v;
			    break;
			case VAProcColorBalanceContrast:
			    VaapiConfigContrast.active = 1;
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)", "Contrast",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value,
				decoder->filter_n);
			    VaapiNormalizeConfig(&VaapiConfigContrast, colorbalance_caps[v].range.min_value,
				colorbalance_caps[v].range.max_value, colorbalance_caps[v].range.default_value,
				colorbalance_caps[v].range.step);
			    decoder->vpp_contrast_idx = v;
			    break;
			case VAProcColorBalanceAutoSaturation:
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)", "AutoSaturation",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value,
				decoder->filter_n);
			    break;
			case VAProcColorBalanceAutoBrightness:
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)", "AutoBrightness",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value,
				decoder->filter_n);
			    break;
			case VAProcColorBalanceAutoContrast:
			    Info("%s (%.2f - %.2f ++ %.2f = %.2f) (pos = %d)", "AutoContrast",
				colorbalance_caps[v].range.min_value, colorbalance_caps[v].range.max_value,
				colorbalance_caps[v].range.step, colorbalance_caps[v].range.default_value,
				decoder->filter_n);
			    break;

			default:
			    Info("video/vaapi: unsupported color balance filter #%02x", colorbalance_caps[v].type);
			    break;
		    }

		    cbal_param[v].type = VAProcFilterColorBalance;
		    cbal_param[v].attrib = colorbalance_caps[v].type;
		    cbal_param[v].value = colorbalance_caps[v].range.default_value;
		}
		va_status =
		    vaCreateBuffer(decoder->VaDisplay, decoder->vpp_ctx, VAProcFilterParameterBufferType,
		    sizeof(VAProcFilterParameterBufferColorBalance), colorbalance_cap_n, &cbal_param, &filter_buf_id);
		if (va_status != VA_STATUS_SUCCESS) {
		    Error("video/vaapi: Could not create buffer for color balance settings: %s",
			vaErrorStr(va_status));
		    break;
		}

		decoder->vpp_cbal_buf = &decoder->filters[decoder->filter_n];
		decoder->filters[decoder->filter_n++] = filter_buf_id;
		break;
	    case VAProcFilterSkinToneEnhancement:
		VaapiConfigStde.active = 1;
		Info("video/vaapi: skin tone enhancement supported");
		filter_buf_id =
		    VaapiSetupParameterBufferProcessing(decoder, filtertypes[u],
		    VaapiConfigStde.def_value * VaapiConfigStde.scale);
		if (filter_buf_id != VA_INVALID_ID) {
		    Info("Enabling skin tone filter (pos = %d)", decoder->filter_n);
		    decoder->vpp_stde_buf = &decoder->filters[decoder->filter_n];
		    decoder->filters[decoder->filter_n++] = filter_buf_id;
		}
		break;
	    default:
		Info("video/vaapi: unsupported filter #%02x", filtertypes[u]);
		break;
	}
	VaapiInitSurfaceFlags(decoder);
    }
    //
    //	query pipeline caps
    //
    pipeline_caps.input_color_standards = in_color_standards;
    pipeline_caps.num_input_color_standards = ARRAY_ELEMS(in_color_standards);
    pipeline_caps.output_color_standards = out_color_standards;
    pipeline_caps.num_output_color_standards = ARRAY_ELEMS(out_color_standards);

    va_status =
	vaQueryVideoProcPipelineCaps(decoder->VaDisplay, decoder->vpp_ctx, decoder->filters, decoder->filter_n,
	&pipeline_caps);
    if (va_status != VA_STATUS_SUCCESS) {
	Fatal("Failed to query proc pipeline caps, error = %s", vaErrorStr(va_status));
    }

    Info("Allocating %d forward reference surfaces for postprocessing", pipeline_caps.num_forward_references);
    decoder->ForwardRefSurfaces =
	realloc(decoder->ForwardRefSurfaces, pipeline_caps.num_forward_references * sizeof(VASurfaceID));
    decoder->ForwardRefCount = pipeline_caps.num_forward_references;

    Info("Allocating %d backward reference surfaces for postprocessing", pipeline_caps.num_backward_references);
    decoder->BackwardRefSurfaces =
	realloc(decoder->BackwardRefSurfaces, pipeline_caps.num_backward_references * sizeof(VASurfaceID));
    decoder->BackwardRefCount = pipeline_caps.num_backward_references;

    //TODO: Verify that rest of the capabilities are set properly
}

///
/// Configure VA-API for new video format.
///
/// @param decoder  VA-API decoder
///
static void VaapiSetup(VaapiDecoder * decoder, const AVCodecContext * video_ctx)
{
    int entrypoint_n;
    VAStatus status;
    VAImageFormat format[1];
    VAEntrypoint entrypoints[vaMaxNumEntrypoints(decoder->VaDisplay)];

    if (decoder->VppEntrypoint == VA_INVALID_ID) {
	status = vaQueryConfigEntrypoints(decoder->VaDisplay, VAProfileNone, entrypoints, &entrypoint_n);
	if (status != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't query entrypoints: %s", vaErrorStr(status));
	} else {
	    for (int i = 0; i < entrypoint_n; i++) {
		if (entrypoints[i] == VAEntrypointVideoProc) {
		    Info("video/vaapi: supports video processing");
		    decoder->VppEntrypoint = entrypoints[i];
		    VaapiVideoProcessing = 1;
		    break;
		}
	    }
	}
    }
    // create initial black surface and display
    VaapiBlackSurface(decoder);

    VaapiFindImageFormat(decoder, decoder->PixFmt, format);

    // FIXME: this image is only needed for software decoder and auto-crop
    if (decoder->GetPutImage
	&& vaCreateImage(decoder->VaDisplay, format, video_ctx->width, video_ctx->height,
	    decoder->Image) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't create image!");
    }
    Debug7("video/vaapi: created image %dx%d with id 0x%08x and buffer id 0x%08x", video_ctx->width, video_ctx->height,
	decoder->Image->image_id, decoder->Image->buf);

    // FIXME: interlaced not valid here?
    decoder->Resolution = VideoResolutionGroup(video_ctx->width, video_ctx->height, decoder->Interlaced);
    VaapiCreateSurfaces(decoder, VideoWindowWidth, VideoWindowHeight);

    status = vaCreateConfig(decoder->VaDisplay, VAProfileNone, decoder->VppEntrypoint, NULL, 0, &decoder->VppConfig);
    if (status != VA_STATUS_SUCCESS) {
	Fatal("video/vaapi: can't create config '%s'", vaErrorStr(status));
    }
    status =
	vaCreateContext(decoder->VaDisplay, decoder->VppConfig, VideoWindowWidth, VideoWindowHeight, VA_PROGRESSIVE,
	decoder->PostProcSurfacesRb, POSTPROC_SURFACES_MAX, &decoder->vpp_ctx);
    if (status != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't create context '%s'", vaErrorStr(status));
    }

    VaapiSetupVideoProcessing(decoder);

    VaapiUpdateOutput(decoder);

    //
    //	update OSD associate
    //
    if (video_ctx->hw_frames_ctx) {
	// FIXME: associate only if osd is displayed
	if (vaAssociateSubpicture(decoder->VaDisplay, VaOsdSubpicture,
		TO_VAAPI_FRAMES_CTX(video_ctx->hw_frames_ctx)->surface_ids,
		TO_VAAPI_FRAMES_CTX(video_ctx->hw_frames_ctx)->nb_surfaces, 0, 0, VideoWindowWidth, VideoWindowHeight,
		0, 0, VideoWindowWidth, VideoWindowHeight, VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD)
	    != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't associate subpicture");
	}
    }

    vaAssociateSubpicture(decoder->VaDisplay, VaOsdSubpicture, decoder->PostProcSurfacesRb, POSTPROC_SURFACES_MAX, 0,
	0, VideoWindowWidth, VideoWindowHeight, 0, 0, VideoWindowWidth, VideoWindowHeight,
	VA_SUBPICTURE_DESTINATION_IS_SCREEN_COORD);
}

///
/// Called from ffmpeg.
///
/// @param decoder  VA-API decoder
/// @param video_ctx	ffmpeg video codec context
///
///
static int gSurfacePtr = 0;
static VASurfaceID VaapiGetSurface(VaapiDecoder * decoder, const AVCodecContext * video_ctx)
{
    VASurfaceID surface;
    int num_surfaces = TO_VAAPI_FRAMES_CTX(video_ctx->hw_frames_ctx)->nb_surfaces;

    if (!TO_VAAPI_FRAMES_CTX(video_ctx->hw_frames_ctx)->surface_ids)
	return VA_INVALID_ID;

    if (!num_surfaces || num_surfaces < 0)
	return VA_INVALID_ID;

    if (!(gSurfacePtr % num_surfaces) || gSurfacePtr > num_surfaces)
	gSurfacePtr = 0;

    // Use ffmpeg internal surface structure as a ringbuffer to return least recently used surface
    // This is opposite of what ffmpeg does which by default returns most recently used one
    surface = TO_VAAPI_FRAMES_CTX(video_ctx->hw_frames_ctx)->surface_ids[gSurfacePtr++];

    return surface;
}

///
/// Callback to negotiate the PixelFormat.
///
/// @param fmt	is the list of formats which are supported by the codec,
/// it is terminated by -1 as 0 is a valid format, the
/// formats are ordered by quality.
///
static enum AVPixelFormat Vaapi_get_format(VaapiDecoder * decoder, AVCodecContext * video_ctx,
    const enum AVPixelFormat *fmt)
{
    const enum AVPixelFormat *fmt_idx;

    // Prefer VAAPI if found in list
    for (fmt_idx = fmt; *fmt_idx != -1; fmt_idx++) {
	if (*fmt_idx == AV_PIX_FMT_VAAPI) {
	    return *fmt_idx;
	}
    }

    // No VAAPI? pick default format
    return avcodec_default_get_format(video_ctx, fmt);
}

///
/// Draw surface of the VA-API decoder with x11.
///
/// vaPutSurface with intel backend does sync on v-sync.
///
/// @param decoder  VA-API decoder
/// @param surface  VA-API surface id
/// @param interlaced	flag interlaced source
/// @param deinterlaced flag source was deinterlaced
/// @param top_field_first  flag top_field_first for interlaced source
/// @param field    interlaced draw: 0 first field, 1 second field
///
static void VaapiPutSurfaceX11(VaapiDecoder * decoder, VASurfaceID surface, int interlaced, int deinterlaced,
    int top_field_first, int field)
{
    unsigned type;
    VAStatus status;
    uint32_t s;
    uint32_t e;

    // deinterlace
    if (interlaced && !deinterlaced && VideoDeinterlace[decoder->Resolution] != VAProcDeinterlacingNone) {
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
    status = vaSyncSurface(decoder->VaDisplay, surface);
    if (status != VA_STATUS_SUCCESS) {
	Error("video/vaapi: vaSyncSurface failed: %s", vaErrorStr(status));
	return;
    }
    if ((status = vaPutSurface(decoder->VaDisplay, surface, decoder->Window,
		// decoder src
		decoder->CropX, decoder->CropY, decoder->CropWidth, decoder->CropHeight,
		// video dst
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth, decoder->OutputHeight, NULL, 0,
		type | decoder->SurfaceFlagsTable[decoder->Resolution]))
	!= VA_STATUS_SUCCESS) {
	// switching video kills VdpPresentationQueueBlockUntilSurfaceIdle
	Error("video/vaapi: vaPutSurface failed: %s", vaErrorStr(status));
    }
    status = vaSyncSurface(decoder->VaDisplay, surface);
    if (status != VA_STATUS_SUCCESS) {
	Error("video/vaapi: vaSyncSurface failed: %s", vaErrorStr(status));
    }
    e = GetMsTicks();
    if (e - s > 2000) {
	Error("video/vaapi: gpu hung %dms %d", e - s, decoder->FrameCounter);
    }
}

///
/// VA-API auto-crop support.
///
/// @param decoder  VA-API hw decoder
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

	Debug7("video/vaapi: download image not available");

	// FIXME: PixFmt not set!
	//VaapiFindImageFormat(decoder, decoder->PixFmt, format);
	VaapiFindImageFormat(decoder, AV_PIX_FMT_NV12, format);
	//VaapiFindImageFormat(decoder, AV_PIX_FMT_YUV420P, format);
	if (vaCreateImage(VaDisplay, format, width, height, decoder->Image) != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't create image!");
	    return;
	}
    }
    // no problem to go back, we just wrote it
    // FIXME: we can pass the surface through.
    surface = decoder->SurfacesRb[(decoder->SurfaceWrite + VIDEO_SURFACES_MAX - 1) % VIDEO_SURFACES_MAX];

    //	Copy data from frame to image
    if (!decoder->GetPutImage && vaDeriveImage(decoder->VaDisplay, surface, decoder->Image) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: vaDeriveImage failed");
	decoder->GetPutImage = 1;
	goto again;
    }
    if (decoder->GetPutImage
	&& (i =
	    vaGetImage(decoder->VaDisplay, surface, 0, 0, decoder->InputWidth, decoder->InputHeight,
		decoder->Image->image_id)) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't get auto-crop image %d", i);
	return;
    }
    if (vaMapBuffer(VaDisplay, decoder->Image->buf, &va_image_data) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't map auto-crop image!");
	return;
    }
    // convert vaapi to our frame format
    for (i = 0; (unsigned)i < decoder->Image->num_planes; ++i) {
	data[i] = va_image_data + decoder->Image->offsets[i];
	pitches[i] = decoder->Image->pitches[i];
    }

    AutoCropDetect(decoder->AutoCrop, width, height, data, pitches);

    if (vaUnmapBuffer(VaDisplay, decoder->Image->buf) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't unmap auto-crop image!");
    }
    if (!decoder->GetPutImage) {
	if (vaDestroyImage(VaDisplay, decoder->Image->image_id) != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't destroy image!");
	}
	decoder->Image->image_id = VA_INVALID_ID;
    }
    // ignore black frames
    if (decoder->AutoCrop->Y1 >= decoder->AutoCrop->Y2) {
	return;
    }

    crop14 = (decoder->InputWidth * decoder->InputAspect.num * 9) / (decoder->InputAspect.den * 14);
    crop14 = (decoder->InputHeight - crop14) / 2;
    crop16 = (decoder->InputWidth * decoder->InputAspect.num * 9) / (decoder->InputAspect.den * 16);
    crop16 = (decoder->InputHeight - crop16) / 2;

    if (decoder->AutoCrop->Y1 >= crop16 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >= crop16 - AutoCropTolerance) {
	next_state = 16;
    } else if (decoder->AutoCrop->Y1 >= crop14 - AutoCropTolerance
	&& decoder->InputHeight - decoder->AutoCrop->Y2 >= crop14 - AutoCropTolerance) {
	next_state = 14;
    } else {
	next_state = 0;
    }

    if (decoder->AutoCrop->State == next_state) {
	return;
    }

    Debug7("video: crop aspect %d:%d %d/%d %+d%+d", decoder->InputAspect.num, decoder->InputAspect.den, crop14, crop16,
	decoder->AutoCrop->Y1, decoder->InputHeight - decoder->AutoCrop->Y2);

    Debug7("video: crop aspect %d -> %d", decoder->AutoCrop->State, next_state);

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
	decoder->CropY = (next_state == 16 ? crop16 : crop14) + VideoCutTopBottom[decoder->Resolution];
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
	    decoder->OutputY = (decoder->VideoHeight - decoder->OutputHeight) / 2;
	} else if (decoder->OutputHeight > decoder->VideoHeight) {
	    decoder->OutputHeight = decoder->VideoHeight;
	    decoder->OutputX = (decoder->VideoWidth - decoder->OutputWidth) / 2;
	}
	Debug7("video: aspect output %dx%d %dx%d%+d%+d", decoder->InputWidth, decoder->InputHeight,
	    decoder->OutputWidth, decoder->OutputHeight, decoder->OutputX, decoder->OutputY);
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
/// VA-API check if auto-crop todo.
///
/// @param decoder  VA-API hw decoder
///
/// @note auto-crop only supported with normal 4:3 display mode
///
static void VaapiCheckAutoCrop(VaapiDecoder * decoder)
{
    // reduce load, check only n frames
    if (Video4to3ZoomMode == VideoNormal && AutoCropInterval && !(decoder->FrameCounter % AutoCropInterval)) {
	AVRational input_aspect_ratio;
	AVRational tmp_ratio;

	av_reduce(&input_aspect_ratio.num, &input_aspect_ratio.den, decoder->InputWidth * decoder->InputAspect.num,
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
/// VA-API reset auto-crop.
///
static void VaapiResetAutoCrop(void)
{
    int i;

    for (i = 0; i < VaapiDecoderN; ++i) {
	VaapiDecoders[i]->AutoCrop->State = 0;
	VaapiDecoders[i]->AutoCrop->Count = 0;
    }
}

///
/// Queue output surface.
///
/// @param decoder  VA-API decoder
/// @param surface  output surface
///
/// @note we can't mix software and hardware decoder surfaces
///
static void VaapiQueueSurfaceNew(VaapiDecoder * decoder, VASurfaceID surface)
{
    unsigned int i;

    /* Advance surfaces in queue:
     * Playback position -> last forward temporal reference (past-surface)
     * Nearest surface to playback -> playback position
     * New surface -> first surface in backward (future-surface) reference queue
     */
    if (decoder->ForwardRefCount) {
	for (i = 0; i < decoder->ForwardRefCount - 1; ++i) {
	    decoder->ForwardRefSurfaces[i] = decoder->ForwardRefSurfaces[i + 1];
	}
	decoder->ForwardRefSurfaces[decoder->ForwardRefCount - 1] = decoder->PlaybackSurface;
    }

    if (decoder->BackwardRefCount) {
	decoder->PlaybackSurface = decoder->BackwardRefSurfaces[0];
	for (i = decoder->BackwardRefCount - 1; i > 0; --i) {
	    decoder->BackwardRefSurfaces[i - 1] = decoder->BackwardRefSurfaces[i];
	}
	decoder->BackwardRefSurfaces[decoder->BackwardRefCount - 1] = surface;
    } else {
	/* No backward (future-surface) references needed so put new surface
	   to playback position */
	decoder->PlaybackSurface = surface;
    }
}

///
/// Queue output surface.
///
/// @param decoder  VA-API decoder
/// @param surface  output surface
/// @param softdec  software decoder
///
/// @note we can't mix software and hardware decoder surfaces
///
static void VaapiQueueSurface(VaapiDecoder * decoder, VASurfaceID surface, int softdec)
{
    VASurfaceID old;
    VASurfaceID *firstfield = NULL;
    VASurfaceID *secondfield = NULL;

    ++decoder->FrameCounter;

    if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX - 1) {
	++decoder->FramesDropped;
	Error("video: output buffer full, dropping frame (%d/%d)", decoder->FramesDropped, decoder->FrameCounter);
	if (!(decoder->FramesDisplayed % 300)) {
	    VaapiPrintFrames(decoder);
	}
	if (softdec) {			// software surfaces only
	    VaapiReleaseSurface(decoder, surface);
	}
	return;
    }
    //
    //	Check and release, old surface
    //
    if ((old = decoder->SurfacesRb[decoder->SurfaceWrite]) != VA_INVALID_ID) {
	// now we can release the surface
	if (softdec) {			// software surfaces only
	    VaapiReleaseSurface(decoder, old);
	}
    }

    /* No point in adding new surface if cleanup is in progress */
    if (pthread_mutex_trylock(&VideoMutex))
	return;

    /* Queue new surface and run postprocessing filters */
    VaapiQueueSurfaceNew(decoder, surface);
    firstfield = VaapiApplyFilters(decoder, decoder->TopFieldFirst ? 1 : 0);
    if (!firstfield) {
	/* Use unprocessed surface if postprocessing fails */
	decoder->Deinterlaced = 0;

	decoder->SurfacesRb[decoder->SurfaceWrite] = surface;
    } else {
	decoder->Deinterlaced = 1;

	decoder->SurfacesRb[decoder->SurfaceWrite] = *firstfield;
    }

    /* Queue the first field */
    decoder->SurfaceWrite = (decoder->SurfaceWrite + 1) % VIDEO_SURFACES_MAX;
    decoder->SurfaceField = decoder->TopFieldFirst ? 0 : 1;
    atomic_inc(&decoder->SurfacesFilled);

    /* Run postprocessing twice for top & bottom fields */
    if (decoder->Interlaced) {
	secondfield = VaapiApplyFilters(decoder, decoder->TopFieldFirst ? 0 : 1);
	if (!secondfield) {
	    /* Use unprocessed surface if postprocessing fails */
	    decoder->Deinterlaced = 0;

	    decoder->SurfacesRb[decoder->SurfaceWrite] = surface;
	} else {
	    decoder->Deinterlaced = 1;

	    decoder->SurfacesRb[decoder->SurfaceWrite] = *secondfield;
	}
	decoder->SurfaceWrite = (decoder->SurfaceWrite + 1) % VIDEO_SURFACES_MAX;
	decoder->SurfaceField = decoder->TopFieldFirst ? 1 : 0;
	atomic_inc(&decoder->SurfacesFilled);
    }

    pthread_mutex_unlock(&VideoMutex);

    Debug8("video/vaapi: yy video surface %#010x ready", surface);
}

///
/// Create and display a black empty surface.
///
/// @param decoder  VA-API decoder
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
	Error("video/vaapi: no osd subpicture yet");
	return;
    }

    if (decoder->BlackSurface == VA_INVALID_ID) {
	uint8_t *va_image_data;
	unsigned u;

	status =
	    vaCreateSurfaces(decoder->VaDisplay, VA_RT_FORMAT_YUV420, VideoWindowWidth, VideoWindowHeight,
	    &decoder->BlackSurface, 1, NULL, 0);
	if (status != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't create a surface: %s", vaErrorStr(status));
	    return;
	}
	// full sized surface, no difference unscaled/scaled osd
	status =
	    vaAssociateSubpicture(decoder->VaDisplay, VaOsdSubpicture, &decoder->BlackSurface, 1, 0, 0,
	    VaOsdImage.width, VaOsdImage.height, 0, 0, VideoWindowWidth, VideoWindowHeight, 0);
	if (status != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't associate subpicture: %s", vaErrorStr(status));
	}
	Debug7("video/vaapi: associate %08x", decoder->BlackSurface);

	if (decoder->Image->image_id == VA_INVALID_ID) {
	    VAImageFormat format[1];

	    VaapiFindImageFormat(decoder, AV_PIX_FMT_NV12, format);
	    status = vaCreateImage(decoder->VaDisplay, format, VideoWindowWidth, VideoWindowHeight, decoder->Image);
	    if (status != VA_STATUS_SUCCESS) {
		Error("video/vaapi: can't create image: %s", vaErrorStr(status));
		return;
	    }
	}

	status = vaMapBuffer(decoder->VaDisplay, decoder->Image->buf, (void **)&va_image_data);
	if (status != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't map the image: %s", vaErrorStr(status));
	    return;
	}

	for (u = 0; u < decoder->Image->data_size; ++u) {
	    if (u < decoder->Image->offsets[1]) {
		va_image_data[u] = 0x00;    // Y
	    } else if (u % 2 == 0) {
		va_image_data[u] = 0x80;    // U
	    } else {
#ifdef DEBUG
		// make black surface visible
		va_image_data[u] = 0xFF;    // V
#else
		va_image_data[u] = 0x80;    // V
#endif
	    }
	}

	if (vaUnmapBuffer(decoder->VaDisplay, decoder->Image->buf) != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't unmap the image!");
	}

	if (decoder->GetPutImage) {
	    status =
		vaPutImage(decoder->VaDisplay, decoder->BlackSurface, decoder->Image->image_id, 0, 0, VideoWindowWidth,
		VideoWindowHeight, 0, 0, VideoWindowWidth, VideoWindowHeight);
	    if (status != VA_STATUS_SUCCESS) {
		Error("video/vaapi: can't put image!");
	    }
	} else {
	    // FIXME: PutImage isn't always supported
	    Debug7("video/vaapi: put image not supported, alternative path not written");
	}

#ifdef DEBUG
	start = GetMsTicks();
#endif
	if (vaSyncSurface(decoder->VaDisplay, decoder->BlackSurface) != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: vaSyncSurface failed");
	}
    } else {
#ifdef DEBUG
	start = GetMsTicks();
#endif
    }

    Debug8("video/vaapi: yy black video surface %#010x displayed", decoder->BlackSurface);
    sync = GetMsTicks();
    xcb_flush(Connection);
    if ((status = vaPutSurface(decoder->VaDisplay, decoder->BlackSurface, decoder->Window,
		// decoder src
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth, decoder->OutputHeight,
		// video dst
		decoder->OutputX, decoder->OutputY, decoder->OutputWidth, decoder->OutputHeight, NULL, 0,
		VA_FRAME_PICTURE)) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: vaPutSurface failed %d", status);
    }
    clock_gettime(CLOCK_MONOTONIC, &decoder->FrameTime);

    put1 = GetMsTicks();
    if (put1 - sync > 2000) {
	Error("video/vaapi: gpu hung %dms %d", put1 - sync, decoder->FrameCounter);
    }
#ifdef DEBUG
    Debug8("video/vaapi: sync %2u put1 %2u", sync - start, put1 - sync);
#endif
    usleep(1 * 1000);
}

static int VaapiIsPictureChanged(VaapiDecoder * decoder, const AVCodecContext * video_ctx, const AVFrame * frame)
{
    if (video_ctx->width != decoder->InputWidth || video_ctx->height != decoder->InputHeight
	|| video_ctx->pix_fmt != decoder->PixFmt) {
	Debug7("video/vaapi: Picture change detected");
	return 1;
    }

    if (decoder->Closing < 0) {
	return 1;
    }

    if (decoder->VaDisplay != TO_VAAPI_DEVICE_CTX(video_ctx->hw_device_ctx)->display) {
	Debug7("video/vaapi: VaDisplay changed");
	return 1;
    }

    return 0;
}

///
/// Render a ffmpeg frame
///
/// @param decoder  VA-API decoder
/// @param video_ctx	ffmpeg video codec context
/// @param frame    frame to display
///
static void VaapiRenderFrame(VaapiDecoder * decoder, const AVCodecContext * video_ctx, const AVFrame * frame)
{
    VASurfaceID surface;
    int interlaced;

    //
    //	Check image, format, size
    //
    if (VaapiIsPictureChanged(decoder, video_ctx, frame)) {

	// Cleanup previous VA-API allocations
	VaapiCleanup(decoder);

	// set new sizes and parameters
	decoder->PixFmt = video_ctx->pix_fmt;
	decoder->InputWidth = frame->width;
	decoder->InputHeight = frame->height;

	decoder->VaDisplay = TO_VAAPI_DEVICE_CTX(video_ctx->hw_device_ctx)->display;
	VaDisplay = TO_VAAPI_DEVICE_CTX(video_ctx->hw_device_ctx)->display;

	// Configure VA-API to process new frame
	VaapiSetup(decoder, video_ctx);
    }
    // FIXME: some tv-stations toggle interlace on/off
    // frame->interlaced_frame isn't always correct set
    interlaced = frame->interlaced_frame;

    // FIXME: should be done by init video_ctx->field_order
    if (decoder->Interlaced != interlaced || decoder->TopFieldFirst != frame->top_field_first) {

	Debug7("video/vaapi: interlaced %d top-field-first %d", interlaced, frame->top_field_first);

	decoder->Interlaced = interlaced;
	decoder->TopFieldFirst = frame->top_field_first;
	decoder->SurfaceField = 0;
    }
    // update aspect ratio changes
    if (decoder->InputWidth && decoder->InputHeight && av_cmp_q(decoder->InputAspect, frame->sample_aspect_ratio)) {
	Debug7("video/vaapi: aspect ratio changed");

	decoder->InputAspect = frame->sample_aspect_ratio;
	VaapiUpdateOutput(decoder);
    }
    //
    // Hardware render
    //
    if (video_ctx->hw_frames_ctx) {

	surface = (VASurfaceID) (uintptr_t) frame->data[3];
	Debug8("video/vaapi: hw render hw surface %#010x", surface);

	VaapiQueueSurface(decoder, surface, 0);

	//
	// VAImage render
	//
    } else {
	void *va_image_data;
	VAStatus status;
	AVFrame picture[1];

	Debug8("video/vaapi: hw render sw surface");

	// FIXME: Need to insert software deinterlace here
	// FIXME: can/must insert auto-crop here (is done after upload)

	// get a free surface and upload the image
	surface = VaapiGetPluginSurface(decoder);
	Debug8("video/vaapi: video surface %#010x displayed", surface);

	if (!decoder->GetPutImage && vaDeriveImage(decoder->VaDisplay, surface, decoder->Image) != VA_STATUS_SUCCESS) {
	    VAImageFormat format[1];

	    Error("video/vaapi: vaDeriveImage failed");

	    decoder->GetPutImage = 1;
	    VaapiFindImageFormat(decoder, decoder->PixFmt, format);
	    if (vaCreateImage(VaDisplay, format, video_ctx->width, video_ctx->height,
		    decoder->Image) != VA_STATUS_SUCCESS) {
		Error("video/vaapi: can't create image!");
	    }
	}
	//
	//  Copy data from frame to image
	//
	if (vaMapBuffer(VaDisplay, decoder->Image->buf, &va_image_data) != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't map the image!");
	}
	// crazy: intel mixes YV12 and NV12 with mpeg
	if (decoder->Image->format.fourcc == VA_FOURCC_NV12) {
	    // intel NV12 convert YV12 to NV12
	    struct SwsContext *img_convert_ctx =
		sws_getContext(video_ctx->width, video_ctx->height, frame->format, video_ctx->width, video_ctx->height,
		AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, NULL, NULL, NULL);

	    if (img_convert_ctx) {
		picture->data[0] = va_image_data + decoder->Image->offsets[0];
		picture->linesize[0] = decoder->Image->pitches[0];
		picture->data[1] = va_image_data + decoder->Image->offsets[1];
		picture->linesize[1] = decoder->Image->pitches[1];
		picture->data[2] = va_image_data + decoder->Image->offsets[2];
		picture->linesize[2] = decoder->Image->pitches[2];
		sws_scale(img_convert_ctx, (const uint8_t * const *)frame->data, frame->linesize, 0, video_ctx->height,
		    picture->data, picture->linesize);
		sws_freeContext(img_convert_ctx);
	    } else {
		Error("video/vaapi: can't create context for image conversion!");
	    }
	} else if (decoder->Image->format.fourcc == VA_FOURCC_I420) {
	    picture->data[0] = va_image_data + decoder->Image->offsets[0];
	    picture->linesize[0] = decoder->Image->pitches[0];
	    picture->data[1] = va_image_data + decoder->Image->offsets[1];
	    picture->linesize[1] = decoder->Image->pitches[2];
	    picture->data[2] = va_image_data + decoder->Image->offsets[2];
	    picture->linesize[2] = decoder->Image->pitches[1];

	    av_image_copy(picture->data, picture->linesize, (const uint8_t **)frame->data, frame->linesize,
		video_ctx->pix_fmt, video_ctx->width, video_ctx->height);
	} else if (decoder->Image->num_planes == 3) {
	    picture->data[0] = va_image_data + decoder->Image->offsets[0];
	    picture->linesize[0] = decoder->Image->pitches[0];
	    picture->data[1] = va_image_data + decoder->Image->offsets[2];
	    picture->linesize[1] = decoder->Image->pitches[2];
	    picture->data[2] = va_image_data + decoder->Image->offsets[1];
	    picture->linesize[2] = decoder->Image->pitches[1];

	    av_image_copy(picture->data, picture->linesize, (const uint8_t **)frame->data, frame->linesize,
		video_ctx->pix_fmt, video_ctx->width, video_ctx->height);
	}

	if (vaUnmapBuffer(VaDisplay, decoder->Image->buf) != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't unmap the image!");
	}

	Debug8("video/vaapi: buffer %dx%d <- %dx%d", decoder->Image->width, decoder->Image->height, video_ctx->width,
	    video_ctx->height);

	if (decoder->GetPutImage
	    && (status =
		vaPutImage(VaDisplay, surface, decoder->Image->image_id, 0, 0, video_ctx->width, video_ctx->height, 0,
		    0, VideoWindowWidth, VideoWindowHeight)) != VA_STATUS_SUCCESS) {
	    Error("video/vaapi: can't put image err:%s!", vaErrorStr(status));
	}

	if (!decoder->GetPutImage) {
	    if (vaDestroyImage(VaDisplay, decoder->Image->image_id) != VA_STATUS_SUCCESS) {
		Error("video/vaapi: can't destroy image!");
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
/// Advance displayed frame of decoder.
///
/// @param decoder  VA-API hw decoder
///
static void VaapiAdvanceDecoderFrame(VaapiDecoder * decoder)
{
    VASurfaceID surface;
    int filled;

    filled = atomic_read(&decoder->SurfacesFilled);
    // FIXME: this should check the caller
    // check decoder, if new surface is available
    if (filled <= 1) {
	// keep use of last surface
	++decoder->FramesDuped;
	// FIXME: don't warn after stream start, don't warn during pause
	Error("video: display buffer empty, duping frame (%d/%d) %d", decoder->FramesDuped, decoder->FrameCounter,
	    VideoGetBuffers(decoder->Stream));
	return;
    }
    // wait for rendering finished
    surface = decoder->SurfacesRb[decoder->SurfaceRead];
    if (vaSyncSurface(decoder->VaDisplay, surface) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: vaSyncSurface failed");
    }

    decoder->SurfaceRead = (decoder->SurfaceRead + 1) % VIDEO_SURFACES_MAX;
    atomic_dec(&decoder->SurfacesFilled);
}

///
/// Display a video frame.
///
/// @todo FIXME: add detection of missed frames
///
static void VaapiDisplayFrame(void)
{
#ifdef DEBUG
    uint32_t start;
#endif
    struct timespec nowtime;
    VaapiDecoder *decoder;

    if (VideoSurfaceModesChanged) {	// handle changed modes
	VideoSurfaceModesChanged = 0;
	for (int i = 0; i < VaapiDecoderN; ++i) {
	    VaapiInitSurfaceFlags(VaapiDecoders[i]);
	}
    }
    // look if any stream have a new surface available
    for (int i = 0; i < VaapiDecoderN; ++i) {
	VASurfaceID surface;
	int filled;

	decoder = VaapiDecoders[i];
	decoder->FramesDisplayed++;
	decoder->StartCounter++;

	filled = atomic_read(&decoder->SurfacesFilled);
	// no surface available show black with possible osd
	if (!filled) {
	    VaapiBlackSurface(decoder);
	    VaapiMessage(2, "video/vaapi: black surface displayed");
	    continue;
	}

	surface = decoder->SurfacesRb[decoder->SurfaceRead];
#ifdef DEBUG
	if (surface == VA_INVALID_ID) {
	    Debug8("video/vaapi: invalid surface in ringbuffer");
	}
	Debug8("video/vaapi: yy video surface %#010x displayed", surface);
	start = GetMsTicks();
#endif
	VaapiPutSurfaceX11(decoder, surface, decoder->Interlaced, decoder->Deinterlaced, decoder->TopFieldFirst,
	    decoder->SurfaceField);
#ifdef DEBUG
	Debug8("video/vaapi: put %2ums", GetMsTicks() - start);
#endif
	clock_gettime(CLOCK_MONOTONIC, &nowtime);
	// FIXME: 31 only correct for 50Hz
	if ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
	    * 1000 * 1000 * 1000 + (nowtime.tv_nsec - decoder->FrameTime.tv_nsec) > 31 * 1000 * 1000) {
	    // FIXME: ignore still-frame, trick-speed
	    Debug7("video/vaapi: time/frame too long %ldms", ((nowtime.tv_sec - decoder->FrameTime.tv_sec)
		    * 1000 * 1000 * 1000 + (nowtime.tv_nsec - decoder->FrameTime.tv_nsec)) / (1000 * 1000));
	}
	decoder->FrameTime = nowtime;
    }
}

///
/// Set VA-API decoder video clock.
///
/// @param decoder  VA-API hardware decoder
/// @param pts	audio presentation timestamp
///
void VaapiSetClock(VaapiDecoder * decoder, int64_t pts)
{
    decoder->PTS = pts;
}

///
/// Get VA-API decoder video clock.
///
/// @param decoder  VA-API decoder
///
static int64_t VaapiGetClock(const VaapiDecoder * decoder)
{
    // pts is the timestamp of the latest decoded frame
    if (decoder->PTS == (int64_t) AV_NOPTS_VALUE) {
	return AV_NOPTS_VALUE;
    }
    // subtract buffered decoded frames
    if (decoder->Interlaced) {
	return decoder->PTS - 20 * 90 * (2 * atomic_read(&decoder->SurfacesFilled)
	    - decoder->SurfaceField);
    }
    return decoder->PTS - 20 * 90 * (atomic_read(&decoder->SurfacesFilled) + 2);
}

///
/// Set VA-API decoder closing stream flag.
///
/// @param decoder  VA-API decoder
///
static void VaapiSetClosing(VaapiDecoder * decoder)
{
    decoder->Closing = 1;
}

///
/// Reset start of frame counter.
///
/// @param decoder  VA-API decoder
///
static void VaapiResetStart(VaapiDecoder * decoder)
{
    decoder->StartCounter = 0;
}

///
/// Set trick play speed.
///
/// @param decoder  VA-API decoder
/// @param speed    trick speed (0 = normal)
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
/// Get VA-API decoder statistics.
///
/// @param decoder  VA-API decoder
/// @param[out] missed	missed frames
/// @param[out] duped	duped frames
/// @param[out] dropped dropped frames
/// @param[out] count	number of decoded frames
///
void VaapiGetStats(VaapiDecoder * decoder, int *missed, int *duped, int *dropped, int *counter)
{
    *missed = decoder->FramesMissed;
    *duped = decoder->FramesDuped;
    *dropped = decoder->FramesDropped;
    *counter = decoder->FrameCounter;
}

///
/// Sync decoder output to audio.
///
/// trick-speed show frame <n> times
/// still-picture   show frame until new frame arrives
/// 60hz-mode	repeat every 5th picture
/// video>audio slow down video by duplicating frames
/// video<audio speed up video by skipping frames
/// soft-start	show every second frame
///
/// @param decoder  VAAPI hw decoder
///
static void VaapiSyncDecoder(VaapiDecoder * decoder)
{
    int err;
    int filled;
    int64_t audio_clock;
    int64_t video_clock;

    err = 0;
    mutex_start_time = GetMsTicks();
    pthread_mutex_lock(&PTS_mutex);
    pthread_mutex_lock(&ReadAdvance_mutex);
    audio_clock = AudioGetClock();
    pthread_mutex_unlock(&ReadAdvance_mutex);
    pthread_mutex_unlock(&PTS_mutex);
    if (GetMsTicks() - mutex_start_time > max_mutex_delay) {
	max_mutex_delay = GetMsTicks() - mutex_start_time;
	Debug7("video: mutex delay: %" PRIu32 "ms", max_mutex_delay);
    }
    video_clock = VaapiGetClock(decoder);
    filled = atomic_read(&decoder->SurfacesFilled);

    // 60Hz: repeat every 5th field
    if (Video60HzMode && !(decoder->FramesDisplayed % 6)) {
	if (audio_clock == (int64_t) AV_NOPTS_VALUE || video_clock == (int64_t) AV_NOPTS_VALUE) {
	    goto out;
	}
	// both clocks are known
	if (audio_clock + VideoAudioDelay <= video_clock + 25 * 90) {
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
    if (!VideoSoftStartSync && decoder->StartCounter < VideoSoftStartFrames && video_clock != (int64_t) AV_NOPTS_VALUE
	&& (audio_clock == (int64_t) AV_NOPTS_VALUE || video_clock > audio_clock + VideoAudioDelay + 120 * 90)) {
	err = VaapiMessage(2, "video: initial slow down video, frame %d", decoder->StartCounter);
	goto out;
    }

    if (decoder->SyncCounter && decoder->SyncCounter--) {
	goto skip_sync;
    }

    if (audio_clock != (int64_t) AV_NOPTS_VALUE && video_clock != (int64_t) AV_NOPTS_VALUE) {
	// both clocks are known
	int diff;
	int lower_limit;

	diff = video_clock - audio_clock - VideoAudioDelay;
	lower_limit = !IsReplay()? -25 : 32;
	if (!IsReplay()) {
	    diff = (decoder->LastAVDiff + diff) / 2;
	    decoder->LastAVDiff = diff;
	}

	if (abs(diff) > 5000 * 90) {	// more than 5s
	    err = VaapiMessage(1, "video: audio/video difference too big");
	} else if (diff > 100 * 90) {
	    // FIXME: this quicker sync step, did not work with new code!
	    err = VaapiMessage(1, "video: slow down video, duping frame");
	    ++decoder->FramesDuped;
	    if (VideoSoftStartSync) {
		decoder->SyncCounter = 1;
		goto out;
	    }
	} else if (diff > 55 * 90) {
	    err = VaapiMessage(1, "video: slow down video, duping frame");
	    ++decoder->FramesDuped;
	    if (VideoSoftStartSync) {
		decoder->SyncCounter = 1;
		goto out;
	    }
	} else if (diff < lower_limit * 90 && filled > 1 + 2 * decoder->Interlaced) {
	    err = VaapiMessage(1, "video: speed up video, droping frame");
	    ++decoder->FramesDropped;
	    VaapiAdvanceDecoderFrame(decoder);
	    if (VideoSoftStartSync) {
		decoder->SyncCounter = 1;
	    }
	}
#if defined(DEBUG) || defined(AV_INFO)
	if (!decoder->SyncCounter && decoder->StartCounter < 1000) {
#ifdef DEBUG
	    Debug7("video/vaapi: synced after %d frames %dms", decoder->StartCounter, GetMsTicks() - VideoSwitch);
#else
	    Info("video/vaapi: synced after %d frames", decoder->StartCounter);
#endif
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
		VaapiMessage(0, "video: decoder buffer empty, duping frame (%d/%d) %d v-buf", decoder->FramesDuped,
		decoder->FrameCounter, VideoGetBuffers(decoder->Stream));
	    // some time no new picture
	    if (decoder->Closing < -300) {
		// clear ring buffer to trigger black picture
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
	Info("video: %s%+5" PRId64 " %4" PRId64 " %3d/\\ms %3d%+d v-buf", Timestamp2String(video_clock),
	    abs((video_clock - audio_clock) / 90) < 8888 ? ((video_clock - audio_clock) / 90) : 8888,
	    AudioGetDelay() / 90, (int)VideoDeltaPTS / 90, VideoGetBuffers(decoder->Stream),
	    decoder->Interlaced ? (2 * atomic_read(&decoder->SurfacesFilled)
		- decoder->SurfaceField)
	    : atomic_read(&decoder->SurfacesFilled));
	if (!(decoder->FramesDisplayed % (5 * 60 * 60))) {
	    VaapiPrintFrames(decoder);
	}
    }
#endif
    return;				// fix gcc bug!
}

///
/// Sync a video frame.
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
/// Sync and display surface.
///
static void VaapiSyncDisplayFrame(void)
{
    VaapiDisplayFrame();
    VaapiSyncFrame();
}

///
/// Sync and render a ffmpeg frame
///
/// @param decoder  VA-API decoder
/// @param video_ctx	ffmpeg video codec context
/// @param frame    frame to display
///
static void VaapiSyncRenderFrame(VaapiDecoder * decoder, const AVCodecContext * video_ctx, const AVFrame * frame)
{
#ifdef DEBUG
    if (!atomic_read(&decoder->SurfacesFilled)) {
	Debug7("video: new stream frame %dms", GetMsTicks() - VideoSwitch);
    }
#endif

    // if video output buffer is full, wait and display surface.
    // loop for interlace
    if (atomic_read(&decoder->SurfacesFilled) >= VIDEO_SURFACES_MAX - 1) {
	Info("video/vaapi: this code part shouldn't be used");
	return;
    }

    if (!decoder->Closing) {
	VideoSetPts(&decoder->PTS, decoder->Interlaced, video_ctx, frame);
    }
    VaapiRenderFrame(decoder, video_ctx, frame);
    VaapiCheckAutoCrop(decoder);
}

///
/// Set VA-API background color.
///
/// @param rgba 32 bit RGBA color.
///
static void VaapiSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
    Error("video/vaapi: FIXME: SetBackground not supported");
}

///
/// Set VA-API video mode.
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
	VaapiCleanup(VaapiDecoders[i]);
	VaapiUpdateOutput(VaapiDecoders[i]);
    }
}

///
/// Set VA-API video output position.
///
/// @param decoder  VA-API decoder
/// @param x	video output x coordinate inside the window
/// @param y	video output y coordinate inside the window
/// @param width    video output width
/// @param height   video output height
///
static void VaapiSetOutputPosition(VaapiDecoder * decoder, int x, int y, int width, int height)
{
    Debug7("video/vaapi: output %dx%d%+d%+d", width, height, x, y);

    decoder->VideoX = x;
    decoder->VideoY = y;
    decoder->VideoWidth = width;
    decoder->VideoHeight = height;
}

///
/// Handle a va-api display.
///
static void VaapiDisplayHandlerThread(void)
{
    int i;
    int err;
    int allfull;
    int decoded;
    struct timespec nowtime;
    VaapiDecoder *decoder;

    allfull = 1;
    decoded = 0;
    pthread_mutex_lock(&VideoLockMutex);
    for (i = 0; i < VaapiDecoderN; ++i) {
	int filled;

	decoder = VaapiDecoders[i];

	//
	// fill frame output ring buffer
	//
	filled = atomic_read(&decoder->SurfacesFilled);
	if (filled < VIDEO_SURFACES_MAX - 1) {
	    // FIXME: hot polling
	    // fetch+decode or reopen
	    allfull = 0;
	    err = VideoDecodeInput(decoder->Stream);
	} else {
	    err = VideoPollInput(decoder->Stream);
	}
	// decoder can be invalid here
	if (err) {
	    // nothing buffered?
	    if (err == -1 && decoder->Closing) {
		decoder->Closing--;
		if (!decoder->Closing) {
		    Debug7("video/vaapi: closing eof");
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
	usleep(1 * 1000);
    }
    // all decoder buffers are full
    // speed up filling display queue, wait on display queue empty
    if (!allfull) {
	clock_gettime(CLOCK_MONOTONIC, &nowtime);
	// time for one frame over?
	if ((nowtime.tv_sec - VaapiDecoders[0]->FrameTime.tv_sec) * 1000 * 1000 * 1000 + (nowtime.tv_nsec -
		VaapiDecoders[0]->FrameTime.tv_nsec) < 15 * 1000 * 1000) {
	    return;
	}
    }

    pthread_mutex_lock(&VideoLockMutex);
    VaapiSyncDisplayFrame();
    pthread_mutex_unlock(&VideoLockMutex);
}

//----------------------------------------------------------------------------
//  VA-API OSD
//----------------------------------------------------------------------------

///
/// Clear subpicture image.
///
/// @note looked by caller
///
static void VaapiOsdClear(void)
{
    void *image_buffer;

    // osd image available?
    if (VaOsdImage.image_id == VA_INVALID_ID) {
	return;
    }

    Debug7("video/vaapi: clear image");

    if (VaOsdImage.width < OsdDirtyWidth + OsdDirtyX || VaOsdImage.height < OsdDirtyHeight + OsdDirtyY) {
	Debug7("video/vaapi: OSD dirty area will not fit");
    }
    if (VaOsdImage.width < OsdDirtyX || VaOsdImage.height < OsdDirtyY)
	return;

    if (VaOsdImage.width < OsdDirtyWidth + OsdDirtyX)
	OsdDirtyWidth = VaOsdImage.width - OsdDirtyX;
    if (VaOsdImage.height < OsdDirtyHeight + OsdDirtyY)
	OsdDirtyHeight = VaOsdImage.height - OsdDirtyY;

    // map osd surface/image into memory.
    if (vaMapBuffer(VaDisplay, VaOsdImage.buf, &image_buffer) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't map osd image buffer");
	return;
    }
    // have dirty area.
    if (OsdDirtyWidth && OsdDirtyHeight) {
	int o;

	for (o = 0; o < OsdDirtyHeight; ++o) {
	    memset(image_buffer + (OsdDirtyX + (o + OsdDirtyY) * VaOsdImage.width) * 4, 0x00, OsdDirtyWidth * 4);
	}
    } else {
	// 100% transparent
	memset(image_buffer, 0x00, VaOsdImage.data_size);
    }

    if (vaUnmapBuffer(VaDisplay, VaOsdImage.buf) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't unmap osd image buffer");
    }
}

///
/// Upload ARGB to subpicture image.
///
/// @param xi	x-coordinate in argb image
/// @param yi	y-coordinate in argb image
/// @paran height   height in pixel in argb image
/// @paran width    width in pixel in argb image
/// @param pitch    pitch of argb image
/// @param argb 32bit ARGB image data
/// @param x	x-coordinate on screen of argb image
/// @param y	y-coordinate on screen of argb image
///
/// @note looked by caller
///
static void VaapiOsdDrawARGB(int xi, int yi, int width, int height, int pitch, const uint8_t * argb, int x, int y)
{
#ifdef DEBUG
    uint32_t start;
    uint32_t end;
#endif
    void *image_buffer;
    int o;
    int copywidth, copyheight;

    // osd image available?
    if (VaOsdImage.image_id == VA_INVALID_ID) {
	return;
    }

    if (VaOsdImage.width < width + x || VaOsdImage.height < height + y) {
	Error("video/vaapi: OSD will not fit (w: %d+%d, w-avail: %d, h: %d+%d, h-avail: %d", width, x,
	    VaOsdImage.width, height, y, VaOsdImage.height);
    }
    if (VaOsdImage.width < x || VaOsdImage.height < y)
	return;

    copywidth = width;
    copyheight = height;
    if (VaOsdImage.width < width + x)
	copywidth = VaOsdImage.width - x;
    if (VaOsdImage.height < height + y)
	copyheight = VaOsdImage.height - y;

#ifdef DEBUG
    start = GetMsTicks();
#endif
    // map osd surface/image into memory.
    if (vaMapBuffer(VaDisplay, VaOsdImage.buf, &image_buffer) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't map osd image buffer");
	return;
    }
    // FIXME: convert image from ARGB to subpicture format, if not argb

    // copy argb to image
    for (o = 0; o < copyheight; ++o) {
	memcpy(image_buffer + (x + (y + o) * VaOsdImage.width) * 4, argb + xi * 4 + (o + yi) * pitch, copywidth * 4);
    }

    if (vaUnmapBuffer(VaDisplay, VaOsdImage.buf) != VA_STATUS_SUCCESS) {
	Error("video/vaapi: can't unmap osd image buffer");
    }
#ifdef DEBUG
    end = GetMsTicks();

    Debug7("video/vaapi: osd upload %dx%d%+d%+d %dms %d", width, height, x, y, end - start, width * height * 4);
#endif
}

///
/// VA-API module.
///
static const VideoModule VaapiModule = {
    .Name = "va-api",
    .Enabled = 1,
    .NewHwDecoder = (VideoHwDecoder * (*const)(VideoStream *)) VaapiNewHwDecoder,
    .DelHwDecoder = (void (*const) (VideoHwDecoder *))VaapiDelHwDecoder,
    .GetSurface = (unsigned (*const) (VideoHwDecoder *,
	    const AVCodecContext *))VaapiGetSurface,
    .ReleaseSurface = (void (*const) (VideoHwDecoder *, unsigned))VaapiReleaseSurface,
    .get_format = (enum AVPixelFormat(*const) (VideoHwDecoder *,
	    AVCodecContext *, const enum AVPixelFormat *))Vaapi_get_format,
    .RenderFrame = (void (*const) (VideoHwDecoder *,
	    const AVCodecContext *, const AVFrame *))VaapiSyncRenderFrame,
    .SetClock = (void (*const) (VideoHwDecoder *, int64_t))VaapiSetClock,
    .GetClock = (int64_t(*const) (const VideoHwDecoder *))VaapiGetClock,
    .SetClosing = (void (*const) (const VideoHwDecoder *))VaapiSetClosing,
    .ResetStart = (void (*const) (const VideoHwDecoder *))VaapiResetStart,
    .SetTrickSpeed = (void (*const) (const VideoHwDecoder *, int))VaapiSetTrickSpeed,
    .GrabOutput = VaapiGrabOutputSurface,
    .GetStats = (void (*const) (VideoHwDecoder *, int *, int *, int *,
	    int *))VaapiGetStats,
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

//----------------------------------------------------------------------------
//  NOOP
//----------------------------------------------------------------------------

///
/// Allocate new noop decoder.
///
/// @param stream   video stream
///
/// @returns always NULL.
///
static VideoHwDecoder *NoopNewHwDecoder( __attribute__ ((unused)) VideoStream * stream)
{
    return NULL;
}

///
/// Release a surface.
///
/// Can be called while exit.
///
/// @param decoder  noop hw decoder
/// @param surface  surface no longer used
///
static void NoopReleaseSurface( __attribute__ ((unused)) VideoHwDecoder * decoder, __attribute__ ((unused))
    unsigned surface)
{
}

///
/// Set noop background color.
///
/// @param rgba 32 bit RGBA color.
///
static void NoopSetBackground( __attribute__ ((unused)) uint32_t rgba)
{
}

///
/// Noop initialize OSD.
///
/// @param width    osd width
/// @param height   osd height
///
static void NoopOsdInit( __attribute__ ((unused))
    int width, __attribute__ ((unused))
    int height)
{
}

///
/// Draw OSD ARGB image.
///
/// @param xi	x-coordinate in argb image
/// @param yi	y-coordinate in argb image
/// @paran height   height in pixel in argb image
/// @paran width    width in pixel in argb image
/// @param pitch    pitch of argb image
/// @param argb 32bit ARGB image data
/// @param x	x-coordinate on screen of argb image
/// @param y	y-coordinate on screen of argb image
///
/// @note looked by caller
///
static void NoopOsdDrawARGB( __attribute__ ((unused))
    int xi, __attribute__ ((unused))
    int yi, __attribute__ ((unused))
    int width, __attribute__ ((unused))
    int height, __attribute__ ((unused))
    int pitch, __attribute__ ((unused))
    const uint8_t * argb, __attribute__ ((unused))
    int x, __attribute__ ((unused))
    int y)
{
}

///
/// Noop setup.
///
/// @param display_name x11/xcb display name
///
/// @returns always true.
///
static int NoopInit(const char *display_name)
{
    Info("video/noop: noop driver running on display '%s'", display_name);
    return 1;
}

///
/// Handle a noop display.
///
static void NoopDisplayHandlerThread(void)
{
    // avoid 100% cpu use
    usleep(20 * 1000);
}

///
/// Noop void function.
///
static void NoopVoid(void)
{
}

///
/// Noop video module.
///
static const VideoModule NoopModule = {
    .Name = "noop",
    .Enabled = 1,
    .NewHwDecoder = NoopNewHwDecoder,
    .ReleaseSurface = NoopReleaseSurface,
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
//  OSD
//----------------------------------------------------------------------------

///
/// Clear the OSD.
///
/// @todo I use glTexImage2D to clear the texture, are there faster and
/// better ways to clear a texture?
///
void VideoOsdClear(void)
{
    VideoThreadLock();
    VideoUsedModule->OsdClear();

    OsdDirtyX = VideoWindowWidth;	// reset dirty area
    OsdDirtyY = VideoWindowHeight;
    OsdDirtyWidth = 0;
    OsdDirtyHeight = 0;
    OsdShown = 0;

    VideoThreadUnlock();
}

///
/// Draw an OSD ARGB image.
///
/// @param xi	x-coordinate in argb image
/// @param yi	y-coordinate in argb image
/// @paran height   height in pixel in argb image
/// @paran width    width in pixel in argb image
/// @param pitch    pitch of argb image
/// @param argb 32bit ARGB image data
/// @param x	x-coordinate on screen of argb image
/// @param y	y-coordinate on screen of argb image
///
void VideoOsdDrawARGB(int xi, int yi, int width, int height, int pitch, const uint8_t * argb, int x, int y)
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
    Debug8("video: osd dirty %dx%d%+d%+d -> %dx%d%+d%+d", width, height, x, y, OsdDirtyWidth, OsdDirtyHeight,
	OsdDirtyX, OsdDirtyY);

    VideoUsedModule->OsdDrawARGB(xi, yi, width, height, pitch, argb, x, y);
    OsdShown = 1;

    VideoThreadUnlock();
}

///
/// Get OSD size.
///
/// @param[out] width	OSD width
/// @param[out] height	OSD height
///
void VideoGetOsdSize(int *width, int *height)
{
    if (VideoWindowWidth <= 0 || VideoWindowHeight <= 0) {
	Debug7("video: %s: osd/window size not set yet - using default", __FUNCTION__);
	*width = 1920;
	*height = 1080;
    } else {
	*width = VideoWindowWidth;
	*height = VideoWindowHeight;
    }
}

///
/// Setup osd.
///
/// FIXME: looking for BGRA, but this fourcc isn't supported by the
/// drawing functions yet.
///
void VideoOsdInit(void)
{
    VideoThreadLock();
    VideoUsedModule->OsdInit(VideoWindowWidth, VideoWindowHeight);
    VideoThreadUnlock();
    VideoOsdClear();
}

///
/// Cleanup OSD.
///
void VideoOsdExit(void)
{
    VideoThreadLock();
    VideoUsedModule->OsdExit();
    VideoThreadUnlock();
    OsdDirtyWidth = 0;
    OsdDirtyHeight = 0;
}

//----------------------------------------------------------------------------
//  Events
//----------------------------------------------------------------------------

/// C callback feed key press
extern void FeedKeyPress(const char *, const char *, int, int, const char *);

///
/// Handle XLib I/O Errors.
///
/// @param display  display with i/o error
///
static int VideoIOErrorHandler( __attribute__ ((unused)) Display * display)
{

    Error("video: fatal i/o error");
    // should be called from VideoThread
    if (VideoThread && VideoThread == pthread_self()) {
	Debug7("video: called from video thread");
	VideoUsedModule = &NoopModule;
	XlibDisplay = NULL;
	VideoWindow = XCB_NONE;
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_cond_destroy(&VideoWakeupCond);
	pthread_mutex_destroy(&VideoLockMutex);
	pthread_mutex_destroy(&VideoMutex);
	VideoThread = 0;
	pthread_exit("video thread exit");
    }
    do {
	sleep(1000);
    } while (1);			// let other threads running

    return -1;
}

///
/// Handle X11 events.
///
/// @todo   Signal WmDeleteMessage to application.
///
static void VideoEvent(void)
{
    XEvent event;
    KeySym keysym;
    const char *keynam;
    char buf[64];
    char letter[64];
    int letter_len;
    uint32_t values[1];

    VideoThreadLock();
    XNextEvent(XlibDisplay, &event);
    VideoThreadUnlock();
    switch (event.type) {
	case ClientMessage:
	    Debug7("video/event: ClientMessage");
	    if (event.xclient.data.l[0] == (long)WmDeleteWindowAtom) {
		Debug7("video/event: wm-delete-message");
		FeedKeyPress("XKeySym", "Close", 0, 0, NULL);
	    }
	    break;

	case MapNotify:
	    Debug7("video/event: MapNotify");
	    // uwm workaround
	    VideoThreadLock();
	    xcb_change_window_attributes(Connection, VideoWindow, XCB_CW_CURSOR, &VideoBlankCursor);
	    VideoThreadUnlock();
	    VideoBlankTick = 0;
	    break;
	case Expose:
	    //Debug7("video/event: Expose");
	    break;
	case ReparentNotify:
	    Debug7("video/event: ReparentNotify");
	    break;
	case ConfigureNotify:
	    //Debug7("video/event: ConfigureNotify");
	    VideoSetVideoMode(event.xconfigure.x, event.xconfigure.y, event.xconfigure.width, event.xconfigure.height);
	    break;
	case ButtonPress:
	    VideoSetFullscreen(-1);
	    break;
	case KeyPress:
	    VideoThreadLock();
	    letter_len = XLookupString(&event.xkey, letter, sizeof(letter) - 1, &keysym, NULL);
	    VideoThreadUnlock();
	    if (letter_len < 0) {
		letter_len = 0;
	    }
	    letter[letter_len] = '\0';
	    if (keysym == NoSymbol) {
		Error("video/event: No symbol for %d", event.xkey.keycode);
		break;
	    }
	    VideoThreadLock();
	    keynam = XKeysymToString(keysym);
	    VideoThreadUnlock();
	    // check for key modifiers (Alt/Ctrl)
	    if (event.xkey.state & (Mod1Mask | ControlMask)) {
		if (event.xkey.state & Mod1Mask) {
		    strcpy(buf, "Alt+");
		} else {
		    buf[0] = '\0';
		}
		if (event.xkey.state & ControlMask) {
		    strcat(buf, "Ctrl+");
		}
		strncat(buf, keynam, sizeof(buf) - 10);
		keynam = buf;
	    }
	    FeedKeyPress("XKeySym", keynam, 0, 0, letter);
	    break;
	case KeyRelease:
	    break;
	case MotionNotify:
	    values[0] = XCB_NONE;
	    VideoThreadLock();
	    xcb_change_window_attributes(Connection, VideoWindow, XCB_CW_CURSOR, values);
	    VideoThreadUnlock();
	    VideoBlankTick = GetMsTicks();
	    break;
	default:
	    Debug7("Unsupported event type %d", event.type);
	    break;
    }
}

///
/// Poll all x11 events.
///
void VideoPollEvent(void)
{
    // hide cursor, after xx ms
    if (VideoBlankTick && VideoWindow != XCB_NONE && VideoBlankTick + 200 < GetMsTicks()) {
	VideoThreadLock();
	xcb_change_window_attributes(Connection, VideoWindow, XCB_CW_CURSOR, &VideoBlankCursor);
	VideoThreadUnlock();
	VideoBlankTick = 0;
    }
    while (XlibDisplay) {
	VideoThreadLock();
	if (!XPending(XlibDisplay)) {
	    VideoThreadUnlock();
	    break;
	}
	VideoThreadUnlock();
	VideoEvent();
    }
}

//----------------------------------------------------------------------------
//  Thread
//----------------------------------------------------------------------------

///
/// Lock video thread.
///
static void VideoThreadLock(void)
{
    if (VideoThread) {
	if (pthread_mutex_lock(&VideoLockMutex)) {
	    Error("video: can't lock thread");
	}
    }
}

///
/// Unlock video thread.
///
static void VideoThreadUnlock(void)
{
    if (VideoThread) {
	if (pthread_mutex_unlock(&VideoLockMutex)) {
	    Error("video: can't unlock thread");
	}
    }
}

///
/// Video render thread.
///
static void *VideoDisplayHandlerThread(void *dummy)
{
    Debug7("video: display thread started");

    for (;;) {
	// fix dead-lock
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_testcancel();
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	VideoPollEvent();

	VideoUsedModule->DisplayHandlerThread();
    }

    return dummy;
}

///
/// Initialize video threads.
///
static void VideoThreadInit(void)
{
    pthread_mutex_init(&VideoMutex, NULL);
    pthread_mutex_init(&VideoLockMutex, NULL);
    pthread_cond_init(&VideoWakeupCond, NULL);
    pthread_create(&VideoThread, NULL, VideoDisplayHandlerThread, NULL);
    pthread_setname_np(VideoThread, "vaapidevice video");
}

///
/// Exit and cleanup video threads.
///
static void VideoThreadExit(void)
{
    if (VideoThread) {
	void *retval;

	Debug7("video: video thread canceled");
	//VideoThreadLock();
	// FIXME: can't cancel locked
	if (pthread_cancel(VideoThread)) {
	    Error("video: can't queue cancel video display thread");
	}
	//VideoThreadUnlock();
	if (pthread_join(VideoThread, &retval) || retval != PTHREAD_CANCELED) {
	    Error("video: can't cancel video display thread");
	}
	VideoThread = 0;
	pthread_cond_destroy(&VideoWakeupCond);
	pthread_mutex_destroy(&VideoLockMutex);
	pthread_mutex_destroy(&VideoMutex);
    }
}

///
/// Video display wakeup.
///
/// New video arrived, wakeup video thread.
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

//----------------------------------------------------------------------------
//  Video API
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

///
/// Table of all video modules.
///
static const VideoModule *VideoModules[] = {
    &VaapiModule,
    &NoopModule
};

///
/// Video hardware decoder
///
struct _video_hw_decoder_
{
    union
    {
	VaapiDecoder Vaapi;		///< VA-API decoder structure
    };
};

///
/// Allocate new video hw decoder.
///
/// @param stream   video stream
///
/// @returns a new initialized video hardware decoder.
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
/// Destroy a video hw decoder.
///
/// @param hw_decoder	video hardware decoder
///
void VideoDelHwDecoder(VideoHwDecoder * hw_decoder)
{
    if (hw_decoder) {
#ifdef DEBUG
	if (!pthread_equal(pthread_self(), VideoThread)) {
	    Debug7("video: should only be called from inside the thread");
	}
#endif
	// only called from inside the thread
	//VideoThreadLock();
	VideoUsedModule->DelHwDecoder(hw_decoder);
	//VideoThreadUnlock();
    }
}

///
/// Get a free hardware decoder surface.
///
/// @param hw_decoder	video hardware decoder
/// @param video_ctx	ffmpeg video codec context
///
/// @returns the oldest free surface or invalid surface
///
unsigned VideoGetSurface(VideoHwDecoder * hw_decoder, const AVCodecContext * video_ctx)
{
    return VideoUsedModule->GetSurface(hw_decoder, video_ctx);
}

///
/// Release a hardware decoder surface.
///
/// @param hw_decoder	video hardware decoder
/// @param surface  surface no longer used
///
void VideoReleaseSurface(VideoHwDecoder * hw_decoder, unsigned surface)
{
    // FIXME: must be guarded against calls, after VideoExit
    VideoUsedModule->ReleaseSurface(hw_decoder, surface);
}

///
/// Callback to negotiate the PixelFormat.
///
/// @param hw_decoder	video hardware decoder
/// @param video_ctx	ffmpeg video codec context
/// @param fmt	    is the list of formats which are supported by
/// the codec, it is terminated by -1 as 0 is a
/// valid format, the formats are ordered by
/// quality.
///
enum AVPixelFormat Video_get_format(VideoHwDecoder * hw_decoder, AVCodecContext * video_ctx,
    const enum AVPixelFormat *fmt)
{
#ifdef DEBUG
    int ms_delay;

    // FIXME: use frame time
    ms_delay = (1000 * video_ctx->time_base.num * video_ctx->ticks_per_frame)
	/ video_ctx->time_base.den;

    Debug7("video: ready %s %2dms/frame %dms", Timestamp2String(VideoGetClock(hw_decoder)), ms_delay,
	GetMsTicks() - VideoSwitch);
#endif

    return VideoUsedModule->get_format(hw_decoder, video_ctx, fmt);
}

///
/// Display a ffmpeg frame
///
/// @param hw_decoder	video hardware decoder
/// @param video_ctx	ffmpeg video codec context
/// @param frame    frame to display
///
void VideoRenderFrame(VideoHwDecoder * hw_decoder, const AVCodecContext * video_ctx, const AVFrame * frame)
{
    if (frame->repeat_pict && !VideoIgnoreRepeatPict) {
	Error("video: repeated pict %d found, but not handled", frame->repeat_pict);
    }
    VideoUsedModule->RenderFrame(hw_decoder, video_ctx, frame);
}

///
/// Set video clock.
///
/// @param hw_decoder	video hardware decoder
/// @param pts	    audio presentation timestamp
///
void VideoSetClock(VideoHwDecoder * hw_decoder, int64_t pts)
{
    Debug7("video: set clock %s", Timestamp2String(pts));
    if (hw_decoder) {
	VideoUsedModule->SetClock(hw_decoder, pts);
    }
}

///
/// Get video clock.
///
/// @param hw_decoder	video hardware decoder
///
/// @note this isn't monoton, decoding reorders frames, setter keeps it
/// monotonic
///
int64_t VideoGetClock(const VideoHwDecoder * hw_decoder)
{
    if (hw_decoder) {
	return VideoUsedModule->GetClock(hw_decoder);
    }
    return AV_NOPTS_VALUE;
}

///
/// Set closing stream flag.
///
/// @param hw_decoder	video hardware decoder
///
void VideoSetClosing(VideoHwDecoder * hw_decoder)
{
    Debug7("video: set closing");
    VideoUsedModule->SetClosing(hw_decoder);
    // clear clock to avoid further sync
    VideoSetClock(hw_decoder, AV_NOPTS_VALUE);
}

///
/// Reset start of frame counter.
///
/// @param hw_decoder	video hardware decoder
///
void VideoResetStart(VideoHwDecoder * hw_decoder)
{
    Debug7("video: reset start");
    VideoUsedModule->ResetStart(hw_decoder);
    // clear clock to trigger new video stream
    VideoSetClock(hw_decoder, AV_NOPTS_VALUE);
}

///
/// Set trick play speed.
///
/// @param hw_decoder	video hardware decoder
/// @param speed    trick speed (0 = normal)
///
void VideoSetTrickSpeed(VideoHwDecoder * hw_decoder, int speed)
{
    Debug7("video: set trick-speed %d", speed);
    VideoUsedModule->SetTrickSpeed(hw_decoder, speed);
}

///
/// Grab full screen image.
///
/// @param size[out]	size of allocated image
/// @param width[in,out]    width of image
/// @param height[in,out]   height of image
///
uint8_t *VideoGrab(int *size, int *width, int *height, int write_header)
{
    Debug7("video: grab");

    if (VideoUsedModule->GrabOutput) {
	char buf[64];
	uint8_t *rgb;
	int scale_width = *width;
	int scale_height = *height;
	int n = 0;
	uint8_t *data = VideoUsedModule->GrabOutput(size, width, height);

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
	    double src_y;
	    double scale_x;
	    double scale_y;

	    if (write_header) {
		n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255", scale_width, scale_height);
	    }
	    rgb = malloc(scale_width * scale_height * 3 + n);
	    if (!rgb) {
		Error("video: out of memory");
		free(data);
		return NULL;
	    }
	    *size = scale_width * scale_height * 3 + n;
	    memcpy(rgb, buf, n);	// header

	    scale_x = (double)*width / scale_width;
	    scale_y = (double)*height / scale_height;

	    src_y = 0.0;
	    for (int y = 0; y < scale_height; y++) {
		int o;
		double src_x = 0.0;

		o = (int)src_y **width;

		for (int x = 0; x < scale_width; x++) {
		    int i = 4 * (o + (int)src_x);

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
		n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255", *width, *height);
	    }
	    rgb = malloc(*width * *height * 3 + n);
	    if (!rgb) {
		Error("video: out of memory");
		free(data);
		return NULL;
	    }
	    memcpy(rgb, buf, n);	// header

	    for (int i = 0; i < *size / 4; ++i) {   // convert bgra -> rgb
		rgb[n + i * 3 + 0] = data[i * 4 + 2];
		rgb[n + i * 3 + 1] = data[i * 4 + 1];
		rgb[n + i * 3 + 2] = data[i * 4 + 0];
	    }

	    *size = *width * *height * 3 + n;
	}
	free(data);

	return rgb;
    } else {
	Error("vaapidevice: grab unsupported");
    }

    return NULL;
}

///
/// Get decoder statistics.
///
/// @param hw_decoder	video hardware decoder
/// @param[out] missed	missed frames
/// @param[out] duped	duped frames
/// @param[out] dropped dropped frames
/// @param[out] count	number of decoded frames
///
void VideoGetStats(VideoHwDecoder * hw_decoder, int *missed, int *duped, int *dropped, int *counter)
{
    VideoUsedModule->GetStats(hw_decoder, missed, duped, dropped, counter);
}

///
/// Get decoder video stream size.
///
/// @param hw_decoder	video hardware decoder
/// @param[out] width	video stream width
/// @param[out] height	video stream height
/// @param[out] aspect_num  video stream aspect numerator
/// @param[out] aspect_den  video stream aspect denominator
///
void VideoGetVideoSize(VideoHwDecoder * hw_decoder, int *width, int *height, int *aspect_num, int *aspect_den)
{
    *width = 1920;
    *height = 1080;
    *aspect_num = 16;
    *aspect_den = 9;
    // FIXME: test to check if working, than make module function
    if (VideoUsedModule == &VaapiModule) {
	*width = hw_decoder->Vaapi.InputWidth;
	*height = hw_decoder->Vaapi.InputHeight;
	av_reduce(aspect_num, aspect_den, hw_decoder->Vaapi.InputWidth * hw_decoder->Vaapi.InputAspect.num,
	    hw_decoder->Vaapi.InputHeight * hw_decoder->Vaapi.InputAspect.den, 1024 * 1024);
    }
}

//----------------------------------------------------------------------------
//  DPMS / Screensaver
//----------------------------------------------------------------------------

///
/// Suspend X11 screen saver.
///
/// @param connection	X11 connection to enable/disable screensaver
/// @param suspend  True suspend screensaver, false enable screensaver
///
static void X11SuspendScreenSaver(xcb_connection_t * connection, int suspend)
{
    const xcb_query_extension_reply_t *query_extension_reply;

    query_extension_reply = xcb_get_extension_data(connection, &xcb_screensaver_id);
    if (query_extension_reply && query_extension_reply->present) {
	xcb_screensaver_query_version_cookie_t cookie;
	xcb_screensaver_query_version_reply_t *reply;

	Debug7("video: screen saver extension present");

	cookie =
	    xcb_screensaver_query_version_unchecked(connection, XCB_SCREENSAVER_MAJOR_VERSION,
	    XCB_SCREENSAVER_MINOR_VERSION);
	reply = xcb_screensaver_query_version_reply(connection, cookie, NULL);
	if (reply && (reply->server_major_version >= XCB_SCREENSAVER_MAJOR_VERSION)
	    && (reply->server_minor_version >= XCB_SCREENSAVER_MINOR_VERSION)) {
	    xcb_screensaver_suspend(connection, suspend);
	}
	free(reply);
    }
}

///
/// DPMS (Display Power Management Signaling) extension available.
///
/// @param connection	X11 connection to check for DPMS
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

	Debug7("video: dpms extension present");

	cookie = xcb_dpms_get_version_unchecked(connection, XCB_DPMS_MAJOR_VERSION, XCB_DPMS_MINOR_VERSION);
	reply = xcb_dpms_get_version_reply(connection, cookie, NULL);
	// use locals to avoid gcc warning
	major = XCB_DPMS_MAJOR_VERSION;
	minor = XCB_DPMS_MINOR_VERSION;
	if (reply && (reply->server_major_version >= major) && (reply->server_minor_version >= minor)) {
	    have_dpms = 1;
	}
	free(reply);
    }
    return have_dpms;
}

///
/// Disable DPMS (Display Power Management Signaling)
///
/// @param connection	X11 connection to disable DPMS
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
		Debug7("video: dpms was enabled");
		xcb_dpms_disable(connection);	// monitor powersave off
	    }
	    free(reply);
	}
	DPMSDisabled = 1;
    }
}

///
/// Re-enable DPMS (Display Power Management Signaling)
///
/// @param connection	X11 connection to enable DPMS
///
static void X11DPMSReenable(xcb_connection_t * connection)
{
    if (DPMSDisabled && X11HaveDPMS(connection)) {
	xcb_dpms_enable(connection);	// monitor powersave on
	xcb_dpms_force_level(connection, XCB_DPMS_DPMS_MODE_ON);
	DPMSDisabled = 0;
    }
}

//----------------------------------------------------------------------------
//  Setup
//----------------------------------------------------------------------------

///
/// Create main window.
///
/// @param parent   parent of new window
/// @param visual   visual of parent
/// @param depth    depth of parent
///
static void VideoCreateWindow(xcb_window_t parent, xcb_visualid_t visual, uint8_t depth)
{
    uint32_t values[4];
    xcb_intern_atom_reply_t *reply;
    xcb_pixmap_t pixmap;
    xcb_cursor_t cursor;

    Debug7("video: visual %#0x depth %d", visual, depth);

    // Color map
    VideoColormap = xcb_generate_id(Connection);
    xcb_create_colormap(Connection, XCB_COLORMAP_ALLOC_NONE, VideoColormap, parent, visual);

    values[0] = 0;
    values[1] = 0;
    values[2] =
	XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS |
	XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_EXPOSURE |
	XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    values[3] = VideoColormap;
    VideoWindow = xcb_generate_id(Connection);
    xcb_create_window(Connection, depth, VideoWindow, parent, VideoWindowX, VideoWindowY, VideoWindowWidth,
	VideoWindowHeight, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, visual,
	XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP, values);

    // FIXME: utf _NET_WM_NAME
    xcb_icccm_set_wm_name(Connection, VideoWindow, XCB_ATOM_STRING, 8, sizeof("vaapidevice") - 1, "vaapidevice");
    xcb_icccm_set_wm_icon_name(Connection, VideoWindow, XCB_ATOM_STRING, 8, sizeof("vaapidevice") - 1, "vaapidevice");

    // FIXME: size hints

    // register interest in the delete window message
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0, sizeof("WM_DELETE_WINDOW") - 1,
		    "WM_DELETE_WINDOW"), NULL))) {
	WmDeleteWindowAtom = reply->atom;
	free(reply);
	if ((reply =
		xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0, sizeof("WM_PROTOCOLS") - 1,
			"WM_PROTOCOLS"), NULL))) {
	    xcb_icccm_set_wm_protocols(Connection, VideoWindow, reply->atom, 1, &WmDeleteWindowAtom);
	    free(reply);
	}
    }
    //
    //	prepare fullscreen.
    //
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0, sizeof("_NET_WM_STATE") - 1,
		    "_NET_WM_STATE"), NULL))) {
	NetWmState = reply->atom;
	free(reply);
    }
    if ((reply =
	    xcb_intern_atom_reply(Connection, xcb_intern_atom(Connection, 0, sizeof("_NET_WM_STATE_FULLSCREEN") - 1,
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
    xcb_create_cursor(Connection, cursor, pixmap, pixmap, 0, 0, 0, 0, 0, 0, 1, 1);

    values[0] = cursor;
    xcb_change_window_attributes(Connection, VideoWindow, XCB_CW_CURSOR, values);
    VideoCursorPixmap = pixmap;
    VideoBlankCursor = cursor;
    VideoBlankTick = 0;
}

///
/// Set video device.
///
/// Currently this only choose the driver.
///
void VideoSetDevice(const char *device)
{
    VideoDriverName = device;
}

///
/// Set video geometry.
///
/// @param geometry  [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]
///
int VideoSetGeometry(const char *geometry)
{
    XParseGeometry(geometry, &VideoWindowX, &VideoWindowY, &VideoWindowWidth, &VideoWindowHeight);

    return 0;
}

///
/// Set 60hz display mode.
///
/// Pull up 50 Hz video for 60 Hz display.
///
/// @param onoff    enable / disable the 60 Hz mode.
///
void VideoSet60HzMode(int onoff)
{
    Video60HzMode = onoff;
}

///
/// Set soft start audio/video sync.
///
/// @param onoff    enable / disable the soft start sync.
///
void VideoSetSoftStartSync(int onoff)
{
    VideoSoftStartSync = onoff;
}

///
/// Vaapi helper to set various video params (brightness, contrast etc.)
///
/// @param buf	Pointer to value to set
/// @param Index    which part of the buffer to touch
/// @param value    new value to set
/// @return status whether successful
///
static VAStatus VaapiVideoSetColorbalance(VABufferID * buf, int Index, float value)
{
    VAStatus va_status;
    VAProcFilterParameterBufferColorBalance *cbal_param;

    if (!buf || Index < 0)
	return VA_STATUS_ERROR_INVALID_PARAMETER;

    va_status = vaMapBuffer(VaDisplay, *buf, (void **)&cbal_param);
    if (va_status != VA_STATUS_SUCCESS)
	return va_status;

    /* Assuming here that the type is set before and does not need to be modified */
    cbal_param[Index].value = value;

    vaUnmapBuffer(VaDisplay, *buf);

    return va_status;
}

///
/// Set brightness adjustment.
///
/// @param brightness	between min and max.
///
void VideoSetBrightness(int brightness)
{
    // FIXME: test to check if working, than make module function
    if (VideoUsedModule == &VaapiModule && VaapiDecoders[0]->vpp_brightness_idx >= 0) {
	VaapiVideoSetColorbalance(VaapiDecoders[0]->vpp_cbal_buf, VaapiDecoders[0]->vpp_brightness_idx,
	    VideoConfigClamp(&VaapiConfigBrightness, brightness) * VaapiConfigBrightness.scale);
    }
}

///
/// Get brightness configurations.
///
int VideoGetBrightnessConfig(int *minvalue, int *defvalue, int *maxvalue)
{
    if (VideoUsedModule == &VaapiModule) {
	*minvalue = VaapiConfigBrightness.min_value;
	*defvalue = VaapiConfigBrightness.def_value;
	*maxvalue = VaapiConfigBrightness.max_value;
	return VaapiConfigBrightness.active;
    }
    return 0;
}

///
/// Set contrast adjustment.
///
/// @param contrast between min and max.
///
void VideoSetContrast(int contrast)
{
    // FIXME: test to check if working, than make module function
    if (VideoUsedModule == &VaapiModule && VaapiDecoders[0]->vpp_contrast_idx >= 0) {
	VaapiVideoSetColorbalance(VaapiDecoders[0]->vpp_cbal_buf, VaapiDecoders[0]->vpp_contrast_idx,
	    VideoConfigClamp(&VaapiConfigContrast, contrast) * VaapiConfigContrast.scale);
    }
}

///
/// Get contrast configurations.
///
int VideoGetContrastConfig(int *minvalue, int *defvalue, int *maxvalue)
{
    if (VideoUsedModule == &VaapiModule) {
	*minvalue = VaapiConfigContrast.min_value;
	*defvalue = VaapiConfigContrast.def_value;
	*maxvalue = VaapiConfigContrast.max_value;
	return VaapiConfigContrast.active;
    }
    return 0;
}

///
/// Set saturation adjustment.
///
/// @param saturation	between min and max.
///
void VideoSetSaturation(int saturation)
{
    // FIXME: test to check if working, than make module function
    if (VideoUsedModule == &VaapiModule && VaapiDecoders[0]->vpp_saturation_idx >= 0) {
	VaapiVideoSetColorbalance(VaapiDecoders[0]->vpp_cbal_buf, VaapiDecoders[0]->vpp_saturation_idx,
	    VideoConfigClamp(&VaapiConfigSaturation, saturation) * VaapiConfigSaturation.scale);
    }
}

///
/// Get saturation configurations.
///
int VideoGetSaturationConfig(int *minvalue, int *defvalue, int *maxvalue)
{
    if (VideoUsedModule == &VaapiModule) {
	*minvalue = VaapiConfigSaturation.min_value;
	*defvalue = VaapiConfigSaturation.def_value;
	*maxvalue = VaapiConfigSaturation.max_value;
	return VaapiConfigSaturation.active;
    }
    return 0;
}

///
/// Set hue adjustment.
///
/// @param hue	between min and max.
///
void VideoSetHue(int hue)
{
    // FIXME: test to check if working, than make module function
    if (VideoUsedModule == &VaapiModule && VaapiDecoders[0]->vpp_hue_idx >= 0) {
	VaapiVideoSetColorbalance(VaapiDecoders[0]->vpp_cbal_buf, VaapiDecoders[0]->vpp_hue_idx,
	    VideoConfigClamp(&VaapiConfigHue, hue) * VaapiConfigHue.scale);
    }
}

///
/// Get hue configurations.
///
int VideoGetHueConfig(int *minvalue, int *defvalue, int *maxvalue)
{
    if (VideoUsedModule == &VaapiModule) {
	*minvalue = VaapiConfigHue.min_value;
	*defvalue = VaapiConfigHue.def_value;
	*maxvalue = VaapiConfigHue.max_value;
	return VaapiConfigHue.active;
    }
    return 0;
}

///
/// Set skin tone enhancement.
///
/// @param stde	   between min and max.
///
void VideoSetSkinToneEnhancement(int stde)
{
    // FIXME: test to check if working, than make module function
    if (VideoUsedModule == &VaapiModule) {
	VideoSkinToneEnhancement = VideoConfigClamp(&VaapiConfigStde, stde);
    }
    VideoSurfaceModesChanged = 1;
}

///
/// Get skin tone enhancement configurations.
///
int VideoGetSkinToneEnhancementConfig(int *minvalue, int *defvalue, int *maxvalue)
{
    if (VideoUsedModule == &VaapiModule) {
	*minvalue = VaapiConfigStde.min_value;
	*defvalue = VaapiConfigStde.def_value;
	*maxvalue = VaapiConfigStde.max_value;
	return VaapiConfigStde.active;
    }
    return 0;
}

///
/// Set video output position.
///
/// @param hw_decoder	video hardware decoder
/// @param x	    video output x coordinate OSD relative
/// @param y	    video output y coordinate OSD relative
/// @param width    video output width
/// @param height   video output height
///
void VideoSetOutputPosition(VideoHwDecoder * hw_decoder, int x, int y, int width, int height)
{
    if (!width || !height) {
	// restore full size
	x = 0;
	y = 0;
	width = VideoWindowWidth;
	height = VideoWindowHeight;
    }
    // FIXME: add function to module class
    if (VideoUsedModule == &VaapiModule) {
	// check values to be able to avoid
	// interfering with the video thread if possible

	if (x == hw_decoder->Vaapi.VideoX && y == hw_decoder->Vaapi.VideoY && width == hw_decoder->Vaapi.VideoWidth
	    && height == hw_decoder->Vaapi.VideoHeight) {
	    // not necessary...
	    return;
	}
	VideoThreadLock();
	VaapiSetOutputPosition(&hw_decoder->Vaapi, x, y, width, height);
	VaapiUpdateOutput(&hw_decoder->Vaapi);
	VideoThreadUnlock();
    }
}

///
/// Set video window position.
///
/// @param x	window x coordinate
/// @param y	window y coordinate
/// @param width    window width
/// @param height   window height
///
/// @note no need to lock, only called from inside the video thread
///
void VideoSetVideoMode( __attribute__ ((unused))
    int x, __attribute__ ((unused))
    int y, int width, int height)
{
    Debug8("video: %s %dx%d%+d%+d", __FUNCTION__, width, height, x, y);

    if ((unsigned)width == VideoWindowWidth && (unsigned)height == VideoWindowHeight) {
	return;				// same size nothing todo
    }
    if (width <= 0 || height <= 0) {
	return;				// ignore zero or negative sizes
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
/// Set 4:3 video display format.
///
/// @param format   video format (stretch, normal, center cut-out)
///
void VideoSet4to3DisplayFormat(int format)
{
    // convert api to internal format
    switch (format) {
	case -1:		       // rotate settings
	    format = (Video4to3ZoomMode + 1) % (VideoCenterCutOut + 1);
	    break;
	case 0:			       // pan&scan (we have no pan&scan)
	    format = VideoStretch;
	    break;
	case 1:			       // letter box
	    format = VideoNormal;
	    break;
	case 2:			       // center cut-out
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
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();

    VideoOsdInit();
}

///
/// Set other video display format.
///
/// @param format   video format (stretch, normal, center cut-out)
///
void VideoSetOtherDisplayFormat(int format)
{
    // convert api to internal format
    switch (format) {
	case -1:		       // rotate settings
	    format = (VideoOtherZoomMode + 1) % (VideoCenterCutOut + 1);
	    break;
	case 0:			       // pan&scan (we have no pan&scan)
	    format = VideoStretch;
	    break;
	case 1:			       // letter box
	    format = VideoNormal;
	    break;
	case 2:			       // center cut-out
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
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();

    VideoOsdInit();
}

///
/// Send fullscreen message to window.
///
/// @param onoff    -1 toggle, true turn on, false turn off
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

	xcb_send_event(Connection, XCB_SEND_EVENT_DEST_POINTER_WINDOW, DefaultRootWindow(XlibDisplay),
	    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (void *)&event);
	Debug7("video/x11: send fullscreen message %x %x", event.data.data32[0], event.data.data32[1]);
    }
}

///
/// Get scaling modes.
///
static const char *vaapi_scaling[] = {
    "Normal",				///< VideoScalingNormal
    "Fast",				///< VideoScalingFast
    "HighQuality"			///< VideoScalingHQ
};

static const char *vaapi_scaling_short[] = {
    "N",				///< VideoScalingNormal
    "F",				///< VideoScalingFast
    "HQ"				///< VideoScalingHQ
};

int VideoGetScalingModes(const char * **long_table, const char * **short_table)
{
    if (VideoUsedModule == &VaapiModule) {
	*long_table = vaapi_scaling;
	*short_table = vaapi_scaling_short;
	return ARRAY_ELEMS(vaapi_scaling);
    }
    return 0;
}

///
/// Get deinterlace modes.
///
static const char *vaapi_deinterlace_default[VAProcDeinterlacingCount] = {
    "None",				///< VAProcDeinterlacingNone
    "Bob",				///< VAProcDeinterlacingBob
    "Weave",				///< VAProcDeinterlacingWeave
    "Motion Adaptive",			///< VAProcDeinterlacingMotionAdaptive
    "Motion Compensated",		///< VAProcDeinterlacingMotionCompensated
};

static const char *vaapi_deinterlace_long[VAProcDeinterlacingCount];

static const char *vaapi_deinterlace_short[VAProcDeinterlacingCount] = {
    "N",				///< VAProcDeinterlacingNone
    "B",				///< VAProcDeinterlacingBob
    "W",				///< VAProcDeinterlacingWeave
    "MADI",				///< VAProcDeinterlacingMotionAdaptive
    "MCDI"				///< VAProcDeinterlacingMotionCompensated
};

int VideoGetDeinterlaceModes(const char * **long_table, const char * **short_table)
{
    if (VideoUsedModule == &VaapiModule) {
	// TODO: Supported deinterlacers may not be a linear table
	unsigned int len = ARRAY_ELEMS(vaapi_deinterlace_default);

	for (unsigned int i = 0; i < len; ++i) {
	    if (VaapiDecoders[0]->SupportedDeinterlacers[i])
		vaapi_deinterlace_long[i] = vaapi_deinterlace_default[i];
	    else
		vaapi_deinterlace_long[i] = "Not supported";
	}
	*long_table = vaapi_deinterlace_long;
	*short_table = vaapi_deinterlace_short;
	return len;
    }
    return 0;
}

///
/// Set deinterlace mode.
///
void VideoSetDeinterlace(int mode[VideoResolutionMax])
{
    if (VideoUsedModule == &VaapiModule) {
	for (int i = 0; i < VideoResolutionMax; ++i) {
	    if (!VaapiDecoders[0]->SupportedDeinterlacers[mode[i]])
		mode[i] = VAProcDeinterlacingNone;
	}
    }
    VideoDeinterlace[0] = mode[0];
    VideoDeinterlace[1] = mode[1];
    VideoDeinterlace[2] = mode[2];
    VideoDeinterlace[3] = mode[3];
    VideoDeinterlace[4] = mode[4];
    VideoSurfaceModesChanged = 1;
}

///
/// Set denoise level.
///
void VideoSetDenoise(int level[VideoResolutionMax])
{
    if (VideoUsedModule == &VaapiModule) {
	for (int i = 0; i < VideoResolutionMax; ++i) {
	    level[i] = VideoConfigClamp(&VaapiConfigDenoise, level[i]);
	}
    }
    VideoDenoise[0] = level[0];
    VideoDenoise[1] = level[1];
    VideoDenoise[2] = level[2];
    VideoDenoise[3] = level[3];
    VideoDenoise[4] = level[4];
    VideoSurfaceModesChanged = 1;
}

///
/// Get denoise configurations.
///
int VideoGetDenoiseConfig(int *minvalue, int *defvalue, int *maxvalue)
{
    if (VideoUsedModule == &VaapiModule) {
	*minvalue = VaapiConfigDenoise.min_value;
	*defvalue = VaapiConfigDenoise.def_value;
	*maxvalue = VaapiConfigDenoise.max_value;
	return VaapiConfigDenoise.active;
    }
    return 0;
}

///
/// Set sharpness level.
///
void VideoSetSharpen(int level[VideoResolutionMax])
{
    if (VideoUsedModule == &VaapiModule) {
	for (int i = 0; i < VideoResolutionMax; ++i) {
	    level[i] = VideoConfigClamp(&VaapiConfigSharpen, level[i]);
	}
    }
    VideoSharpen[0] = level[0];
    VideoSharpen[1] = level[1];
    VideoSharpen[2] = level[2];
    VideoSharpen[3] = level[3];
    VideoSharpen[4] = level[4];
    VideoSurfaceModesChanged = 1;
}

///
/// Get sharpness configurations.
///
int VideoGetSharpenConfig(int *minvalue, int *defvalue, int *maxvalue)
{
    if (VideoUsedModule == &VaapiModule) {
	*minvalue = VaapiConfigSharpen.min_value;
	*defvalue = VaapiConfigSharpen.def_value;
	*maxvalue = VaapiConfigSharpen.max_value;
	return VaapiConfigSharpen.active;
    }
    return 0;
}

///
/// Set scaling mode.
///
/// @param mode table with VideoResolutionMax values
///
void VideoSetScaling(int mode[VideoResolutionMax])
{
    VideoScaling[0] = mode[0];
    VideoScaling[1] = mode[1];
    VideoScaling[2] = mode[2];
    VideoScaling[3] = mode[3];
    VideoScaling[4] = mode[4];
    VideoSurfaceModesChanged = 1;
}

///
/// Set cut top and bottom.
///
/// @param pixels table with VideoResolutionMax values
///
void VideoSetCutTopBottom(int pixels[VideoResolutionMax])
{
    VideoThreadLock();
    VideoCutTopBottom[0] = pixels[0];
    VideoCutTopBottom[1] = pixels[1];
    VideoCutTopBottom[2] = pixels[2];
    VideoCutTopBottom[3] = pixels[3];
    VideoCutTopBottom[4] = pixels[4];
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();
}

///
/// Set cut left and right.
///
/// @param pixels   table with VideoResolutionMax values
///
void VideoSetCutLeftRight(int pixels[VideoResolutionMax])
{
    VideoThreadLock();
    VideoCutLeftRight[0] = pixels[0];
    VideoCutLeftRight[1] = pixels[1];
    VideoCutLeftRight[2] = pixels[2];
    VideoCutLeftRight[3] = pixels[3];
    VideoCutLeftRight[4] = pixels[4];
    VideoUsedModule->SetVideoMode();
    VideoThreadUnlock();
}

///
/// Set studio levels.
///
/// @param onoff    flag on/off
///
void VideoSetStudioLevels(int onoff)
{
    VideoStudioLevels = onoff;
}

///
/// Set background color.
///
/// @param rgba 32 bit RGBA color.
///
void VideoSetBackground(uint32_t rgba)
{
    VideoBackground = rgba;		// saved for later start
    VideoUsedModule->SetBackground(rgba);
}

///
/// Set audio delay.
///
/// @param ms	delay in ms
///
void VideoSetAudioDelay(int ms)
{
    VideoAudioDelay = ms * 90;
}

///
/// Set auto-crop parameters.
///
void VideoSetAutoCrop(int interval, int delay, int tolerance)
{
    AutoCropInterval = interval;
    AutoCropDelay = delay;
    AutoCropTolerance = tolerance;

    VideoThreadLock();
    VideoUsedModule->ResetAutoCrop();
    VideoThreadUnlock();
}

///
/// Raise video window.
///
int VideoRaiseWindow(void)
{
    static const uint32_t values[] = { XCB_STACK_MODE_ABOVE };

    xcb_configure_window(Connection, VideoWindow, XCB_CONFIG_WINDOW_STACK_MODE, values);

    return 1;
}

///
/// Initialize video output module.
///
/// @param display_name X11 display name
///
void VideoInit(const char *display_name)
{
    int screen_nr;
    int i;
    xcb_screen_iterator_t screen_iter;
    xcb_screen_t const *screen;

    if (XlibDisplay) {			// allow multiple calls
	Debug7("video: x11 already setup");
	return;
    }
    // Open the connection to the X server.
    // use the DISPLAY environment variable as the default display name
    if (!display_name && !(display_name = getenv("DISPLAY"))) {
	// if no environment variable, use :0.0 as default display name
	display_name = ":0.0";
    }
    if (!(XlibDisplay = XOpenDisplay(display_name))) {
	Error("video: Can't connect to X11 server on '%s'", display_name);
	// FIXME: we need to retry connection
	return;
    }
    // Register error handler
    XSetIOErrorHandler(VideoIOErrorHandler);

    // Convert XLIB display to XCB connection
    if (!(Connection = XGetXCBConnection(XlibDisplay))) {
	Error("video: Can't convert XLIB display to XCB connection");
	VideoExit();
	return;
    }
    // prefetch extensions
    //xcb_prefetch_extension_data(Connection, &xcb_big_requests_id);
    //xcb_prefetch_extension_data(Connection, &xcb_randr_id);
    xcb_prefetch_extension_data(Connection, &xcb_screensaver_id);
    xcb_prefetch_extension_data(Connection, &xcb_dpms_id);
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

    //
    // Create output window
    //
    VideoCreateWindow(screen->root, screen->root_visual, screen->root_depth);

    Debug7("video: window prepared");

    //
    //	prepare hardware decoder VA-API
    //
    for (i = 0; i < (int)(sizeof(VideoModules) / sizeof(*VideoModules)); ++i) {
	// FIXME: support list of drivers and include display name
	// use user device or first working enabled device driver
	if ((VideoDriverName && !strcasecmp(VideoDriverName, VideoModules[i]->Name))
	    || (!VideoDriverName && VideoModules[i]->Enabled)) {
	    if (VideoModules[i]->Init(display_name)) {
		VideoUsedModule = VideoModules[i];
		goto found;
	    }
	}
    }
    Error("video: '%s' output module isn't supported", VideoDriverName);
    VideoUsedModule = &NoopModule;

  found:
    //
    // Disable screensaver / DPMS.
    ///
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
/// Cleanup video output module.
///
void VideoExit(void)
{
    if (!XlibDisplay) {			// no init or failed
	return;
    }
    //
    //	Re-enable screensaver / DPMS.
    //
    X11DPMSReenable(Connection);
    X11SuspendScreenSaver(Connection, 0);

    VideoThreadExit();
    VideoUsedModule->Exit();
    VideoUsedModule = &NoopModule;

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
	    Error("video: error closing display");
	}
	XlibDisplay = NULL;
	Connection = 0;
    }
}
