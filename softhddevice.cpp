///
///	@file softhddevice.cpp	@brief A software HD device plugin for VDR.
///
///	Copyright (c) 2011 - 2013 by Johns.  All Rights Reserved.
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

#define __STDC_CONSTANT_MACROS		///< needed for ffmpeg UINT64_C

#include <vdr/interface.h>
#include <vdr/plugin.h>
#include <vdr/player.h>
#include <vdr/osd.h>
#include <vdr/dvbspu.h>
#include <vdr/shutdown.h>

#ifdef HAVE_CONFIG
#include "config.h"
#endif

#include "softhddev.h"
#include "softhddevice.h"
#include "softhddevice_service.h"

extern "C"
{
#include <stdint.h>
#include <libavcodec/avcodec.h>

#include "audio.h"
#include "video.h"
#include "codec.h"
}

//////////////////////////////////////////////////////////////////////////////

    /// vdr-plugin version number.
    /// Makefile extracts the version number for generating the file name
    /// for the distribution archive.
static const char *const VERSION = "0.6.1rc1"
#ifdef GIT_REV
    "-GIT" GIT_REV
#endif
    ;

    /// vdr-plugin description.
static const char *const DESCRIPTION =
trNOOP("A software and GPU emulated HD device");

    /// vdr-plugin text of main menu entry
static const char *MAINMENUENTRY = trNOOP("SoftHdDevice");

    /// single instance of softhddevice plugin device.
static class cSoftHdDevice *MyDevice;

//////////////////////////////////////////////////////////////////////////////

#define RESOLUTIONS 4			///< number of resolutions

    /// resolutions names
static const char *const Resolution[RESOLUTIONS] = {
    "576i", "720p", "1080i_fake", "1080i"
};

static char ConfigMakePrimary;		///< config primary wanted
static char ConfigHideMainMenuEntry;	///< config hide main menu entry
static char ConfigDetachFromMainMenu;	///< detach from main menu entry instead of suspend
static char ConfigSuspendClose;		///< suspend should close devices
static char ConfigSuspendX11;		///< suspend should stop x11

static char Config4to3DisplayFormat = 1;	///< config 4:3 display format
static char ConfigOtherDisplayFormat = 1;	///< config other display format
static uint32_t ConfigVideoBackground;	///< config video background color
static int ConfigOsdWidth;		///< config OSD width
static int ConfigOsdHeight;		///< config OSD height
static char ConfigVideoStudioLevels;	///< config use studio levels
static char ConfigVideo60HzMode;	///< config use 60Hz display mode
static char ConfigVideoSoftStartSync;	///< config use softstart sync
static char ConfigVideoBlackPicture;	///< config enable black picture mode
char ConfigVideoClearOnSwitch;		///< config enable Clear on channel switch

static int ConfigVideoBrightness;	///< config video brightness
static int ConfigVideoContrast = 1000;	///< config video contrast
static int ConfigVideoSaturation = 1000;	///< config video saturation
static int ConfigVideoHue;		///< config video hue

    /// config deinterlace
static int ConfigVideoDeinterlace[RESOLUTIONS];

    /// config skip chroma
static int ConfigVideoSkipChromaDeinterlace[RESOLUTIONS];

    /// config inverse telecine
static int ConfigVideoInverseTelecine[RESOLUTIONS];

    /// config denoise
static int ConfigVideoDenoise[RESOLUTIONS];

    /// config sharpen
static int ConfigVideoSharpen[RESOLUTIONS];

    /// config scaling
static int ConfigVideoScaling[RESOLUTIONS];

    /// config cut top and bottom pixels
static int ConfigVideoCutTopBottom[RESOLUTIONS];

    /// config cut left and right pixels
static int ConfigVideoCutLeftRight[RESOLUTIONS];

    /// config vaapi field ordering for first & second field
static int ConfigVideoFirstField[RESOLUTIONS];
static int ConfigVideoSecondField[RESOLUTIONS];

static int ConfigAutoCropEnabled;	///< auto crop detection enabled
static int ConfigAutoCropInterval;	///< auto crop detection interval
static int ConfigAutoCropDelay;		///< auto crop detection delay
static int ConfigAutoCropTolerance;	///< auto crop detection tolerance

static int ConfigVideoAudioDelay;	///< config audio delay
static char ConfigAudioDrift;		///< config audio drift
static char ConfigAudioPassthrough;	///< config audio pass-through mask
static char AudioPassthroughState;	///< flag audio pass-through on/off
static char ConfigAudioDownmix;		///< config ffmpeg audio downmix
static char ConfigAudioSoftvol;		///< config use software volume
static char ConfigAudioNormalize;	///< config use normalize volume
static int ConfigAudioMaxNormalize;	///< config max normalize factor
static char ConfigAudioCompression;	///< config use volume compression
static int ConfigAudioMaxCompression;	///< config max volume compression
static int ConfigAudioStereoDescent;	///< config reduce stereo loudness
int ConfigAudioBufferTime;		///< config size ms of audio buffer
static int ConfigAudioAutoAES;		///< config automatic AES handling

static char *ConfigX11Display;		///< config x11 display
static char *ConfigAudioDevice;		///< config audio stereo device
static char *ConfigPassthroughDevice;	///< config audio pass-through device

#ifdef USE_PIP
static int ConfigPipX = 100 - 3 - 18;	///< config pip pip x in %
static int ConfigPipY = 100 - 4 - 18;	///< config pip pip y in %
static int ConfigPipWidth = 18;		///< config pip pip width in %
static int ConfigPipHeight = 18;	///< config pip pip height in %
static int ConfigPipVideoX;		///< config pip video x in %
static int ConfigPipVideoY;		///< config pip video y in %
static int ConfigPipVideoWidth;		///< config pip video width in %
static int ConfigPipVideoHeight;	///< config pip video height in %
static int ConfigPipAltX;		///< config pip alt. pip x in %
static int ConfigPipAltY = 50;		///< config pip alt. pip y in %
static int ConfigPipAltWidth;		///< config pip alt. pip width in %
static int ConfigPipAltHeight = 50;	///< config pip alt. pip height in %
static int ConfigPipAltVideoX;		///< config pip alt. video x in %
static int ConfigPipAltVideoY;		///< config pip alt. video y in %
static int ConfigPipAltVideoWidth;	///< config pip alt. video width in %
static int ConfigPipAltVideoHeight = 50;	///< config pip alt. video height in %
#endif

static volatile int DoMakePrimary;	///< switch primary device to this

#define SUSPEND_EXTERNAL	-1	///< play external suspend mode
#define NOT_SUSPENDED		0	///< not suspend mode
#define SUSPEND_NORMAL		1	///< normal suspend mode
#define SUSPEND_DETACHED	2	///< detached suspend mode
static char SuspendMode;		///< suspend mode

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
//	C Callbacks
//////////////////////////////////////////////////////////////////////////////

/**
**	Soft device plugin remote class.
*/
class cSoftRemote:public cRemote
{
  public:

    /**
    **	Soft device remote class constructor.
    **
    **	@param name	remote name
    */
    cSoftRemote(const char *name):cRemote(name)
    {
    }

    /**
    **	Put keycode into vdr event queue.
    **
    **	@param code	key code
    **	@param repeat	flag key repeated
    **	@param release	flag key released
    */
    bool Put(const char *code, bool repeat = false, bool release = false) {
	return cRemote::Put(code, repeat, release);
    }
};

/**
**	Feed key press as remote input (called from C part).
**
**	@param keymap	target keymap "XKeymap" name
**	@param key	pressed/released key name
**	@param repeat	repeated key flag
**	@param release	released key flag
**	@param letter	x11 character string (system setting locale)
*/
extern "C" void FeedKeyPress(const char *keymap, const char *key, int repeat,
    int release, const char *letter)
{
    cRemote *remote;
    cSoftRemote *csoft;

    if (!keymap || !key) {
	return;
    }
    // find remote
    for (remote = Remotes.First(); remote; remote = Remotes.Next(remote)) {
	if (!strcmp(remote->Name(), keymap)) {
	    break;
	}
    }
    // if remote not already exists, create it
    if (remote) {
	csoft = (cSoftRemote *) remote;
    } else {
	dsyslog("[softhddev]%s: remote '%s' not found\n", __FUNCTION__,
	    keymap);
	csoft = new cSoftRemote(keymap);
    }

    //dsyslog("[softhddev]%s %s, %s, %s\n", __FUNCTION__, keymap, key, letter);
    if (key[1]) {			// no single character
	if (!csoft->Put(key, repeat, release) && letter
	    && !cRemote::IsLearning()) {
	    cCharSetConv conv;
	    unsigned code;

	    code = Utf8CharGet(conv.Convert(letter));
	    if (code <= 0xFF) {
		cRemote::Put(KBDKEY(code));	// feed it for edit mode
	    }
	}
    } else if (!csoft->Put(key, repeat, release)) {
	cRemote::Put(KBDKEY(key[0]));	// feed it for edit mode
    }
}

//////////////////////////////////////////////////////////////////////////////
//	OSD
//////////////////////////////////////////////////////////////////////////////

/**
**	Soft device plugin OSD class.
*/
class cSoftOsd:public cOsd
{
  public:
    static volatile char Dirty;		///< flag force redraw everything
    int OsdLevel;			///< current osd level FIXME: remove

     cSoftOsd(int, int, uint);		///< osd constructor
     virtual ~ cSoftOsd(void);		///< osd destructor
    /// set the sub-areas to the given areas
    virtual eOsdError SetAreas(const tArea *, int);
    virtual void Flush(void);		///< commits all data to the hardware
    virtual void SetActive(bool);	///< sets OSD to be the active one
};

volatile char cSoftOsd::Dirty;		///< flag force redraw everything

/**
**	Sets this OSD to be the active one.
**
**	@param on	true on, false off
**
**	@note only needed as workaround for text2skin plugin with
**	undrawn areas.
*/
void cSoftOsd::SetActive(bool on)
{
#ifdef OSD_DEBUG
    dsyslog("[softhddev]%s: %d level %d\n", __FUNCTION__, on, OsdLevel);
#endif

    if (Active() == on) {
	return;				// already active, no action
    }
    cOsd::SetActive(on);

    if (on) {
	Dirty = 1;
	// only flush here if there are already bitmaps
	if (GetBitmap(0)) {
	    Flush();
	}
    } else {
	OsdClose();
    }
}

/**
**	Constructor OSD.
**
**	Initializes the OSD with the given coordinates.
**
**	@param left	x-coordinate of osd on display
**	@param top	y-coordinate of osd on display
**	@param level	level of the osd (smallest is shown)
*/
cSoftOsd::cSoftOsd(int left, int top, uint level)
:cOsd(left, top, level)
{
#ifdef OSD_DEBUG
    /* FIXME: OsdWidth/OsdHeight not correct!
     */
    dsyslog("[softhddev]%s: %dx%d%+d%+d, %d\n", __FUNCTION__, OsdWidth(),
	OsdHeight(), left, top, level);
#endif

    OsdLevel = level;
}

/**
**	OSD Destructor.
**
**	Shuts down the OSD.
*/
cSoftOsd::~cSoftOsd(void)
{
#ifdef OSD_DEBUG
    dsyslog("[softhddev]%s: level %d\n", __FUNCTION__, OsdLevel);
#endif

    SetActive(false);
    // done by SetActive: OsdClose();

#ifdef USE_YAEPG
    // support yaepghd, video window
    if (vidWin.bpp) {			// restore fullsized video
	int width;
	int height;
	double video_aspect;

	::GetOsdSize(&width, &height, &video_aspect);
	// works osd relative
	::ScaleVideo(0, 0, width, height);
    }
#endif
}

/**
+*	Set the sub-areas to the given areas
*/
eOsdError cSoftOsd::SetAreas(const tArea * areas, int n)
{
#ifdef OSD_DEBUG
    dsyslog("[softhddev]%s: %d areas \n", __FUNCTION__, n);
#endif

    // clear old OSD, when new areas are set
    if (!IsTrueColor()) {
	cBitmap *bitmap;
	int i;

	for (i = 0; (bitmap = GetBitmap(i)); i++) {
	    bitmap->Clean();
	}
    }
    if (Active()) {
	VideoOsdClear();
	Dirty = 1;
    }
    return cOsd::SetAreas(areas, n);
}

