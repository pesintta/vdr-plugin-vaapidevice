///
///	@file audio.c		@brief Audio module
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

///
///	@defgroup Audio The audio module.
///
///		This module contains all audio output functions.
///
///		ALSA PCM/Mixer api is supported.
///		@see http://www.alsa-project.org/alsa-doc/alsa-lib
///
///	@note alsa async playback is broken, don't use it!
///
///		OSS PCM/Mixer api is supported.
///		@see http://manuals.opensound.com/developer/
///
///
///	@todo FIXME: there can be problems with little/big endian.
///	@todo FIXME: can combine OSS and alsa ring buffer
///

//#define USE_ALSA			///< enable alsa support
//#define USE_OSS			///< enable OSS support
#define USE_AUDIO_THREAD		///< use thread for audio playback
#define noUSE_AUDIORING			///< new audio ring code (incomplete)

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#ifdef USE_ALSA
#include <alsa/asoundlib.h>
#endif
#ifdef USE_OSS
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
// SNDCTL_DSP_HALT_OUTPUT compatibility
#ifndef SNDCTL_DSP_HALT_OUTPUT
#  if defined(SNDCTL_DSP_RESET_OUTPUT)
#    define SNDCTL_DSP_HALT_OUTPUT SNDCTL_DSP_RESET_OUTPUT
#  elif defined(SNDCTL_DSP_RESET)
#    define SNDCTL_DSP_HALT_OUTPUT SNDCTL_DSP_RESET
#  else
#    error "No valid SNDCTL_DSP_HALT_OUTPUT found."
#  endif
#endif
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

#ifdef USE_AUDIO_THREAD
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>
#ifndef HAVE_PTHREAD_NAME
    /// only available with newer glibc
#define pthread_setname_np(thread, name)
#endif
#endif

#include <alsa/iatomic.h>		// portable atomic_t

#include "ringbuffer.h"
#include "misc.h"
#include "audio.h"

//----------------------------------------------------------------------------
//	Declarations
//----------------------------------------------------------------------------

/**
**	Audio output module structure and typedef.
*/
typedef struct _audio_module_
{
    const char *Name;			///< audio output module name

    void (*Thread) (void);		///< module thread handler
    void (*Enqueue) (const void *, int);	///< enqueue samples for output
    void (*FlushBuffers) (void);	///< flush sample buffers
    void (*Poller) (void);		///< output poller
    int (*FreeBytes) (void);		///< number of bytes free in buffer
     uint64_t(*GetDelay) (void);	///< get current audio delay
    void (*SetVolume) (int);		///< set output volume
    int (*Setup) (int *, int *, int);	///< setup channels, samplerate
    void (*Play) (void);		///< play
    void (*Pause) (void);		///< pause
    void (*Init) (void);		///< initialize audio output module
    void (*Exit) (void);		///< cleanup audio output module
} AudioModule;

static const AudioModule NoopModule;	///< forward definition of noop module

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

char AudioAlsaDriverBroken;		///< disable broken driver message

static const char *AudioModuleName;	///< which audio module to use

    /// Selected audio module.
static const AudioModule *AudioUsedModule = &NoopModule;
static const char *AudioPCMDevice;	///< alsa/OSS PCM device name
static const char *AudioAC3Device;	///< alsa/OSS AC3 device name
static const char *AudioMixerDevice;	///< alsa/OSS mixer device name
static const char *AudioMixerChannel;	///< alsa/OSS mixer channel name
static volatile char AudioRunning;	///< thread running / stopped
static volatile char AudioPaused;	///< audio paused
static unsigned AudioSampleRate;	///< audio sample rate in hz
static unsigned AudioChannels;		///< number of audio channels
static const int AudioBytesProSample = 2;	///< number of bytes per sample
static int64_t AudioPTS;		///< audio pts clock
static const int AudioBufferTime = 350;	///< audio buffer time in ms

#ifdef USE_AUDIO_THREAD
static pthread_t AudioThread;		///< audio play thread
static pthread_mutex_t AudioMutex;	///< audio condition mutex
static pthread_cond_t AudioStartCond;	///< condition variable
#else
static const int AudioThread;		///< dummy audio thread
#endif

extern int VideoAudioDelay;		/// import audio/video delay

#ifdef USE_AUDIORING

//----------------------------------------------------------------------------
//	ring buffer
//----------------------------------------------------------------------------

// FIXME: use this code, to combine alsa&OSS ring buffers

#define AUDIO_RING_MAX 8		///< number of audio ring buffers

/**
**	Audio ring buffer.
*/
typedef struct _audio_ring_ring_
{
    char FlushBuffers;			///< flag: flush buffers
    unsigned SampleRate;		///< sample rate in hz
    unsigned Channels;			///< number of channels
} AudioRingRing;

    /// ring of audio ring buffers
static AudioRingRing AudioRing[AUDIO_RING_MAX];
static int AudioRingWrite;		///< audio ring write pointer
static int AudioRingRead;		///< audio ring read pointer
static atomic_t AudioRingFilled;	///< how many of the ring is used

/**
**	Add sample rate, number of channel change to ring.
**
**	@param freq	sample frequency
**	@param channels	number of channels
*/
static int AudioRingAdd(int freq, int channels)
{
    int filled;

    filled = atomic_read(&AudioRingFilled);
    if (filled == AUDIO_RING_MAX) {	// no free slot
	// FIXME: can wait for ring buffer empty
	Error(_("audio: out of ring buffers\n"));
	return -1;
    }
    AudioRing[AudioRingWrite].FlushBuffers = 1;
    AudioRing[AudioRingWrite].SampleRate = freq;
    AudioRing[AudioRingWrite].Channels = channels;

    AudioRingWrite = (AudioRingWrite + 1) % AUDIO_RING_MAX;
    atomic_inc(&AudioRingFilled);

#ifdef USE_AUDIO_THREAD
    // tell thread, that something todo
    AudioRunning = 1;
    pthread_cond_signal(&AudioStartCond);
#endif

    return 0;
}

/**
**	Setup audio ring.
*/
static void AudioRingInit(void)
{
    int i;

    for (i = 0; i < AUDIO_RING_MAX; ++i) {
	// FIXME:
	//AlsaRingBuffer = RingBufferNew(48000 * 8 * 2);	// ~1s 8ch 16bit
    }
    // one slot always reservered
    AudioRingWrite = 1;
    atomic_set(&AudioRingFilled, 1);
}

/**
**	Cleanup audio ring.
*/
static void AudioRingExit(void)
{
    int i;

    for (i = 0; i < AUDIO_RING_MAX; ++i) {
	// FIXME:
	//RingBufferDel(AlsaRingBuffer);
    }
}

#endif

#ifdef USE_ALSA

//============================================================================
//	A L S A
//============================================================================

//----------------------------------------------------------------------------
//	Alsa variables
//----------------------------------------------------------------------------

static snd_pcm_t *AlsaPCMHandle;	///< alsa pcm handle
static char AlsaCanPause;		///< hw supports pause
static int AlsaUseMmap;			///< use mmap

static RingBuffer *AlsaRingBuffer;	///< audio ring buffer
static unsigned AlsaStartThreshold;	///< start play, if filled

#ifdef USE_AUDIO_THREAD
static volatile char AlsaFlushBuffer;	///< flag empty buffer
#endif

static snd_mixer_t *AlsaMixer;		///< alsa mixer handle
static snd_mixer_elem_t *AlsaMixerElem;	///< alsa pcm mixer element
static int AlsaRatio;			///< internal -> mixer ratio * 1000

//----------------------------------------------------------------------------
//	alsa pcm
//----------------------------------------------------------------------------

/**
**	Place samples in ringbuffer.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
**
**	@returns true if play should be started.
*/
static int AlsaAddToRingbuffer(const void *samples, int count)
{
    int n;

    n = RingBufferWrite(AlsaRingBuffer, samples, count);
    if (n != count) {
	Error(_("audio/alsa: can't place %d samples in ring buffer\n"), count);
	// too many bytes are lost
	// FIXME: should skip more, longer skip, but less often?
    }
    // Update audio clock (stupid gcc developers thinks INT64_C is unsigned)
    if (AudioPTS != (int64_t) INT64_C(0x8000000000000000)) {
	AudioPTS +=
	    ((int64_t) count * 90000) / (AudioSampleRate * AudioChannels *
	    AudioBytesProSample);
    }

    if (!AudioRunning) {
	if (AlsaStartThreshold < RingBufferUsedBytes(AlsaRingBuffer)) {
	    // restart play-back
	    return 1;
	}
    }

    return 0;
}

