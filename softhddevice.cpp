///
///	@file softhddevice.cpp	@brief A software HD device plugin for VDR.
///
///	Copyright (c) 2011, 2012 by Johns.  All Rights Reserved.
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
extern "C" {
    #include "video.h"
    extern void AudioPoller(void);
}

//////////////////////////////////////////////////////////////////////////////

static const char *const VERSION = "0.1.5";
static const char *const DESCRIPTION =
trNOOP("A software and GPU emulated HD device");

//static const char *MAINMENUENTRY = trNOOP("Soft-HD-Device");
static class cSoftHdDevice *MyDevice;

//////////////////////////////////////////////////////////////////////////////

static char ConfigMakePrimary;		///< config primary wanted
static char ConfigVideoDeinterlace;	///< config deinterlace
static char ConfigVideoSkipChromaDeinterlace;	///< config skip chroma
static int ConfigVideoDenoise;		///< config denoise
static int ConfigVideoSharpen;		///< config sharpen
static char ConfigVideoScaling;		///< config scaling
static int ConfigVideoAudioDelay;	///< config audio delay
static char DoMakePrimary;		///< flag switch primary

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
//	C Callbacks
//////////////////////////////////////////////////////////////////////////////

class cSoftRemote:public cRemote
{
  public:
    cSoftRemote(const char *name):cRemote(name)
    {
    }

    bool Put(const char *code, bool repeat = false, bool release = false) {
	return cRemote::Put(code, repeat, release);
    }
};

extern "C" void FeedKeyPress(const char *keymap, const char *key, int repeat,
    int release)
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

    if (remote) {
	csoft = (cSoftRemote *) remote;
    } else {
	dsyslog("[softhddev]%s: remote '%s' not found\n", __FUNCTION__,
	    keymap);
	csoft = new cSoftRemote(keymap);
    }

    dsyslog("[softhddev]%s %s, %s\n", __FUNCTION__, keymap, key);
    csoft->Put(key, repeat, release);
}

//////////////////////////////////////////////////////////////////////////////
//	OSD
//////////////////////////////////////////////////////////////////////////////

class cSoftOsd:public cOsd
{
  public:
    cSoftOsd(int, int, uint);
     virtual ~ cSoftOsd(void);
    virtual void Flush(void);
    // virtual void SetActive(bool);
};

cSoftOsd::cSoftOsd(int left, int top, uint level)
:cOsd(left, top, level)
{
    // FIXME: OsdWidth/OsdHeight not correct!
    dsyslog("[softhddev]%s: %dx%d+%d+%d, %d\n", __FUNCTION__, OsdWidth(),
	OsdHeight(), left, top, level);

    //SetActive(true);
}

cSoftOsd::~cSoftOsd(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);
    SetActive(false);

    OsdClose();
}

///
///	Actually commits all data to the OSD hardware.
///
void cSoftOsd::Flush(void)
{
    cPixmapMemory *pm;

    if (!Active()) {
	return;
    }
    //dsyslog("[softhddev]%s:\n", __FUNCTION__);
    if (!IsTrueColor()) {
	static char warned;
	cBitmap *bitmap;
	int i;

	if (!warned) {
	    dsyslog("[softhddev]%s: FIXME: should be truecolor\n",
		__FUNCTION__);
	    warned = 1;
	}
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
	    if (!bitmap->Dirty(x1, y1, x2, y2)) {
		continue;		// nothing dirty continue
	    }
	    // FIXME: need only to convert and upload dirty areas

	    // DrawBitmap(bitmap);
	    w = bitmap->Width();
	    h = bitmap->Height();
	    argb = (uint8_t *) malloc(w * h * sizeof(uint32_t));

	    for (y = 0; y < h; ++y) {
		for (x = 0; x < w; ++x) {
		    ((uint32_t *) argb)[x + y * w] = bitmap->GetColor(x, y);
		}
	    }

	    OsdDrawARGB(Left() + bitmap->X0(), Top() + bitmap->Y0(),
		bitmap->Width(), bitmap->Height(), argb);

	    bitmap->Clean();
	    free(argb);
	}
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

	dsyslog("[softhddev]%s: draw %dx%d+%d+%d %p\n", __FUNCTION__, w, h, x,
	    y, pm->Data());

	OsdDrawARGB(x, y, w, h, pm->Data());

	delete pm;
    }
}