/**
**	Actually commits all data to the OSD hardware.
*/
void cSoftOsd::Flush(void)
{
    cPixmapMemory *pm;

#ifdef OSD_DEBUG
    dsyslog("[softhddev]%s: level %d active %d\n", __FUNCTION__, OsdLevel,
	Active());
#endif

    if (!Active()) {			// this osd is not active
	return;
    }
#ifdef USE_YAEPG
    // support yaepghd, video window
    if (vidWin.bpp) {
#ifdef OSD_DEBUG
	dsyslog("[softhddev]%s: %dx%d%+d%+d\n", __FUNCTION__, vidWin.Width(),
	    vidWin.Height(), vidWin.x1, vidWin.y2);
#endif
	// FIXME: vidWin is OSD relative not video window.
	// FIXME: doesn't work if fixed OSD width != real window width
	// FIXME: solved in VideoSetOutputPosition
	::ScaleVideo(Left() + vidWin.x1, Top() + vidWin.y1, vidWin.Width(),
	    vidWin.Height());
    }
#endif

    if (!IsTrueColor()) {
	cBitmap *bitmap;
	int i;

#ifdef OSD_DEBUG
	static char warned;

	if (!warned) {
	    dsyslog("[softhddev]%s: FIXME: should be truecolor\n",
		__FUNCTION__);
	    warned = 1;
	}
#endif
	// draw all bitmaps
	for (i = 0; (bitmap = GetBitmap(i)); ++i) {
	    uint8_t *argb;
	    int x;
	    int y;
	    int w;
	    int h;
	    int x1;
	    int y1;
	    int x2;
	    int y2;

	    // get dirty bounding box
	    if (Dirty) {		// forced complete update
		x1 = 0;
		y1 = 0;
		x2 = bitmap->Width() - 1;
		y2 = bitmap->Height() - 1;
	    } else if (!bitmap->Dirty(x1, y1, x2, y2)) {
		continue;		// nothing dirty continue
	    }
	    // convert and upload only dirty areas
	    w = x2 - x1 + 1;
	    h = y2 - y1 + 1;
	    if (1) {			// just for the case it makes trouble
		int width;
		int height;
		double video_aspect;

		::GetOsdSize(&width, &height, &video_aspect);
		if (w > width) {
		    w = width;
		    x2 = x1 + width - 1;
		}
		if (h > height) {
		    h = height;
		    y2 = y1 + height - 1;
		}
	    }
#ifdef DEBUG
	    if (w > bitmap->Width() || h > bitmap->Height()) {
		esyslog(tr("[softhddev]: dirty area too big\n"));
		abort();
	    }
#endif
	    argb = (uint8_t *) malloc(w * h * sizeof(uint32_t));
	    for (y = y1; y <= y2; ++y) {
		for (x = x1; x <= x2; ++x) {
		    ((uint32_t *) argb)[x - x1 + (y - y1) * w] =
			bitmap->GetColor(x, y);
		}
	    }
#ifdef OSD_DEBUG
	    dsyslog("[softhddev]%s: draw %dx%d%+d%+d bm\n", __FUNCTION__, w, h,
		Left() + bitmap->X0() + x1, Top() + bitmap->Y0() + y1);
#endif
	    OsdDrawARGB(Left() + bitmap->X0() + x1, Top() + bitmap->Y0() + y1,
		w, h, argb);

	    bitmap->Clean();
	    // FIXME: reuse argb
	    free(argb);
	}
	Dirty = 0;
	return;
    }

    LOCK_PIXMAPS;
    while ((pm = RenderPixmaps())) {
	int x;
	int y;
	int w;
	int h;

	x = Left() + pm->ViewPort().X();
	y = Top() + pm->ViewPort().Y();
	w = pm->ViewPort().Width();
	h = pm->ViewPort().Height();

#ifdef OSD_DEBUG
	dsyslog("[softhddev]%s: draw %dx%d%+d%+d %p\n", __FUNCTION__, w, h, x,
	    y, pm->Data());
#endif
	OsdDrawARGB(x, y, w, h, pm->Data());

	delete pm;
    }
    Dirty = 0;
}

//////////////////////////////////////////////////////////////////////////////
//	OSD provider
//////////////////////////////////////////////////////////////////////////////

/**
**	Soft device plugin OSD provider class.
*/
class cSoftOsdProvider:public cOsdProvider
{
  private:
    static cOsd *Osd;			///< single OSD
  public:
    virtual cOsd * CreateOsd(int, int, uint);
    virtual bool ProvidesTrueColor(void);
    cSoftOsdProvider(void);		///< OSD provider constructor
    //virtual ~cSoftOsdProvider();	///< OSD provider destructor
};

cOsd *cSoftOsdProvider::Osd;		///< single osd

/**
**	Create a new OSD.
**
**	@param left	x-coordinate of OSD
**	@param top	y-coordinate of OSD
**	@param level	layer level of OSD
*/
cOsd *cSoftOsdProvider::CreateOsd(int left, int top, uint level)
{
#ifdef OSD_DEBUG
    dsyslog("[softhddev]%s: %d, %d, %d\n", __FUNCTION__, left, top, level);
#endif

    return Osd = new cSoftOsd(left, top, level);
}

/**
**	Check if this OSD provider is able to handle a true color OSD.
**
**	@returns true we are able to handle a true color OSD.
*/
bool cSoftOsdProvider::ProvidesTrueColor(void)
{
    return true;
}

/**
**	Create cOsdProvider class.
*/
cSoftOsdProvider::cSoftOsdProvider(void)
:  cOsdProvider()
{
#ifdef OSD_DEBUG
    dsyslog("[softhddev]%s:\n", __FUNCTION__);
#endif
}

/**
**	Destroy cOsdProvider class.
cSoftOsdProvider::~cSoftOsdProvider()
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);
}
*/

//////////////////////////////////////////////////////////////////////////////
//	cMenuSetupPage
//////////////////////////////////////////////////////////////////////////////

/**
**	Soft device plugin menu setup page class.
*/
class cMenuSetupSoft:public cMenuSetupPage
{
  protected:
    ///
    /// local copies of global setup variables:
    /// @{
    int General;
    int MakePrimary;
    int HideMainMenuEntry;
    int DetachFromMainMenu;
    int OsdSize;
    int OsdWidth;
    int OsdHeight;
    int SuspendClose;
    int SuspendX11;

    int Video;
    int Video4to3DisplayFormat;
    int VideoOtherDisplayFormat;
    uint32_t Background;
    uint32_t BackgroundAlpha;
    int StudioLevels;
    int _60HzMode;
    int SoftStartSync;
    int BlackPicture;
    int ClearOnSwitch;

    int Brightness;
    int Contrast;
    int Saturation;
    int Hue;

    int ResolutionShown[RESOLUTIONS];
    int Scaling[RESOLUTIONS];
    int Deinterlace[RESOLUTIONS];
    int SkipChromaDeinterlace[RESOLUTIONS];
    int InverseTelecine[RESOLUTIONS];
    int Denoise[RESOLUTIONS];
    int Sharpen[RESOLUTIONS];
    int CutTopBottom[RESOLUTIONS];
    int CutLeftRight[RESOLUTIONS];
    int FirstField[RESOLUTIONS];
    int SecondField[RESOLUTIONS];

    int AutoCropInterval;
    int AutoCropDelay;
    int AutoCropTolerance;

    int Audio;
    int AudioDelay;
    int AudioDrift;
    int AudioPassthroughDefault;
    int AudioPassthroughPCM;
    int AudioPassthroughAC3;
    int AudioPassthroughEAC3;
    int AudioDownmix;
    int AudioSoftvol;
    int AudioNormalize;
    int AudioMaxNormalize;
    int AudioCompression;
    int AudioMaxCompression;
    int AudioStereoDescent;
    int AudioBufferTime;
    int AudioAutoAES;

#ifdef USE_PIP
    int Pip;
    int PipX;
    int PipY;
    int PipWidth;
    int PipHeight;
    int PipVideoX;
    int PipVideoY;
    int PipVideoWidth;
    int PipVideoHeight;
    int PipAltX;
    int PipAltY;
    int PipAltWidth;
    int PipAltHeight;
    int PipAltVideoX;
    int PipAltVideoY;
    int PipAltVideoWidth;
    int PipAltVideoHeight;
#endif

    /// @}
  private:
     inline cOsdItem * CollapsedItem(const char *, int &, const char * = NULL);
    void Create(void);			// create sub-menu
  protected:
     virtual void Store(void);
  public:
     cMenuSetupSoft(void);
    virtual eOSState ProcessKey(eKeys);	// handle input
};

/**
**	Create a seperator item.
**
**	@param label	text inside separator
*/
static inline cOsdItem *SeparatorItem(const char *label)
{
    cOsdItem *item;

    item = new cOsdItem(cString::sprintf("* %s: ", label));
    item->SetSelectable(false);

    return item;
}

/**
**	Create a collapsed item.
**
**	@param label	text inside collapsed
**	@param flag	flag handling collapsed or opened
**	@param msg	open message
*/
inline cOsdItem *cMenuSetupSoft::CollapsedItem(const char *label, int &flag,
    const char *msg)
{
    cOsdItem *item;

    item =
	new cMenuEditBoolItem(cString::sprintf("* %s", label), &flag,
	msg ? msg : tr("show"), tr("hide"));

    return item;
}

