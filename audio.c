///
///	@file audio.c		@brief Audio module
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
///	@defgroup Audio The audio module.
///
///		This module contains all audio output functions.
///
///		ALSA PCM api is used.
///		@see http://www.alsa-project.org/alsa-doc/alsa-lib
///
///	alsa async playback is broken, don't use it!
///

#define USE_AUDIO_THREAD

#include <stdio.h>
#include <stdint.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#include <alsa/asoundlib.h>

#ifdef USE_AUDIO_THREAD
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>
#endif

#include "ringbuffer.h"
#include "misc.h"
#include "audio.h"

//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

static const char *AudioPCMDevice;	///< alsa PCM device name
static const char *AudioMixerDevice;	///< alsa mixer device name
static volatile char AudioRunning;	///< thread running / stopped
static int AudioPaused;			///< audio paused
static unsigned AudioSampleRate;	///< audio sample rate in hz
static unsigned AudioChannels;		///< number of audio channels
static uint64_t AudioPTS;		///< audio pts clock

//----------------------------------------------------------------------------
//	Alsa variables
//----------------------------------------------------------------------------

static snd_pcm_t *AlsaPCMHandle;	///< alsa pcm handle
static char AlsaCanPause;		///< hw supports pause
static int AlsaUseMmap;			///< use mmap

static RingBuffer *AlsaRingBuffer;	///< audio ring buffer
static unsigned AlsaStartThreshold;	///< start play, if filled

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
    }
    // Update audio clock
    AudioPTS += (count * 90000) / (AudioSampleRate * AudioChannels * 2);

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
		Error(_("audio/alsa: broken driver %d\n"), avail);
	    }
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
	Debug(4, "audio/alsa: wrote %d/%d frames\n", err, frames);
	if (err < 0) {
	    if (err == -EAGAIN) {
		goto again;
	    }
	    Error(_("audio/alsa: underrun error?\n"));
	    err = snd_pcm_recover(AlsaPCMHandle, err, 0);
	    if (err >= 0) {
		goto again;
	    }
	    Error(_("audio/alsa: snd_pcm_writei failed: %s\n"),
		snd_strerror(err));
	    return -1;
	}
	if (err != frames) {
	    // this could happen, if underrun happened
	    Error(_("audio/alsa: error not all frames written\n"));
	    avail = snd_pcm_frames_to_bytes(AlsaPCMHandle, err);
	}
	RingBufferReadAdvance(AlsaRingBuffer, avail);
	first = 0;
    }

    return 0;
}

#if 0

//	async playback is broken, don't use it!

//----------------------------------------------------------------------------
//	async playback
//----------------------------------------------------------------------------

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
void AudioEnqueue(const void *samples, int count)
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
	    Debug(3, "audio/alsa: state %d - %s\n", state,
		snd_pcm_state_name(state));
	    Debug(3, "audio/alsa: unpaused\n");
	    AudioPaused = 0;
	}
    }
    // Update audio clock
    // AudioPTS += (size * 90000) / (AudioSampleRate * AudioChannels * 2);
}

#endif

//----------------------------------------------------------------------------
//	thread playback
//----------------------------------------------------------------------------

#ifdef USE_AUDIO_THREAD

static pthread_t AudioThread;		///< audio play thread
static pthread_cond_t AudioStartCond;	///< condition variable
static pthread_mutex_t AudioMutex;	///< audio condition mutex

/**
**	Audio play thread.
*/
static void *AudioPlayHandlerThread(void *dummy)
{
    int err;

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

	Debug(3, "audio: play start\n");
	for (;;) {
	    Debug(4, "audio: play loop\n");
	    pthread_testcancel();
	    if ((err = snd_pcm_wait(AlsaPCMHandle, 100)) < 0) {
		Error(_("audio/alsa: wait underrun error?\n"));
		err = snd_pcm_recover(AlsaPCMHandle, err, 0);
		if (err >= 0) {
		    continue;
		}
		Error(_("audio/alsa: snd_pcm_wait(): %s\n"),
		    snd_strerror(err));
		usleep(100 * 1000);
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
		usleep(20 * 1000);
	    }
	}
    }

    return dummy;
}