//////////////////////////////////////////////////////////////////////////////
//	OSD provider
//////////////////////////////////////////////////////////////////////////////

class cSoftOsdProvider:public cOsdProvider
{
  private:
    static cOsd *Osd;
  public:
    virtual cOsd * CreateOsd(int, int, uint);
    virtual bool ProvidesTrueColor(void);
    cSoftOsdProvider(void);
};

cOsd *cSoftOsdProvider::Osd;		///< single osd

/**
**	Create a new OSD.
*/
cOsd *cSoftOsdProvider::CreateOsd(int left, int top, uint level)
{
    dsyslog("[softhddev]%s: %d, %d, %d\n", __FUNCTION__, left, top, level);

    Osd = new cSoftOsd(left, top, level);
    return Osd;
}

/**
**	 Returns true if this OSD provider is able to handle a true color OSD.
*/
bool cSoftOsdProvider::ProvidesTrueColor(void)
{
    return true;
}

cSoftOsdProvider::cSoftOsdProvider(void)
:  cOsdProvider()
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

//////////////////////////////////////////////////////////////////////////////
//	cMenuSetupPage
//////////////////////////////////////////////////////////////////////////////

class cMenuSetupSoft:public cMenuSetupPage
{
  protected:
    int MakePrimary;
    int Deinterlace;
    int SkipChromaDeinterlace;
    int Denoise;
    int Sharpen;
    int Scaling;
    int AudioDelay;
  protected:
     virtual void Store(void);
  public:
     cMenuSetupSoft(void);
};

/**
**	Constructor setup menu.
*/
cMenuSetupSoft::cMenuSetupSoft(void)
{
    static const char *const deinterlace[] = {
	"Bob", "Weave", "Temporal", "TemporalSpatial", "Software"
    };
    static const char *const scaling[] = {
	"Normal", "Fast", "HQ", "Anamorphic"
    };

    // cMenuEditBoolItem cMenuEditBitItem cMenuEditNumItem
    // cMenuEditStrItem cMenuEditStraItem cMenuEditIntItem
    MakePrimary = ConfigMakePrimary;
    Add(new cMenuEditBoolItem(tr("Make primary device"), &MakePrimary,
	    tr("no"), tr("yes")));
    Deinterlace = ConfigVideoDeinterlace;
    Add(new cMenuEditStraItem(tr("Deinterlace"), &Deinterlace, 5,
	    deinterlace));
    SkipChromaDeinterlace = ConfigVideoSkipChromaDeinterlace;
    Add(new cMenuEditBoolItem(tr("SkipChromaDeinterlace (vdpau)"),
	    &SkipChromaDeinterlace, tr("no"), tr("yes")));
    Denoise = ConfigVideoDenoise;
    Add(new cMenuEditIntItem(tr("Denoise (vdpau 0..1000)"), &Denoise, 0,
	    1000));
    Sharpen = ConfigVideoSharpen;
    Add(new cMenuEditIntItem(tr("Sharpen (vdpau -1000..1000)"), &Sharpen,
	    -1000, 1000));
    Scaling = ConfigVideoScaling;
    Add(new cMenuEditStraItem(tr("Scaling"), &Scaling, 4, scaling));
    AudioDelay = ConfigVideoAudioDelay;
    Add(new cMenuEditIntItem(tr("Audio delay (ms)"), &AudioDelay, -1000,
	    1000));
}