/**
**	Play samples from ringbuffer.
*/
static int AlsaPlayRingbuffer(void)
{
    int first;
    int avail;
    int n;
    int err;
    int frames;
    const void *p;

    first = 1;
    for (;;) {
	// how many bytes can be written?
	n = snd_pcm_avail_update(AlsaPCMHandle);
	if (n < 0) {
	    if (n == -EAGAIN) {
		continue;
	    }
	    Error(_("audio/alsa: underrun error?\n"));
	    err = snd_pcm_recover(AlsaPCMHandle, n, 0);
	    if (err >= 0) {
		continue;
	    }
	    Error(_("audio/alsa: snd_pcm_avail_update(): %s\n"),
		snd_strerror(n));
	    return -1;
	}
	avail = snd_pcm_frames_to_bytes(AlsaPCMHandle, n);
	if (avail < 256) {		// too much overhead
	    if (first) {
		// happens with broken alsa drivers
		if (AudioThread) {
		    if (!AudioAlsaDriverBroken) {
			Error(_("audio/alsa: broken driver %d\n"), avail);
		    }
		    usleep(5 * 1000);
		}
	    }
	    Debug(4, "audio/alsa: break state %s\n",
		snd_pcm_state_name(snd_pcm_state(AlsaPCMHandle)));
	    break;
	}
	n = RingBufferGetReadPointer(AlsaRingBuffer, &p);
	if (!n) {			// ring buffer empty
	    if (first) {		// only error on first loop
		return 1;
	    }
	    return 0;
	}
	if (n < avail) {		// not enough bytes in ring buffer
	    avail = n;
	}
	if (!avail) {			// full or buffer empty
	    break;
	}
	frames = snd_pcm_bytes_to_frames(AlsaPCMHandle, avail);

      again:
	if (AlsaUseMmap) {
	    err = snd_pcm_mmap_writei(AlsaPCMHandle, p, frames);
	} else {
	    err = snd_pcm_writei(AlsaPCMHandle, p, frames);
	}
	//Debug(3, "audio/alsa: wrote %d/%d frames\n", err, frames);
	if (err != frames) {
	    if (err < 0) {
		if (err == -EAGAIN) {
		    goto again;
		}
		/*
		   if (err == -EBADFD) {
		   goto again;
		   }
		 */
		Error(_("audio/alsa: underrun error?\n"));
		err = snd_pcm_recover(AlsaPCMHandle, err, 0);
		if (err >= 0) {
		    goto again;
		}
		Error(_("audio/alsa: snd_pcm_writei failed: %s\n"),
		    snd_strerror(err));
		return -1;
	    }
	    // this could happen, if underrun happened
	    Error(_("audio/alsa: error not all frames written\n"));
	    avail = snd_pcm_frames_to_bytes(AlsaPCMHandle, err);
	}
	RingBufferReadAdvance(AlsaRingBuffer, avail);
	first = 0;
    }

    return 0;
}

/**
**	Flush alsa buffers.
*/
static void AlsaFlushBuffers(void)
{
    int err;
    snd_pcm_state_t state;

    if (AlsaRingBuffer && AlsaPCMHandle) {
	RingBufferReadAdvance(AlsaRingBuffer,
	    RingBufferUsedBytes(AlsaRingBuffer));
	state = snd_pcm_state(AlsaPCMHandle);
	Debug(3, "audio/alsa: state %d - %s\n", state,
	    snd_pcm_state_name(state));
	if (state != SND_PCM_STATE_OPEN) {
	    if ((err = snd_pcm_drop(AlsaPCMHandle)) < 0) {
		Error(_("audio: snd_pcm_drop(): %s\n"), snd_strerror(err));
	    }
	    // ****ing alsa crash, when in open state here
	    if ((err = snd_pcm_prepare(AlsaPCMHandle)) < 0) {
		Error(_("audio: snd_pcm_prepare(): %s\n"), snd_strerror(err));
	    }
	}
    }
    AudioRunning = 0;
    AudioPTS = INT64_C(0x8000000000000000);
}

/**
**	Call back to play audio polled.
*/
static void AlsaPoller(void)
{
    if (!AlsaPCMHandle) {		// setup failure
	return;
    }
    if (!AudioThread && AudioRunning) {
	AlsaPlayRingbuffer();
    }
}

/**
**	Get free bytes in audio output.
*/
static int AlsaFreeBytes(void)
{
    return AlsaRingBuffer ? RingBufferFreeBytes(AlsaRingBuffer) : INT32_MAX;
}

#if 0

//----------------------------------------------------------------------------
//	async playback
//----------------------------------------------------------------------------

//	async playback is broken, don't use it!

/**
**	Alsa async pcm callback function.
**
**	@param handler	alsa async handler
*/
static void AlsaAsyncCallback(snd_async_handler_t * handler)
{

    Debug(3, "audio/%s: %p\n", __FUNCTION__, handler);

    // how many bytes can be written?
    for (;;) {
	n = snd_pcm_avail_update(AlsaPCMHandle);
	if (n < 0) {
	    Error(_("audio/alsa: snd_pcm_avail_update(): %s\n"),
		snd_strerror(n));
	    break;
	}
	avail = snd_pcm_frames_to_bytes(AlsaPCMHandle, n);
	if (avail < 512) {		// too much overhead
	    break;
	}

	n = RingBufferGetReadPointer(AlsaRingBuffer, &p);
	if (!n) {			// ring buffer empty
	    Debug(3, "audio/alsa: ring buffer empty\n");
	    break;
	}
	if (n < avail) {		// not enough bytes in ring buffer
	    avail = n;
	}
	if (!avail) {			// full
	    break;
	}
	frames = snd_pcm_bytes_to_frames(AlsaPCMHandle, avail);

      again:
	if (AlsaUseMmap) {
	    err = snd_pcm_mmap_writei(AlsaPCMHandle, p, frames);
	} else {
	    err = snd_pcm_writei(AlsaPCMHandle, p, frames);
	}
	Debug(3, "audio/alsa: %d => %d\n", frames, err);
	if (err < 0) {
	    Error(_("audio/alsa: underrun error?\n"));
	    err = snd_pcm_recover(AlsaPCMHandle, err, 0);
	    if (err >= 0) {
		goto again;
	    }
	    Error(_("audio/alsa: snd_pcm_writei failed: %s\n"),
		snd_strerror(err));
	}
	if (err != frames) {
	    Error(_("audio/alsa: error not all frames written\n"));
	    avail = snd_pcm_frames_to_bytes(AlsaPCMHandle, err);
	}
	RingBufferReadAdvance(AlsaRingBuffer, avail);
    }
}

/**
**	Place samples in audio output queue.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
static void AlsaEnqueue(const void *samples, int count)
{
    snd_pcm_state_t state;
    int n;

    //int err;

    Debug(3, "audio: %6zd + %4d\n", RingBufferUsedBytes(AlsaRingBuffer),
	count);
    n = RingBufferWrite(AlsaRingBuffer, samples, count);
    if (n != count) {
	Fatal(_("audio: can't place %d samples in ring buffer\n"), count);
    }
    // check if running, wait until enough buffered
    state = snd_pcm_state(AlsaPCMHandle);
    if (state == SND_PCM_STATE_PREPARED) {
	Debug(3, "audio/alsa: state %d - %s\n", state,
	    snd_pcm_state_name(state));
	// FIXME: adjust start ratio
	if (RingBufferFreeBytes(AlsaRingBuffer)
	    < RingBufferUsedBytes(AlsaRingBuffer)) {
	    // restart play-back
#if 0
	    if (AlsaCanPause) {
		if ((err = snd_pcm_pause(AlsaPCMHandle, 0))) {
		    Error(_("audio: snd_pcm_pause(): %s\n"),
			snd_strerror(err));
		}
	    } else {
		if ((err = snd_pcm_prepare(AlsaPCMHandle)) < 0) {
		    Error(_("audio: snd_pcm_prepare(): %s\n"),
			snd_strerror(err));
		}
	    }
	    if ((err = snd_pcm_prepare(AlsaPCMHandle)) < 0) {
		Error(_("audio: snd_pcm_prepare(): %s\n"), snd_strerror(err));
	    }

	    Debug(3, "audio/alsa: unpaused\n");
	    if ((err = snd_pcm_start(AlsaPCMHandle)) < 0) {
		Error(_("audio: snd_pcm_start(): %s\n"), snd_strerror(err));
	    }
#endif
	    state = snd_pcm_state(AlsaPCMHandle);
	    Debug(3, "audio/alsa: state %s\n", snd_pcm_state_name(state));
	    Debug(3, "audio/alsa: unpaused\n");
	}
    }
    // Update audio clock
    // AudioPTS += (size * 90000) / (AudioSampleRate * AudioChannels * AudioBytesProSample);
}

#endif

//----------------------------------------------------------------------------
//	direct playback
//----------------------------------------------------------------------------

// direct play produces underuns on some hardware

#ifndef USE_AUDIO_THREAD

/**
**	Place samples in audio output queue.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
static void AlsaEnqueue(const void *samples, int count)
{
    if (AlsaAddToRingbuffer(samples, count)) {
	AudioRunning = 1;
    }
}

#endif

#ifdef USE_AUDIO_THREAD

//----------------------------------------------------------------------------
//	thread playback
//----------------------------------------------------------------------------

/**
**	Alsa thread
*/
static void AlsaThread(void)
{
    for (;;) {
	int err;

	pthread_testcancel();
	if (AlsaFlushBuffer) {
	    // we can flush too many, but wo cares
	    Debug(3, "audio/alsa: flushing buffers\n");
	    AlsaFlushBuffers();
	    /*
	       if ((err = snd_pcm_prepare(AlsaPCMHandle))) {
	       Error(_("audio: snd_pcm_prepare(): %s\n"), snd_strerror(err));
	       }
	     */
	    AlsaFlushBuffer = 0;
	    break;
	}
	if (AudioPaused) {
	    break;
	}
	// wait for space in kernel buffers
	if ((err = snd_pcm_wait(AlsaPCMHandle, 100)) < 0) {
	    Error(_("audio/alsa: wait underrun error?\n"));
	    err = snd_pcm_recover(AlsaPCMHandle, err, 0);
	    if (err >= 0) {
		continue;
	    }
	    Error(_("audio/alsa: snd_pcm_wait(): %s\n"), snd_strerror(err));
	    usleep(100 * 1000);
	    continue;
	}
	if (AlsaFlushBuffer || AudioPaused) {
	    continue;
	}
	if ((err = AlsaPlayRingbuffer())) {	// empty / error
	    snd_pcm_state_t state;

	    if (err < 0) {		// underrun error
		break;
	    }
	    state = snd_pcm_state(AlsaPCMHandle);
	    if (state != SND_PCM_STATE_RUNNING) {
		Debug(3, "audio/alsa: stopping play\n");
		break;
	    }
	    pthread_yield();
	    usleep(20 * 1000);		// let fill/empty the buffers
	}
    }
}