/**
**	Create setup menu.
*/
void cMenuSetupSoft::Create(void)
{
    static const char *const osd_size[] = {
	"auto", "1920x1080", "1280x720", "custom",
    };
    static const char *const video_display_formats_4_3[] = {
	"pan&scan", "letterbox", "center cut-out",
    };
    static const char *const video_display_formats_16_9[] = {
	"pan&scan", "pillarbox", "center cut-out",
    };
#ifdef USE_VAAPI
    static const char *const deinterlace[] = {
	"Bob", "Weave/None", "MotionAdaptive", "MotionCompensated", "Software Bob",
	"Software Spatial",
    };
    static const char *const deinterlace_short[] = {
	"B", "W", "MADI", "MCDI", "S+B", "S+S",
    };
#else
    static const char *const deinterlace[] = {
	"Bob", "Weave/None", "Temporal", "TemporalSpatial", "Software Bob",
	"Software Spatial",
    };
    static const char *const deinterlace_short[] = {
	"B", "W", "T", "T+S", "S+B", "S+S",
    };
#endif
    static const char *const scaling[] = {
	"Normal", "Fast", "HQ", "Anamorphic"
    };
    static const char *const scaling_short[] = {
	"N", "F", "HQ", "A"
    };
    static const char *const audiodrift[] = {
	"None", "PCM", "AC-3", "PCM + AC-3"
    };
    static const char *const resolution[RESOLUTIONS] = {
	"576i", "720p", "fake 1080i", "1080i"
    };
    int current;
    int i;

    current = Current();		// get current menu item index
    Clear();				// clear the menu

    //
    //	general
    //
    Add(CollapsedItem(tr("General"), General));

    if (General) {
	Add(new cMenuEditBoolItem(tr("Make primary device"), &MakePrimary,
		trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Hide main menu entry"),
		&HideMainMenuEntry, trVDR("no"), trVDR("yes")));
	//
	//	osd
	//
	Add(new cMenuEditStraItem(tr("Osd size"), &OsdSize, 4, osd_size));
	if (OsdSize == 3) {
	    Add(new cMenuEditIntItem(tr("Osd width"), &OsdWidth, 0, 4096));
	    Add(new cMenuEditIntItem(tr("Osd height"), &OsdHeight, 0, 4096));
	}
	//
	//	suspend
	//
	Add(SeparatorItem(tr("Suspend")));
	Add(new cMenuEditBoolItem(tr("Detach from main menu entry"),
		&DetachFromMainMenu, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Suspend closes video+audio"),
		&SuspendClose, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Suspend stops x11"), &SuspendX11,
		trVDR("no"), trVDR("yes")));
    }
    //
    //	video
    //
    Add(CollapsedItem(tr("Video"), Video));
    if (Video) {
	Add(new cMenuEditStraItem(trVDR("4:3 video display format"),
		&Video4to3DisplayFormat, 3, video_display_formats_4_3));
	Add(new cMenuEditStraItem(trVDR("16:9+other video display format"),
		&VideoOtherDisplayFormat, 3, video_display_formats_16_9));

	// FIXME: switch config gray/color configuration
	Add(new cMenuEditIntItem(tr("Video background color (RGB)"),
		(int *)&Background, 0, 0x00FFFFFF));
	Add(new cMenuEditIntItem(tr("Video background color (Alpha)"),
		(int *)&BackgroundAlpha, 0, 0xFF));
	Add(new cMenuEditBoolItem(tr("Use studio levels (vdpau only)"),
		&StudioLevels, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("60hz display mode"), &_60HzMode,
		trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Soft start a/v sync"), &SoftStartSync,
		trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Black during channel switch"),
		&BlackPicture, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Clear decoder on channel switch"),
		&ClearOnSwitch, trVDR("no"), trVDR("yes")));

	Add(new cMenuEditIntItem(tr("Brightness (-1000..1000) (vdpau)"),
		&Brightness, -1000, 1000, tr("min"), tr("max")));
	Add(new cMenuEditIntItem(tr("Contrast (0..10000) (vdpau)"), &Contrast,
		0, 10000, tr("min"), tr("max")));
	Add(new cMenuEditIntItem(tr("Saturation (0..10000) (vdpau)"),
		&Saturation, 0, 10000, tr("min"), tr("max")));
	Add(new cMenuEditIntItem(tr("Hue (-3141..3141) (vdpau)"), &Hue, -3141,
		3141, tr("min"), tr("max")));

	for (i = 0; i < RESOLUTIONS; ++i) {
	    cString msg;

	    // short hidden informations
	    msg =
		cString::sprintf("%s,%s%s%s%s,...", scaling_short[Scaling[i]],
		deinterlace_short[Deinterlace[i]],
		SkipChromaDeinterlace[i] ? ",skip" : "",
		InverseTelecine[i] ? ",ITC" : "", Denoise[i] ? ",DN" : "");
	    Add(CollapsedItem(resolution[i], ResolutionShown[i], msg));

	    if (ResolutionShown[i]) {
		Add(new cMenuEditStraItem(tr("Scaling"), &Scaling[i], 4,
			scaling));
		Add(new cMenuEditStraItem(tr("Deinterlace"), &Deinterlace[i],
			6, deinterlace));
		Add(new cMenuEditBoolItem(tr("SkipChromaDeinterlace (vdpau)"),
			&SkipChromaDeinterlace[i], trVDR("no"), trVDR("yes")));
		Add(new cMenuEditBoolItem(tr("Inverse Telecine (vdpau)"),
			&InverseTelecine[i], trVDR("no"), trVDR("yes")));
		Add(new cMenuEditIntItem(tr("Denoise (0..1000) (vdpau)"),
			&Denoise[i], 0, 1000, tr("off"), tr("max")));
		Add(new cMenuEditIntItem(tr("Sharpen (-1000..1000) (vdpau)"),
			&Sharpen[i], -1000, 1000, tr("blur max"),
			tr("sharpen max")));

		Add(new cMenuEditIntItem(tr("Cut top and bottom (pixel)"),
			&CutTopBottom[i], 0, 250));
		Add(new cMenuEditIntItem(tr("Cut left and right (pixel)"),
			&CutLeftRight[i], 0, 250));

		Add(new cMenuEditIntItem(tr("First field order (0-2) (vaapi)"),
			&FirstField[i], 0, 2));
		Add(new cMenuEditIntItem(tr("Second field order (0-2) (vaapi)"),
			&SecondField[i], 0, 2));
	    }
	}
	//
	//  auto-crop
	//
	Add(SeparatorItem(tr("Auto-crop")));
	Add(new cMenuEditIntItem(tr("Autocrop interval (frames)"),
		&AutoCropInterval, 0, 200, tr("off")));
	Add(new cMenuEditIntItem(tr("Autocrop delay (n * interval)"),
		&AutoCropDelay, 0, 200));
	Add(new cMenuEditIntItem(tr("Autocrop tolerance (pixel)"),
		&AutoCropTolerance, 0, 32));
    }
    //
    //	audio
    //
    Add(CollapsedItem(tr("Audio"), Audio));

    if (Audio) {
	Add(new cMenuEditIntItem(tr("Audio/Video delay (ms)"), &AudioDelay,
		-1000, 1000));
	Add(new cMenuEditStraItem(tr("Audio drift correction"), &AudioDrift, 4,
		audiodrift));
	Add(new cMenuEditBoolItem(tr("Pass-through default"),
		&AudioPassthroughDefault, trVDR("off"), trVDR("on")));
	Add(new cMenuEditBoolItem(tr("\040\040PCM pass-through"),
		&AudioPassthroughPCM, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("\040\040AC-3 pass-through"),
		&AudioPassthroughAC3, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("\040\040E-AC-3 pass-through"),
		&AudioPassthroughEAC3, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Enable (E-)AC-3 (decoder) downmix"),
		&AudioDownmix, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Volume control"), &AudioSoftvol,
		tr("Hardware"), tr("Software")));
	Add(new cMenuEditBoolItem(tr("Enable normalize volume"),
		&AudioNormalize, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditIntItem(tr("  Max normalize factor (/1000)"),
		&AudioMaxNormalize, 0, 10000));
	Add(new cMenuEditBoolItem(tr("Enable volume compression"),
		&AudioCompression, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditIntItem(tr("  Max compression factor (/1000)"),
		&AudioMaxCompression, 0, 10000));
	Add(new cMenuEditIntItem(tr("Reduce stereo volume (/1000)"),
		&AudioStereoDescent, 0, 1000));
	Add(new cMenuEditIntItem(tr("Audio buffer size (ms)"),
		&AudioBufferTime, 0, 1000));
	Add(new cMenuEditBoolItem(tr("Enable automatic AES"), &AudioAutoAES,
		trVDR("no"), trVDR("yes")));
    }
#ifdef USE_PIP
    //
    //	PIP
    //
    Add(CollapsedItem(tr("Picture-In-Picture"), Pip));
    if (Pip) {
	// FIXME: predefined modes/custom mode
	Add(new cMenuEditIntItem(tr("Pip X (%)"), &PipX, 0, 100));
	Add(new cMenuEditIntItem(tr("Pip Y (%)"), &PipY, 0, 100));
	Add(new cMenuEditIntItem(tr("Pip Width (%)"), &PipWidth, 0, 100));
	Add(new cMenuEditIntItem(tr("Pip Height (%)"), &PipHeight, 0, 100));
	Add(new cMenuEditIntItem(tr("Video X (%)"), &PipVideoX, 0, 100));
	Add(new cMenuEditIntItem(tr("Video Y (%)"), &PipVideoY, 0, 100));
	Add(new cMenuEditIntItem(tr("Video Width (%)"), &PipVideoWidth, 0,
		100));
	Add(new cMenuEditIntItem(tr("Video Height (%)"), &PipVideoHeight, 0,
		100));
	Add(new cMenuEditIntItem(tr("Alternative Pip X (%)"), &PipAltX, 0,
		100));
	Add(new cMenuEditIntItem(tr("Alternative Pip Y (%)"), &PipAltY, 0,
		100));
	Add(new cMenuEditIntItem(tr("Alternative Pip Width (%)"), &PipAltWidth,
		0, 100));
	Add(new cMenuEditIntItem(tr("Alternative Pip Height (%)"),
		&PipAltHeight, 0, 100));
	Add(new cMenuEditIntItem(tr("Alternative Video X (%)"), &PipAltVideoX,
		0, 100));
	Add(new cMenuEditIntItem(tr("Alternative Video Y (%)"), &PipAltVideoY,
		0, 100));
	Add(new cMenuEditIntItem(tr("Alternative Video Width (%)"),
		&PipAltVideoWidth, 0, 100));
	Add(new cMenuEditIntItem(tr("Alternative Video Height (%)"),
		&PipAltVideoHeight, 0, 100));
    }
#endif

    SetCurrent(Get(current));		// restore selected menu entry
    Display();				// display build menu
}

/**
**	Process key for setup menu.
*/
eOSState cMenuSetupSoft::ProcessKey(eKeys key)
{
    eOSState state;
    int old_general;
    int old_video;
    int old_audio;

#ifdef USE_PIP
    int old_pip;
#endif
    int old_osd_size;
    int old_resolution_shown[RESOLUTIONS];
    int i;

    old_general = General;
    old_video = Video;
    old_audio = Audio;
#ifdef USE_PIP
    old_pip = Pip;
#endif
    old_osd_size = OsdSize;
    memcpy(old_resolution_shown, ResolutionShown, sizeof(ResolutionShown));
    state = cMenuSetupPage::ProcessKey(key);

    if (key != kNone) {
	// update menu only, if something on the structure has changed
	// this is needed because VDR menus are evil slow
	if (old_general != General || old_video != Video || old_audio != Audio
#ifdef USE_PIP
	    || old_pip != Pip
#endif
	    || old_osd_size != OsdSize) {
	    Create();			// update menu
	} else {
	    for (i = 0; i < RESOLUTIONS; ++i) {
		if (old_resolution_shown[i] != ResolutionShown[i]) {
		    Create();		// update menu
		    break;
		}
	    }
	}
    }

    return state;
}

/**
**	Constructor setup menu.
**
**	Import global config variables into setup.
*/
cMenuSetupSoft::cMenuSetupSoft(void)
{
    int i;

    //
    //	general
    //
    General = 0;
    MakePrimary = ConfigMakePrimary;
    HideMainMenuEntry = ConfigHideMainMenuEntry;
    DetachFromMainMenu = ConfigDetachFromMainMenu;
    //
    //	osd
    //
    OsdWidth = ConfigOsdWidth;
    OsdHeight = ConfigOsdHeight;
    if (!OsdWidth && !OsdHeight) {
	OsdSize = 0;
    } else if (OsdWidth == 1920 && OsdHeight == 1080) {
	OsdSize = 1;
    } else if (OsdWidth == 1280 && OsdHeight == 720) {
	OsdSize = 2;
    } else {
	OsdSize = 3;
    }
    //
    //	suspend
    //
    SuspendClose = ConfigSuspendClose;
    SuspendX11 = ConfigSuspendX11;

    //
    //	video
    //
    Video = 0;
    Video4to3DisplayFormat = Config4to3DisplayFormat;
    VideoOtherDisplayFormat = ConfigOtherDisplayFormat;
    // no unsigned int menu item supported, split background color/alpha
    Background = ConfigVideoBackground >> 8;
    BackgroundAlpha = ConfigVideoBackground & 0xFF;
    StudioLevels = ConfigVideoStudioLevels;
    _60HzMode = ConfigVideo60HzMode;
    SoftStartSync = ConfigVideoSoftStartSync;
    BlackPicture = ConfigVideoBlackPicture;
    ClearOnSwitch = ConfigVideoClearOnSwitch;

    Brightness = ConfigVideoBrightness;
    Contrast = ConfigVideoContrast;
    Saturation = ConfigVideoSaturation;
    Hue = ConfigVideoHue;

    for (i = 0; i < RESOLUTIONS; ++i) {
	ResolutionShown[i] = 0;
	Scaling[i] = ConfigVideoScaling[i];
	Deinterlace[i] = ConfigVideoDeinterlace[i];
	SkipChromaDeinterlace[i] = ConfigVideoSkipChromaDeinterlace[i];
	InverseTelecine[i] = ConfigVideoInverseTelecine[i];
	Denoise[i] = ConfigVideoDenoise[i];
	Sharpen[i] = ConfigVideoSharpen[i];

	CutTopBottom[i] = ConfigVideoCutTopBottom[i];
	CutLeftRight[i] = ConfigVideoCutLeftRight[i];

	FirstField[i] = ConfigVideoFirstField[i];
	SecondField[i] = ConfigVideoSecondField[i];

    }
    //
    //	auto-crop
    //
    AutoCropInterval = ConfigAutoCropInterval;
    AutoCropDelay = ConfigAutoCropDelay;
    AutoCropTolerance = ConfigAutoCropTolerance;

    //
    //	audio
    //
    Audio = 0;
    AudioDelay = ConfigVideoAudioDelay;
    AudioDrift = ConfigAudioDrift;
    AudioPassthroughDefault = AudioPassthroughState;
    AudioPassthroughPCM = ConfigAudioPassthrough & CodecPCM;
    AudioPassthroughAC3 = ConfigAudioPassthrough & CodecAC3;
    AudioPassthroughEAC3 = ConfigAudioPassthrough & CodecEAC3;
    AudioDownmix = ConfigAudioDownmix;
    AudioSoftvol = ConfigAudioSoftvol;
    AudioNormalize = ConfigAudioNormalize;
    AudioMaxNormalize = ConfigAudioMaxNormalize;
    AudioCompression = ConfigAudioCompression;
    AudioMaxCompression = ConfigAudioMaxCompression;
    AudioStereoDescent = ConfigAudioStereoDescent;
    AudioBufferTime = ConfigAudioBufferTime;
    AudioAutoAES = ConfigAudioAutoAES;

#ifdef USE_PIP
    //
    //	PIP
    //
    Pip = 0;
    PipX = ConfigPipX;
    PipY = ConfigPipY;
    PipWidth = ConfigPipWidth;
    PipHeight = ConfigPipHeight;
    PipVideoX = ConfigPipVideoX;
    PipVideoY = ConfigPipVideoY;
    PipVideoWidth = ConfigPipVideoWidth;
    PipVideoHeight = ConfigPipVideoHeight;
    PipAltX = ConfigPipAltX;
    PipAltY = ConfigPipAltY;
    PipAltWidth = ConfigPipAltWidth;
    PipAltHeight = ConfigPipAltHeight;
    PipAltVideoX = ConfigPipAltVideoX;
    PipAltVideoY = ConfigPipAltVideoY;
    PipAltVideoWidth = ConfigPipAltVideoWidth;
    PipAltVideoHeight = ConfigPipAltVideoHeight;
#endif
    Create();
}

/**
**	Store setup.
*/
void cMenuSetupSoft::Store(void)
{
    int i;

    SetupStore("MakePrimary", ConfigMakePrimary = MakePrimary);
    SetupStore("HideMainMenuEntry", ConfigHideMainMenuEntry =
	HideMainMenuEntry);
    SetupStore("DetachFromMainMenu", ConfigDetachFromMainMenu =
	DetachFromMainMenu);
    switch (OsdSize) {
	case 0:
	    OsdWidth = 0;
	    OsdHeight = 0;
	    break;
	case 1:
	    OsdWidth = 1920;
	    OsdHeight = 1080;
	    break;
	case 2:
	    OsdWidth = 1280;
	    OsdHeight = 720;
	default:
	    break;
    }
    if (ConfigOsdWidth != OsdWidth || ConfigOsdHeight != OsdHeight) {
	VideoSetOsdSize(ConfigOsdWidth = OsdWidth, ConfigOsdHeight =
	    OsdHeight);
	// FIXME: shown osd size not updated
    }
    SetupStore("Osd.Width", ConfigOsdWidth);
    SetupStore("Osd.Height", ConfigOsdHeight);

    SetupStore("Suspend.Close", ConfigSuspendClose = SuspendClose);
    SetupStore("Suspend.X11", ConfigSuspendX11 = SuspendX11);

    SetupStore("Video4to3DisplayFormat", Config4to3DisplayFormat =
	Video4to3DisplayFormat);
    VideoSet4to3DisplayFormat(Config4to3DisplayFormat);
    SetupStore("VideoOtherDisplayFormat", ConfigOtherDisplayFormat =
	VideoOtherDisplayFormat);
    VideoSetOtherDisplayFormat(ConfigOtherDisplayFormat);

    ConfigVideoBackground = Background << 8 | (BackgroundAlpha & 0xFF);
    SetupStore("Background", ConfigVideoBackground);
    VideoSetBackground(ConfigVideoBackground);
    SetupStore("StudioLevels", ConfigVideoStudioLevels = StudioLevels);
    VideoSetStudioLevels(ConfigVideoStudioLevels);
    SetupStore("60HzMode", ConfigVideo60HzMode = _60HzMode);
    VideoSet60HzMode(ConfigVideo60HzMode);
    SetupStore("SoftStartSync", ConfigVideoSoftStartSync = SoftStartSync);
    VideoSetSoftStartSync(ConfigVideoSoftStartSync);
    SetupStore("BlackPicture", ConfigVideoBlackPicture = BlackPicture);
    VideoSetBlackPicture(ConfigVideoBlackPicture);
    SetupStore("ClearOnSwitch", ConfigVideoClearOnSwitch = ClearOnSwitch);

    SetupStore("Brightness", ConfigVideoBrightness = Brightness);
    VideoSetBrightness(ConfigVideoBrightness);
    SetupStore("Contrast", ConfigVideoContrast = Contrast);
    VideoSetContrast(ConfigVideoContrast);
    SetupStore("Saturation", ConfigVideoSaturation = Saturation);
    VideoSetSaturation(ConfigVideoSaturation);
    SetupStore("Hue", ConfigVideoHue = Hue);
    VideoSetHue(ConfigVideoHue);

    for (i = 0; i < RESOLUTIONS; ++i) {
	char buf[128];

	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Scaling");
	SetupStore(buf, ConfigVideoScaling[i] = Scaling[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Deinterlace");
	SetupStore(buf, ConfigVideoDeinterlace[i] = Deinterlace[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i],
	    "SkipChromaDeinterlace");
	SetupStore(buf, ConfigVideoSkipChromaDeinterlace[i] =
	    SkipChromaDeinterlace[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "InverseTelecine");
	SetupStore(buf, ConfigVideoInverseTelecine[i] = InverseTelecine[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Denoise");
	SetupStore(buf, ConfigVideoDenoise[i] = Denoise[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Sharpen");
	SetupStore(buf, ConfigVideoSharpen[i] = Sharpen[i]);

	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "CutTopBottom");
	SetupStore(buf, ConfigVideoCutTopBottom[i] = CutTopBottom[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "CutLeftRight");
	SetupStore(buf, ConfigVideoCutLeftRight[i] = CutLeftRight[i]);

	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "FirstField");
	SetupStore(buf, ConfigVideoFirstField[i] = FirstField[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "SecondField");
	SetupStore(buf, ConfigVideoSecondField[i] = SecondField[i]);

    }
    VideoSetScaling(ConfigVideoScaling);
    VideoSetDeinterlace(ConfigVideoDeinterlace);
    VideoSetSkipChromaDeinterlace(ConfigVideoSkipChromaDeinterlace);
    VideoSetInverseTelecine(ConfigVideoInverseTelecine);
    VideoSetDenoise(ConfigVideoDenoise);
    VideoSetSharpen(ConfigVideoSharpen);
    VideoSetCutTopBottom(ConfigVideoCutTopBottom);
    VideoSetCutLeftRight(ConfigVideoCutLeftRight);
    VideoSetFirstField(ConfigVideoFirstField);
    VideoSetSecondField(ConfigVideoSecondField);

    SetupStore("AutoCrop.Interval", ConfigAutoCropInterval = AutoCropInterval);
    SetupStore("AutoCrop.Delay", ConfigAutoCropDelay = AutoCropDelay);
    SetupStore("AutoCrop.Tolerance", ConfigAutoCropTolerance =
	AutoCropTolerance);
    VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay,
	ConfigAutoCropTolerance);
    ConfigAutoCropEnabled = ConfigAutoCropInterval != 0;

    SetupStore("AudioDelay", ConfigVideoAudioDelay = AudioDelay);
    VideoSetAudioDelay(ConfigVideoAudioDelay);
    SetupStore("AudioDrift", ConfigAudioDrift = AudioDrift);
    CodecSetAudioDrift(ConfigAudioDrift);
    ConfigAudioPassthrough = (AudioPassthroughPCM ? CodecPCM : 0)
	| (AudioPassthroughAC3 ? CodecAC3 : 0)
	| (AudioPassthroughEAC3 ? CodecEAC3 : 0);
    AudioPassthroughState = AudioPassthroughDefault;
    if (AudioPassthroughState) {
	SetupStore("AudioPassthrough", ConfigAudioPassthrough);
	CodecSetAudioPassthrough(ConfigAudioPassthrough);
    } else {
	SetupStore("AudioPassthrough", -ConfigAudioPassthrough);
	CodecSetAudioPassthrough(0);
    }
    SetupStore("AudioDownmix", ConfigAudioDownmix = AudioDownmix);
    CodecSetAudioDownmix(ConfigAudioDownmix);
    SetupStore("AudioSoftvol", ConfigAudioSoftvol = AudioSoftvol);
    AudioSetSoftvol(ConfigAudioSoftvol);
    SetupStore("AudioNormalize", ConfigAudioNormalize = AudioNormalize);
    SetupStore("AudioMaxNormalize", ConfigAudioMaxNormalize =
	AudioMaxNormalize);
    AudioSetNormalize(ConfigAudioNormalize, ConfigAudioMaxNormalize);
    SetupStore("AudioCompression", ConfigAudioCompression = AudioCompression);
    SetupStore("AudioMaxCompression", ConfigAudioMaxCompression =
	AudioMaxCompression);
    AudioSetCompression(ConfigAudioCompression, ConfigAudioMaxCompression);
    SetupStore("AudioStereoDescent", ConfigAudioStereoDescent =
	AudioStereoDescent);
    AudioSetStereoDescent(ConfigAudioStereoDescent);
    SetupStore("AudioBufferTime", ConfigAudioBufferTime = AudioBufferTime);
    SetupStore("AudioAutoAES", ConfigAudioAutoAES = AudioAutoAES);
    AudioSetAutoAES(ConfigAudioAutoAES);

#ifdef USE_PIP
    SetupStore("pip.X", ConfigPipX = PipX);
    SetupStore("pip.Y", ConfigPipY = PipY);
    SetupStore("pip.Width", ConfigPipWidth = PipWidth);
    SetupStore("pip.Height", ConfigPipHeight = PipHeight);
    SetupStore("pip.VideoX", ConfigPipVideoX = PipVideoX);
    SetupStore("pip.VideoY", ConfigPipVideoY = PipVideoY);
    SetupStore("pip.VideoWidth", ConfigPipVideoWidth = PipVideoWidth);
    SetupStore("pip.VideoHeight", ConfigPipVideoHeight = PipVideoHeight);
    SetupStore("pip.Alt.X", ConfigPipAltX = PipAltX);
    SetupStore("pip.Alt.Y", ConfigPipAltY = PipAltY);
    SetupStore("pip.Alt.Width", ConfigPipAltWidth = PipAltWidth);
    SetupStore("pip.Alt.Height", ConfigPipAltHeight = PipAltHeight);
    SetupStore("pip.Alt.VideoX", ConfigPipAltVideoX = PipAltVideoX);
    SetupStore("pip.Alt.VideoY", ConfigPipAltVideoY = PipAltVideoY);
    SetupStore("pip.Alt.VideoWidth", ConfigPipAltVideoWidth =
	PipAltVideoWidth);
    SetupStore("pip.Alt.VideoHeight", ConfigPipAltVideoHeight =
	PipAltVideoHeight);
#endif
}

//////////////////////////////////////////////////////////////////////////////
//	cPlayer
//////////////////////////////////////////////////////////////////////////////

/**
**	Dummy player for suspend mode.
*/
class cSoftHdPlayer:public cPlayer
{
  protected:
  public:
    cSoftHdPlayer(void);
    virtual ~ cSoftHdPlayer();
};

cSoftHdPlayer::cSoftHdPlayer(void)
{
}

cSoftHdPlayer::~cSoftHdPlayer()
{
    Detach();
}

//////////////////////////////////////////////////////////////////////////////
//	cControl
//////////////////////////////////////////////////////////////////////////////

/**
**	Dummy control class for suspend mode.
*/
class cSoftHdControl:public cControl
{
  public:
    static cSoftHdPlayer *Player;	///< dummy player
    virtual void Hide(void)		///< hide control
    {
    }
    virtual eOSState ProcessKey(eKeys);	///< process input events

    cSoftHdControl(void);		///< control constructor

    virtual ~ cSoftHdControl();		///< control destructor
};

cSoftHdPlayer *cSoftHdControl::Player;	///< dummy player instance

/**
**	Handle a key event.
**
**	@param key	key pressed
*/
eOSState cSoftHdControl::ProcessKey(eKeys key)
{
    if (SuspendMode == SUSPEND_NORMAL && (!ISMODELESSKEY(key)
	    || key == kMenu || key == kBack || key == kStop)) {
	delete Player;

	Player = NULL;
	Resume();
	SuspendMode = NOT_SUSPENDED;
	return osEnd;
    }
    return osContinue;
}

/**
**	Player control constructor.
*/
cSoftHdControl::cSoftHdControl(void)
:  cControl(Player = new cSoftHdPlayer)
{
}

/**
**	Player control destructor.
*/
cSoftHdControl::~cSoftHdControl()
{
    delete Player;

    Player = NULL;
    // loose control resume
    if (SuspendMode == SUSPEND_NORMAL) {
	Resume();
	SuspendMode = NOT_SUSPENDED;
    }

    dsyslog("[softhddev]%s: dummy player stopped\n", __FUNCTION__);
}

//////////////////////////////////////////////////////////////////////////////
//	PIP
//////////////////////////////////////////////////////////////////////////////

#ifdef USE_PIP

extern "C" void DelPip(void);		///< remove PIP
static int PipAltPosition;		///< flag alternative position

//////////////////////////////////////////////////////////////////////////////
//	cReceiver
//////////////////////////////////////////////////////////////////////////////

#include <vdr/receiver.h>

/**
**	Receiver class for PIP mode.
*/
class cSoftReceiver:public cReceiver
{
  protected:
    virtual void Activate(bool);
    virtual void Receive(uchar *, int);
  public:
     cSoftReceiver(const cChannel *);	///< receiver constructor
     virtual ~ cSoftReceiver();		///< receiver destructor
};

/**
**	Receiver constructor.
**
**	@param channel	channel to receive
*/
cSoftReceiver::cSoftReceiver(const cChannel * channel):cReceiver(NULL,
    MINPRIORITY)
{
    // cReceiver::channelID not setup, this can cause trouble
    // we want video only
    AddPid(channel->Vpid());
}

/**
**	Receiver destructor.
*/
cSoftReceiver::~cSoftReceiver()
{
    Detach();
}

/**
**	Called before the receiver gets attached or detached.
**
**	@param on	flag attached, detached
*/
void cSoftReceiver::Activate(bool on)
{
    if (on) {
	int width;
	int height;
	double video_aspect;

	GetOsdSize(&width, &height, &video_aspect);
	if (PipAltPosition) {
	    PipStart((ConfigPipAltVideoX * width) / 100,
		(ConfigPipAltVideoY * height) / 100,
		ConfigPipAltVideoWidth ? (ConfigPipAltVideoWidth * width) /
		100 : width,
		ConfigPipAltVideoHeight ? (ConfigPipAltVideoHeight * height) /
		100 : height, (ConfigPipAltX * width) / 100,
		(ConfigPipAltY * height) / 100,
		ConfigPipAltWidth ? (ConfigPipAltWidth * width) / 100 : width,
		ConfigPipAltHeight ? (ConfigPipAltHeight * height) /
		100 : height);
	} else {
	    PipStart((ConfigPipVideoX * width) / 100,
		(ConfigPipVideoY * height) / 100,
		ConfigPipVideoWidth ? (ConfigPipVideoWidth * width) /
		100 : width,
		ConfigPipVideoHeight ? (ConfigPipVideoHeight * height) /
		100 : height, (ConfigPipX * width) / 100,
		(ConfigPipY * height) / 100,
		ConfigPipWidth ? (ConfigPipWidth * width) / 100 : width,
		ConfigPipHeight ? (ConfigPipHeight * height) / 100 : height);
	}
    } else {
	PipStop();
    }
}

///
///	Parse packetized elementary stream.
///
///	@param data	payload data of transport stream
///	@param size	number of payload data bytes
///	@param is_start flag, start of pes packet
///
static void PipPesParse(const uint8_t * data, int size, int is_start)
{
    static uint8_t *pes_buf;
    static int pes_size;
    static int pes_index;

    // FIXME: quick&dirty

    if (!pes_buf) {
	pes_size = 500 * 1024 * 1024;
	pes_buf = (uint8_t *) malloc(pes_size);
	if (!pes_buf) {			// out of memory, should never happen
	    return;
	}
	pes_index = 0;
    }
    if (is_start) {			// start of pes packet
	if (pes_index) {
	    if (0) {
		fprintf(stderr, "pip: PES packet %8d %02x%02x\n", pes_index,
		    pes_buf[2], pes_buf[3]);
	    }
	    if (pes_buf[0] || pes_buf[1] || pes_buf[2] != 0x01) {
		// FIXME: first should always fail
		esyslog(tr("[softhddev]pip: invalid PES packet %d\n"),
		    pes_index);
	    } else {
		PipPlayVideo(pes_buf, pes_index);
		// FIXME: buffer full: pes packet is dropped
	    }
	    pes_index = 0;
	}
    }

    if (pes_index + size > pes_size) {
	esyslog(tr("[softhddev]pip: pes buffer too small\n"));
	pes_size *= 2;
	if (pes_index + size > pes_size) {
	    pes_size = (pes_index + size) * 2;
	}
	pes_buf = (uint8_t *) realloc(pes_buf, pes_size);
	if (!pes_buf) {			// out of memory, should never happen
	    return;
	}
    }
    memcpy(pes_buf + pes_index, data, size);
    pes_index += size;
}

    /// Transport stream packet size
#define TS_PACKET_SIZE	188
    /// Transport stream packet sync byte
#define TS_PACKET_SYNC	0x47

/**
**	Receive TS packet from device.
**
**	@param data	ts packet
**	@param size	size (#TS_PACKET_SIZE=188) of tes packet
*/
void cSoftReceiver::Receive(uchar * data, int size)
{
    const uint8_t *p;

    p = data;
    while (size >= TS_PACKET_SIZE) {
	int payload;

	if (p[0] != TS_PACKET_SYNC) {
	    esyslog(tr("[softhddev]tsdemux: transport stream out of sync\n"));
	    // FIXME: kill all buffers
	    return;
	}
	if (p[1] & 0x80) {		// error indicatord
	    dsyslog("[softhddev]tsdemux: transport error\n");
	    // FIXME: kill all buffers
	    goto next_packet;
	}
	if (0) {
	    int pid;

	    pid = (p[1] & 0x1F) << 8 | p[2];
	    fprintf(stderr, "tsdemux: PID: %#04x%s%s\n", pid,
		p[1] & 0x40 ? " start" : "", p[3] & 0x10 ? " payload" : "");
	}
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
		    dsyslog
			("[softhddev]tsdemux: illegal adaption field length\n");
		    goto next_packet;
		}
		break;
	}

	PipPesParse(p + payload, TS_PACKET_SIZE - payload, p[1] & 0x40);

      next_packet:
	p += TS_PACKET_SIZE;
	size -= TS_PACKET_SIZE;
    }
}

//////////////////////////////////////////////////////////////////////////////

static cSoftReceiver *PipReceiver;	///< PIP receiver
static int PipChannelNr;		///< last PIP channel number
static const cChannel *PipChannel;	///< current PIP channel

/**
**	Stop PIP.
*/
extern "C" void DelPip(void)
{
    delete PipReceiver;

    PipReceiver = NULL;
    PipChannel = NULL;
}

/**
**	Prepare new PIP.
**
**	@param channel_nr	channel number
*/
static void NewPip(int channel_nr)
{
    const cChannel *channel;
    cDevice *device;
    cSoftReceiver *receiver;

#ifdef DEBUG
    // is device replaying?
    if (cDevice::PrimaryDevice()->Replaying() && cControl::Control()) {
	dsyslog("[softhddev]%s: replay active\n", __FUNCTION__);
	// FIXME: need to find PID
    }
#endif

    if (!channel_nr) {
	channel_nr = cDevice::CurrentChannel();
    }
    if (channel_nr && (channel = Channels.GetByNumber(channel_nr))
	&& (device = cDevice::GetDevice(channel, 0, false, false))) {

	DelPip();

	device->SwitchChannel(channel, false);
	receiver = new cSoftReceiver(channel);
	device->AttachReceiver(receiver);
	PipReceiver = receiver;
	PipChannel = channel;
	PipChannelNr = channel_nr;
    }
}

/**
**	Toggle PIP on/off.
*/
static void TogglePip(void)
{
    if (PipReceiver) {
	int attached;

	attached = PipReceiver->IsAttached();
	DelPip();
	if (attached) {			// turn off only if last PIP was on
	    return;
	}
    }
    NewPip(PipChannelNr);
}

/**
**	Switch PIP to next available channel.
**
**	@param direction	direction of channel switch
*/
static void PipNextAvailableChannel(int direction)
{
    const cChannel *channel;
    const cChannel *first;

    channel = PipChannel;
    first = channel;

    DelPip();				// disable PIP to free the device

    while (channel) {
	bool ndr;
	cDevice *device;

	channel = direction > 0 ? Channels.Next(channel)
	    : Channels.Prev(channel);
	if (!channel && Setup.ChannelsWrap) {
	    channel = direction > 0 ? Channels.First() : Channels.Last();
	}
	if (channel && !channel->GroupSep()
	    && (device = cDevice::GetDevice(channel, 0, false, true))
	    && device->ProvidesChannel(channel, 0, &ndr) && !ndr) {

	    NewPip(channel->Number());
	    return;
	}
	if (channel == first) {
	    Skins.Message(mtError, tr("Channel not available!"));
	    break;
	}
    }
}

/**
**	Swap PIP channels.
*/
static void SwapPipChannels(void)
{
    const cChannel *channel;

    channel = PipChannel;

    DelPip();
    NewPip(0);

    if (channel) {
	Channels.SwitchTo(channel->Number());
    }
}

/**
**	Swap PIP position.
*/
static void SwapPipPosition(void)
{
    int width;
    int height;
    double video_aspect;

    PipAltPosition ^= 1;
    if (!PipReceiver) {			// no PIP visible, no update needed
	return;
    }

    GetOsdSize(&width, &height, &video_aspect);
    if (PipAltPosition) {
	PipSetPosition((ConfigPipAltVideoX * width) / 100,
	    (ConfigPipAltVideoY * height) / 100,
	    ConfigPipAltVideoWidth ? (ConfigPipAltVideoWidth * width) /
	    100 : width,
	    ConfigPipAltVideoHeight ? (ConfigPipAltVideoHeight * height) /
	    100 : height, (ConfigPipAltX * width) / 100,
	    (ConfigPipAltY * height) / 100,
	    ConfigPipAltWidth ? (ConfigPipAltWidth * width) / 100 : width,
	    ConfigPipAltHeight ? (ConfigPipAltHeight * height) / 100 : height);
    } else {
	PipSetPosition((ConfigPipVideoX * width) / 100,
	    (ConfigPipVideoY * height) / 100,
	    ConfigPipVideoWidth ? (ConfigPipVideoWidth * width) / 100 : width,
	    ConfigPipVideoHeight ? (ConfigPipVideoHeight * height) /
	    100 : height, (ConfigPipX * width) / 100,
	    (ConfigPipY * height) / 100,
	    ConfigPipWidth ? (ConfigPipWidth * width) / 100 : width,
	    ConfigPipHeight ? (ConfigPipHeight * height) / 100 : height);
    }
}

#endif

//////////////////////////////////////////////////////////////////////////////
//	cOsdMenu
//////////////////////////////////////////////////////////////////////////////

/**
**	Hotkey parsing state machine.
*/
typedef enum
{
    HksInitial,				///< initial state
    HksBlue,				///< blue button pressed
    HksBlue1,				///< blue and 1 number pressed
    HksRed,				///< red button pressed
} HkState;

/**
**	Soft device plugin menu class.
*/
class cSoftHdMenu:public cOsdMenu
{
  private:
    HkState HotkeyState;		///< current hot-key state
    int HotkeyCode;			///< current hot-key code
    void Create(void);			///< create plugin main menu
  public:
    cSoftHdMenu(const char *, int = 0, int = 0, int = 0, int = 0, int = 0);
    virtual ~ cSoftHdMenu();
    virtual eOSState ProcessKey(eKeys);
};

/**
**	Create main menu.
*/
void cSoftHdMenu::Create(void)
{
    int current;
    int missed;
    int duped;
    int dropped;
    int counter;

    current = Current();		// get current menu item index
    Clear();				// clear the menu

    SetHasHotkeys();

    if (ConfigDetachFromMainMenu) {
	Add(new cOsdItem(hk(tr("Detach SoftHdDevice")), osUser1));
    } else {
	Add(new cOsdItem(hk(tr("Suspend SoftHdDevice")), osUser1));
    }
#ifdef USE_PIP
    if (PipReceiver) {
	Add(new cOsdItem(hk(tr("PIP toggle on/off: off")), osUser2));
    } else {
	Add(new cOsdItem(hk(tr("PIP toggle on/off: on")), osUser2));
    }
    Add(new cOsdItem(hk(tr("PIP zapmode (not working)")), osUser3));
    Add(new cOsdItem(hk(tr("PIP channel +")), osUser4));
    Add(new cOsdItem(hk(tr("PIP channel -")), osUser5));
    if (PipReceiver) {
	Add(new cOsdItem(hk(tr("PIP on/swap channels: swap")), osUser6));
    } else {
	Add(new cOsdItem(hk(tr("PIP on/swap channels: on")), osUser6));
    }
    if (PipAltPosition) {
	Add(new cOsdItem(hk(tr("PIP swap position: normal")), osUser7));
    } else {
	Add(new cOsdItem(hk(tr("PIP swap position: alternative")), osUser7));
    }
    Add(new cOsdItem(hk(tr("PIP close")), osUser8));
#endif
    Add(new cOsdItem(NULL, osUnknown, false));
    Add(new cOsdItem(NULL, osUnknown, false));
    GetStats(&missed, &duped, &dropped, &counter);
    Add(new
	cOsdItem(cString::sprintf(tr
		(" Frames missed(%d) duped(%d) dropped(%d) total(%d)"), missed,
		duped, dropped, counter), osUnknown, false));

    SetCurrent(Get(current));		// restore selected menu entry
    Display();				// display build menu
}

/**
**	Soft device menu constructor.
*/
cSoftHdMenu::cSoftHdMenu(const char *title, int c0, int c1, int c2, int c3,
    int c4)
:cOsdMenu(title, c0, c1, c2, c3, c4)
{
    HotkeyState = HksInitial;

    Create();
}

/**
**	Soft device menu destructor.
*/
cSoftHdMenu::~cSoftHdMenu()
{
}

/**
**	Handle hot key commands.
**
**	@param code	numeric hot key code
*/
static void HandleHotkey(int code)
{
    switch (code) {
	case 10:			// disable pass-through
	    AudioPassthroughState = 0;
	    CodecSetAudioPassthrough(0);
	    Skins.QueueMessage(mtInfo, tr("pass-through disabled"));
	    break;
	case 11:			// enable pass-through
	    // note: you can't enable, without configured pass-through
	    AudioPassthroughState = 1;
	    CodecSetAudioPassthrough(ConfigAudioPassthrough);
	    Skins.QueueMessage(mtInfo, tr("pass-through enabled"));
	    break;
	case 12:			// toggle pass-through
	    AudioPassthroughState ^= 1;
	    if (AudioPassthroughState) {
		CodecSetAudioPassthrough(ConfigAudioPassthrough);
		Skins.QueueMessage(mtInfo, tr("pass-through enabled"));
	    } else {
		CodecSetAudioPassthrough(0);
		Skins.QueueMessage(mtInfo, tr("pass-through disabled"));
	    }
	    break;
	case 13:			// decrease audio delay
	    ConfigVideoAudioDelay -= 10;
	    VideoSetAudioDelay(ConfigVideoAudioDelay);
	    Skins.QueueMessage(mtInfo,
		cString::sprintf(tr("audio delay changed to %d"),
		    ConfigVideoAudioDelay));
	    break;
	case 14:			// increase audio delay
	    ConfigVideoAudioDelay += 10;
	    VideoSetAudioDelay(ConfigVideoAudioDelay);
	    Skins.QueueMessage(mtInfo,
		cString::sprintf(tr("audio delay changed to %d"),
		    ConfigVideoAudioDelay));
	    break;

	case 20:			// disable full screen
	    VideoSetFullscreen(0);
	    break;
	case 21:			// enable full screen
	    VideoSetFullscreen(1);
	    break;
	case 22:			// toggle full screen
	    VideoSetFullscreen(-1);
	    break;
	case 23:			// disable auto-crop
	    ConfigAutoCropEnabled = 0;
	    VideoSetAutoCrop(0, ConfigAutoCropDelay, ConfigAutoCropTolerance);
	    Skins.QueueMessage(mtInfo, tr("auto-crop disabled and freezed"));
	    break;
	case 24:			// enable auto-crop
	    ConfigAutoCropEnabled = 1;
	    if (!ConfigAutoCropInterval) {
		ConfigAutoCropInterval = 50;
	    }
	    VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay,
		ConfigAutoCropTolerance);
	    Skins.QueueMessage(mtInfo, tr("auto-crop enabled"));
	    break;
	case 25:			// toggle auto-crop
	    ConfigAutoCropEnabled ^= 1;
	    // no interval configured, use some default
	    if (!ConfigAutoCropInterval) {
		ConfigAutoCropInterval = 50;
	    }
	    VideoSetAutoCrop(ConfigAutoCropEnabled * ConfigAutoCropInterval,
		ConfigAutoCropDelay, ConfigAutoCropTolerance);
	    if (ConfigAutoCropEnabled) {
		Skins.QueueMessage(mtInfo, tr("auto-crop enabled"));
	    } else {
		Skins.QueueMessage(mtInfo,
		    tr("auto-crop disabled and freezed"));
	    }
	    break;
	case 30:			// change 4:3 -> window mode
	case 31:
	case 32:
	    VideoSet4to3DisplayFormat(code - 30);
	    break;
	case 39:			// rotate 4:3 -> window mode
	    VideoSet4to3DisplayFormat(-1);
	    break;
	case 40:			// change 16:9 -> window mode
	case 41:
	case 42:
	    VideoSetOtherDisplayFormat(code - 40);
	    break;
	case 49:			// rotate 16:9 -> window mode
	    VideoSetOtherDisplayFormat(-1);
	    break;

#ifdef USE_PIP
	case 102:			// PIP toggle
	    TogglePip();
	    break;
	case 104:
	    PipNextAvailableChannel(1);
	    break;
	case 105:
	    PipNextAvailableChannel(-1);
	    break;
	case 106:
	    SwapPipChannels();
	    break;
	case 107:
	    SwapPipPosition();
	    break;
	case 108:
	    DelPip();
	    PipChannelNr = 0;
	    break;
#endif

	default:
	    esyslog(tr("[softhddev]: hot key %d is not supported\n"), code);
	    break;
    }
}

/**
**	Handle key event.
**
**	@param key	key event
*/
eOSState cSoftHdMenu::ProcessKey(eKeys key)
{
    eOSState state;

    //dsyslog("[softhddev]%s: %x\n", __FUNCTION__, key);

    switch (HotkeyState) {
	case HksInitial:		// initial state, waiting for hot key
	    if (key == kBlue) {
		HotkeyState = HksBlue;	// blue button
		return osContinue;
	    }
	    if (key == kRed) {
		HotkeyState = HksRed;	// red button
		return osContinue;
	    }
	    break;
	case HksBlue:			// blue and first number
	    if (k0 <= key && key <= k9) {
		HotkeyCode = key - k0;
		HotkeyState = HksBlue1;
		return osContinue;
	    }
	    HotkeyState = HksInitial;
	    break;
	case HksBlue1:			// blue and second number/enter
	    if (k0 <= key && key <= k9) {
		HotkeyCode *= 10;
		HotkeyCode += key - k0;
		HotkeyState = HksInitial;
		dsyslog("[softhddev]%s: hot-key %d\n", __FUNCTION__,
		    HotkeyCode);
		HandleHotkey(HotkeyCode);
		return osEnd;
	    }
	    if (key == kOk) {
		HotkeyState = HksInitial;
		dsyslog("[softhddev]%s: hot-key %d\n", __FUNCTION__,
		    HotkeyCode);
		HandleHotkey(HotkeyCode);
		return osEnd;
	    }
	    HotkeyState = HksInitial;
	case HksRed:			// red and first number
	    if (k0 <= key && key <= k9) {
		HotkeyCode = 100 + key - k0;
		HotkeyState = HksInitial;
		HandleHotkey(HotkeyCode);
		return osEnd;
	    }
	    HotkeyState = HksInitial;
	    break;
    }

    // call standard function
    state = cOsdMenu::ProcessKey(key);

    switch (state) {
	case osUser1:
	    // not already suspended
	    if (SuspendMode == NOT_SUSPENDED && !cSoftHdControl::Player) {
		cControl::Launch(new cSoftHdControl);
		cControl::Attach();
		if (ConfigDetachFromMainMenu) {
		    Suspend(1, 1, 0);
		    SuspendMode = SUSPEND_DETACHED;
		} else {
		    Suspend(ConfigSuspendClose, ConfigSuspendClose,
			ConfigSuspendX11);
		    SuspendMode = SUSPEND_NORMAL;
		}
		if (ShutdownHandler.GetUserInactiveTime()) {
		    dsyslog("[softhddev]%s: set user inactive\n",
			__FUNCTION__);
		    ShutdownHandler.SetUserInactive();
		}
	    }
	    return osEnd;
#ifdef USE_PIP
	case osUser2:
	    TogglePip();
	    return osEnd;
	case osUser4:
	    PipNextAvailableChannel(1);
	    return osEnd;
	case osUser5:
	    PipNextAvailableChannel(-1);
	    return osEnd;
	case osUser6:
	    SwapPipChannels();
	    return osEnd;
	case osUser7:
	    SwapPipPosition();
	    return osEnd;
	case osUser8:
	    DelPip();
	    PipChannelNr = 0;
	    return osEnd;
#endif
	default:
	    Create();
	    break;
    }
    return state;
}

//////////////////////////////////////////////////////////////////////////////
//	cDevice
//////////////////////////////////////////////////////////////////////////////

class cSoftHdDevice:public cDevice
{
  public:
    cSoftHdDevice(void);
    virtual ~ cSoftHdDevice(void);

    virtual bool HasDecoder(void) const;
    virtual bool CanReplay(void) const;
    virtual bool SetPlayMode(ePlayMode);
#if APIVERSNUM >= 20103
    virtual void TrickSpeed(int, bool);
#else
    virtual void TrickSpeed(int);
#endif
    virtual void Clear(void);
    virtual void Play(void);
    virtual void Freeze(void);
    virtual void Mute(void);
    virtual void StillPicture(const uchar *, int);
    virtual bool Poll(cPoller &, int = 0);
    virtual bool Flush(int = 0);
    virtual int64_t GetSTC(void);
#if APIVERSNUM >= 10733
    virtual cRect CanScaleVideo(const cRect &, int = taCenter);
    virtual void ScaleVideo(const cRect & = cRect::Null);
#endif
    virtual void SetVideoDisplayFormat(eVideoDisplayFormat);
    virtual void SetVideoFormat(bool);
    virtual void GetVideoSize(int &, int &, double &);
    virtual void GetOsdSize(int &, int &, double &);
    virtual int PlayVideo(const uchar *, int);
    virtual int PlayAudio(const uchar *, int, uchar);
#ifdef USE_TS_VIDEO
    virtual int PlayTsVideo(const uchar *, int);
#endif
#if !defined(USE_AUDIO_THREAD) || !defined(NO_TS_AUDIO)
    virtual int PlayTsAudio(const uchar *, int);
#endif
    virtual void SetAudioChannelDevice(int);
    virtual int GetAudioChannelDevice(void);
    virtual void SetDigitalAudioDevice(bool);
    virtual void SetAudioTrackDevice(eTrackType);
    virtual void SetVolumeDevice(int);

// Image Grab facilities

    virtual uchar *GrabImage(int &, bool, int, int, int);

#ifdef USE_VDR_SPU
// SPU facilities
  private:
    cDvbSpuDecoder * spuDecoder;
  public:
    virtual cSpuDecoder * GetSpuDecoder(void);
#endif

  protected:
    virtual void MakePrimaryDevice(bool);
};

/**
**	Constructor device.
*/
cSoftHdDevice::cSoftHdDevice(void)
{
    //dsyslog("[softhddev]%s\n", __FUNCTION__);

#ifdef USE_VDR_SPU
    spuDecoder = NULL;
#endif
}

/**
**	Destructor device.
*/
cSoftHdDevice::~cSoftHdDevice(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
#ifdef USE_VDR_SPU
    delete spuDecoder;
#endif
}

/**
**	Informs a device that it will be the primary device.
**
**	@param on	flag if becoming or loosing primary
*/
void cSoftHdDevice::MakePrimaryDevice(bool on)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, on);

    cDevice::MakePrimaryDevice(on);
    if (on) {
	new cSoftOsdProvider();

	if (SuspendMode == SUSPEND_DETACHED) {
	    Resume();
	    SuspendMode = NOT_SUSPENDED;
	}
    } else if (SuspendMode == NOT_SUSPENDED) {
	Suspend(1, 1, 0);
	SuspendMode = SUSPEND_DETACHED;
    }
}

#ifdef USE_VDR_SPU

/**
**	Get the device SPU decoder.
**
**	@returns a pointer to the device's SPU decoder (or NULL, if this
**	device doesn't have an SPU decoder)
*/
cSpuDecoder *cSoftHdDevice::GetSpuDecoder(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (!spuDecoder && IsPrimaryDevice()) {
	spuDecoder = new cDvbSpuDecoder();
    }
    return spuDecoder;
}

#endif

/**
**	Tells whether this device has a MPEG decoder.
*/
bool cSoftHdDevice::HasDecoder(void) const
{
    return true;
}

/**
**	Returns true if this device can currently start a replay session.
*/
bool cSoftHdDevice::CanReplay(void) const
{
    return true;
}

/**
**	Sets the device into the given play mode.
**
**	@param play_mode	new play mode (Audio/Video/External...)
*/
bool cSoftHdDevice::SetPlayMode(ePlayMode play_mode)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, play_mode);

    switch (play_mode) {
	case pmAudioVideo:
	    break;
	case pmAudioOnly:
	case pmAudioOnlyBlack:
	    break;
	case pmVideoOnly:
	    break;
	case pmNone:
	    break;
	case pmExtern_THIS_SHOULD_BE_AVOIDED:
	    dsyslog("[softhddev] play mode external\n");
	    // FIXME: what if already suspended?
	    Suspend(1, 1, 0);
	    SuspendMode = SUSPEND_EXTERNAL;
	    return true;
	default:
	    dsyslog("[softhddev] playmode not implemented... %d\n", play_mode);
	    break;
    }

    if (SuspendMode != NOT_SUSPENDED) {
	if (SuspendMode != SUSPEND_EXTERNAL) {
	    return false;
	}
	Resume();
	SuspendMode = NOT_SUSPENDED;
    }

    return::SetPlayMode(play_mode);
}

/**
**	Gets the current System Time Counter, which can be used to
**	synchronize audio, video and subtitles.
*/
int64_t cSoftHdDevice::GetSTC(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return::GetSTC();
}

/**
**	Set trick play speed.
**
**	Every single frame shall then be displayed the given number of
**	times.
**
**	@param speed	trick speed
**	@param forward	flag forward direction
*/
#if APIVERSNUM >= 20103
void cSoftHdDevice::TrickSpeed(int speed, bool forward)
{
    dsyslog("[softhddev]%s: %d $d\n", __FUNCTION__, speed, forward);

    ::TrickSpeed(speed);
}
#else
void cSoftHdDevice::TrickSpeed(int speed)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, speed);

    ::TrickSpeed(speed);
}
#endif

