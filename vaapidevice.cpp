/// Copyright (C) 2011 - 2015 by Johns. All Rights Reserved.
/// Copyright (C) 2018 by pesintta, rofafor.
///
/// SPDX-License-Identifier: AGPL-3.0-only

#define __STDC_CONSTANT_MACROS		///< needed for ffmpeg UINT64_C

#include <vdr/interface.h>
#include <vdr/plugin.h>
#include <vdr/player.h>
#include <vdr/osd.h>
#include <vdr/dvbspu.h>
#include <vdr/shutdown.h>
#include <vdr/tools.h>

#include "vaapidevice.h"

extern "C"
{
#include <stdint.h>
#include <libavcodec/avcodec.h>

#include "audio.h"
#include "video.h"
#include "codec.h"
#include "misc.h"
}

#if defined(APIVERSNUM) && APIVERSNUM < 20200
#error "VDR-2.2.0 API version or greater is required!"
#endif

//////////////////////////////////////////////////////////////////////////////

    /// vdr-plugin version number.
    /// Makefile extracts the version number for generating the file name
    /// for the distribution archive.
static const char *const VERSION = "1.0.0"
#ifdef GIT_REV
    "-GIT" GIT_REV
#endif
    ;

    /// vdr-plugin description.
static const char *const DESCRIPTION = trNOOP("VA-API Output Device");

    /// vdr-plugin text of main menu entry
static const char *MAINMENUENTRY = trNOOP("VA-API Device");

    /// single instance of vaapidevice plugin device.
static class cVaapiDevice *MyDevice;

//////////////////////////////////////////////////////////////////////////////

#define RESOLUTIONS 5			///< number of resolutions

    /// resolutions names
static const char *const Resolution[RESOLUTIONS] = {
    "576i", "720p", "1080i", "1080p", "2160p"
};

static char ConfigMakePrimary;		///< config primary wanted
static char ConfigHideMainMenuEntry;	///< config hide main menu entry
static char ConfigDetachFromMainMenu;	///< detach from main menu entry instead of suspend
static char ConfigSuspendClose;		///< suspend should close devices
static char ConfigSuspendX11;		///< suspend should stop x11

static char Config4to3DisplayFormat = 1;    ///< config 4:3 display format
static char ConfigOtherDisplayFormat = 1;   ///< config other display format
static uint32_t ConfigVideoBackground;	///< config video background color
static char ConfigVideo60HzMode;	///< config use 60Hz display mode
static char ConfigVideoSoftStartSync;	///< config use softstart sync

static int ConfigVideoColorBalance = 1; ///< config video color balance
static int ConfigVideoBrightness;	///< config video brightness
static int ConfigVideoContrast = 1;	///< config video contrast
static int ConfigVideoSaturation = 1;	///< config video saturation
static int ConfigVideoHue;		///< config video hue
static int ConfigVideoStde = 0;		///< config video skin tone enhancement

    /// config deinterlace
static int ConfigVideoDeinterlace[RESOLUTIONS];

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

static char *ConfigX11Display;		///< config x11 display
static char *ConfigAudioDevice;		///< config audio stereo device
static char *ConfigPassthroughDevice;	///< config audio pass-through device

static volatile int DoMakePrimary;	///< switch primary device to this

#define SUSPEND_EXTERNAL	-1	    ///< play external suspend mode
#define NOT_SUSPENDED		0	    ///< not suspend mode
#define SUSPEND_NORMAL		1	    ///< normal suspend mode
#define SUSPEND_DETACHED	2	    ///< detached suspend mode
static signed char SuspendMode;		///< suspend mode
volatile char SoftIsPlayingVideo;	///< stream contains video data
static cString CommandLineParameters = "";  ///< plugin's command-line parameters

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
//  C Callbacks
//////////////////////////////////////////////////////////////////////////////

/**
**	Logging function with thread information
*/
extern "C" void LogMessage(int trace, int level, const char *format, ...)
{
    if (SysLogLevel > level) {
	va_list ap;
	char fmt[256];
	int priority, mask;
	const char *prefix = "VAAPI: ";

	switch (level) {
	    case 0:		       // ERROR
		prefix = "VAAPI-ERROR: ";
		priority = LOG_ERR;
		break;
	    case 1:		       // INFO
		priority = LOG_INFO;
		break;
	    case 2:		       // DEBUG
		mask = (1 << (trace - 1)) & 0xFFFF;
		if (!(mask & TraceMode))
		    return;
		priority = LOG_DEBUG;
		break;
	    default:
		priority = LOG_DEBUG;
		break;
	}
	snprintf(fmt, sizeof(fmt), "[%d] %s%s", cThread::ThreadId(), prefix, format);
	va_start(ap, format);
	vsyslog(priority, fmt, ap);
	va_end(ap);
    }
}

/**
**	Debug statistics on OSD class.
*/
class cDebugStatistics:public cThread
{
  private:
    cOsd * osd;
    int area_w;
    int area_h;
    int area_bpp;

    cString VideoStats(void)
    {
	cString stats = "";
	char *info = GetVideoStats();
	if (info)
	{
	    stats = info;
	    free(info);
	}
	return stats;
    }

    cString VideoInfo(void)
    {
	cString stats = "";
	char *info = GetVideoInfo();

	if (info) {
	    stats = info;
	    free(info);
	}
	return stats;
    }

    cString AudioInfo(void)
    {
	cString stats = "";
	char *info = GetAudioInfo();

	if (info) {
	    stats = info;
	    free(info);
	}
	return stats;
    }

    void Draw(void)
    {
	LOCK_THREAD;
	if (osd) {
	    const cFont *font = cFont::GetFont(fontSml);
	    int y = 0, h = font->Height();

	    osd->DrawText(0, y, *VideoStats(), clrWhite, clrGray50, font, area_w, h);
	    y += h;
	    osd->DrawText(0, y, *VideoInfo(), clrWhite, clrGray50, font, area_w, h);
	    y += h;
	    osd->DrawText(0, y, *AudioInfo(), clrWhite, clrGray50, font, area_w, h);
	    y += h;

	    osd->Flush();
	}
    }

    bool Delete(void)
    {
	LOCK_THREAD;
	if (Running()) {
	    Cancel(3);
	    DELETENULL(osd);
	    return true;
	}
	return false;
    }

    bool Create(void)
    {
	LOCK_THREAD;
	if (!osd) {
	    osd = cOsdProvider::NewOsd(0, 0, 1);
	    tArea Area = { 0, 0, area_w, area_h, area_bpp };
	    osd->SetAreas(&Area, 1);
	}
	return osd != NULL;
    }

  protected:
    virtual void Action(void)
    {
	Create();
	while (Running()) {
	    Draw();
	    cCondWait::SleepMs(500);
	}
    }

  public:
  cDebugStatistics():cThread("VAAPI Stats"), osd(NULL), area_w(4096), area_h(2160), area_bpp(32) {
    }

    virtual ~ cDebugStatistics() {
	Delete();
    }

    bool Toggle(void)
    {
	if (Delete()) {
	    return false;
	}
	Start();
	return true;
    }

    cString Dump(void)
    {
	return cString::sprintf("%s\n%s\n%s\nCommand:%s\n", *VideoStats(), *VideoInfo(), *AudioInfo(),
	    *CommandLineParameters);
    }
};

static class cDebugStatistics *MyDebug;

/**
**	Soft device plugin remote class.
*/
class cSoftRemote:public cRemote, private cThread
{
  private:
    cMutex mutex;
    cCondVar keyReceived;
    cString Command;
    virtual void Action(void);
  public:

    /**
    **	Soft device remote class constructor.
    **
    **	@param name	remote name
    */
    cSoftRemote(void) : cRemote("XKeySym")
    {
      Start();
    }

    virtual ~cSoftRemote()
    {
      Cancel(3);
    }

    /**
    **	Receive keycode.
    **
    **	@param code	key code
    */
    void Receive(const char *code) {
      cMutexLock MutexLock(&mutex);
      Command = code;
      keyReceived.Broadcast();
    }
};