/**
**	Place samples in audio output queue.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
void AudioEnqueue(const void *samples, int count)
{
    if (AlsaAddToRingbuffer(samples, count)) {
	snd_pcm_state_t state;

	state = snd_pcm_state(AlsaPCMHandle);
	Debug(3, "audio/alsa: enqueue state %s\n", snd_pcm_state_name(state));

	// no lock needed, can wakeup next time
	AudioRunning = 1;
	pthread_cond_signal(&AudioStartCond);
    }
}

#endif

//----------------------------------------------------------------------------
//	direct playback
//----------------------------------------------------------------------------

#if 0

// direct play produces underuns on some hardware

/**
**	Place samples in audio output queue.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
void AudioEnqueue(const void *samples, int count)
{
    snd_pcm_state_t state;
    int avail;
    int n;
    int err;
    int frames;
    const void *p;

    Debug(3, "audio/alsa: %6zd + %4d\n", RingBufferUsedBytes(AlsaRingBuffer),
	count);
    n = RingBufferWrite(AlsaRingBuffer, samples, count);
    if (n != count) {
	Error(_("audio/alsa: can't place %d samples in ring buffer\n"), count);
    }
    // check if running, wait until enough buffered
    state = snd_pcm_state(AlsaPCMHandle);
    Debug(4, "audio/alsa: state %d - %s\n", state, snd_pcm_state_name(state));
    if (state == SND_PCM_STATE_PREPARED) {
	// FIXME: adjust start ratio
	if (RingBufferFreeBytes(AlsaRingBuffer)
	    > RingBufferUsedBytes(AlsaRingBuffer)) {
	    return;
	}
	Debug(3, "audio/alsa: state %d - %s start play\n", state,
	    snd_pcm_state_name(state));
    }
    // Update audio clock
    AudioPTS += (size * 90000) / (AudioSampleRate * AudioChannels * 2);
}

#endif

/**
**	Initialize alsa pcm device.
**
**	@see AudioPCMDevice
*/
static void AlsaInitPCM(void)
{
    const char *device;
    snd_pcm_t *handle;
    snd_pcm_hw_params_t *hw_params;
    int err;
    snd_pcm_uframes_t buffer_size;

    if (!(device = AudioPCMDevice)) {
	if (!(device = getenv("ALSA_DEVICE"))) {
	    device = "default";
	}
    }
    // FIXME: must set alsa error output to /dev/null
    if ((err =
	    snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK,
		SND_PCM_NONBLOCK)) < 0) {
	Fatal(_("audio/alsa: playback open '%s' error: %s\n"), device,
	    snd_strerror(err));
    }
    AlsaPCMHandle = handle;

    if ((err = snd_pcm_nonblock(handle, SND_PCM_NONBLOCK)) < 0) {
	Error(_("audio/alsa: can't set block mode: %s\n"), snd_strerror(err));
    }

    snd_pcm_hw_params_alloca(&hw_params);
    // choose all parameters
    if ((err = snd_pcm_hw_params_any(handle, hw_params)) < 0) {
	Error(_
	    ("audio: snd_pcm_hw_params_any: no configurations available: %s\n"),
	    snd_strerror(err));
    }
    AlsaCanPause = snd_pcm_hw_params_can_pause(hw_params);
    Info(_("audio/alsa: hw '%s' supports pause: %s\n"), device,
	AlsaCanPause ? "yes" : "no");
    snd_pcm_hw_params_get_buffer_size_max(hw_params, &buffer_size);
    Info(_("audio/alsa: max buffer size %lu\n"), buffer_size);

    pthread_mutex_init(&AudioMutex, NULL);
    pthread_cond_init(&AudioStartCond, NULL);
    pthread_create(&AudioThread, NULL, AudioPlayHandlerThread, NULL);
    pthread_detach(AudioThread);
}

//----------------------------------------------------------------------------
//	Alsa Mixer
//----------------------------------------------------------------------------