/**
**	Clears all video and audio data from the device.
*/
void cSoftHdDevice::Clear(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Clear();
    ::Clear();
}

/**
**	Sets the device into play mode (after a previous trick mode)
*/
void cSoftHdDevice::Play(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Play();
    ::Play();
}

/**
**	Puts the device into "freeze frame" mode.
*/
void cSoftHdDevice::Freeze(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Freeze();
    ::Freeze();
}

/**
**	Turns off audio while replaying.
*/
void cSoftHdDevice::Mute(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Mute();
    ::Mute();
}

/**
**	Display the given I-frame as a still picture.
**
**	@param data	pes or ts data of a frame
**	@param length	length of data area
*/
void cSoftHdDevice::StillPicture(const uchar * data, int length)
{
    dsyslog("[softhddev]%s: %s %p %d\n", __FUNCTION__,
	data[0] == 0x47 ? "ts" : "pes", data, length);

    if (data[0] == 0x47) {		// ts sync
	cDevice::StillPicture(data, length);
	return;
    }

    ::StillPicture(data, length);
}

/**
**	Check if the device is ready for further action.
**
**	@param poller		file handles (unused)
**	@param timeout_ms	timeout in ms to become ready
**
**	@retval true	if ready
**	@retval false	if busy
*/
bool cSoftHdDevice::Poll(
    __attribute__ ((unused)) cPoller & poller, int timeout_ms)
{
    //dsyslog("[softhddev]%s: %d\n", __FUNCTION__, timeout_ms);

    return::Poll(timeout_ms);
}