/**
**	Store setup.
*/
void cMenuSetupSoft::Store(void)
{
    SetupStore("MakePrimary", ConfigMakePrimary = MakePrimary);
    SetupStore("Deinterlace", ConfigVideoDeinterlace = Deinterlace);
    VideoSetDeinterlace(ConfigVideoDeinterlace);
    SetupStore("SkipChromaDeinterlace", ConfigVideoSkipChromaDeinterlace =
	SkipChromaDeinterlace);
    VideoSetSkipChromaDeinterlace(ConfigVideoSkipChromaDeinterlace);
    SetupStore("Denoise", ConfigVideoDenoise = Denoise);
    VideoSetDenoise(ConfigVideoDenoise);
    SetupStore("Sharpen", ConfigVideoSharpen = Sharpen);
    VideoSetSharpen(ConfigVideoSharpen);
    SetupStore("Scaling", ConfigVideoScaling = Scaling);
    VideoSetScaling(ConfigVideoScaling);
    SetupStore("AudioDelay", ConfigVideoAudioDelay = AudioDelay);
    VideoSetAudioDelay(ConfigVideoAudioDelay);
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
    virtual void TrickSpeed(int);
    virtual void Clear(void);
    virtual void Play(void);
    virtual void Freeze(void);
    virtual void Mute(void);
    virtual void SetVolumeDevice(int);
    virtual void StillPicture(const uchar *, int);
    virtual bool Poll(cPoller &, int = 0);
    virtual bool Flush(int = 0);
    virtual int64_t GetSTC(void);
    virtual void GetOsdSize(int &, int &, double &);
    virtual int PlayVideo(const uchar *, int);
    //virtual int PlayTsVideo(const uchar *, int);
#ifdef USE_OSS				// FIXME: testing only oss
    virtual int PlayTsAudio(const uchar *, int);
#endif
    virtual void SetAudioChannelDevice(int);
    virtual int GetAudioChannelDevice(void);
    virtual void SetDigitalAudioDevice(bool);
    virtual void SetAudioTrackDevice(eTrackType);
    virtual int PlayAudio(const uchar *, int, uchar);

// Image Grab facilities

    virtual uchar *GrabImage(int &, bool, int, int, int);

    virtual int ProvidesCa(const cChannel *) const;

// SPU facilities
  private:
    cDvbSpuDecoder * spuDecoder;
  public:
    virtual cSpuDecoder * GetSpuDecoder(void);

  protected:
    virtual void MakePrimaryDevice(bool);
};

cSoftHdDevice::cSoftHdDevice(void)
{
    dsyslog("[softhddev]%s\n", __FUNCTION__);

    spuDecoder = NULL;
}

cSoftHdDevice::~cSoftHdDevice(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

void cSoftHdDevice::MakePrimaryDevice(bool on)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, on);

    cDevice::MakePrimaryDevice(on);
    if (on) {
	new cSoftOsdProvider();
    }
}

int cSoftHdDevice::ProvidesCa(
    __attribute__ ((unused)) const cChannel * channel) const
{
    dsyslog("[softhddev]%s: %p\n", __FUNCTION__, channel);

    return 0;
}

cSpuDecoder *cSoftHdDevice::GetSpuDecoder(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (IsPrimaryDevice() && !spuDecoder) {
	spuDecoder = new cDvbSpuDecoder();
    }
    return spuDecoder;
}

bool cSoftHdDevice::HasDecoder(void) const
{
    return true;
}

bool cSoftHdDevice::CanReplay(void) const
{
    return true;
}

bool cSoftHdDevice::SetPlayMode(ePlayMode PlayMode)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, PlayMode);

    switch (PlayMode) {
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
	    break;
	default:
	    dsyslog("[softhddev]playmode not implemented... %d\n", PlayMode);
	    break;
    }
    ::SetPlayMode();
    return true;
}

int64_t cSoftHdDevice::GetSTC(void)
{
    // dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return::VideoGetClock();
}

void cSoftHdDevice::TrickSpeed(int Speed)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, Speed);
}

void cSoftHdDevice::Clear(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Clear();
    ::Clear();
}

void cSoftHdDevice::Play(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Play();
    ::Play();
}

void cSoftHdDevice::Freeze(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Freeze();
    ::Freeze();
}