/**
**	Set mixer volume (0-100)
**
**	@param volume	volume (0 .. 100)
*/
void AudioSetVolume(int volume)
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
    snd_mixer_t *alsa_mixer;
    snd_mixer_elem_t *alsa_mixer_elem;
    long alsa_mixer_elem_min;
    long alsa_mixer_elem_max;

    if (!(device = AudioMixerDevice)) {
	if (!(device = getenv("ALSA_MIXER"))) {
	    device = "default";
	}
    }
    Debug(3, "audio/alsa: mixer open\n");
    snd_mixer_open(&alsa_mixer, 0);
    if (alsa_mixer && snd_mixer_attach(alsa_mixer, device) >= 0
	&& snd_mixer_selem_register(alsa_mixer, NULL, NULL) >= 0
	&& snd_mixer_load(alsa_mixer) >= 0) {

	const char *const alsa_mixer_elem_name = "PCM";

	alsa_mixer_elem = snd_mixer_first_elem(alsa_mixer);
	while (alsa_mixer_elem) {
	    const char *name;

	    name = snd_mixer_selem_get_name(alsa_mixer_elem);
	    if (strcasecmp(name, alsa_mixer_elem_name) == 0) {
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
	Error(_("audio/alsa: can't open alsa mixer '%s'\n"), device);
    }
}

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

/**
**	Get audio delay in time stamps.
*/
uint64_t AudioGetDelay(void)
{
    int err;
    snd_pcm_sframes_t delay;

    if ((err = snd_pcm_delay(AlsaPCMHandle, &delay)) < 0) {
	//Debug(3, "audio/alsa: no hw delay\n");
	delay = 0UL;
    } else if (snd_pcm_state(AlsaPCMHandle) != SND_PCM_STATE_RUNNING) {
	//Debug(3, "audio/alsa: %lu delay ok, but not running\n", delay);
    }
    delay = (delay * 90000) / AudioSampleRate;
    //Debug(3, "audio/alsa: hw delay %lu\n", delay);
    delay +=
	(RingBufferUsedBytes(AlsaRingBuffer) * 90000) / (AudioSampleRate *
	AudioChannels);
    //Debug(3, "audio/alsa: hw+sw delay %lu ms\n", delay / 90);

    return delay;
}

/**
**	Setup audio for requested format.
**
**	@param freq	sample frequency
**	@param channels	number of channels
**
**	@todo audio changes must be queued and done when the buffer is empty
*/
void AudioSetup(int freq, int channels)
{
    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size;
    int err;

#if 1
    Debug(3, "audio/alsa: channels %d frequency %d hz\n", channels, freq);

    if (!freq || !channels) {		// invalid parameter
	// FIXME: set flag invalid setup
	return;
    }

    AudioChannels = channels;
    AudioSampleRate = freq;
    // FIXME: thread!!
    RingBufferReadAdvance(AlsaRingBuffer, RingBufferUsedBytes(AlsaRingBuffer));

    if ((err =
	    snd_pcm_set_params(AlsaPCMHandle, SND_PCM_FORMAT_S16,
		AlsaUseMmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED :
		SND_PCM_ACCESS_RW_INTERLEAVED, channels, freq, 1,
		125 * 1000))) {
	Error(_("audio/alsa: set params error: %s\n"), snd_strerror(err));
	if (channels == 2) {
	    // FIXME: must stop sound
	    return;
	}
	// FIXME: enable channel downmix
	// AudioChannels = downmix_channels;
	return;
    }
#else
    snd_pcm_hw_params_t *hw_params;
    int dir;
    unsigned buffer_time;
    snd_pcm_uframes_t buffer_size;

    Debug(3, "audio/alsa: channels %d frequency %d hz\n", channels, freq);

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

    // update buffer

    snd_pcm_get_params(AlsaPCMHandle, &buffer_size, &period_size);
    Info(_("audio/alsa: buffer size %lu, period size %lu\n"), buffer_size,
	period_size);
    Debug(3, "audio/alsa: state %s\n",
	snd_pcm_state_name(snd_pcm_state(AlsaPCMHandle)));

    AlsaStartThreshold =
	snd_pcm_frames_to_bytes(AlsaPCMHandle, buffer_size + period_size);
    // min 500ms
    if (AlsaStartThreshold < (freq * channels * 2U) / 2) {
	AlsaStartThreshold = (freq * channels * 2U) / 2;
    }
    Debug(3, "audio/alsa: delay %u ms\n", (AlsaStartThreshold * 1000)
	/ (AudioSampleRate * AudioChannels * 2));
}

/**
**	Set alsa pcm audio device.
**
**	@param device	name of pcm device (fe. "hw:0,9")
*/
void AudioSetDevice(const char *device)
{
    AudioPCMDevice = device;
}

/**
**	Initialize audio output module.
*/
void AudioInit(void)
{
    AlsaRingBuffer = RingBufferNew(48000 * 8 * 2);	// ~1s 8ch 16bit

    AlsaInitPCM();
    AlsaInitMixer();

    AudioSetup(48000, 2);		// set default parameters

    AudioPaused = 1;
}

/**
**	Cleanup audio output module.
*/
void AudioExit(void)
{
    void *retval;

    pthread_cancel(AudioThread);
    pthread_join(AudioThread, &retval);
    if (retval != PTHREAD_CANCELED) {
	Error(_("audio: can't cancel alsa play thread\n"));
    }
    pthread_cond_destroy(&AudioStartCond);
    pthread_mutex_destroy(&AudioMutex);

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
}

//----------------------------------------------------------------------------
//	Test
//----------------------------------------------------------------------------

void AudioTest(void)
{
    for (;;) {
	unsigned u;
	uint8_t buffer[16 * 1024];	// some random data

	for (u = 0; u < sizeof(buffer); u++) {
	    buffer[u] = random() & 0xffff;
	}

	Debug(3, "audio/test: loop\n");
	for (;;) {
	    while (RingBufferFreeBytes(AlsaRingBuffer) > sizeof(buffer)) {
		AudioEnqueue(buffer, sizeof(buffer));
	    }
	}
    }
}

#ifdef AUDIO_TEST

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
	",\n\t(c) 2009 - 2011 by Johns\n"
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
		AudioEnqueue(buffer, sizeof(buffer));
	    }
	}
    }
    AudioExit();

    return 0;
}

#endif