/**
**	Flush the device output buffers.
**
**	@param timeout_ms	timeout in ms to become ready
*/
bool cSoftHdDevice::Flush(int timeout_ms)
{
    dsyslog("[softhddev]%s: %d ms\n", __FUNCTION__, timeout_ms);

    return::Flush(timeout_ms);
}

// ----------------------------------------------------------------------------

/**
**	Sets the video display format to the given one (only useful if this
**	device has an MPEG decoder).
*/
void cSoftHdDevice:: SetVideoDisplayFormat(eVideoDisplayFormat
    video_display_format)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, video_display_format);

    cDevice::SetVideoDisplayFormat(video_display_format);
#if 0
    static int last = -1;

    // called on every channel switch, no need to kill osd...
    if (last != video_display_format) {
	last = video_display_format;

	::VideoSetDisplayFormat(video_display_format);
	cSoftOsd::Dirty = 1;
    }
#endif
}

/**
**	Sets the output video format to either 16:9 or 4:3 (only useful
**	if this device has an MPEG decoder).
**
**	Should call SetVideoDisplayFormat.
**
**	@param video_format16_9	flag true 16:9.
*/
void cSoftHdDevice::SetVideoFormat(bool video_format16_9)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, video_format16_9);

    // FIXME: 4:3 / 16:9 video format not supported.

    SetVideoDisplayFormat(eVideoDisplayFormat(Setup.VideoDisplayFormat));
}