/**
**	Place samples in audio output queue.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
static void AlsaThreadEnqueue(const void *samples, int count)
{
    if (!AlsaRingBuffer || !AlsaPCMHandle || !AudioSampleRate) {
	Debug(3, "audio/alsa: enqueue not ready\n");
	return;
    }
    if (AlsaAddToRingbuffer(samples, count)) {
	snd_pcm_state_t state;

	state = snd_pcm_state(AlsaPCMHandle);
	Debug(3, "audio/alsa: enqueue state %s\n", snd_pcm_state_name(state));

	// no lock needed, can wakeup next time
	AudioRunning = 1;
	pthread_cond_signal(&AudioStartCond);
    }
}

/**
**	Flush alsa buffers with thread.
*/
static void AlsaThreadFlushBuffers(void)
{
    // signal thread to flush buffers
    if (AudioThread) {
	AlsaFlushBuffer = 1;
	do {
	    AudioRunning = 1;		// wakeup in case of sleeping
	    pthread_cond_signal(&AudioStartCond);
	    usleep(1 * 1000);
	} while (AlsaFlushBuffer);	// wait until flushed
    }
}

#endif

//----------------------------------------------------------------------------

/**
**	Open alsa pcm device.
**
**	@param use_ac3	use ac3/pass-through device
*/
static snd_pcm_t *AlsaOpenPCM(int use_ac3)
{
    const char *device;
    snd_pcm_t *handle;
    int err;

    // &&|| hell
    if (!(use_ac3 && ((device = AudioAC3Device)
		|| (device = getenv("ALSA_AC3_DEVICE"))
		|| (device = getenv("ALSA_PASSTHROUGH_DEVICE"))))
	&& !(device = AudioPCMDevice) && !(device = getenv("ALSA_DEVICE"))) {
	device = "default";
    }
    Debug(3, "audio/alsa: &&|| hell '%s'\n", device);

    // open none blocking; if device is already used, we don't want wait
    if ((err =
	    snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK,
		SND_PCM_NONBLOCK)) < 0) {
	Error(_("audio/alsa: playback open '%s' error: %s\n"), device,
	    snd_strerror(err));
	return NULL;
    }

    if ((err = snd_pcm_nonblock(handle, 0)) < 0) {
	Error(_("audio/alsa: can't set block mode: %s\n"), snd_strerror(err));
    }
    return handle;
}

/**
**	Initialize alsa pcm device.
**
**	@see AudioPCMDevice
*/
static void AlsaInitPCM(void)
{
    snd_pcm_t *handle;
    snd_pcm_hw_params_t *hw_params;
    int err;
    snd_pcm_uframes_t buffer_size;

    if (!(handle = AlsaOpenPCM(0))) {
	return;
    }

    snd_pcm_hw_params_alloca(&hw_params);
    // choose all parameters
    if ((err = snd_pcm_hw_params_any(handle, hw_params)) < 0) {
	Error(_
	    ("audio: snd_pcm_hw_params_any: no configurations available: %s\n"),
	    snd_strerror(err));
    }
    AlsaCanPause = snd_pcm_hw_params_can_pause(hw_params);
    Info(_("audio/alsa: supports pause: %s\n"), AlsaCanPause ? "yes" : "no");
    snd_pcm_hw_params_get_buffer_size_max(hw_params, &buffer_size);
    Info(_("audio/alsa: max buffer size %lu\n"), buffer_size);

    AlsaPCMHandle = handle;
}

//----------------------------------------------------------------------------
//	Alsa Mixer
//----------------------------------------------------------------------------

/**
**	Set alsa mixer volume (0-100)
**
**	@param volume	volume (0 .. 100)
*/
static void AlsaSetVolume(int volume)
{
    int v;

    if (AlsaMixer && AlsaMixerElem) {
	v = (volume * AlsaRatio) / 1000;
	snd_mixer_selem_set_playback_volume(AlsaMixerElem, 0, v);
	snd_mixer_selem_set_playback_volume(AlsaMixerElem, 1, v);
    }
}

/**
**	Initialize alsa mixer.
*/
static void AlsaInitMixer(void)
{
    const char *device;
    const char *channel;
    snd_mixer_t *alsa_mixer;
    snd_mixer_elem_t *alsa_mixer_elem;
    long alsa_mixer_elem_min;
    long alsa_mixer_elem_max;

    if (!(device = AudioMixerDevice)) {
	if (!(device = getenv("ALSA_MIXER"))) {
	    device = "default";
	}
    }
    if (!(channel = AudioMixerChannel)) {
	if (!(channel = getenv("ALSA_MIXER_CHANNEL"))) {
	    channel = "PCM";
	}
    }
    Debug(3, "audio/alsa: mixer %s - %s open\n", device, channel);
    snd_mixer_open(&alsa_mixer, 0);
    if (alsa_mixer && snd_mixer_attach(alsa_mixer, device) >= 0
	&& snd_mixer_selem_register(alsa_mixer, NULL, NULL) >= 0
	&& snd_mixer_load(alsa_mixer) >= 0) {

	const char *const alsa_mixer_elem_name = channel;

	alsa_mixer_elem = snd_mixer_first_elem(alsa_mixer);
	while (alsa_mixer_elem) {
	    const char *name;

	    name = snd_mixer_selem_get_name(alsa_mixer_elem);
	    if (!strcasecmp(name, alsa_mixer_elem_name)) {
		snd_mixer_selem_get_playback_volume_range(alsa_mixer_elem,
		    &alsa_mixer_elem_min, &alsa_mixer_elem_max);
		AlsaRatio =
		    (1000 * (alsa_mixer_elem_max - alsa_mixer_elem_min)) / 100;
		Debug(3, "audio/alsa: PCM mixer found %ld - %ld ratio %d\n",
		    alsa_mixer_elem_min, alsa_mixer_elem_max, AlsaRatio);
		break;
	    }

	    alsa_mixer_elem = snd_mixer_elem_next(alsa_mixer_elem);
	}

	AlsaMixer = alsa_mixer;
	AlsaMixerElem = alsa_mixer_elem;
    } else {
	Error(_("audio/alsa: can't open mixer '%s'\n"), device);
    }
}

//----------------------------------------------------------------------------
//	Alsa API
//----------------------------------------------------------------------------