void cSoftHdDevice::Mute(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    cDevice::Mute();

    ::Mute();
}

void cSoftHdDevice::SetVolumeDevice(int volume)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, volume);

    ::SetVolumeDevice(volume);
}

void cSoftHdDevice::StillPicture(
    __attribute__ ((unused)) const uchar * data, __attribute__ ((unused))
    int length)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

bool cSoftHdDevice::Poll(
    __attribute__ ((unused)) cPoller & poller, int timeout_ms)
{
    // dsyslog("[softhddev]%s: %d\n", __FUNCTION__, timeout_ms);

    return::Poll(timeout_ms);
}

bool cSoftHdDevice::Flush(int timeout_ms)
{
    dsyslog("[softhddev]%s: %d ms\n", __FUNCTION__, timeout_ms);

    return true;
}

// ----------------------------------------------------------------------------

/**
**	Returns the With, Height and PixelAspect ratio the OSD.
**
**	FIXME: Called every second, for nothing (no OSD displayed)?
*/
void cSoftHdDevice::GetOsdSize(int &width, int &height, double &pixel_aspect)
{
    ::GetOsdSize(&width, &height, &pixel_aspect);
}

// ----------------------------------------------------------------------------

int cSoftHdDevice::PlayAudio(const uchar * data, int length, uchar id)
{
    //dsyslog("[softhddev]%s: %p %p %d %d\n", __FUNCTION__, this, data, length, id);

    ::PlayAudio(data, length, id);

    return length;
}

void cSoftHdDevice::SetAudioTrackDevice(
    __attribute__ ((unused)) eTrackType type)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

void cSoftHdDevice::SetDigitalAudioDevice(bool on)
{
    dsyslog("[softhddev]%s: %s\n", __FUNCTION__, on ? "true" : "false");
}

void cSoftHdDevice::SetAudioChannelDevice(int audio_channel)
{
    dsyslog("[softhddev]%s: %d\n", __FUNCTION__, audio_channel);
}

int cSoftHdDevice::GetAudioChannelDevice(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);
    return 0;
}

// ----------------------------------------------------------------------------

///
///	Play a video packet.
///
int cSoftHdDevice::PlayVideo(const uchar * data, int length)
{
    //dsyslog("[softhddev]%s: %p %d\n", __FUNCTION__, data, length);

    return::PlayVideo(data, length);
}

#if 0
///
///	Play a TS video packet.
///
int cSoftHdDevice::PlayTsVideo(const uchar * Data, int Length)
{
    // many code to repeat
}
#endif

#ifdef USE_OSS				// FIXME: testing only oss
///
///	Play a TS audio packet.
///
///	misuse this function as audio poller
///
int cSoftHdDevice::PlayTsAudio(const uchar * data, int length)
{
    AudioPoller();

    return cDevice::PlayTsAudio(data, length);
}
#endif

uchar *cSoftHdDevice::GrabImage(int &size, bool jpeg, int quality, int sizex,
    int sizey)
{
    dsyslog("[softhddev]%s: %d, %d, %d, %dx%d\n", __FUNCTION__, size, jpeg,
	quality, sizex, sizey);

    return NULL;
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
//    virtual void Housekeeping(void);
    virtual void MainThreadHook(void);
//    virtual const char *MainMenuEntry(void);
//    virtual cOsdObject *MainMenuAction(void);
    virtual cMenuSetupPage *SetupMenu(void);
    virtual bool SetupParse(const char *, const char *);
//    virtual bool Service(const char *Id, void *Data = NULL);
};

cPluginSoftHdDevice::cPluginSoftHdDevice(void)
{
    // Initialize any member variables here.
    // DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
    // VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
    dsyslog("[softhddev]%s:\n", __FUNCTION__);
}

cPluginSoftHdDevice::~cPluginSoftHdDevice(void)
{
    // Clean up after yourself!
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    ::SoftHdDeviceExit();
}

const char *cPluginSoftHdDevice::Version(void)
{
    return VERSION;
}