void cSoftRemote::Action(void)
{
  // see also VDR's cKbdRemote::Action()
  cTimeMs FirstTime;
  cTimeMs LastTime;
  cString FirstCommand = "";
  cString LastCommand = "";
  bool Delayed = false;
  bool Repeat = false;

  while (Running()) {
        cMutexLock MutexLock(&mutex);
        if (keyReceived.TimedWait(mutex, Setup.RcRepeatDelta * 3 / 2) && **Command) {
           if (strcmp(Command, LastCommand) == 0) {
              // If two keyboard events with the same command come in without an intermediate
              // timeout, this is a long key press that caused the repeat function to kick in:
              Delayed = false;
              FirstCommand = "";
              if (FirstTime.Elapsed() < (uint)Setup.RcRepeatDelay)
                 continue; // repeat function kicks in after a short delay
              if (LastTime.Elapsed() < (uint)Setup.RcRepeatDelta)
                 continue; // skip same keys coming in too fast
              cRemote::Put(Command, true);
              Repeat = true;
              LastTime.Set();
              }
           else if (strcmp(Command, FirstCommand) == 0) {
              // If the same command comes in twice with an intermediate timeout, we
              // need to delay the second command to see whether it is going to be
              // a repeat function or a separate key press:
              Delayed = true;
              }
           else {
              // This is a totally new key press, so we accept it immediately:
              cRemote::Put(Command);
              Delayed = false;
              FirstCommand = Command;
              FirstTime.Set();
              }
           }
        else if (Repeat) {
           // Timeout after a repeat function, so we generate a 'release':
           cRemote::Put(LastCommand, false, true);
           Repeat = false;
           }
        else if (Delayed && *FirstCommand) {
           // Timeout after two normal key presses of the same key, so accept the
           // delayed key:
           cRemote::Put(FirstCommand);
           Delayed = false;
           FirstCommand = "";
           FirstTime.Set();
           }
        else if (**FirstCommand && FirstTime.Elapsed() > (uint)Setup.RcRepeatDelay) {
           Delayed = false;
           FirstCommand = "";
           FirstTime.Set();
           }
        LastCommand = Command;
        Command = "";
        }
}

static cSoftRemote *csoft = NULL;

/**
**	Feed key press as remote input (called from C part).
**
**	@param keymap	target keymap "XKeymap" name (obsolete, ignored)
**	@param key	pressed/released key name
**	@param repeat	repeated key flag (obsolete, ignored)
**	@param release	released key flag (obsolete, ignored)
**	@param letter	x11 character string (system setting locale)
*/
extern "C" void FeedKeyPress(const char *keymap, const char *key, int repeat, int release, const char *letter)
{
    if (!csoft || !keymap || !key) {
	return;
    }

    csoft->Receive(key);
    /* TODO clarify what this is supposed to do (kls 2019-05-13)
    if (key[1]) {			// no single character
	if (!csoft->Put(key, repeat, release) && letter && !cRemote::IsLearning()) {
	    cCharSetConv conv;
	    unsigned code;

	    code = Utf8CharGet(conv.Convert(letter));
	    if (code <= 0xFF) {
		cRemote::Put(KBDKEY(code)); // feed it for edit mode
	    }
	}
    } else if (!csoft->Put(key, repeat, release)) {
	cRemote::Put(KBDKEY(key[0]));	// feed it for edit mode
    }
    */
}

//////////////////////////////////////////////////////////////////////////////
//  OSD
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
    OsdLevel = level;
}

/**
**	OSD Destructor.
**
**	Shuts down the OSD.
*/
cSoftOsd::~cSoftOsd(void)
{
    SetActive(false);
    // done by SetActive: OsdClose();
}

/**
+*	Set the sub-areas to the given areas
*/
eOsdError cSoftOsd::SetAreas(const tArea * areas, int n)
{
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

    if (!Active()) {			// this osd is not active
	return;
    }

    if (!IsTrueColor()) {
	cBitmap *bitmap;
	int i;

	// draw all bitmaps
	for (i = 0; (bitmap = GetBitmap(i)); ++i) {
	    uint8_t *argb;
	    int xs;
	    int ys;
	    int x;
	    int y;
	    int w;
	    int h;
	    int x1;
	    int y1;
	    int x2;
	    int y2;
	    int width;
	    int height;
	    double video_aspect;

	    // get dirty bounding box
	    if (Dirty) {		// forced complete update
		x1 = 0;
		y1 = 0;
		x2 = bitmap->Width() - 1;
		y2 = bitmap->Height() - 1;
	    } else if (!bitmap->Dirty(x1, y1, x2, y2)) {
		continue;		// nothing dirty continue
	    }
	    // convert and upload only visible dirty areas
	    xs = bitmap->X0() + Left();
	    ys = bitmap->Y0() + Top();
	    // FIXME: negtative position bitmaps
	    w = x2 - x1 + 1;
	    h = y2 - y1 + 1;
	    // clip to screen
	    if (xs < 0) {
		if (xs + x1 < 0) {
		    x1 -= xs + x1;
		    w += xs + x1;
		    if (w <= 0) {
			continue;
		    }
		}
		xs = 0;
	    }
	    if (ys < 0) {
		if (ys + y1 < 0) {
		    y1 -= ys + y1;
		    h += ys + y1;
		    if (h <= 0) {
			continue;
		    }
		}
		ys = 0;
	    }
	    ::GetOsdSize(&width, &height, &video_aspect);
	    if (w > width - xs - x1) {
		w = width - xs - x1;
		if (w <= 0) {
		    continue;
		}
		x2 = x1 + w - 1;
	    }
	    if (h > height - ys - y1) {
		h = height - ys - y1;
		if (h <= 0) {
		    continue;
		}
		y2 = y1 + h - 1;
	    }
#ifdef DEBUG
	    if (w > bitmap->Width() || h > bitmap->Height()) {
		Error("Dirty area too big");
		abort();
	    }
#endif
	    argb = (uint8_t *) malloc(w * h * sizeof(uint32_t));
	    for (y = y1; y <= y2; ++y) {
		for (x = x1; x <= x2; ++x) {
		    ((uint32_t *) argb)[x - x1 + (y - y1) * w] = bitmap->GetColor(x, y);
		}
	    }
	    OsdDrawARGB(0, 0, w, h, w * sizeof(uint32_t), argb, xs + x1, ys + y1);

	    bitmap->Clean();
	    // FIXME: reuse argb
	    free(argb);
	}
	Dirty = 0;
	return;
    }

    LOCK_PIXMAPS;
    while ((pm = (dynamic_cast < cPixmapMemory * >(RenderPixmaps())))) {
	int xp;
	int yp;
	int stride;
	int x;
	int y;
	int w;
	int h;
	int width;
	int height;
	double video_aspect;

	x = pm->ViewPort().X();
	y = pm->ViewPort().Y();
	w = pm->ViewPort().Width();
	h = pm->ViewPort().Height();
	stride = w * sizeof(tColor);

	// clip to osd
	xp = 0;
	if (x < 0) {
	    xp = -x;
	    w -= xp;
	    x = 0;
	}

	yp = 0;
	if (y < 0) {
	    yp = -y;
	    h -= yp;
	    y = 0;
	}

	if (w > Width() - x) {
	    w = Width() - x;
	}
	if (h > Height() - y) {
	    h = Height() - y;
	}

	x += Left();
	y += Top();

	// clip to screen
	if (x < 0) {
	    w += x;
	    xp += -x;
	    x = 0;
	}
	if (y < 0) {
	    h += y;
	    yp += -y;
	    y = 0;
	}
	::GetOsdSize(&width, &height, &video_aspect);
	if (w > width - x) {
	    w = width - x;
	}
	if (h > height - y) {
	    h = height - y;
	}
	OsdDrawARGB(xp, yp, w, h, stride, pm->Data(), x, y);

	DestroyPixmap(pm);
    }
    Dirty = 0;
}

//////////////////////////////////////////////////////////////////////////////
//  OSD provider
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
    //virtual ~cSoftOsdProvider();  ///< OSD provider destructor
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
}

/**
**	Destroy cOsdProvider class.
cSoftOsdProvider::~cSoftOsdProvider()
{
    Debug1("%s", __FUNCTION__);
}
*/