/**
**	Get alsa audio delay in time stamps.
**
**	@returns audio delay in time stamps.
**
**	@todo FIXME: handle the case no audio running
*/
static uint64_t AlsaGetDelay(void)
{
    int err;
    snd_pcm_sframes_t delay;
    uint64_t pts;

    if (!AlsaPCMHandle || !AudioSampleRate) {
	return 0UL;
    }
    // FIXME: thread safe? __assert_fail_base in snd_pcm_delay

    // delay in frames in alsa + kernel buffers
    if ((err = snd_pcm_delay(AlsaPCMHandle, &delay)) < 0) {
	//Debug(3, "audio/alsa: no hw delay\n");
	delay = 0L;
    } else if (snd_pcm_state(AlsaPCMHandle) != SND_PCM_STATE_RUNNING) {
	//Debug(3, "audio/alsa: %ld frames delay ok, but not running\n", delay);
    }
    //Debug(3, "audio/alsa: %ld frames hw delay\n", delay);

    // delay can be negative when underrun occur
    if (delay < 0) {
	delay = 0L;
    }

    pts = ((uint64_t) delay * 90 * 1000) / AudioSampleRate;
    pts += ((uint64_t) RingBufferUsedBytes(AlsaRingBuffer) * 90 * 1000)
	/ (AudioSampleRate * AudioChannels * AudioBytesProSample);
    Debug(4, "audio/alsa: hw+sw delay %zd %" PRId64 " ms\n",
	RingBufferUsedBytes(AlsaRingBuffer), pts / 90);

    return pts;
}