/**
**	Returns the width, height and video_aspect ratio of the currently
**	displayed video material.
**
**	@note the video_aspect is used to scale the subtitle.
*/
void cSoftHdDevice::GetVideoSize(int &width, int &height, double &video_aspect)
{
    ::GetVideoSize(&width, &height, &video_aspect);
}

/**
**	Returns the width, height and pixel_aspect ratio the OSD.
**
**	FIXME: Called every second, for nothing (no OSD displayed)?
*/
void cSoftHdDevice::GetOsdSize(int &width, int &height, double &pixel_aspect)
{
    ::GetOsdSize(&width, &height, &pixel_aspect);
}

// ----------------------------------------------------------------------------

/**
**	Play a audio packet.
**
**	@param data	exactly one complete PES packet (which is incomplete)
**	@param length	length of PES packet
**	@param id	type of audio data this packet holds
*/
int cSoftHdDevice::PlayAudio(const uchar * data, int length, uchar id)
{
    //dsyslog("[softhddev]%s: %p %p %d %d\n", __FUNCTION__, this, data, length, id);

    return::PlayAudio(data, length, id);
}

void cSoftHdDevice::SetAudioTrackDevice(
    __attribute__ ((unused)) eTrackType type)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

void cSoftHdDevice::SetDigitalAudioDevice( __attribute__ ((unused)) bool on)
{
    //dsyslog("[softhddev]%s: %s\n", __FUNCTION__, on ? "true" : "false");
}

void cSoftHdDevice::SetAudioChannelDevice( __attribute__ ((unused))
    int audio_channel)
{
    //dsyslog("[softhddev]%s: %d\n", __FUNCTION__, audio_channel);
}

int cSoftHdDevice::GetAudioChannelDevice(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
    return 0;
}

/**
**	Sets the audio volume on this device (Volume = 0...255).
**
**	@param volume	device volume
*/
void cSoftHdDevice::SetVolumeDevice(int volume)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, volume);

    ::SetVolumeDevice(volume);
}

// ----------------------------------------------------------------------------

/**
**	Play a video packet.
**
**	@param data	exactly one complete PES packet (which is incomplete)
**	@param length	length of PES packet
*/
int cSoftHdDevice::PlayVideo(const uchar * data, int length)
{
    //dsyslog("[softhddev]%s: %p %d\n", __FUNCTION__, data, length);
    return::PlayVideo(data, length);
}

#ifdef USE_TS_VIDEO

/**
**	Play a TS video packet.
**
**	@param data	ts data buffer
**	@param length	ts packet length (188)
*/
int cSoftHdDevice::PlayTsVideo(const uchar * data, int length)
{
}

#endif

#if !defined(USE_AUDIO_THREAD) || !defined(NO_TS_AUDIO)