//////////////////////////////////////////////////////////////////////////////
//  cMenuSetupPage
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
    int SuspendClose;
    int SuspendX11;

    int Video;
    int Video4to3DisplayFormat;
    int VideoOtherDisplayFormat;
    uint32_t Background;
    uint32_t BackgroundAlpha;
    int _60HzMode;
    int SoftStartSync;

    int ColorBalance;
    int Brightness;
    int Contrast;
    int Saturation;
    int Hue;
    int Stde;

    int ResolutionShown[RESOLUTIONS];
    int Scaling[RESOLUTIONS];
    int Deinterlace[RESOLUTIONS];
    int Denoise[RESOLUTIONS];
    int Sharpen[RESOLUTIONS];
    int CutTopBottom[RESOLUTIONS];
    int CutLeftRight[RESOLUTIONS];

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

    /// @}
  private:
     inline cOsdItem * CollapsedItem(const char *, int &, const char * = NULL);
    inline bool IsResolutionProgressive(int mode);
    void Create(void);			// create sub-menu
  protected:
     virtual void Store(void);
  public:
     cMenuSetupSoft(void);
    ~cMenuSetupSoft();
    virtual eOSState ProcessKey(eKeys); // handle input
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
inline cOsdItem *cMenuSetupSoft::CollapsedItem(const char *label, int &flag, const char *msg)
{
    cOsdItem *item;

    item = new cMenuEditBoolItem(cString::sprintf("* %s", label), &flag, msg ? msg : tr("show"), tr("hide"));

    return item;
}

bool cMenuSetupSoft::IsResolutionProgressive(int mode)
{
    return ! !strstr(Resolution[mode], "p");
}