/**
**	Setup alsa audio for requested format.
**
**	@param freq	sample frequency
**	@param channels	number of channels
**	@param use_ac3	use ac3/pass-through device
**
**	@retval 0	everything ok
**	@retval 1	didn't support frequency/channels combination
**	@retval -1	something gone wrong
**
**	@todo audio changes must be queued and done when the buffer is empty
*/
static int AlsaSetup(int *freq, int *channels, int use_ac3)
{
    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size;
    int err;
    int ret;
    int delay;
    snd_pcm_t *handle;

    if (!AlsaPCMHandle) {		// alsa not running yet
	return -1;
    }
#if 1					// easy alsa hw setup way
    // flush any buffered data
    AudioFlushBuffers();

    if (1) {				// close+open to fix hdmi no sound bugs
	handle = AlsaPCMHandle;
	AlsaPCMHandle = NULL;
	snd_pcm_close(handle);
	if (!(handle = AlsaOpenPCM(use_ac3))) {
	    return -1;
	}
	AlsaPCMHandle = handle;
    }

    ret = 0;
  try_again:
    AudioChannels = *channels;
    AudioSampleRate = *freq;

    if ((err =
	    snd_pcm_set_params(AlsaPCMHandle, SND_PCM_FORMAT_S16,
		AlsaUseMmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED :
		SND_PCM_ACCESS_RW_INTERLEAVED, *channels, *freq, 1,
		125 * 1000))) {
	Error(_("audio/alsa: set params error: %s\n"), snd_strerror(err));

	/*
	   if ( err == -EBADFD ) {
	   snd_pcm_close(AlsaPCMHandle);
	   AlsaPCMHandle = NULL;
	   goto try_again;
	   }
	 */

	switch (*channels) {
	    case 1:
		// FIXME: enable channel upmix
		ret = 1;
		*channels = 2;
		goto try_again;
	    case 2:
		return -1;
	    case 3:
	    case 4:
	    case 5:
	    case 6:
	    case 7:
	    case 8:
		// FIXME: enable channel downmix
		// FIXME: try 8 -> 7 -> 6 -> 5 -> 4 -> 3 -> 2
		ret = 1;
		*channels = 2;
		goto try_again;
	    default:
		Error(_("audio/alsa: unsupported number of channels\n"));
		// FIXME: must stop sound, AudioChannels ... invalid
		return -1;
	}
    }
#else
    //
    //	complex way to setup parameters
    //
    snd_pcm_hw_params_t *hw_params;
    int dir;
    unsigned buffer_time;
    snd_pcm_uframes_t buffer_size;

    snd_pcm_hw_params_alloca(&hw_params);
    // choose all parameters
    if ((err = snd_pcm_hw_params_any(AlsaPCMHandle, hw_params)) < 0) {
	Error(_
	    ("audio: snd_pcm_hw_params_any: no configurations available: %s\n"),
	    snd_strerror(err));
    }

    if ((err =
	    snd_pcm_hw_params_set_rate_resample(AlsaPCMHandle, hw_params, 1))
	< 0) {
	Error(_("audio: can't set rate resample: %s\n"), snd_strerror(err));
    }
    if ((err =
	    snd_pcm_hw_params_set_format(AlsaPCMHandle, hw_params,
		SND_PCM_FORMAT_S16)) < 0) {
	Error(_("audio: can't set 16-bit: %s\n"), snd_strerror(err));
    }
    if ((err =
	    snd_pcm_hw_params_set_access(AlsaPCMHandle, hw_params,
		SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
	Error(_("audio: can't set interleaved read/write %s\n"),
	    snd_strerror(err));
    }
    if ((err =
	    snd_pcm_hw_params_set_channels(AlsaPCMHandle, hw_params,
		channels)) < 0) {
	Error(_("audio: can't set channels: %s\n"), snd_strerror(err));
    }
    if ((err =
	    snd_pcm_hw_params_set_rate(AlsaPCMHandle, hw_params, freq,
		0)) < 0) {
	Error(_("audio: can't set rate: %s\n"), snd_strerror(err));
    }
    // 500000
    // 170667us
    buffer_time = 1000 * 1000 * 1000;
    dir = 1;
#if 0
    snd_pcm_hw_params_get_buffer_time_max(hw_params, &buffer_time, &dir);
    Info(_("audio/alsa: %dus max buffer time\n"), buffer_time);

    buffer_time = 5 * 200 * 1000;	// 1s
    if ((err =
	    snd_pcm_hw_params_set_buffer_time_near(AlsaPCMHandle, hw_params,
		&buffer_time, &dir)) < 0) {
	Error(_("audio: snd_pcm_hw_params_set_buffer_time_near failed: %s\n"),
	    snd_strerror(err));
    }
    Info(_("audio/alsa: %dus buffer time\n"), buffer_time);
#endif
    snd_pcm_hw_params_get_buffer_size_max(hw_params, &buffer_size);
    Info(_("audio/alsa: buffer size %lu\n"), buffer_size);
    buffer_size = buffer_size < 65536 ? buffer_size : 65536;
    if ((err =
	    snd_pcm_hw_params_set_buffer_size_near(AlsaPCMHandle, hw_params,
		&buffer_size))) {
	Error(_("audio: can't set buffer size: %s\n"), snd_strerror(err));
    }
    Info(_("audio/alsa: buffer size %lu\n"), buffer_size);

    if ((err = snd_pcm_hw_params(AlsaPCMHandle, hw_params)) < 0) {
	Error(_("audio: snd_pcm_hw_params failed: %s\n"), snd_strerror(err));
    }
    // FIXME: use hw_params for buffer_size period_size
#endif

#if 1
    if (0) {				// no underruns allowed, play silence
	snd_pcm_sw_params_t *sw_params;
	snd_pcm_uframes_t boundary;

	snd_pcm_sw_params_alloca(&sw_params);
	err = snd_pcm_sw_params_current(AlsaPCMHandle, sw_params);
	if (err < 0) {
	    Error(_("audio: snd_pcm_sw_params_current failed: %s\n"),
		snd_strerror(err));
	}
	if ((err = snd_pcm_sw_params_get_boundary(sw_params, &boundary)) < 0) {
	    Error(_("audio: snd_pcm_sw_params_get_boundary failed: %s\n"),
		snd_strerror(err));
	}
	Debug(4, "audio/alsa: boundary %lu frames\n", boundary);
	if ((err =
		snd_pcm_sw_params_set_stop_threshold(AlsaPCMHandle, sw_params,
		    boundary)) < 0) {
	    Error(_("audio: snd_pcm_sw_params_set_silence_size failed: %s\n"),
		snd_strerror(err));
	}
	if ((err =
		snd_pcm_sw_params_set_silence_size(AlsaPCMHandle, sw_params,
		    boundary)) < 0) {
	    Error(_("audio: snd_pcm_sw_params_set_silence_size failed: %s\n"),
		snd_strerror(err));
	}
	if ((err = snd_pcm_sw_params(AlsaPCMHandle, sw_params)) < 0) {
	    Error(_("audio: snd_pcm_sw_params failed: %s\n"),
		snd_strerror(err));
	}
    }
#endif

    // update buffer

    snd_pcm_get_params(AlsaPCMHandle, &buffer_size, &period_size);
    Info(_("audio/alsa: buffer size %lu, period size %lu\n"), buffer_size,
	period_size);
    Debug(3, "audio/alsa: state %s\n",
	snd_pcm_state_name(snd_pcm_state(AlsaPCMHandle)));

    AlsaStartThreshold = snd_pcm_frames_to_bytes(AlsaPCMHandle, period_size);
    // buffer time/delay in ms
    delay = AudioBufferTime;
    if (VideoAudioDelay > -100) {
	delay += 100 + VideoAudioDelay / 90;
    }
    if (AlsaStartThreshold <
	(*freq * *channels * AudioBytesProSample * delay) / 1000U) {
	AlsaStartThreshold =
	    (*freq * *channels * AudioBytesProSample * delay) / 1000U;
    }
    // no bigger, than the buffer
    if (AlsaStartThreshold > RingBufferFreeBytes(AlsaRingBuffer)) {
	AlsaStartThreshold = RingBufferFreeBytes(AlsaRingBuffer);
    }
    Info(_("audio/alsa: delay %u ms\n"), (AlsaStartThreshold * 1000)
	/ (AudioSampleRate * AudioChannels * AudioBytesProSample));

    return ret;
}

/**
**	Play audio.
*/
void AlsaPlay(void)
{
    int err;

    if (AlsaCanPause) {
	if ((err = snd_pcm_pause(AlsaPCMHandle, 0))) {
	    Error(_("audio/alsa: snd_pcm_pause(): %s\n"), snd_strerror(err));
	}
    } else {
	if ((err = snd_pcm_prepare(AlsaPCMHandle)) < 0) {
	    Error(_("audio/alsa: snd_pcm_prepare(): %s\n"), snd_strerror(err));
	}
    }
#ifdef DEBUG
    if (snd_pcm_state(AlsaPCMHandle) == SND_PCM_STATE_PAUSED) {
	Error(_("audio/alsa: still paused\n"));
    }
#endif
}

/**
**	Pause audio.
*/
void AlsaPause(void)
{
    int err;

    if (AlsaCanPause) {
	if ((err = snd_pcm_pause(AlsaPCMHandle, 1))) {
	    Error(_("snd_pcm_pause(): %s\n"), snd_strerror(err));
	}
    } else {
	if ((err = snd_pcm_drop(AlsaPCMHandle)) < 0) {
	    Error(_("snd_pcm_drop(): %s\n"), snd_strerror(err));
	}
    }
}

/**
**	Empty log callback
*/
static void AlsaNoopCallback( __attribute__ ((unused))
    const char *file, __attribute__ ((unused))
    int line, __attribute__ ((unused))
    const char *function, __attribute__ ((unused))
    int err, __attribute__ ((unused))
    const char *fmt, ...)
{
}

/**
**	Initialize alsa audio output module.
*/
static void AlsaInit(void)
{
#ifndef DEBUG
    // disable display alsa error messages
    snd_lib_error_set_handler(AlsaNoopCallback);
#else
    (void)AlsaNoopCallback;
#endif
    AlsaRingBuffer = RingBufferNew(48000 * 8 * 2);	// ~1s 8ch 16bit

    AlsaInitPCM();
    AlsaInitMixer();
}

/**
**	Cleanup alsa audio output module.
*/
static void AlsaExit(void)
{
    if (AlsaPCMHandle) {
	snd_pcm_close(AlsaPCMHandle);
	AlsaPCMHandle = NULL;
    }
    if (AlsaMixer) {
	snd_mixer_close(AlsaMixer);
	AlsaMixer = NULL;
	AlsaMixerElem = NULL;
    }
    if (AlsaRingBuffer) {
	RingBufferDel(AlsaRingBuffer);
	AlsaRingBuffer = NULL;
    }
    AlsaFlushBuffer = 0;
}

/**
**	Alsa module.
*/
static const AudioModule AlsaModule = {
    .Name = "alsa",
#ifdef USE_AUDIO_THREAD
    .Thread = AlsaThread,
    .Enqueue = AlsaThreadEnqueue,
    .FlushBuffers = AlsaThreadFlushBuffers,
#else
    .Enqueue = AlsaEnqueue,
    .FlushBuffers = AlsaFlushBuffers,
#endif
    .Poller = AlsaPoller,
    .FreeBytes = AlsaFreeBytes,
    .GetDelay = AlsaGetDelay,
    .SetVolume = AlsaSetVolume,
    .Setup = AlsaSetup,
    .Play = AlsaPlay,
    .Pause = AlsaPause,
    .Init = AlsaInit,
    .Exit = AlsaExit,
};

#endif // USE_ALSA

#ifdef USE_OSS

//============================================================================
//	O S S
//============================================================================

//----------------------------------------------------------------------------
//	OSS variables
//----------------------------------------------------------------------------

static int OssPcmFildes = -1;		///< pcm file descriptor
static int OssMixerFildes = -1;		///< mixer file descriptor
static int OssMixerChannel;		///< mixer channel index
static RingBuffer *OssRingBuffer;	///< audio ring buffer
static unsigned OssStartThreshold;	///< start play, if filled

#ifdef USE_AUDIO_THREAD
static volatile char OssFlushBuffer;	///< flag empty buffer
#endif

//----------------------------------------------------------------------------
//	OSS pcm
//----------------------------------------------------------------------------

/**
**	Place samples in ringbuffer.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
**
**	@returns true if play should be started.
*/
static int OssAddToRingbuffer(const void *samples, int count)
{
    int n;

    n = RingBufferWrite(OssRingBuffer, samples, count);
    if (n != count) {
	Error(_("audio/oss: can't place %d samples in ring buffer\n"), count);
	// too many bytes are lost
	// FIXME: should skip more, longer skip, but less often?
    }
    // Update audio clock (stupid gcc developers thinks INT64_C is unsigned)
    if (AudioPTS != (int64_t) INT64_C(0x8000000000000000)) {
	AudioPTS +=
	    ((int64_t) count * 90000) / (AudioSampleRate * AudioChannels *
	    AudioBytesProSample);
    }

    if (!AudioRunning) {
	if (OssStartThreshold < RingBufferUsedBytes(OssRingBuffer)) {
	    // restart play-back
	    return 1;
	}
    }

    return 0;
}

/**
**	Play samples from ringbuffer.
*/
static int OssPlayRingbuffer(void)
{
    int first;
    const void *p;

    first = 1;
    for (;;) {
	audio_buf_info bi;
	int n;

	if (ioctl(OssPcmFildes, SNDCTL_DSP_GETOSPACE, &bi) == -1) {
	    Error(_("audio/oss: ioctl(SNDCTL_DSP_GETOSPACE): %s\n"),
		strerror(errno));
	    return -1;
	}
	Debug(4, "audio/oss: %d bytes free\n", bi.bytes);

	n = RingBufferGetReadPointer(OssRingBuffer, &p);
	if (!n) {			// ring buffer empty
	    if (first) {		// only error on first loop
		return 1;
	    }
	    return 0;
	}
	if (n < bi.bytes) {		// not enough bytes in ring buffer
	    bi.bytes = n;
	}
	if (bi.bytes <= 0) {		// full or buffer empty
	    break;			// bi.bytes could become negative!
	}

	n = write(OssPcmFildes, p, bi.bytes);
	if (n != bi.bytes) {
	    if (n < 0) {
		Error(_("audio/oss: write error: %s\n"), strerror(errno));
		return 1;
	    }
	    Error(_("audio/oss: error not all bytes written\n"));
	}
	// advance how many could written
	RingBufferReadAdvance(OssRingBuffer, n);
	first = 0;
    }

    return 0;
}

/**
**	Flush OSS buffers.
*/
static void OssFlushBuffers(void)
{
    if (OssRingBuffer && OssPcmFildes != -1) {
	RingBufferReadAdvance(OssRingBuffer,
	    RingBufferUsedBytes(OssRingBuffer));
	// flush kernel buffers
	if (ioctl(OssPcmFildes, SNDCTL_DSP_HALT_OUTPUT, NULL) < 0) {
	    Error(_("audio/oss: ioctl(SNDCTL_DSP_HALT_OUTPUT): %s\n"),
		strerror(errno));
	}
    }
    AudioRunning = 0;
    AudioPTS = INT64_C(0x8000000000000000);
}

//----------------------------------------------------------------------------
//	OSS pcm polled
//----------------------------------------------------------------------------

#ifndef USE_AUDIO_THREAD

/**
**	Place samples in audio output queue.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
static void OssEnqueue(const void *samples, int count)
{
#ifdef DEBUG
    static uint32_t last_tick;
    uint32_t tick;

    tick = GetMsTicks();
    Debug(4, "audio/oss: %4d %d ms\n", count, tick - last_tick);
    last_tick = tick;
#endif

    if (OssPcmFildes == -1) {		// setup failure
	Debug(3, "audio/oss: not ready\n");
	return;
    }
    if (OssAddToRingbuffer(samples, count)) {
	AudioRunning = 1;
    }
}

#endif

/**
**	Play all samples possible, without blocking.
*/
static void OssPoller(void)
{
    if (OssPcmFildes == -1) {		// setup failure
	return;
    }
    if (!AudioThread && AudioRunning) {
	OssPlayRingbuffer();
    }
}

/**
**	Get free bytes in audio output.
*/
static int OssFreeBytes(void)
{
    return OssRingBuffer ? RingBufferFreeBytes(OssRingBuffer) : INT32_MAX;
}

#ifdef USE_AUDIO_THREAD

//----------------------------------------------------------------------------
//	thread playback
//----------------------------------------------------------------------------

/**
**	OSS thread
*/
static void OssThread(void)
{
    for (;;) {
	struct pollfd fds[1];
	int err;

	pthread_testcancel();
	if (OssFlushBuffer) {
	    // we can flush too many, but wo cares
	    Debug(3, "audio/oss: flushing buffers\n");
	    OssFlushBuffers();
	    OssFlushBuffer = 0;
	    break;
	}
	if (AudioPaused) {
	    break;
	}

	fds[0].fd = OssPcmFildes;
	fds[0].events = POLLOUT | POLLERR;
	// wait for space in kernel buffers
	err = poll(fds, 1, 100);
	if (err < 0) {
	    Error(_("audio/oss: error poll %s\n"), strerror(errno));
	    usleep(100 * 1000);
	    continue;
	}

	if (OssFlushBuffer || AudioPaused) {
	    continue;
	}

	if ((err = OssPlayRingbuffer())) {	// empty / error
	    if (err < 0) {		// underrun error
		break;
	    }
	    pthread_yield();
	    usleep(20 * 1000);		// let fill/empty the buffers
	}
    }
}

/**
**	Place samples in audio output queue.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
static void OssThreadEnqueue(const void *samples, int count)
{
    if (!OssRingBuffer || OssPcmFildes == -1 || !AudioSampleRate) {
	Debug(3, "audio/oss: enqueue not ready\n");
	return;
    }
    if (OssAddToRingbuffer(samples, count)) {
	// no lock needed, can wakeup next time
	AudioRunning = 1;
	pthread_cond_signal(&AudioStartCond);
    }
}

/**
**	Flush OSS buffers with thread.
*/
static void OssThreadFlushBuffers(void)
{
    // signal thread to flush buffers
    if (AudioThread) {
	OssFlushBuffer = 1;
	do {
	    AudioRunning = 1;		// wakeup in case of sleeping
	    pthread_cond_signal(&AudioStartCond);
	    usleep(1 * 1000);
	} while (OssFlushBuffer);	// wait until flushed
    }
}

#endif

//----------------------------------------------------------------------------

/**
**	Open OSS pcm device.
**
**	@param use_ac3	use ac3/pass-through device
*/
static int OssOpenPCM(int use_ac3)
{
    const char *device;
    int fildes;

    // &&|| hell
    if (!(use_ac3 && ((device = AudioAC3Device)
		|| (device = getenv("OSS_AC3_AUDIODEV"))))
	&& !(device = AudioPCMDevice) && !(device = getenv("OSS_AUDIODEV"))) {
	device = "/dev/dsp";
    }
    Debug(3, "audio/oss: &&|| hell '%s'\n", device);

    if ((fildes = open(device, O_WRONLY)) < 0) {
	Error(_("audio/oss: can't open dsp device '%s': %s\n"), device,
	    strerror(errno));
	return -1;
    }
    return fildes;
}

/**
**	Initialize OSS pcm device.
**
**	@see AudioPCMDevice
*/
static void OssInitPCM(void)
{
    int fildes;

    fildes = OssOpenPCM(0);

    OssPcmFildes = fildes;
}

//----------------------------------------------------------------------------
//	OSS Mixer
//----------------------------------------------------------------------------

/**
**	Set OSS mixer volume (0-100)
**
**	@param volume	volume (0 .. 100)
*/
static void OssSetVolume(int volume)
{
    int v;

    if (OssMixerFildes != -1) {
	v = (volume * 255) / 100;
	v &= 0xff;
	v = (v << 8) | v;
	if (ioctl(OssMixerFildes, MIXER_WRITE(OssMixerChannel), &v) < 0) {
	    Error(_("audio/oss: ioctl(MIXER_WRITE): %s\n"), strerror(errno));
	}
    }
}

/**
**	Mixer channel name table.
*/
static const char *OssMixerChannelNames[SOUND_MIXER_NRDEVICES] =
    SOUND_DEVICE_NAMES;

/**
**	Initialize OSS mixer.
*/
static void OssInitMixer(void)
{
    const char *device;
    const char *channel;
    int fildes;
    int devmask;
    int i;

    if (!(device = AudioMixerDevice)) {
	if (!(device = getenv("OSS_MIXERDEV"))) {
	    device = "/dev/mixer";
	}
    }
    if (!(channel = AudioMixerChannel)) {
	if (!(channel = getenv("OSS_MIXER_CHANNEL"))) {
	    channel = "pcm";
	}
    }
    Debug(3, "audio/oss: mixer %s - %s open\n", device, channel);

    if ((fildes = open(device, O_RDWR)) < 0) {
	Error(_("audio/oss: can't open mixer device '%s': %s\n"), device,
	    strerror(errno));
	return;
    }
    // search channel name
    if (ioctl(fildes, SOUND_MIXER_READ_DEVMASK, &devmask) < 0) {
	Error(_("audio/oss: ioctl(SOUND_MIXER_READ_DEVMASK): %s\n"),
	    strerror(errno));
	close(fildes);
	return;
    }
    for (i = 0; i < SOUND_MIXER_NRDEVICES; ++i) {
	if (!strcasecmp(OssMixerChannelNames[i], channel)) {
	    if (devmask & (1 << i)) {
		OssMixerFildes = fildes;
		OssMixerChannel = i;
		return;
	    }
	    Error(_("audio/oss: channel '%s' not supported\n"), channel);
	    break;
	}
    }
    Error(_("audio/oss: channel '%s' not found\n"), channel);
    close(fildes);
}

//----------------------------------------------------------------------------
//	OSS API
//----------------------------------------------------------------------------

/**
**	Get OSS audio delay in time stamps.
**
**	@returns audio delay in time stamps.
*/
static uint64_t OssGetDelay(void)
{
    int delay;
    uint64_t pts;

    if (OssPcmFildes == -1) {		// setup failure
	return 0UL;
    }

    if (!AudioRunning) {
	return 0UL;
    }
    // delay in bytes in kernel buffers
    delay = -1;
    if (ioctl(OssPcmFildes, SNDCTL_DSP_GETODELAY, &delay) == -1) {
	Error(_("audio/oss: ioctl(SNDCTL_DSP_GETODELAY): %s\n"),
	    strerror(errno));
	return 0UL;
    }
    if (delay == -1) {
	delay = 0UL;
    }

    pts = ((uint64_t) delay * 90 * 1000)
	/ (AudioSampleRate * AudioChannels * AudioBytesProSample);
    pts += ((uint64_t) RingBufferUsedBytes(OssRingBuffer) * 90 * 1000)
	/ (AudioSampleRate * AudioChannels * AudioBytesProSample);
    if (pts > 600 * 90) {
	Debug(4, "audio/oss: hw+sw delay %zd %" PRId64 " ms\n",
	    RingBufferUsedBytes(OssRingBuffer), pts / 90);
    }

    return pts;
}

/**
**	Setup OSS audio for requested format.
**
**	@param freq	sample frequency
**	@param channels	number of channels
**	@param use_ac3	use ac3/pass-through device
**
**	@retval 0	everything ok
**	@retval 1	didn't support frequency/channels combination
**	@retval -1	something gone wrong
**
**	@todo audio changes must be queued and done when the buffer is empty
*/
static int OssSetup(int *freq, int *channels, int use_ac3)
{
    int ret;
    int tmp;
    int delay;

    if (OssPcmFildes == -1) {		// OSS not ready
	return -1;
    }
    // flush any buffered data
    AudioFlushBuffers();

    if (1) {				// close+open for pcm / ac3
	int fildes;

	fildes = OssPcmFildes;
	OssPcmFildes = -1;
	close(fildes);
	if (!(fildes = OssOpenPCM(use_ac3))) {
	    return -1;
	}
	OssPcmFildes = fildes;
    }

    ret = 0;

    tmp = AFMT_S16_NE;			// native 16 bits
    if (ioctl(OssPcmFildes, SNDCTL_DSP_SETFMT, &tmp) == -1) {
	Error(_("audio/oss: ioctl(SNDCTL_DSP_SETFMT): %s\n"), strerror(errno));
	// FIXME: stop player, set setup failed flag
	return -1;
    }
    if (tmp != AFMT_S16_NE) {
	Error(_("audio/oss: device doesn't support 16 bit sample format.\n"));
	// FIXME: stop player, set setup failed flag
	return -1;
    }

    tmp = *channels;
    if (ioctl(OssPcmFildes, SNDCTL_DSP_CHANNELS, &tmp) == -1) {
	Error(_("audio/oss: ioctl(SNDCTL_DSP_CHANNELS): %s\n"),
	    strerror(errno));
	return -1;
    }
    if (tmp != *channels) {
	Warning(_("audio/oss: device doesn't support %d channels.\n"),
	    *channels);
	*channels = tmp;
	ret = 1;
    }

    tmp = *freq;
    if (ioctl(OssPcmFildes, SNDCTL_DSP_SPEED, &tmp) == -1) {
	Error(_("audio/oss: ioctl(SNDCTL_DSP_SPEED): %s\n"), strerror(errno));
	return -1;
    }
    if (tmp != *freq) {
	Warning(_("audio/oss: device doesn't support %d Hz sample rate.\n"),
	    *freq);
	*freq = tmp;
	ret = 1;
    }

    AudioChannels = *channels;
    AudioSampleRate = *freq;

    // FIXME: setup buffers

    if (1) {
	audio_buf_info bi;

	if (ioctl(OssPcmFildes, SNDCTL_DSP_GETOSPACE, &bi) == -1) {
	    Error(_("audio/oss: ioctl(SNDCTL_DSP_GETOSPACE): %s\n"),
		strerror(errno));
	} else {
	    Debug(3, "audio/oss: %d bytes buffered\n", bi.bytes);
	}

	tmp = -1;
	if (ioctl(OssPcmFildes, SNDCTL_DSP_GETODELAY, &tmp) == -1) {
	    Error(_("audio/oss: ioctl(SNDCTL_DSP_GETODELAY): %s\n"),
		strerror(errno));
	    // FIXME: stop player, set setup failed flag
	    return -1;
	}
	if (tmp == -1) {
	    tmp = 0;
	}
	// start when enough bytes for initial write
	OssStartThreshold = bi.bytes + tmp;
	// buffer time/delay in ms
	delay = AudioBufferTime;
	if (VideoAudioDelay > -100) {
	    delay += 100 + VideoAudioDelay / 90;
	}
	if (OssStartThreshold <
	    (*freq * *channels * AudioBytesProSample * delay) / 1000U) {
	    OssStartThreshold =
		(*freq * *channels * AudioBytesProSample * delay) / 1000U;
	}
	// no bigger, than the buffer
	if (OssStartThreshold > RingBufferFreeBytes(OssRingBuffer)) {
	    OssStartThreshold = RingBufferFreeBytes(OssRingBuffer);
	}

	Info(_("audio/oss: delay %u ms\n"), (OssStartThreshold * 1000)
	    / (AudioSampleRate * AudioChannels * AudioBytesProSample));
    }

    return ret;
}

/**
**	Play audio.
*/
void OssPlay(void)
{
}

/**
**	Pause audio.
*/
void OssPause(void)
{
}

/**
**	Initialize OSS audio output module.
*/
static void OssInit(void)
{
    OssRingBuffer = RingBufferNew(48000 * 8 * 2);	// ~1s 8ch 16bit

    OssInitPCM();
    OssInitMixer();
}

/**
**	Cleanup OSS audio output module.
*/
static void OssExit(void)
{
    if (OssPcmFildes != -1) {
	close(OssPcmFildes);
	OssPcmFildes = -1;
    }
    if (OssMixerFildes != -1) {
	close(OssMixerFildes);
	OssMixerFildes = -1;
    }
    OssFlushBuffer = 0;
}

/**
**	OSS module.
*/
static const AudioModule OssModule = {
    .Name = "oss",
#ifdef USE_AUDIO_THREAD
    .Thread = OssThread,
    .Enqueue = OssThreadEnqueue,
    .FlushBuffers = OssThreadFlushBuffers,
#else
    .Enqueue = OssEnqueue,
    .FlushBuffers = OssFlushBuffers,
#endif
    .Poller = OssPoller,
    .FreeBytes = OssFreeBytes,
    .GetDelay = OssGetDelay,
    .SetVolume = OssSetVolume,
    .Setup = OssSetup,
    .Play = OssPlay,
    .Pause = OssPause,
    .Init = OssInit,
    .Exit = OssExit,
};

#endif // USE_OSS

//============================================================================
//	Noop
//============================================================================

/**
**	Noop enqueue samples.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
static void NoopEnqueue( __attribute__ ((unused))
    const void *samples, __attribute__ ((unused))
    int count)
{
}

/**
**	Get free bytes in audio output.
*/
static int NoopFreeBytes(void)
{
    return INT32_MAX;			// no driver, much space
}

/**
**	Get audio delay in time stamps.
**
**	@returns audio delay in time stamps.
*/
static uint64_t NoopGetDelay(void)
{
    return 0UL;
}

/**
**	Set mixer volume (0-100)
**
**	@param volume	volume (0 .. 100)
*/
static void NoopSetVolume( __attribute__ ((unused))
    int volume)
{
}

/**
**	Noop setup.
**
**	@param freq	sample frequency
**	@param channels	number of channels
*/
static int NoopSetup( __attribute__ ((unused))
    int *channels, __attribute__ ((unused))
    int *freq, __attribute__ ((unused))
    int use_ac3)
{
    return -1;
}

/**
**	Noop void
*/
static void NoopVoid(void)
{
}

/**
**	Noop module.
*/
static const AudioModule NoopModule = {
    .Name = "noop",
    .Enqueue = NoopEnqueue,
    .FlushBuffers = NoopVoid,
    .Poller = NoopVoid,
    .FreeBytes = NoopFreeBytes,
    .GetDelay = NoopGetDelay,
    .SetVolume = NoopSetVolume,
    .Setup = NoopSetup,
    .Play = NoopVoid,
    .Pause = NoopVoid,
    .Init = NoopVoid,
    .Exit = NoopVoid,
};

//----------------------------------------------------------------------------
//	thread playback
//----------------------------------------------------------------------------

#ifdef USE_AUDIO_THREAD

/**
**	Audio play thread.
*/
static void *AudioPlayHandlerThread(void *dummy)
{
    Debug(3, "audio: play thread started\n");
    for (;;) {
	Debug(3, "audio: wait on start condition\n");
	pthread_mutex_lock(&AudioMutex);
	AudioRunning = 0;
	do {
	    pthread_cond_wait(&AudioStartCond, &AudioMutex);
	    // cond_wait can return, without signal!
	} while (!AudioRunning);
	pthread_mutex_unlock(&AudioMutex);

#ifdef USE_AUDIORING
	if (atomic_read(&AudioRingFilled) > 1) {
	    int sample_rate;
	    int channels;

	    // skip all sample changes between
	    while (atomic_read(&AudioRingFilled) > 1) {
		Debug(3, "audio: skip ring buffer\n");
		AudioRingRead = (AudioRingRead + 1) % AUDIO_RING_MAX;
		atomic_dec(&AudioRingFilled);
	    }

#ifdef USE_ALSA
	    // FIXME: flush only if there is something to flush
	    AlsaFlushBuffers();

	    sample_rate = AudioRing[AudioRingRead].SampleRate;
	    channels = AudioRing[AudioRingRead].Channels;
	    Debug(3, "audio: thread channels %d sample-rate %d hz\n", channels,
		sample_rate);

	    if (AlsaSetup(&sample_rate, &channels)) {
		Error(_("audio: can't set channels %d sample-rate %d hz\n"),
		    channels, sample_rate);
	    }
	    Debug(3, "audio: thread channels %d sample-rate %d hz\n",
		AudioChannels, AudioSampleRate);
#endif
	}
#endif

	Debug(3, "audio: play start\n");
	AudioUsedModule->Thread();
    }

    return dummy;
}

/**
**	Initialize audio thread.
*/
static void AudioInitThread(void)
{
    pthread_mutex_init(&AudioMutex, NULL);
    pthread_cond_init(&AudioStartCond, NULL);
    pthread_create(&AudioThread, NULL, AudioPlayHandlerThread, NULL);
    pthread_setname_np(AudioThread, "softhddev audio");

    pthread_yield();
    usleep(5 * 1000);			// give thread some time to start
}

/**
**	Cleanup audio thread.
*/
static void AudioExitThread(void)
{
    void *retval;

    if (AudioThread) {
	if (pthread_cancel(AudioThread)) {
	    Error(_("audio: can't queue cancel play thread\n"));
	}
	if (pthread_join(AudioThread, &retval) || retval != PTHREAD_CANCELED) {
	    Error(_("audio: can't cancel play thread\n"));
	}
	pthread_cond_destroy(&AudioStartCond);
	pthread_mutex_destroy(&AudioMutex);
	AudioThread = 0;
    }
}

#endif

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

    /**
    **	Table of all audio modules.
    */
static const AudioModule *AudioModules[] = {
#ifdef USE_ALSA
    &AlsaModule,
#endif
#ifdef USE_OSS
    &OssModule,
#endif
    &NoopModule,
};

/**
**	Place samples in audio output queue.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
void AudioEnqueue(const void *samples, int count)
{
    AudioUsedModule->Enqueue(samples, count);
}

/**
**	Flush audio buffers.
*/
void AudioFlushBuffers(void)
{
    AudioUsedModule->FlushBuffers();
}

/**
**	Call back to play audio polled.
*/
void AudioPoller(void)
{
    AudioUsedModule->Poller();
}

/**
**	Get free bytes in audio output.
*/
int AudioFreeBytes(void)
{
    return AudioUsedModule->FreeBytes();
}

/**
**	Get audio delay in time stamps.
**
**	@returns audio delay in time stamps.
*/
uint64_t AudioGetDelay(void)
{
    return AudioUsedModule->GetDelay();
}

/**
**	Set audio clock base.
**
**	@param pts	audio presentation timestamp
*/
void AudioSetClock(int64_t pts)
{
#ifdef DEBUG
    if (AudioPTS != pts) {
	Debug(4, "audio: set clock to %#012" PRIx64 " %#012" PRIx64 " pts\n",
	    AudioPTS, pts);

    }
#endif
    AudioPTS = pts;
}

/**
**	Get current audio clock.
**
**	@returns the audio clock in time stamps.
*/
int64_t AudioGetClock(void)
{
    // (cast) needed for the evil gcc
    if (AudioPTS != (int64_t) INT64_C(0x8000000000000000)) {
	int64_t delay;

	if ((delay = AudioGetDelay())) {
	    return AudioPTS - delay;
	}
    }
    return INT64_C(0x8000000000000000);
}

/**
**	Set mixer volume (0-100)
**
**	@param volume	volume (0 .. 100)
*/
void AudioSetVolume(int volume)
{
    return AudioUsedModule->SetVolume(volume);
}

/**
**	Setup audio for requested format.
**
**	@param freq	sample frequency
**	@param channels	number of channels
**	@param use_ac3	use ac3/pass-through device
**
**	@retval 0	everything ok
**	@retval 1	didn't support frequency/channels combination
**	@retval -1	something gone wrong
**
**	@todo audio changes must be queued and done when the buffer is empty
*/
int AudioSetup(int *freq, int *channels, int use_ac3)
{
    Debug(3, "audio: channels %d frequency %d hz %s\n", *channels, *freq,
	use_ac3 ? "ac3" : "pcm");

    // invalid parameter
    if (!freq || !channels || !*freq || !*channels) {
	Debug(3, "audio: bad channels or frequency parameters\n");
	// FIXME: set flag invalid setup
	return -1;
    }
#ifdef USE_AUDIORING
    // FIXME: need to store possible combination and report this
    return AudioRingAdd(*freq, *channels, use_ac3);
#endif
    return AudioUsedModule->Setup(freq, channels, use_ac3);
}

/**
**	Play audio.
*/
void AudioPlay(void)
{
    if (!AudioPaused) {
	Warning("audio: not paused, check the code\n");
	return;
    }
    Debug(3, "audio: resumed\n");
    AudioPaused = 0;
    AudioEnqueue(NULL, 0);		// wakeup thread
}

/**
**	Pause audio.
*/
void AudioPause(void)
{
    if (AudioPaused) {
	Warning("audio: already paused, check the code\n");
	return;
    }
    Debug(3, "audio: paused\n");
    AudioPaused = 1;
}

/**
**	Set pcm audio device.
**
**	@param device	name of pcm device (fe. "hw:0,9" or "/dev/dsp")
**
**	@note this is currently used to select alsa/OSS output module.
*/
void AudioSetDevice(const char *device)
{
    if (!AudioModuleName) {
	AudioModuleName = "alsa";	// detect alsa/OSS
	if (!device[0]) {
	    AudioModuleName = "noop";
	} else if (device[0] == '/') {
	    AudioModuleName = "oss";
	}
    }
    AudioPCMDevice = device;
}

/**
**	Set pass-through audio device.
**
**	@param device	name of pass-through device (fe. "hw:0,1")
**
**	@note this is currently usable with alsa only.
*/
void AudioSetDeviceAC3(const char *device)
{
    if (!AudioModuleName) {
	AudioModuleName = "alsa";	// detect alsa/OSS
	if (!device[0]) {
	    AudioModuleName = "noop";
	} else if (device[0] == '/') {
	    AudioModuleName = "oss";
	}
    }
    AudioAC3Device = device;
}

/**
**	Set pcm audio mixer channel.
**
**	@param channel	name of the mixer channel (fe. PCM or Master)
**
**	@note this is currently used to select alsa/OSS output module.
*/
void AudioSetChannel(const char *channel)
{
    AudioMixerChannel = channel;
}

/**
**	Initialize audio output module.
**
**	@todo FIXME: make audio output module selectable.
*/
void AudioInit(void)
{
    int freq;
    int chan;
    unsigned u;
    const char *name;

    name = "noop";
#ifdef USE_OSS
    name = "oss";
#endif
#ifdef USE_ALSA
    name = "alsa";
#endif
    if (AudioModuleName) {
	name = AudioModuleName;
    }
    //
    //	search selected audio module.
    //
    for (u = 0; u < sizeof(AudioModules) / sizeof(*AudioModules); ++u) {
	if (!strcasecmp(name, AudioModules[u]->Name)) {
	    AudioUsedModule = AudioModules[u];
	    Info(_("audio: '%s' output module used\n"), AudioUsedModule->Name);
	    goto found;
	}
    }
    Error(_("audio: '%s' output module isn't supported\n"), name);
    AudioUsedModule = &NoopModule;
    return;

  found:
#ifdef USE_AUDIORING
    AudioRingInit();
#endif
    AudioUsedModule->Init();
    freq = 48000;
    chan = 2;
    if (AudioSetup(&freq, &chan, 0)) {	// set default parameters
	Error(_("audio: can't do initial setup\n"));
    }
#ifdef USE_AUDIO_THREAD
    if (AudioUsedModule->Thread) {	// supports threads
	AudioInitThread();
    }
#endif
    AudioPaused = 0;
}

/**
**	Cleanup audio output module.
*/
void AudioExit(void)
{
#ifdef USE_AUDIO_THREAD
    AudioExitThread();
#endif
    AudioUsedModule->Exit();
    AudioUsedModule = &NoopModule;
#ifdef USE_AUDIORING
    AudioRingExit();
#endif
    AudioRunning = 0;
}

#ifdef AUDIO_TEST

//----------------------------------------------------------------------------
//	Test
//----------------------------------------------------------------------------

void AudioTest(void)
{
    for (;;) {
	unsigned u;
	uint8_t buffer[16 * 1024];	// some random data
	int i;

	for (u = 0; u < sizeof(buffer); u++) {
	    buffer[u] = random() & 0xffff;
	}

	Debug(3, "audio/test: loop\n");
	for (i = 0; i < 100; ++i) {
	    while (RingBufferFreeBytes(AlsaRingBuffer) > sizeof(buffer)) {
		AlsaEnqueue(buffer, sizeof(buffer));
	    }
	    usleep(20 * 1000);
	}
	break;
    }
}

#include <getopt.h>

int SysLogLevel;			///< show additional debug informations

/**
**	Print version.
*/
static void PrintVersion(void)
{
    printf("audio_test: audio tester Version " VERSION
#ifdef GIT_REV
	"(GIT-" GIT_REV ")"
#endif
	",\n\t(c) 2009 - 2012 by Johns\n"
	"\tLicense AGPLv3: GNU Affero General Public License version 3\n");
}

/**
**	Print usage.
*/
static void PrintUsage(void)
{
    printf("Usage: audio_test [-?dhv]\n"
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
    AudioInit();
    for (;;) {
	unsigned u;
	uint8_t buffer[16 * 1024];	// some random data

	for (u = 0; u < sizeof(buffer); u++) {
	    buffer[u] = random() & 0xffff;
	}

	Debug(3, "audio/test: loop\n");
	for (;;) {
	    while (RingBufferFreeBytes(AlsaRingBuffer) > sizeof(buffer)) {
		AlsaEnqueue(buffer, sizeof(buffer));
	    }
	}
    }
    AudioExit();

    return 0;
}

#endif