const char *cPluginSoftHdDevice::Description(void)
{
    return tr(DESCRIPTION);
}

/**
**	Return a string that describes all known command line options.
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
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return::ProcessArgs(argc, argv);
}

bool cPluginSoftHdDevice::Initialize(void)
{
    // Start any background activities the plugin shall perform.
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    MyDevice = new cSoftHdDevice();

    return true;
}

bool cPluginSoftHdDevice::Start(void)
{
    const cDevice *primary;

    // Start any background activities the plugin shall perform.
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    primary = cDevice::PrimaryDevice();
    if (MyDevice != primary) {
	isyslog("[softhddev] softhddevice is not the primary device!");
	if (ConfigMakePrimary) {
	    // Must be done in the main thread
	    dsyslog("[softhddev] makeing softhddevice %d the primary device!",
		MyDevice->DeviceNumber());
	    DoMakePrimary = 1;
	} else {
	    isyslog("[softhddev] softhddevice %d is not the primary device!",
		MyDevice->DeviceNumber());
	}
    }

    ::Start();

    return true;
}

void cPluginSoftHdDevice::Stop(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    ::Stop();
}

#if 0

void cPluginSoftHdDevice::Housekeeping(void)
{
    // Perform any cleanup or other regular tasks.
}

const char *cPluginSoftHdDevice::MainMenuEntry(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);
    return tr(MAINMENUENTRY);
    return NULL;
}

#endif

/**
**	Called for every plugin once during every cycle of VDR's main program
**	loop.
*/
void cPluginSoftHdDevice::MainThreadHook(void)
{
    // dsyslog("[softhddev]%s:\n", __FUNCTION__);

    if (DoMakePrimary && MyDevice) {
	dsyslog("[softhddev]%s: switching primary device\n", __FUNCTION__);
	cDevice::SetPrimaryDevice(MyDevice->DeviceNumber() + 1);
	DoMakePrimary = 0;
    }

    ::MainThreadHook();
}

#if 0

bool cPluginSoftHdDevice::Service(const char *Id, void *Data)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return false;
}

cOsdObject *cPluginSoftHdDevice::MainMenuAction(void)
{
    // Perform the action when selected from the main VDR menu.
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return NULL;
}

#endif

/**
**	Return our setup menu.
*/
cMenuSetupPage *cPluginSoftHdDevice::SetupMenu(void)
{
    dsyslog("[softhddev]%s:\n", __FUNCTION__);

    return new cMenuSetupSoft;
}

/**
**	Parse setup parameters
*/
bool cPluginSoftHdDevice::SetupParse(const char *name, const char *value)
{
    dsyslog("[softhddev]%s: '%s' = '%s'\n", __FUNCTION__, name, value);

    // FIXME: handle the values
    if (!strcmp(name, "MakePrimary")) {
	ConfigMakePrimary = atoi(value);
	return true;
    }
    if (!strcmp(name, "Deinterlace")) {
	VideoSetDeinterlace(ConfigVideoDeinterlace = atoi(value));
	return true;
    }
    if (!strcmp(name, "SkipChromaDeinterlace")) {
	VideoSetSkipChromaDeinterlace(ConfigVideoSkipChromaDeinterlace =
	    atoi(value));
	return true;
    }
    if (!strcmp(name, "Denoise")) {
	VideoSetDenoise(ConfigVideoDenoise = atoi(value));
	return true;
    }
    if (!strcmp(name, "Sharpen")) {
	VideoSetSharpen(ConfigVideoSharpen = atoi(value));
	return true;
    }
    if (!strcmp(name, "Scaling")) {
	VideoSetScaling(ConfigVideoScaling = atoi(value));
	return true;
    }
    if (!strcmp(name, "AudioDelay")) {
	VideoSetAudioDelay(ConfigVideoAudioDelay = atoi(value));
	return true;
    }

    return false;
}

VDRPLUGINCREATOR(cPluginSoftHdDevice);	// Don't touch this!