/**
**	Create setup menu.
*/
void cMenuSetupSoft::Create(void)
{
    static const char *const video_display_formats_4_3[] = {
	"pan&scan", "letterbox", "center cut-out",
    };
    static const char *const video_display_formats_16_9[] = {
	"pan&scan", "pillarbox", "center cut-out",
    };
    static const char *const audiodrift[] = {
	"None", "PCM", "AC-3", "PCM + AC-3"
    };
    int current;
    const char **scaling;
    const char **scaling_short;
    const char **deinterlace;
    const char **deinterlace_short;
    int scaling_modes = VideoGetScalingModes(&scaling, &scaling_short);
    int deinterlace_modes = VideoGetDeinterlaceModes(&deinterlace, &deinterlace_short);
    int brightness_min, brightness_def, brightness_max;
    int brightness_active = VideoGetBrightnessConfig(&brightness_min, &brightness_def, &brightness_max);
    int contrast_min, contrast_def, contrast_max;
    int contrast_active = VideoGetContrastConfig(&contrast_min, &contrast_def, &contrast_max);
    int saturation_min, saturation_def, saturation_max;
    int saturation_active = VideoGetSaturationConfig(&saturation_min, &saturation_def, &saturation_max);
    int hue_min, hue_def, hue_max;
    int hue_active = VideoGetHueConfig(&hue_min, &hue_def, &hue_max);
    int stde_min, stde_def, stde_max;
    int stde_active = VideoGetSkinToneEnhancementConfig(&stde_min, &stde_def, &stde_max);
    int denoise_min, denoise_def, denoise_max;
    int denoise_active = VideoGetDenoiseConfig(&denoise_min, &denoise_def, &denoise_max);
    int sharpen_min, sharpen_def, sharpen_max;
    int sharpen_active = VideoGetSharpenConfig(&sharpen_min, &sharpen_def, &sharpen_max);

    current = Current();		// get current menu item index
    Clear();				// clear the menu

    SetHelp(NULL, NULL, NULL, MyDebug->Active()? tr("Debug/OFF") : tr("Debug/ON"));

    //
    //	general
    //
    Add(CollapsedItem(tr("General"), General));

    if (General) {
	Add(new cMenuEditBoolItem(tr("Make primary device"), &MakePrimary, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Hide main menu entry"), &HideMainMenuEntry, trVDR("no"), trVDR("yes")));
	//
	//  suspend
	//
	Add(SeparatorItem(tr("Suspend")));
	Add(new cMenuEditBoolItem(tr("Detach from main menu entry"), &DetachFromMainMenu, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Suspend closes video+audio"), &SuspendClose, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Suspend stops x11"), &SuspendX11, trVDR("no"), trVDR("yes")));
    }
    //
    //	video
    //
    Add(CollapsedItem(tr("Video"), Video));
    if (Video) {
	Add(new cMenuEditStraItem(trVDR("4:3 video display format"), &Video4to3DisplayFormat, 3,
		video_display_formats_4_3));
	Add(new cMenuEditStraItem(trVDR("16:9+other video display format"), &VideoOtherDisplayFormat, 3,
		video_display_formats_16_9));

	// FIXME: switch config gray/color configuration
	Add(new cMenuEditIntItem(tr("Video background color (RGB)"), (int *)&Background, 0, 0x00FFFFFF));
	Add(new cMenuEditIntItem(tr("Video background color (Alpha)"), (int *)&BackgroundAlpha, 0, 0xFF));
	Add(new cMenuEditBoolItem(tr("60hz display mode"), &_60HzMode, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Soft start a/v sync"), &SoftStartSync, trVDR("no"), trVDR("yes")));

	Add(new cMenuEditBoolItem(tr("Color balance"), &ColorBalance, trVDR("off"), trVDR("on")));
	if (ColorBalance) {
	    if (brightness_active)
		Add(new cMenuEditIntItem(*cString::sprintf(tr("\040\040Brightness (%d..[%d]..%d)"), brightness_min,
			    brightness_def, brightness_max), &Brightness, brightness_min, brightness_max));
	    if (contrast_active)
		Add(new cMenuEditIntItem(*cString::sprintf(tr("\040\040Contrast (%d..[%d]..%d)"), contrast_min,
			    contrast_def, contrast_max), &Contrast, contrast_min, contrast_max));
	    if (saturation_active)
		Add(new cMenuEditIntItem(*cString::sprintf(tr("\040\040Saturation (%d..[%d]..%d)"), saturation_min,
			    saturation_def, saturation_max), &Saturation, saturation_min, saturation_max));
	    if (hue_active)
		Add(new cMenuEditIntItem(*cString::sprintf(tr("\040\040Hue (%d..[%d]..%d)"), hue_min, hue_def,
			    hue_max), &Hue, hue_min, hue_max));
	}
	if (stde_active)
	    Add(new cMenuEditIntItem(*cString::sprintf(tr("Skin Tone Enhancement (%d..[%d]..%d)"), stde_min, stde_def,
			stde_max), &Stde, stde_min, stde_max));

	for (int i = 0; i < RESOLUTIONS; ++i) {
	    cString msg;

	    // short hidden informations
	    msg =
		cString::sprintf("%s,%s,%s", scaling_short[Scaling[i]], deinterlace_short[Deinterlace[i]],
		Denoise[i] ? "D" : "N");
	    Add(CollapsedItem(Resolution[i], ResolutionShown[i], msg));

	    if (ResolutionShown[i]) {
		Add(new cMenuEditStraItem(tr("Scaling"), &Scaling[i], scaling_modes, scaling));
		if (!IsResolutionProgressive(i))
		    Add(new cMenuEditStraItem(tr("Deinterlace"), &Deinterlace[i], deinterlace_modes, deinterlace));
		if (denoise_active)
		    Add(new cMenuEditIntItem(*cString::sprintf(tr("Denoise (%d..[%d]..%d)"), denoise_min, denoise_def,
				denoise_max), &Denoise[i], denoise_min, denoise_max));
		if (sharpen_active)
		    Add(new cMenuEditIntItem(*cString::sprintf(tr("Sharpen (%d..[%d]..%d)"), sharpen_min, sharpen_def,
				sharpen_max), &Sharpen[i], sharpen_min, sharpen_max));

		Add(new cMenuEditIntItem(tr("Cut top and bottom (pixel)"), &CutTopBottom[i], 0, 250));
		Add(new cMenuEditIntItem(tr("Cut left and right (pixel)"), &CutLeftRight[i], 0, 250));
	    }
	}
	//
	//  auto-crop
	//
	Add(SeparatorItem(tr("Auto-crop")));
	Add(new cMenuEditIntItem(tr("Autocrop interval (frames)"), &AutoCropInterval, 0, 200, tr("off")));
	Add(new cMenuEditIntItem(tr("Autocrop delay (n * interval)"), &AutoCropDelay, 0, 200));
	Add(new cMenuEditIntItem(tr("Autocrop tolerance (pixel)"), &AutoCropTolerance, 0, 32));
    }
    //
    //	audio
    //
    Add(CollapsedItem(tr("Audio"), Audio));

    if (Audio) {
	Add(new cMenuEditIntItem(tr("Audio/Video delay (ms)"), &AudioDelay, -1000, 1000));
	Add(new cMenuEditStraItem(tr("Audio drift correction"), &AudioDrift, 4, audiodrift));
	Add(new cMenuEditBoolItem(tr("Pass-through default"), &AudioPassthroughDefault, trVDR("off"), trVDR("on")));
	Add(new cMenuEditBoolItem(tr("\040\040PCM pass-through"), &AudioPassthroughPCM, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("\040\040AC-3 pass-through"), &AudioPassthroughAC3, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("\040\040E-AC-3 pass-through"), &AudioPassthroughEAC3, trVDR("no"),
		trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Enable (E-)AC-3 (decoder) downmix"), &AudioDownmix, trVDR("no"), trVDR("yes")));
	Add(new cMenuEditBoolItem(tr("Volume control"), &AudioSoftvol, tr("Hardware"), tr("Software")));
	Add(new cMenuEditBoolItem(tr("Enable normalize volume"), &AudioNormalize, trVDR("no"), trVDR("yes")));
	if (AudioNormalize)
	    Add(new cMenuEditIntItem(tr("\040\040Max normalize factor (/1000)"), &AudioMaxNormalize, 0, 10000));
	Add(new cMenuEditBoolItem(tr("Enable volume compression"), &AudioCompression, trVDR("no"), trVDR("yes")));
	if (AudioCompression)
	    Add(new cMenuEditIntItem(tr("\040\040Max compression factor (/1000)"), &AudioMaxCompression, 0, 10000));
	Add(new cMenuEditIntItem(tr("Reduce stereo volume (/1000)"), &AudioStereoDescent, 0, 1000));
	Add(new cMenuEditIntItem(tr("Audio buffer size (ms)"), &AudioBufferTime, 0, 1000));
    }

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

    int old_resolution_shown[RESOLUTIONS];
    int old_denoise[RESOLUTIONS];
    int old_sharpen[RESOLUTIONS];
    int old_colorbalance;
    int old_brightness;
    int old_contrast;
    int old_saturation;
    int old_hue;
    int old_stde;
    int old_audionormalize;
    int old_audiocompression;

    old_general = General;
    old_video = Video;
    old_audio = Audio;
    memcpy(old_resolution_shown, ResolutionShown, sizeof(ResolutionShown));
    memcpy(old_denoise, Denoise, sizeof(Denoise));
    memcpy(old_sharpen, Sharpen, sizeof(Sharpen));
    old_colorbalance = ColorBalance;
    old_brightness = Brightness;
    old_contrast = Contrast;
    old_saturation = Saturation;
    old_hue = Hue;
    old_stde = Stde;
    old_audionormalize = AudioNormalize;
    old_audiocompression = AudioCompression;
    state = cMenuSetupPage::ProcessKey(key);

    if (state == osUnknown) {
	switch (key) {
	    case kBlue:
		MyDebug->Toggle();
		Create();		// update color key labels
		state = osContinue;
		break;
	    default:
		state = osContinue;
		break;
	}
    }

    if (key != kNone) {
	// update menu only, if something on the structure has changed
	// this is needed because VDR menus are evil slow
	if (old_general != General || old_video != Video || old_audio != Audio || old_colorbalance != ColorBalance
	    || old_audionormalize != AudioNormalize || old_audiocompression != AudioCompression) {
	    if (old_colorbalance != ColorBalance)
		VideoSetColorBalance(ColorBalance);
	    Create();			// update menu
	} else {
	    for (int i = 0; i < RESOLUTIONS; ++i) {
		if (old_resolution_shown[i] != ResolutionShown[i]) {
		    Create();		// update menu
		    break;
		}
		if (old_denoise[i] != Denoise[i]) {
		    VideoSetDenoise(Denoise);
		    break;
		}
		if (old_sharpen[i] != Sharpen[i]) {
		    VideoSetSharpen(Sharpen);
		    break;
		}
	    }
	    if (old_brightness != Brightness)
		VideoSetBrightness(Brightness);
	    if (old_contrast != Contrast)
		VideoSetContrast(Contrast);
	    if (old_saturation != Saturation)
		VideoSetSaturation(Saturation);
	    if (old_hue != Hue)
		VideoSetHue(Hue);
	    if (old_stde != Stde)
		VideoSetSkinToneEnhancement(Stde);
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
    _60HzMode = ConfigVideo60HzMode;
    SoftStartSync = ConfigVideoSoftStartSync;

    ColorBalance = ConfigVideoColorBalance;
    Brightness = ConfigVideoBrightness;
    Contrast = ConfigVideoContrast;
    Saturation = ConfigVideoSaturation;
    Hue = ConfigVideoHue;
    Stde = ConfigVideoStde;

    for (i = 0; i < RESOLUTIONS; ++i) {
	ResolutionShown[i] = 0;
	Scaling[i] = ConfigVideoScaling[i];
	Deinterlace[i] = IsResolutionProgressive(i) ? 0 : ConfigVideoDeinterlace[i];
	Denoise[i] = ConfigVideoDenoise[i];
	Sharpen[i] = ConfigVideoSharpen[i];

	CutTopBottom[i] = ConfigVideoCutTopBottom[i];
	CutLeftRight[i] = ConfigVideoCutLeftRight[i];
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

    Create();
}

cMenuSetupSoft::~cMenuSetupSoft()
{
    int i;

    for (i = 0; i < RESOLUTIONS; ++i) {
	VideoSetDenoise(ConfigVideoDenoise);
	VideoSetSharpen(ConfigVideoSharpen);
    }
    VideoSetColorBalance(ConfigVideoColorBalance);
    VideoSetBrightness(ConfigVideoBrightness);
    VideoSetContrast(ConfigVideoContrast);
    VideoSetSaturation(ConfigVideoSaturation);
    VideoSetHue(ConfigVideoHue);
    VideoSetSkinToneEnhancement(ConfigVideoStde);
}

/**
**	Store setup.
*/
void cMenuSetupSoft::Store(void)
{
    int i;

    SetupStore("MakePrimary", ConfigMakePrimary = MakePrimary);
    SetupStore("HideMainMenuEntry", ConfigHideMainMenuEntry = HideMainMenuEntry);
    SetupStore("DetachFromMainMenu", ConfigDetachFromMainMenu = DetachFromMainMenu);

    SetupStore("Suspend.Close", ConfigSuspendClose = SuspendClose);
    SetupStore("Suspend.X11", ConfigSuspendX11 = SuspendX11);

    SetupStore("Video4to3DisplayFormat", Config4to3DisplayFormat = Video4to3DisplayFormat);
    VideoSet4to3DisplayFormat(Config4to3DisplayFormat);
    SetupStore("VideoOtherDisplayFormat", ConfigOtherDisplayFormat = VideoOtherDisplayFormat);
    VideoSetOtherDisplayFormat(ConfigOtherDisplayFormat);

    ConfigVideoBackground = Background << 8 | (BackgroundAlpha & 0xFF);
    SetupStore("Background", ConfigVideoBackground);
    VideoSetBackground(ConfigVideoBackground);
    SetupStore("60HzMode", ConfigVideo60HzMode = _60HzMode);
    VideoSet60HzMode(ConfigVideo60HzMode);
    SetupStore("SoftStartSync", ConfigVideoSoftStartSync = SoftStartSync);
    VideoSetSoftStartSync(ConfigVideoSoftStartSync);

    SetupStore("ColorBalance", ConfigVideoColorBalance = ColorBalance);
    VideoSetColorBalance(ConfigVideoColorBalance);
    SetupStore("Brightness", ConfigVideoBrightness = Brightness);
    VideoSetBrightness(ConfigVideoBrightness);
    SetupStore("Contrast", ConfigVideoContrast = Contrast);
    VideoSetContrast(ConfigVideoContrast);
    SetupStore("Saturation", ConfigVideoSaturation = Saturation);
    VideoSetSaturation(ConfigVideoSaturation);
    SetupStore("Hue", ConfigVideoHue = Hue);
    VideoSetHue(ConfigVideoHue);
    SetupStore("SkinToneEnhancement", ConfigVideoStde = Stde);
    VideoSetSkinToneEnhancement(ConfigVideoStde);

    for (i = 0; i < RESOLUTIONS; ++i) {
	char buf[128];

	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Scaling");
	SetupStore(buf, ConfigVideoScaling[i] = Scaling[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Deinterlace");
	SetupStore(buf, ConfigVideoDeinterlace[i] = Deinterlace[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Denoise");
	SetupStore(buf, ConfigVideoDenoise[i] = Denoise[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "Sharpen");
	SetupStore(buf, ConfigVideoSharpen[i] = Sharpen[i]);

	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "CutTopBottom");
	SetupStore(buf, ConfigVideoCutTopBottom[i] = CutTopBottom[i]);
	snprintf(buf, sizeof(buf), "%s.%s", Resolution[i], "CutLeftRight");
	SetupStore(buf, ConfigVideoCutLeftRight[i] = CutLeftRight[i]);
    }
    VideoSetScaling(ConfigVideoScaling);
    VideoSetDeinterlace(ConfigVideoDeinterlace);
    VideoSetDenoise(ConfigVideoDenoise);
    VideoSetSharpen(ConfigVideoSharpen);
    VideoSetCutTopBottom(ConfigVideoCutTopBottom);
    VideoSetCutLeftRight(ConfigVideoCutLeftRight);

    SetupStore("AutoCrop.Interval", ConfigAutoCropInterval = AutoCropInterval);
    SetupStore("AutoCrop.Delay", ConfigAutoCropDelay = AutoCropDelay);
    SetupStore("AutoCrop.Tolerance", ConfigAutoCropTolerance = AutoCropTolerance);
    VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay, ConfigAutoCropTolerance);
    ConfigAutoCropEnabled = ConfigAutoCropInterval != 0;

    SetupStore("AudioDelay", ConfigVideoAudioDelay = AudioDelay);
    VideoSetAudioDelay(ConfigVideoAudioDelay);
    SetupStore("AudioDrift", ConfigAudioDrift = AudioDrift);
    CodecSetAudioDrift(ConfigAudioDrift);

    // FIXME: can handle more audio state changes here
    // downmix changed reset audio, to get change direct
    if (ConfigAudioDownmix != AudioDownmix) {
	ResetChannelId();
    }
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
    SetupStore("AudioMaxNormalize", ConfigAudioMaxNormalize = AudioMaxNormalize);
    AudioSetNormalize(ConfigAudioNormalize, ConfigAudioMaxNormalize);
    SetupStore("AudioCompression", ConfigAudioCompression = AudioCompression);
    SetupStore("AudioMaxCompression", ConfigAudioMaxCompression = AudioMaxCompression);
    AudioSetCompression(ConfigAudioCompression, ConfigAudioMaxCompression);
    SetupStore("AudioStereoDescent", ConfigAudioStereoDescent = AudioStereoDescent);
    AudioSetStereoDescent(ConfigAudioStereoDescent);
    SetupStore("AudioBufferTime", ConfigAudioBufferTime = AudioBufferTime);
}

//////////////////////////////////////////////////////////////////////////////
//  cPlayer
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
//  cControl
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
    virtual eOSState ProcessKey(eKeys); ///< process input events

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

    Debug1("%s: dummy player stopped", __FUNCTION__);
}

//////////////////////////////////////////////////////////////////////////////
//  cOsdMenu
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

    current = Current();		// get current menu item index
    Clear();				// clear the menu

    SetHasHotkeys();

    Add(new cOsdItem(hk(ConfigDetachFromMainMenu ? tr("Detach VA-API device") : tr("Suspend VA-API device")),
	    osUser1));
    Add(new cOsdItem(hk(MyDebug->Active()? tr("Disable debug OSD") : tr("Enable debug OSD")), osUser2));
    Add(new cOsdItem(hk(ConfigAutoCropEnabled ? tr("Disable auto-crop") : tr("Enable auto-crop")), osUser3));

    SetCurrent(Get(current));		// restore selected menu entry
    Display();				// display build menu
}

/**
**	Soft device menu constructor.
*/
cSoftHdMenu::cSoftHdMenu(const char *title, int c0, int c1, int c2, int c3, int c4)
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
	case 10:		       // disable pass-through
	    AudioPassthroughState = 0;
	    CodecSetAudioPassthrough(0);
	    Skins.QueueMessage(mtInfo, tr("pass-through disabled"));
	    break;
	case 11:		       // enable pass-through
	    // note: you can't enable, without configured pass-through
	    AudioPassthroughState = 1;
	    CodecSetAudioPassthrough(ConfigAudioPassthrough);
	    Skins.QueueMessage(mtInfo, tr("pass-through enabled"));
	    break;
	case 12:		       // toggle pass-through
	    AudioPassthroughState ^= 1;
	    if (AudioPassthroughState) {
		CodecSetAudioPassthrough(ConfigAudioPassthrough);
		Skins.QueueMessage(mtInfo, tr("pass-through enabled"));
	    } else {
		CodecSetAudioPassthrough(0);
		Skins.QueueMessage(mtInfo, tr("pass-through disabled"));
	    }
	    break;
	case 13:		       // decrease audio delay
	    ConfigVideoAudioDelay -= 10;
	    VideoSetAudioDelay(ConfigVideoAudioDelay);
	    Skins.QueueMessage(mtInfo, cString::sprintf(tr("audio delay changed to %d"), ConfigVideoAudioDelay));
	    break;
	case 14:		       // increase audio delay
	    ConfigVideoAudioDelay += 10;
	    VideoSetAudioDelay(ConfigVideoAudioDelay);
	    Skins.QueueMessage(mtInfo, cString::sprintf(tr("audio delay changed to %d"), ConfigVideoAudioDelay));
	    break;
	case 15:
	    ConfigAudioDownmix ^= 1;
	    CodecSetAudioDownmix(ConfigAudioDownmix);
	    if (ConfigAudioDownmix) {
		Skins.QueueMessage(mtInfo, tr("surround downmix enabled"));
	    } else {
		Skins.QueueMessage(mtInfo, tr("surround downmix disabled"));
	    }
	    ResetChannelId();
	    break;

	case 20:		       // disable full screen
	    VideoSetFullscreen(0);
	    break;
	case 21:		       // enable full screen
	    VideoSetFullscreen(1);
	    break;
	case 22:		       // toggle full screen
	    VideoSetFullscreen(-1);
	    break;
	case 23:		       // disable auto-crop
	    ConfigAutoCropEnabled = 0;
	    VideoSetAutoCrop(0, ConfigAutoCropDelay, ConfigAutoCropTolerance);
	    Skins.QueueMessage(mtInfo, tr("auto-crop disabled and freezed"));
	    break;
	case 24:		       // enable auto-crop
	    ConfigAutoCropEnabled = 1;
	    if (!ConfigAutoCropInterval) {
		ConfigAutoCropInterval = 50;
	    }
	    VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay, ConfigAutoCropTolerance);
	    Skins.QueueMessage(mtInfo, tr("auto-crop enabled"));
	    break;
	case 25:		       // toggle auto-crop
	    ConfigAutoCropEnabled ^= 1;
	    // no interval configured, use some default
	    if (!ConfigAutoCropInterval) {
		ConfigAutoCropInterval = 50;
	    }
	    VideoSetAutoCrop(ConfigAutoCropEnabled * ConfigAutoCropInterval, ConfigAutoCropDelay,
		ConfigAutoCropTolerance);
	    if (ConfigAutoCropEnabled) {
		Skins.QueueMessage(mtInfo, tr("auto-crop enabled"));
	    } else {
		Skins.QueueMessage(mtInfo, tr("auto-crop disabled and freezed"));
	    }
	    break;
	case 30:		       // change 4:3 -> window mode
	case 31:
	case 32:
	    VideoSet4to3DisplayFormat(code - 30);
	    break;
	case 39:		       // rotate 4:3 -> window mode
	    VideoSet4to3DisplayFormat(-1);
	    break;
	case 40:		       // change 16:9 -> window mode
	case 41:
	case 42:
	    VideoSetOtherDisplayFormat(code - 40);
	    break;
	case 49:		       // rotate 16:9 -> window mode
	    VideoSetOtherDisplayFormat(-1);
	    break;
	case 50:		       // toggle debug statistics osd
	    MyDebug->Toggle();
	    break;
	default:
	    Error("Hot key %d is not supported", code);
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

    switch (HotkeyState) {
	case HksInitial:	       // initial state, waiting for hot key
	    if (key == kBlue) {
		HotkeyState = HksBlue;	// blue button
		return osContinue;
	    }
	    if (key == kRed) {
		HotkeyState = HksRed;	// red button
		return osContinue;
	    }
	    break;
	case HksBlue:		       // blue and first number
	    if (k0 <= key && key <= k9) {
		HotkeyCode = key - k0;
		HotkeyState = HksBlue1;
		return osContinue;
	    }
	    HotkeyState = HksInitial;
	    break;
	case HksBlue1:		       // blue and second number/enter
	    if (k0 <= key && key <= k9) {
		HotkeyCode *= 10;
		HotkeyCode += key - k0;
		HotkeyState = HksInitial;
		Debug1("%s: Hot key %d", __FUNCTION__, HotkeyCode);
		HandleHotkey(HotkeyCode);
		return osEnd;
	    }
	    if (key == kOk) {
		HotkeyState = HksInitial;
		Debug1("%s: Hot key %d", __FUNCTION__, HotkeyCode);
		HandleHotkey(HotkeyCode);
		return osEnd;
	    }
	    HotkeyState = HksInitial;
	    break;
	case HksRed:		       // red and first number
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
		    Suspend(ConfigSuspendClose, ConfigSuspendClose, ConfigSuspendX11);
		    SuspendMode = SUSPEND_NORMAL;
		}
		if (ShutdownHandler.GetUserInactiveTime()) {
		    Debug1("%s: set user inactive", __FUNCTION__);
		    ShutdownHandler.SetUserInactive();
		}
	    }
	    return osEnd;
	case osUser2:
	    MyDebug->Toggle();
	    Create();
	    break;
	case osUser3:
	    ConfigAutoCropEnabled ^= 1;
	    // no interval configured, use some default
	    if (!ConfigAutoCropInterval) {
		ConfigAutoCropInterval = 50;
	    }
	    VideoSetAutoCrop(ConfigAutoCropEnabled * ConfigAutoCropInterval, ConfigAutoCropDelay,
		ConfigAutoCropTolerance);
	    Create();
	    break;
	default:
	    break;
    }
    return state;
}

//////////////////////////////////////////////////////////////////////////////
//  cDevice
//////////////////////////////////////////////////////////////////////////////

class cVaapiDevice:public cDevice
{
  public:
    cVaapiDevice(void);
    virtual ~ cVaapiDevice(void);

    virtual cString DeviceName(void) const;
    virtual bool HasDecoder(void) const;
    virtual bool CanReplay(void) const;
    virtual bool SetPlayMode(ePlayMode);
    virtual void TrickSpeed(int, bool);
    virtual void Clear(void);
    virtual void Play(void);
    virtual void Freeze(void);
    virtual void Mute(void);
    virtual void StillPicture(const uchar *, int);
    virtual bool Poll(cPoller &, int = 0);
    virtual bool Flush(int = 0);
    virtual int64_t GetSTC(void);
    virtual cRect CanScaleVideo(const cRect &, int = taCenter);
    virtual void ScaleVideo(const cRect & = cRect::Null);
    virtual void SetVideoDisplayFormat(eVideoDisplayFormat);
    virtual void SetVideoFormat(bool);
    virtual void GetVideoSize(int &, int &, double &);
    virtual void GetOsdSize(int &, int &, double &);
    virtual int PlayVideo(const uchar *, int);
    virtual int PlayAudio(const uchar *, int, uchar);
    virtual int PlayTsVideo(const uchar *, int);
    virtual int PlayTsAudio(const uchar *, int);
    virtual void SetAudioChannelDevice(int);
    virtual int GetAudioChannelDevice(void);
    virtual void SetDigitalAudioDevice(bool);
    virtual void SetAudioTrackDevice(eTrackType);
    virtual void SetVolumeDevice(int);

// Image Grab facilities

    virtual uchar *GrabImage(int &, bool, int, int, int);

// SPU facilities
  private:
    cDvbSpuDecoder * spuDecoder;
  public:
    virtual cSpuDecoder * GetSpuDecoder(void);

  protected:
    virtual void MakePrimaryDevice(bool);
};

/**
**	Constructor device.
*/
cVaapiDevice::cVaapiDevice(void)
{
    spuDecoder = NULL;
}

/**
**	Destructor device.
*/
cVaapiDevice::~cVaapiDevice(void)
{
    delete spuDecoder;
}

/**
**	Informs a device that it will be the primary device.
**
**	@param on	flag if becoming or loosing primary
*/
void cVaapiDevice::MakePrimaryDevice(bool on)
{
    Debug1("%s: %d", __FUNCTION__, on);

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

/**
**	Get the device SPU decoder.
**
**	@returns a pointer to the device's SPU decoder (or NULL, if this
**	device doesn't have an SPU decoder)
*/
cSpuDecoder *cVaapiDevice::GetSpuDecoder(void)
{
    Debug1("%s:", __FUNCTION__);

    if (!spuDecoder && IsPrimaryDevice()) {
	spuDecoder = new cDvbSpuDecoder();
    }
    return spuDecoder;
}

/**
**      Returns a string identifying the name of this device.
*/
cString cVaapiDevice::DeviceName(void) const
{
    return "vaapidevice";
}

/**
**	Tells whether this device has a MPEG decoder.
*/
bool cVaapiDevice::HasDecoder(void) const
{
    return true;
}

/**
**	Returns true if this device can currently start a replay session.
*/
bool cVaapiDevice::CanReplay(void) const
{
    return true;
}

/**
**	Sets the device into the given play mode.
**
**	@param play_mode	new play mode (Audio/Video/External...)
*/
bool cVaapiDevice::SetPlayMode(ePlayMode play_mode)
{
    Debug1("%s: %d", __FUNCTION__, play_mode);

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
	    Debug1("Play mode external");
	    // FIXME: what if already suspended?
	    Suspend(1, 1, 0);
	    SuspendMode = SUSPEND_EXTERNAL;
	    return true;
	default:
	    Debug1("Play mode not implemented... %d", play_mode);
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
int64_t cVaapiDevice::GetSTC(void)
{
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
void cVaapiDevice::TrickSpeed(int speed, bool forward)
{
    Debug1("%s: %d %d", __FUNCTION__, speed, forward);

    ::TrickSpeed(speed);
}

/**
**	Clears all video and audio data from the device.
*/
void cVaapiDevice::Clear(void)
{
    Debug1("%s:", __FUNCTION__);

    cDevice::Clear();
    ::Clear();
}

/**
**	Sets the device into play mode (after a previous trick mode)
*/
void cVaapiDevice::Play(void)
{
    Debug1("%s:", __FUNCTION__);

    cDevice::Play();
    ::Play();
}

/**
**	Puts the device into "freeze frame" mode.
*/
void cVaapiDevice::Freeze(void)
{
    Debug1("%s:", __FUNCTION__);

    cDevice::Freeze();
    ::Freeze();
}

/**
**	Turns off audio while replaying.
*/
void cVaapiDevice::Mute(void)
{
    Debug1("%s:", __FUNCTION__);

    cDevice::Mute();
    ::Mute();
}

/**
**	Display the given I-frame as a still picture.
**
**	@param data	pes or ts data of a frame
**	@param length	length of data area
*/
void cVaapiDevice::StillPicture(const uchar * data, int length)
{
    Debug1("%s: %s %p %d\n", __FUNCTION__, data[0] == 0x47 ? "ts" : "pes", data, length);

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
bool cVaapiDevice::Poll( __attribute__ ((unused)) cPoller & poller, int timeout_ms)
{
    return::Poll(timeout_ms);
}

/**
**	Flush the device output buffers.
**
**	@param timeout_ms	timeout in ms to become ready
*/
bool cVaapiDevice::Flush(int timeout_ms)
{
    Debug1("%s: %d ms", __FUNCTION__, timeout_ms);

    return::Flush(timeout_ms);
}

// ----------------------------------------------------------------------------

/**
**	Sets the video display format to the given one (only useful if this
**	device has an MPEG decoder).
*/
void cVaapiDevice::SetVideoDisplayFormat(eVideoDisplayFormat video_display_format)
{
    Debug1("%s: %d", __FUNCTION__, video_display_format);

    cDevice::SetVideoDisplayFormat(video_display_format);
}

/**
**	Sets the output video format to either 16:9 or 4:3 (only useful
**	if this device has an MPEG decoder).
**
**	Should call SetVideoDisplayFormat.
**
**	@param video_format16_9	flag true 16:9.
*/
void cVaapiDevice::SetVideoFormat(bool video_format16_9)
{
    Debug1("%s: %d", __FUNCTION__, video_format16_9);

    // FIXME: 4:3 / 16:9 video format not supported.

    SetVideoDisplayFormat(eVideoDisplayFormat(Setup.VideoDisplayFormat));
}

/**
**	Returns the width, height and video_aspect ratio of the currently
**	displayed video material.
**
**	@note the video_aspect is used to scale the subtitle.
*/
void cVaapiDevice::GetVideoSize(int &width, int &height, double &video_aspect)
{
    ::GetVideoSize(&width, &height, &video_aspect);
}

/**
**	Returns the width, height and pixel_aspect ratio the OSD.
**
**	FIXME: Called every second, for nothing (no OSD displayed)?
*/
void cVaapiDevice::GetOsdSize(int &width, int &height, double &pixel_aspect)
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
int cVaapiDevice::PlayAudio(const uchar * data, int length, uchar id)
{
    return::PlayAudio(data, length, id);
}

void cVaapiDevice::SetAudioTrackDevice( __attribute__ ((unused)) eTrackType type)
{
}

void cVaapiDevice::SetDigitalAudioDevice( __attribute__ ((unused)) bool on)
{
}

void cVaapiDevice::SetAudioChannelDevice( __attribute__ ((unused))
    int audio_channel)
{
}

int cVaapiDevice::GetAudioChannelDevice(void)
{
    return 0;
}

/**
**	Sets the audio volume on this device (Volume = 0...255).
**
**	@param volume	device volume
*/
void cVaapiDevice::SetVolumeDevice(int volume)
{
    Debug1("%s: %d", __FUNCTION__, volume);

    ::SetVolumeDevice(volume);
}

// ----------------------------------------------------------------------------

/**
**	Play a video packet.
**
**	@param data	exactly one complete PES packet (which is incomplete)
**	@param length	length of PES packet
*/
int cVaapiDevice::PlayVideo(const uchar * data, int length)
{
    return::PlayVideo(data, length);
}

/**
**	Play a TS video packet.
**
**	@param data	ts data buffer
**	@param length	ts packet length (188)
*/
int cVaapiDevice::PlayTsVideo(const uchar * data, int length)
{
    return::PlayTsVideo(data, length);
}

/**
**	Play a TS audio packet.
**
**	@param data	ts data buffer
**	@param length	ts packet length (188)
*/
int cVaapiDevice::PlayTsAudio(const uchar * data, int length)
{
    if (SoftIsPlayingVideo != cDevice::IsPlayingVideo()) {
	SoftIsPlayingVideo = cDevice::IsPlayingVideo();
	Debug1("%s: SoftIsPlayingVideo: %d", __FUNCTION__, SoftIsPlayingVideo);
    }

    return::PlayTsAudio(data, length);
}

/**
**	Grabs the currently visible screen image.
**
**	@param size	size of the returned data
**	@param jpeg	flag true, create JPEG data
**	@param quality	JPEG quality
**	@param width	number of horizontal pixels in the frame
**	@param height	number of vertical pixels in the frame
*/
uchar *cVaapiDevice::GrabImage(int &size, bool jpeg, int quality, int width, int height)
{
    Debug1("%s: %d, %d, %d, %dx%d", __FUNCTION__, size, jpeg, quality, width, height);

    if (SuspendMode != NOT_SUSPENDED) {
	return NULL;
    }
    if (quality < 0) {			// caller should care, but fix it
	quality = 95;
    }

    return::GrabImage(&size, jpeg, quality, width, height);
}

/**
**	Ask the output, if it can scale video.
**
**	@param rect	requested video window rectangle
**
**	@returns the real rectangle or cRect:Null if invalid.
*/
cRect cVaapiDevice::CanScaleVideo(const cRect & rect, __attribute__ ((unused)) int alignment)
{
    return rect;
}

/**
**	Scale the currently shown video.
**
**	@param rect	video window rectangle
*/
void cVaapiDevice::ScaleVideo(const cRect & rect)
{
    ::ScaleVideo(rect.X(), rect.Y(), rect.Width(), rect.Height());
}

/**
**	Call rgb to jpeg for C Plugin.
*/
extern "C" uint8_t * CreateJpeg(uint8_t * image, int *size, int quality, int width, int height)
{
    return (uint8_t *) RgbToJpeg((uchar *) image, width, height, *size, quality);
}

//////////////////////////////////////////////////////////////////////////////
//  cPlugin
//////////////////////////////////////////////////////////////////////////////

class cPluginVaapiDevice:public cPlugin
{
  public:
    cPluginVaapiDevice(void);
    virtual ~ cPluginVaapiDevice(void);
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
cPluginVaapiDevice::cPluginVaapiDevice(void)
{
}

/**
**	Clean up after yourself!
*/
cPluginVaapiDevice::~cPluginVaapiDevice(void)
{
    ::SoftHdDeviceExit();
}

/**
**	Return plugin version number.
**
**	@returns version number as constant string.
*/
const char *cPluginVaapiDevice::Version(void)
{
    return VERSION;
}

/**
**	Return plugin short description.
**
**	@returns short description as constant string.
*/
const char *cPluginVaapiDevice::Description(void)
{
    return tr(DESCRIPTION);
}

/**
**	Return a string that describes all known command line options.
**
**	@returns command line help as constant string.
*/
const char *cPluginVaapiDevice::CommandLineHelp(void)
{
    return::CommandLineHelp();
}

/**
**	Process the command line arguments.
*/
bool cPluginVaapiDevice::ProcessArgs(int argc, char *argv[])
{
    for (int i = 0; i < argc; ++i) {
	CommandLineParameters = *cString::sprintf("%s %s", *CommandLineParameters, argv[i]);
    }
    return::ProcessArgs(argc, argv);
}

/**
**	Initializes the DVB devices.
**
**	Must be called before accessing any DVB functions.
**
**	@returns true if any devices are available.
*/
bool cPluginVaapiDevice::Initialize(void)
{
    MyDevice = new cVaapiDevice();
    MyDebug = new cDebugStatistics();

    return true;
}

/**
**	 Start any background activities the plugin shall perform.
*/
bool cPluginVaapiDevice::Start(void)
{
    if (!MyDevice->IsPrimaryDevice()) {
	Info("vaapidevice %d is not the primary device!", MyDevice->DeviceNumber());
	if (ConfigMakePrimary) {
	    // Must be done in the main thread
	    Debug1("Making vaapidevice %d the primary device!", MyDevice->DeviceNumber());
	    DoMakePrimary = MyDevice->DeviceNumber() + 1;
	}
    }

    csoft = new cSoftRemote;

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
void cPluginVaapiDevice::Stop(void)
{
    ::Stop();
    delete csoft;
    csoft = NULL;
}

/**
**	Perform any cleanup or other regular tasks.
*/
void cPluginVaapiDevice::Housekeeping(void)
{
    ::Housekeeping();
}

/**
**	Create main menu entry.
*/
const char *cPluginVaapiDevice::MainMenuEntry(void)
{
    return ConfigHideMainMenuEntry ? NULL : tr(MAINMENUENTRY);
}

/**
**	Perform the action when selected from the main VDR menu.
*/
cOsdObject *cPluginVaapiDevice::MainMenuAction(void)
{
    return new cSoftHdMenu("VA-API Device");
}

/**
**	Called for every plugin once during every cycle of VDR's main program
**	loop.
*/
void cPluginVaapiDevice::MainThreadHook(void)
{
    if (DoMakePrimary) {
	Debug1("%s: switching primary device to %d", __FUNCTION__, DoMakePrimary);
	cDevice::SetPrimaryDevice(DoMakePrimary);
	DoMakePrimary = 0;
    }

    ::MainThreadHook();
}

/**
**	Return our setup menu.
*/
cMenuSetupPage *cPluginVaapiDevice::SetupMenu(void)
{
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
bool cPluginVaapiDevice::SetupParse(const char *name, const char *value)
{
    int i;

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
    if (!strcasecmp(name, "60HzMode")) {
	VideoSet60HzMode(ConfigVideo60HzMode = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "SoftStartSync")) {
	VideoSetSoftStartSync(ConfigVideoSoftStartSync = atoi(value));
	return true;
    }
    if (!strcasecmp(name, "ColorBalance")) {
	VideoSetColorBalance(ConfigVideoColorBalance = atoi(value));
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
    if (!strcasecmp(name, "SkinToneEnhancement")) {
	VideoSetSkinToneEnhancement(ConfigVideoStde = atoi(value));
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
    }

    if (!strcasecmp(name, "AutoCrop.Interval")) {
	VideoSetAutoCrop(ConfigAutoCropInterval = atoi(value), ConfigAutoCropDelay, ConfigAutoCropTolerance);
	ConfigAutoCropEnabled = ConfigAutoCropInterval != 0;
	return true;
    }
    if (!strcasecmp(name, "AutoCrop.Delay")) {
	VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay = atoi(value), ConfigAutoCropTolerance);
	return true;
    }
    if (!strcasecmp(name, "AutoCrop.Tolerance")) {
	VideoSetAutoCrop(ConfigAutoCropInterval, ConfigAutoCropDelay, ConfigAutoCropTolerance = atoi(value));
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
	AudioSetBufferTime(ConfigAudioBufferTime);
	return true;
    }
    return false;
}

/**
**	Receive requests or messages.
**
**	@param id	unique identification string that identifies the
**			service protocol
**	@param data	custom data structure
*/
bool cPluginVaapiDevice::Service(const char *id, void *data)
{
    return false;
}

//----------------------------------------------------------------------------
//  cPlugin SVDRP
//----------------------------------------------------------------------------

/**
**	SVDRP commands help text.
**	FIXME: translation?
*/
static const char *SVDRPHelpText[] = {
    "SUSP\n" "\040	 Suspend plugin.\n\n" "	   The plugin is suspended to save energy. Depending on the setup\n"
	"    'vaapidevice.Suspend.Close = 0' only the video and audio output\n"
	"    is stopped or with 'vaapidevice.Suspend.Close = 1' the video\n" "	  and audio devices are closed.\n"
	"    If 'vaapidevice.Suspend.X11 = 1' is set and the X11 server was\n"
	"    started by the plugin, the X11 server would also be closed.\n"
	"    (Stopping X11 while suspended isn't supported yet)\n",
    "RESU\n" "\040	 Resume plugin.\n\n" "	  Resume the suspended plugin. The plugin could be suspended by\n"
	"    the command line option '-s' or by a previous SUSP command.\n"
	"    If the x11 server was stopped by the plugin, it will be\n" "    restarted.",
    "DETA\n" "\040	 Detach plugin.\n\n" "	  The plugin will be detached from the audio, video and DVB\n"
	"    devices.  Other programs or plugins can use them now.\n",
    "ATTA <-d display> <-a audio> <-p pass>\n" "	Attach plugin.\n\n"
	"    Attach the plugin to audio, video and DVB devices. Use:\n"
	"    -d display\tdisplay of x11 server (fe. :0.0)\n"
	"    -a audio\taudio device (fe. alsa: hw:0,0 oss: /dev/dsp)\n"
	"    -p pass\t\taudio device for pass-through (hw:0,1 or /dev/dsp1)\n",
    "PRIM <n>\n" "	  Make <n> the primary device.\n\n"
	"    <n> is the number of device. Without number vaapidevice becomes\n"
	"    the primary device. If becoming primary, the plugin is attached\n"
	"    to the devices. If loosing primary, the plugin is detached from\n" "    the devices.",
    "HOTK key\n" "	  Execute hotkey.\n\n" "	key is the hotkey number, following are supported:\n"
	"    10: disable audio pass-through\n" "    11: enable audio pass-through\n"
	"    12: toggle audio pass-through\n" "	   13: decrease audio delay by 10ms\n"
	"    14: increase audio delay by 10ms\n" "    15: toggle ac3 mixdown\n"
	"    20: disable fullscreen\n\040   21: enable fullscreen\n" "	  22: toggle fullscreen\n"
	"    23: disable auto-crop\n\040   24: enable auto-crop\n" "	25: toggle auto-crop\n"
	"    30: stretch 4:3 to display\n\040	31: pillar box 4:3 in display\n"
	"    32: center cut-out 4:3 to display\n" "    39: rotate 4:3 to display zoom mode\n"
	"    40: stretch other aspect ratios to display\n" "	41: letter box other aspect ratios in display\n"
	"    42: center cut-out other aspect ratios to display\n"
	"    49: rotate other aspect ratios to display zoom mode\n" "	 50: toggle debug statistics osd\n",
    "STAT\n" "\040	 Display SuspendMode of the plugin.\n\n" "	  reply code is 910 + SuspendMode\n"
	"    SUSPEND_EXTERNAL == -1  (909)\n" "	   NOT_SUSPENDED    ==	0  (910)\n"
	"    SUSPEND_NORMAL   ==  1  (911)\n" "	   SUSPEND_DETACHED ==	2  (912)\n",
    "RAIS\n" "\040	 Raise vaapidevice window\n\n" "	If Xserver is not started by vaapidevice, the window which\n"
	"    contains the vaapidevice frontend will be raised to the front.\n",
    "TRAC [ <mode> ]\n" "    Get and/or set used tracing mode.\n",
    "DBUG\n" "\040	 Show debug information.\n",
    NULL
};

/**
**	Return SVDRP commands help pages.
**
**	return a pointer to a list of help strings for all of the plugin's
**	SVDRP commands.
*/
const char **cPluginVaapiDevice::SVDRPHelpPages(void)
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
cString cPluginVaapiDevice::SVDRPCommand(const char *command, const char *option,
    __attribute__ ((unused)) int &reply_code)
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
	    return "VA-API device already suspended";
	}
	if (SuspendMode != NOT_SUSPENDED) {
	    return "VA-API device already detached";
	}
	cControl::Launch(new cSoftHdControl);
	cControl::Attach();
	Suspend(ConfigSuspendClose, ConfigSuspendClose, ConfigSuspendX11);
	SuspendMode = SUSPEND_NORMAL;
	return "VA-API device is suspended";
    }
    if (!strcasecmp(command, "RESU")) {
	if (SuspendMode == NOT_SUSPENDED) {
	    return "VA-API device already resumed";
	}
	if (SuspendMode != SUSPEND_NORMAL) {
	    return "can't resume VA-API device";
	}
	if (ShutdownHandler.GetUserInactiveTime()) {
	    ShutdownHandler.SetUserInactiveTimeout();
	}
	if (cSoftHdControl::Player) {	// suspended
	    cControl::Shutdown();	// not need, if not suspended
	}
	Resume();
	SuspendMode = NOT_SUSPENDED;
	return "VA-API device is resumed";
    }
    if (!strcasecmp(command, "DETA")) {
	if (SuspendMode == SUSPEND_DETACHED) {
	    return "VA-API device already detached";
	}
	if (cSoftHdControl::Player) {	// already suspended
	    return "can't suspend VA-API device already suspended";
	}
	cControl::Launch(new cSoftHdControl);
	cControl::Attach();
	Suspend(1, 1, 0);
	SuspendMode = SUSPEND_DETACHED;
	return "VA-API device is detached";
    }
    if (!strcasecmp(command, "ATTA")) {
	char *tmp;
	char *t;
	char *s;
	char *o;

	if (SuspendMode != SUSPEND_DETACHED) {
	    return "can't attach VA-API device not detached";
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
	return "VA-API device is attached";
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
	Debug1("Switching primary device to %d", primary);
	DoMakePrimary = primary;
	return "switching primary device requested";
    }
    if (!strcasecmp(command, "RAIS")) {
	if (!ConfigStartX11Server) {
	    VideoRaiseWindow();
	} else {
	    return "Raise not possible";
	}
	return "Window raised";
    }
    if (!strcasecmp(command, "TRAC")) {
	if (option && *option)
	    TraceMode = strtol(option, NULL, 0) & 0xFFFF;
	return cString::sprintf("tracing mode: 0x%04X\n", TraceMode);
    }
    if (!strcasecmp(command, "DBUG")) {
	return MyDebug->Dump();
    }

    return NULL;
}

VDRPLUGINCREATOR(cPluginVaapiDevice);	// Don't touch this!