/**
**	Play a TS audio packet.
**
**	@param data	ts data buffer
**	@param length	ts packet length (188)
*/
int cSoftHdDevice::PlayTsAudio(const uchar * data, int length)
{
#ifndef NO_TS_AUDIO
    return::PlayTsAudio(data, length);
#else
    AudioPoller();

    return cDevice::PlayTsAudio(data, length);
#endif
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
uchar *cSoftHdDevice::GrabImage(int &size, bool jpeg, int quality, int width,
    int height)
{
    dsyslog("[softhddev]%s: %d, %d, %d, %dx%d\n", __FUNCTION__, size, jpeg,
	quality, width, height);

    if (SuspendMode != NOT_SUSPENDED) {
	return NULL;
    }
    if (quality < 0) {			// caller should care, but fix it
	quality = 95;
    }

    return::GrabImage(&size, jpeg, quality, width, height);
}

#if APIVERSNUM >= 10733

/**
**	Ask the output, if it can scale video.
**
**	@param rect	requested video window rectangle
**
**	@returns the real rectangle or cRect:Null if invalid.
*/
cRect cSoftHdDevice::CanScaleVideo(const cRect & rect,
    __attribute__ ((unused)) int alignment)
{
    return rect;
}

/**
**	Scale the currently shown video.
**
**	@param rect	video window rectangle
*/
void cSoftHdDevice::ScaleVideo(const cRect & rect)
{
#ifdef OSD_DEBUG
    dsyslog("[softhddev]%s: %dx%d%+d%+d\n", __FUNCTION__, rect.Width(),
	rect.Height(), rect.X(), rect.Y());
#endif
    ::ScaleVideo(rect.X(), rect.Y(), rect.Width(), rect.Height());
}

#endif

/**
**	Call rgb to jpeg for C Plugin.
*/
extern "C" uint8_t * CreateJpeg(uint8_t * image, int *size, int quality,
    int width, int height)
{
    return (uint8_t *) RgbToJpeg((uchar *) image, width, height, *size,
	quality);
}

//////////////////////////////////////////////////////////////////////////////
//	cPlugin
//////////////////////////////////////////////////////////////////////////////

class cPluginSoftHdDevice:public cPlugin
{
  public:
    cPluginSoftHdDevice(void);
    virtual ~ cPluginSoftHdDevice(void);
    virtual const char *Version(void);
    virtual const char *Description(void);
    virtual const char *CommandLineHelp(void);
    virtual bool ProcessArgs(int, char *[]);
    virtual bool Initialize(void);
    virtual bool Start(void);
    virtual void Stop(void);
    virtual void Housekeeping(void);
    virtual void MainThreadHook(void);
    virtual const char *MainMenuEntry(void);
    virtual cOsdObject *MainMenuAction(void);
    virtual cMenuSetupPage *SetupMenu(void);
    virtual bool SetupParse(const char *, const char *);
    virtual bool Service(const char *, void * = NULL);
    virtual const char **SVDRPHelpPages(void);
    virtual cString SVDRPCommand(const char *, const char *, int &);
};

/**
**	Initialize any member variables here.
**
**	@note DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
**	VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
*/
cPluginSoftHdDevice::cPluginSoftHdDevice(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

/**
**	Clean up after yourself!
*/
cPluginSoftHdDevice::~cPluginSoftHdDevice(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    ::SoftHdDeviceExit();

    // keep ConfigX11Display ...
}

/**
**	Return plugin version number.
**
**	@returns version number as constant string.
*/
const char *cPluginSoftHdDevice::Version(void)
{
    return VERSION;
}

/**
**	Return plugin short description.
**
**	@returns short description as constant string.
*/
const char *cPluginSoftHdDevice::Description(void)
{
    return tr(DESCRIPTION);
}

/**
**	Return a string that describes all known command line options.
**
**	@returns command line help as constant string.
*/
const char *cPluginSoftHdDevice::CommandLineHelp(void)
{
    return::CommandLineHelp();
}

/**
**	Process the command line arguments.
*/
bool cPluginSoftHdDevice::ProcessArgs(int argc, char *argv[])
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return::ProcessArgs(argc, argv);
}

/**
**	Initializes the DVB devices.
**
**	Must be called before accessing any DVB functions.
**
**	@returns true if any devices are available.
*/
bool cPluginSoftHdDevice::Initialize(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    MyDevice = new cSoftHdDevice();

    return true;
}

/**
**	 Start any background activities the plugin shall perform.
*/
bool cPluginSoftHdDevice::Start(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (!MyDevice->IsPrimaryDevice()) {
	isyslog("[softhddev] softhddevice %d is not the primary device!",
	    MyDevice->DeviceNumber());
	if (ConfigMakePrimary) {
	    // Must be done in the main thread
	    dsyslog("[softhddev] makeing softhddevice %d the primary device!",
		MyDevice->DeviceNumber());
	    DoMakePrimary = MyDevice->DeviceNumber() + 1;
	}
    }

    switch (::Start()) {
	case 1:
	    //cControl::Launch(new cSoftHdControl);
	    //cControl::Attach();
	    // FIXME: VDR overwrites the control
	    SuspendMode = SUSPEND_NORMAL;
	    break;
	case -1:
	    SuspendMode = SUSPEND_DETACHED;
	    break;
	case 0:
	default:
	    break;
    }

    return true;
}

/**
**	Shutdown plugin.  Stop any background activities the plugin is
**	performing.
*/
void cPluginSoftHdDevice::Stop(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    ::Stop();
}

/**
**	Perform any cleanup or other regular tasks.
*/
void cPluginSoftHdDevice::Housekeeping(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    // check if user is inactive, automatic enter suspend mode
    // FIXME: cControl prevents shutdown, disable this until fixed
    if (0 && SuspendMode == NOT_SUSPENDED && ShutdownHandler.IsUserInactive()) {
	// don't overwrite already suspended suspend mode
	cControl::Launch(new cSoftHdControl);
	cControl::Attach();
	Suspend(ConfigSuspendClose, ConfigSuspendClose, ConfigSuspendX11);
	SuspendMode = SUSPEND_NORMAL;
    }

    ::Housekeeping();
}

/**
**	Create main menu entry.
*/
const char *cPluginSoftHdDevice::MainMenuEntry(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return ConfigHideMainMenuEntry ? NULL : tr(MAINMENUENTRY);
}

/**
**	Perform the action when selected from the main VDR menu.
*/
cOsdObject *cPluginSoftHdDevice::MainMenuAction(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return new cSoftHdMenu("SoftHdDevice");
}

/**
**	Called for every plugin once during every cycle of VDR's main program
**	loop.
*/
void cPluginSoftHdDevice::MainThreadHook(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (DoMakePrimary) {
	dsyslog("[softhddev]%s: switching primary device to %d\n",
	    __FUNCTION__, DoMakePrimary);
	cDevice::SetPrimaryDevice(DoMakePrimary);
	DoMakePrimary = 0;
    }

    ::MainThreadHook();
}

/**
**	Return our setup menu.
*/
cMenuSetupPage *cPluginSoftHdDevice::SetupMenu(void)
{
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return new cMenuSetupSoft;
}

/**
**	Parse setup parameters
**
**	@param name	paramter name (case sensetive)
**	@param value	value as string
**
**	@returns true if the parameter is supported.
*/
bool cPluginSoftHdDevice::SetupParse(const char *name, const char *value)
{
    int i;

    //dsyslog("[softhddev]%s: '%s' = '%s'\n", __FUNCTION__, name, value);

    if (!strcasecmp(name, "MakePrimary")) {
	ConfigMakePrimary = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "HideMainMenuEntry")) {
	ConfigHideMainMenuEntry = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "DetachFromMainMenu")) {
	ConfigDetachFromMainMenu = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "Osd.Width")) {
	ConfigOsdWidth = atoi(value);
	VideoSetOsdSize(ConfigOsdWidth, ConfigOsdHeight);
	return true;
    }
    if (!strcasecmp(name, "Osd.Height")) {
	ConfigOsdHeight = atoi(value);
	VideoSetOsdSize(ConfigOsdWidth, ConfigOsdHeight);
	return true;
    }
    if (!strcasecmp(name, "Suspend.Close")) {
	ConfigSuspendClose = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "Suspend.X11")) {
	ConfigSuspendX11 = atoi(value);
	return true;
    }

    if (!strcasecmp(name, "Video4to3DisplayFormat")) {
	Config4to3DisplayFormat = atoi(value);
	VideoSet4to3DisplayFormat(Config4to3DisplayFormat);
	return true;
    }
    if (!strcasecmp(name, "VideoOtherDisplayFormat")) {
	ConfigOtherDisplayFormat = atoi(value);
	VideoSetOtherDisplayFormat(ConfigOtherDisplayFormat);
	return true;
    }
    if (!strcasecmp(name, "Background")) {
	VideoSetBackground(ConfigVideoBackground = strtoul(value, NULL, 0));
	return true;
    }
    if (!strcasecmp(name, "StudioLevels")) {
	VideoSetStudioLevels(ConfigVideoStudioLevels = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "60HzMode")) {
	VideoSet60HzMode(ConfigVideo60HzMode = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "SoftStartSync")) {
	VideoSetSoftStartSync(ConfigVideoSoftStartSync = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "BlackPicture")) {
	VideoSetBlackPicture(ConfigVideoBlackPicture = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "ClearOnSwitch")) {
	ConfigVideoClearOnSwitch = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "Brightness")) {
	VideoSetBrightness(ConfigVideoBrightness = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "Contrast")) {
	VideoSetContrast(ConfigVideoContrast = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "Saturation")) {
	VideoSetSaturation(ConfigVideoSaturation = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "Hue")) {
	VideoSetHue(ConfigVideoHue = atoi(value));
	return true;
    }
    for (i = 0; i < RESOLUTIONS; ++i) {
	char buf[128];

	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Scaling");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoScaling[i] = atoi(value);
	    VideoSetScaling(ConfigVideoScaling);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Deinterlace");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoDeinterlace[i] = atoi(value);
	    VideoSetDeinterlace(ConfigVideoDeinterlace);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i],
	    "SkipChromaDeinterlace");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoSkipChromaDeinterlace[i] = atoi(value);
	    VideoSetSkipChromaDeinterlace(ConfigVideoSkipChromaDeinterlace);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "InverseTelecine");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoInverseTelecine[i] = atoi(value);
	    VideoSetInverseTelecine(ConfigVideoInverseTelecine);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Denoise");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoDenoise[i] = atoi(value);
	    VideoSetDenoise(ConfigVideoDenoise);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Sharpen");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoSharpen[i] = atoi(value);
	    VideoSetSharpen(ConfigVideoSharpen);
	    return true;
	}

	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "CutTopBottom");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoCutTopBottom[i] = atoi(value);
	    VideoSetCutTopBottom(ConfigVideoCutTopBottom);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "CutLeftRight");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoCutLeftRight[i] = atoi(value);
	    VideoSetCutLeftRight(ConfigVideoCutLeftRight);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "FirstField");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoFirstField[i] = atoi(value);
	    VideoSetFirstField(ConfigVideoFirstField);
	    return true;
	}
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "SecondField");
	if (!strcasecmp(name, buf)) {
	    ConfigVideoSecondField[i] = atoi(value);
	    VideoSetSecondField(ConfigVideoSecondField);
	    return true;
	}
    }

    if (!strcasecmp(name, "AutoCrop.Interval")) {
	VideoSetAutoCrop(ConfigAutoCropInterval =
	    atoi(value), ConfigAutoCropDelay, ConfigAutoCropTolerance);
	ConfigAutoCropEnabled = ConfigAutoCropInterval != 0;
	return true;
    }
    if (!strcasecmp(name, "AutoCrop.Delay")) {
	VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay =
	    atoi(value), ConfigAutoCropTolerance);
	return true;
    }
    if (!strcasecmp(name, "AutoCrop.Tolerance")) {
	VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay,
	    ConfigAutoCropTolerance = atoi(value));
	return true;
    }

    if (!strcasecmp(name, "AudioDelay")) {
	VideoSetAudioDelay(ConfigVideoAudioDelay = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioDrift")) {
	CodecSetAudioDrift(ConfigAudioDrift = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioPassthrough")) {
	int i;

	i = atoi(value);
	AudioPassthroughState = i > 0;
	ConfigAudioPassthrough = abs(i);
	if (AudioPassthroughState) {
	    CodecSetAudioPassthrough(ConfigAudioPassthrough);
	} else {
	    CodecSetAudioPassthrough(0);
	}
	return true;
    }
    if (!strcasecmp(name, "AudioDownmix")) {
	CodecSetAudioDownmix(ConfigAudioDownmix = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioSoftvol")) {
	AudioSetSoftvol(ConfigAudioSoftvol = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "AudioNormalize")) {
	ConfigAudioNormalize = atoi(value);
	AudioSetNormalize(ConfigAudioNormalize, ConfigAudioMaxNormalize);
	return true;
    }
    if (!strcasecmp(name, "AudioMaxNormalize")) {
	ConfigAudioMaxNormalize = atoi(value);
	AudioSetNormalize(ConfigAudioNormalize, ConfigAudioMaxNormalize);
	return true;
    }
    if (!strcasecmp(name, "AudioCompression")) {
	ConfigAudioCompression = atoi(value);
	AudioSetCompression(ConfigAudioCompression, ConfigAudioMaxCompression);
	return true;
    }
    if (!strcasecmp(name, "AudioMaxCompression")) {
	ConfigAudioMaxCompression = atoi(value);
	AudioSetCompression(ConfigAudioCompression, ConfigAudioMaxCompression);
	return true;
    }
    if (!strcasecmp(name, "AudioStereoDescent")) {
	ConfigAudioStereoDescent = atoi(value);
	AudioSetStereoDescent(ConfigAudioStereoDescent);
	return true;
    }
    if (!strcasecmp(name, "AudioBufferTime")) {
	ConfigAudioBufferTime = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "AudioAutoAES")) {
	ConfigAudioAutoAES = atoi(value);
	AudioSetAutoAES(ConfigAudioAutoAES);
	return true;
    }
#ifdef USE_PIP
    if (!strcasecmp(name, "pip.X")) {
	ConfigPipX = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.Y")) {
	ConfigPipY = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.Width")) {
	ConfigPipWidth = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.Height")) {
	ConfigPipHeight = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.VideoX")) {
	ConfigPipVideoX = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.VideoY")) {
	ConfigPipVideoY = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.VideoWidth")) {
	ConfigPipVideoWidth = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.VideoHeight")) {
	ConfigPipVideoHeight = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.Alt.X")) {
	ConfigPipAltX = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.Alt.Y")) {
	ConfigPipAltY = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.Alt.Width")) {
	ConfigPipAltWidth = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.Alt.Height")) {
	ConfigPipAltHeight = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.Alt.VideoX")) {
	ConfigPipAltVideoX = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.Alt.VideoY")) {
	ConfigPipAltVideoY = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.Alt.VideoWidth")) {
	ConfigPipAltVideoWidth = atoi(value);
	return true;
    }
    if (!strcasecmp(name, "pip.Alt.VideoHeight")) {
	ConfigPipAltVideoHeight = atoi(value);
	return true;
    }
#endif
    return false;
}

/**
**	Receive requests or messages.
**
**	@param id	unique identification string that identifies the
**			service protocol
**	@param data	custom data structure
*/
bool cPluginSoftHdDevice::Service(const char *id, void *data)
{
    //dsyslog("[softhddev]%s: id %s\n", __FUNCTION__, id);

    if (strcmp(id, OSD_3DMODE_SERVICE) == 0) {
	SoftHDDevice_Osd3DModeService_v1_0_t *r;

	r = (SoftHDDevice_Osd3DModeService_v1_0_t *) data;
	VideoSetOsd3DMode(r->Mode);
	return true;
    }

    if (strcmp(id, ATMO_GRAB_SERVICE) == 0) {
	int width;
	int height;

	if (data == NULL) {
	    return true;
	}

	if (SuspendMode != NOT_SUSPENDED) {
	    return false;
	}

	SoftHDDevice_AtmoGrabService_v1_0_t *r =
	    (SoftHDDevice_AtmoGrabService_v1_0_t *) data;
	if (r->structSize != sizeof(SoftHDDevice_AtmoGrabService_v1_0_t)
	    || r->analyseSize < 64 || r->analyseSize > 256
	    || r->clippedOverscan < 0 || r->clippedOverscan > 200) {
	    return false;
	}

	width = r->analyseSize * -1;	// Internal marker for Atmo grab service
	height = r->clippedOverscan;

	r->img = VideoGrabService(&r->imgSize, &width, &height);
	if (r->img == NULL) {
	    return false;
	}
	r->imgType = GRAB_IMG_RGBA_FORMAT_B8G8R8A8;
	r->width = width;
	r->height = height;
	return true;
    }

    if (strcmp(id, ATMO1_GRAB_SERVICE) == 0) {
	SoftHDDevice_AtmoGrabService_v1_1_t *r;

	if (!data) {
	    return true;
	}

	if (SuspendMode != NOT_SUSPENDED) {
	    return false;
	}

	r = (SoftHDDevice_AtmoGrabService_v1_1_t *) data;
	r->img = VideoGrabService(&r->size, &r->width, &r->height);
	if (!r->img) {
	    return false;
	}

	return true;
    }

    return false;
}

//----------------------------------------------------------------------------
//	cPlugin SVDRP
//----------------------------------------------------------------------------

/**
**	SVDRP commands help text.
**	FIXME: translation?
*/
static const char *SVDRPHelpText[] = {
    "SUSP\n" "\040   Suspend plugin.\n\n"
	"    The plugin is suspended to save energie. Depending on the setup\n"
	"    'softhddevice.Suspend.Close = 0' only the video and audio output\n"
	"    is stopped or with 'softhddevice.Suspend.Close = 1' the video\n"
	"    and audio devices are closed.\n"
	"    If 'softhddevice.Suspend.X11 = 1' is set and the X11 server was\n"
	"    started by the plugin, the X11 server would also be closed.\n"
	"    (Stopping X11 while suspended isn't supported yet)\n",
    "RESU\n" "\040   Resume plugin.\n\n"
	"    Resume the suspended plugin. The plugin could be suspended by\n"
	"    the command line option '-s' or by a previous SUSP command.\n"
	"    If the x11 server was stopped by the plugin, it will be\n"
	"    restarted.",
    "DETA\n" "\040   Detach plugin.\n\n"
	"    The plugin will be detached from the audio, video and DVB\n"
	"    devices.  Other programs or plugins can use them now.\n",
    "ATTA <-d display> <-a audio> <-p pass>\n" "    Attach plugin.\n\n"
	"    Attach the plugin to audio, video and DVB devices. Use:\n"
	"    -d display\tdisplay of x11 server (fe. :0.0)\n"
	"    -a audio\taudio device (fe. alsa: hw:0,0 oss: /dev/dsp)\n"
	"    -p pass\t\taudio device for pass-through (hw:0,1 or /dev/dsp1)\n",
    "PRIM <n>\n" "    Make <n> the primary device.\n\n"
	"    <n> is the number of device. Without number softhddevice becomes\n"
	"    the primary device. If becoming primary, the plugin is attached\n"
	"    to the devices. If loosing primary, the plugin is detached from\n"
	"    the devices.",
    "HOTK key\n" "    Execute hotkey.\n\n"
	"    key is the hotkey number, following are supported:\n"
	"    10: disable audio pass-through\n"
	"    11: enable audio pass-through\n"
	"    12: toggle audio pass-through\n"
	"    13: decrease audio delay by 10ms\n"
	"    14: increase audio delay by 10ms\n"
	"    20: disable fullscreen\n\040   21: enable fullscreen\n"
	"    22: toggle fullscreen\n"
	"    23: disable auto-crop\n\040   24: enable auto-crop\n"
	"    25: toggle auto-crop\n"
	"    30: stretch 4:3 to display\n\040	31: pillar box 4:3 in display\n"
	"    32: center cut-out 4:3 to display\n"
	"    39: rotate 4:3 to display zoom mode\n"
	"    40: stretch other aspect ratios to display\n"
	"    41: letter box other aspect ratios in display\n"
	"    42: center cut-out other aspect ratios to display\n"
	"    49: rotate other aspect ratios to display zoom mode\n",
    "STAT\n" "\040   Display SuspendMode of the plugin.\n\n"
	"    reply code is 910 + SuspendMode\n"
	"    SUSPEND_EXTERNAL == -1  (909)\n"
	"    NOT_SUSPENDED    ==  0  (910)\n"
	"    SUSPEND_NORMAL   ==  1  (911)\n"
	"    SUSPEND_DETACHED ==  2  (912)\n",
    "3DOF\n" "\040   3D OSD off.\n",
    "3DTB\n" "\040   3D OSD Top and Bottom.\n",
    "3DSB\n" "\040   3D OSD Side by Side.\n",
    "RAIS\n" "\040   Raise softhddevice window\n\n"
	"    If Xserver is not started by softhddevice, the window which\n"
	"    contains the softhddevice frontend will be raised to the front.\n",
    NULL
};

/**
**	Return SVDRP commands help pages.
**
**	return a pointer to a list of help strings for all of the plugin's
**	SVDRP commands.
*/
const char **cPluginSoftHdDevice::SVDRPHelpPages(void)
{
    return SVDRPHelpText;
}

/**
**	Handle SVDRP commands.
**
**	@param command		SVDRP command
**	@param option		all command arguments
**	@param reply_code	reply code
*/
cString cPluginSoftHdDevice::SVDRPCommand(const char *command,
    const char *option, __attribute__ ((unused)) int &reply_code)
{
    if (!strcasecmp(command, "STAT")) {
	reply_code = 910 + SuspendMode;
	switch (SuspendMode) {
	    case SUSPEND_EXTERNAL:
		return "SuspendMode is SUSPEND_EXTERNAL";
	    case NOT_SUSPENDED:
		return "SuspendMode is NOT_SUSPENDED";
	    case SUSPEND_NORMAL:
		return "SuspendMode is SUSPEND_NORMAL";
	    case SUSPEND_DETACHED:
		return "SuspendMode is SUSPEND_DETACHED";
	}
    }
    if (!strcasecmp(command, "SUSP")) {
	if (cSoftHdControl::Player) {	// already suspended
	    return "SoftHdDevice already suspended";
	}
	if (SuspendMode != NOT_SUSPENDED) {
	    return "SoftHdDevice already detached";
	}
	cControl::Launch(new cSoftHdControl);
	cControl::Attach();
	Suspend(ConfigSuspendClose, ConfigSuspendClose, ConfigSuspendX11);
	SuspendMode = SUSPEND_NORMAL;
	return "SoftHdDevice is suspended";
    }
    if (!strcasecmp(command, "RESU")) {
	if (SuspendMode == NOT_SUSPENDED) {
	    return "SoftHdDevice already resumed";
	}
	if (SuspendMode != SUSPEND_NORMAL) {
	    return "can't resume SoftHdDevice";
	}
	if (ShutdownHandler.GetUserInactiveTime()) {
	    ShutdownHandler.SetUserInactiveTimeout();
	}
	if (cSoftHdControl::Player) {	// suspended
	    cControl::Shutdown();	// not need, if not suspended
	}
	Resume();
	SuspendMode = NOT_SUSPENDED;
	return "SoftHdDevice is resumed";
    }
    if (!strcasecmp(command, "DETA")) {
	if (SuspendMode == SUSPEND_DETACHED) {
	    return "SoftHdDevice already detached";
	}
	if (cSoftHdControl::Player) {	// already suspended
	    return "can't suspend SoftHdDevice already suspended";
	}
	cControl::Launch(new cSoftHdControl);
	cControl::Attach();
	Suspend(1, 1, 0);
	SuspendMode = SUSPEND_DETACHED;
	return "SoftHdDevice is detached";
    }
    if (!strcasecmp(command, "ATTA")) {
	char *tmp;
	char *t;
	char *s;
	char *o;

	if (SuspendMode != SUSPEND_DETACHED) {
	    return "can't attach SoftHdDevice not detached";
	}
	if (!(tmp = strdup(option))) {
	    return "out of memory";
	}
	t = tmp;
	while ((s = strsep(&t, " \t\n\r"))) {
	    if (!strcmp(s, "-d")) {
		if (!(o = strsep(&t, " \t\n\r"))) {
		    free(tmp);
		    return "missing option argument";
		}
		free(ConfigX11Display);
		ConfigX11Display = strdup(o);
		X11DisplayName = ConfigX11Display;
	    } else if (!strncmp(s, "-d", 2)) {
		free(ConfigX11Display);
		ConfigX11Display = strdup(s + 2);
		X11DisplayName = ConfigX11Display;

	    } else if (!strcmp(s, "-a")) {
		if (!(o = strsep(&t, " \t\n\r"))) {
		    free(tmp);
		    return "missing option argument";
		}
		free(ConfigAudioDevice);
		ConfigAudioDevice = strdup(o);
		AudioSetDevice(ConfigAudioDevice);
	    } else if (!strncmp(s, "-a", 2)) {
		free(ConfigAudioDevice);
		ConfigAudioDevice = strdup(s + 2);
		AudioSetDevice(ConfigAudioDevice);

	    } else if (!strcmp(s, "-p")) {
		if (!(o = strsep(&t, " \t\n\r"))) {
		    free(tmp);
		    return "missing option argument";
		}
		free(ConfigPassthroughDevice);
		ConfigPassthroughDevice = strdup(o);
		AudioSetPassthroughDevice(ConfigPassthroughDevice);
	    } else if (!strncmp(s, "-p", 2)) {
		free(ConfigPassthroughDevice);
		ConfigPassthroughDevice = strdup(s + 2);
		AudioSetPassthroughDevice(ConfigPassthroughDevice);

	    } else if (*s) {
		free(tmp);
		return "unsupported option";
	    }
	}
	free(tmp);
	if (ShutdownHandler.GetUserInactiveTime()) {
	    ShutdownHandler.SetUserInactiveTimeout();
	}
	if (cSoftHdControl::Player) {	// suspended
	    cControl::Shutdown();	// not need, if not suspended
	}
	Resume();
	SuspendMode = NOT_SUSPENDED;
	return "SoftHdDevice is attached";
    }
    if (!strcasecmp(command, "HOTK")) {
	int hotk;

	hotk = strtol(option, NULL, 0);
	HandleHotkey(hotk);
	return "hot-key executed";
    }
    if (!strcasecmp(command, "PRIM")) {
	int primary;

	primary = strtol(option, NULL, 0);
	if (!primary && MyDevice) {
	    primary = MyDevice->DeviceNumber() + 1;
	}
	dsyslog("[softhddev] switching primary device to %d\n", primary);
	DoMakePrimary = primary;
	return "switching primary device requested";
    }
    if (!strcasecmp(command, "3DOF")) {
	VideoSetOsd3DMode(0);
	return "3d off";
    }
    if (!strcasecmp(command, "3DSB")) {
	VideoSetOsd3DMode(1);
	return "3d sbs";
    }
    if (!strcasecmp(command, "3DTB")) {
	VideoSetOsd3DMode(2);
	return "3d tb";
    }

    if (!strcasecmp(command, "RAIS")) {
	if (!ConfigStartX11Server) {
	    VideoRaiseWindow();
	} else {
	    return "Raise not possible";
	}
	return "Window raised";
    }

    return NULL;
}

VDRPLUGINCREATOR(cPluginSoftHdDevice);	// Don't touch this!
