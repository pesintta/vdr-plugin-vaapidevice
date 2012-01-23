@file README.txt		@brief A software HD output device for VDR

Copyright (c) 2011, 2012 by Johns.  All Rights Reserved.

Contributor(s):

License: AGPLv3

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

$Id$

A software and GPU emulated HD output device plugin for VDR.

    o Video VA-API/VA-API (with intel, nvidia and amd backend supported)
    o Video CPU/VA-API
    o Video VDPAU/VDPAU
    o Video CPU/VDPAU
    o Audio FFMpeg/Alsa/Analog
    o Audio FFMpeg/Alsa/Digital
    o Audio FFMpeg/OSS/Analog
    o HDMI/SPDIF Passthrough
    o VA-API bob software deinterlace
    o Auto-crop

    o planned: Video VA-API/Opengl
    o planned: Video VDPAU/Opengl
    o planned: Video CPU/Xv
    o planned: Video CPU/Opengl
    o planned: Improved Software Deinterlacer (yadif or/and ffmpeg filters)
    o planned: Video XvBA/XvBA
    o planned: atmo light support

To compile you must have the 'requires' installed.

Good luck
johns

Quickstart:
-----------

Just type make and use.

Install:
--------
	1a) git

	git clone git://projects.vdr-developer.org/vdr-plugin-softhddevice.git
	cd vdr-plugin-softhddevice
	make VDRDIR=<path-to-your-vdr-files> LIBDIR=.
	gentoo: make VDRDIR=/usr/include/vdr LIBDIR=.

	2a) tarball

	Download latest version from:
	    http://projects.vdr-developer.org/projects/plg-softhddevice/files

	tar vxf vdr-softhddevice-*.tar.bz2
	cd softhddevice-*
	make VDRDIR=<path-to-your-vdr-files> LIBDIR=.

	You can edit Makefile to enable/disable VDPAU / VA-API / Alsa / OSS
	support.

Setup:	environment
------
	Following is supported:

	DISPLAY=:0.0
		x11 display name
    only if alsa is configured
	ALSA_DEVICE=default
		alsa PCM device name
	ALSA_AC3_DEVICE=
		alsa AC3/pass-though device name
	ALSA_MIXER=default
		alsa control device name
	ALSA_MIXER_CHANNEL=PCM
		alsa control channel name
    only if oss is configured
	OSS_AUDIODEV=/dev/dsp
		oss dsp device name
	OSS_AC3_AUDIODEV=
		oss AC3/pass-though device name
	OSS_MIXERDEV=/dev/mixer
		oss mixer device name
	OSS_MIXER_CHANNEL=pcm
		oss mixer channel name

Setup: /etc/vdr/setup.conf
------
	Following is supported:

	softhddevice.MakePrimary = 1
	0 = no change, 1 make softhddevice primary at start

	softhddevice.HideMainMenuEntry = 0
	0 = show softhddevice main menu entry, 1 = hide entry

	<res> of the next parameters is 567i, 720p, 1080i_fake or 1080i.
	1080i_fake is 1280x1080 or 1440x1080
	1080i is "real" 1920x1080

	softhddevice.<res>.Scaling = 0
	0 = normal, 1 = fast, 2 = HQ, 3 = anamorphic

	softhddevice.<res>.Deinterlace = 0
	0 = bob, 1 = weave, 2 = temporal, 3 = temporal_spatial, 4 = software
	(only 0, 1 supported with vaapi)

	softhddevice.<res>.SkipChromaDeinterlace = 0
	0 = disabled, 1 = enabled (for slower cards, poor qualität)

	softhddevice.<res>.Denoise = 0
	0 .. 1000 noise reduction level (0 off, 1000 max)

	softhddevice.<res>.Sharpness = 0
	-1000 .. 1000 noise reduction level (0 off, -1000 max blur,
	    1000 max sharp)

	softhddevice.AudioDelay = 0
	+n or -n ms

	softhddevice.AudioPassthrough = 0
	0 = none, 1 = AC-3

	for AC-3 the pass-through device is used.

	softhddevice.AutoCrop.Interval = 0
	0 disables auto-crop
	n each 'n' frames auto-crop is checked.

	softhddevice.AutoCrop.Delay = 0
	if auto-crop is over after 'n' intervals the same, the cropping is
	used.

	softhddevice.Suspend.Close = 0
	1 suspend closes x11 window, connection and audio device.
	(use svdrpsend plug softhddevice RESU to resume, if you have no lirc)

	softhddevice.Suspend.X11 = 0
	1 suspend stops X11 server (not working yet)

Setup: /etc/vdr/remote.conf
------

	Add "XKeySym." definitions to /etc/vdr/remote.conf to control
	the vdr and plugin with the connected input device.

	fe.
	XKeySym.Up	Up
	XKeySym.Down	Down
	...

	Additional to the x11 input sends the window close button "Close".

	fe.
	XKeySym.Power	Close

Commandline:
------------

	Use vdr -h to see the command line arguments supported by the plugin.

    -a audio_device

	Selects audio output module and device.
	""		to disable audio output
	/...		to use oss audio module (if compiled with oss
			support)
	other		to use alsa audio module (if compiled with alsa
			support)

SVDRP:
------

	Use 'svdrpsend.pl plug softhddevice HELP' to see the SVDRP commands
	help and which are supported by the plugin.

Running:
--------

	Click into video window to toggle fullscreen/window mode, only if you
	have a window manager running.

Warning:
--------
	libav is not supported, expect many bugs with it.

Requires:
---------
	media-video/ffmpeg (version >=0.7)
		Complete solution to record, convert and stream audio and
		video. Includes libavcodec.
		http://ffmpeg.org
	media-libs/alsa-lib
		Advanced Linux Sound Architecture Library
		http://www.alsa-project.org
    or
	kernel support for oss/oss4 or alsa oss emulation

	x11-libs/libva
		Video Acceleration (VA) API for Linux
		http://www.freedesktop.org/wiki/Software/vaapi
	x11-libs/libva-intel-driver
		HW video decode support for Intel integrated graphics
		http://www.freedesktop.org/wiki/Software/vaapi
    or
	x11-libs/vdpau-video
		VDPAU Backend for Video Acceleration (VA) API
		http://www.freedesktop.org/wiki/Software/vaapi
    or
	x11-libs/xvba-video
		XVBA Backend for Video Acceleration (VA) API
		http://www.freedesktop.org/wiki/Software/vaapi
	x11-libs/libxcb,
		X C-language Bindings library
		http://xcb.freedesktop.org
	x11-libs/xcb-util,
	x11-libs/xcb-util-wm,
	x11-libs/xcb-util-keysyms
		X C-language Bindings library
		http://xcb.freedesktop.org
		Only versions >= 0.3.8 are good supported

	x11-libs/libX11
		X.Org X11 library
		http://xorg.freedesktop.org

	GNU Make 3.xx
		http://www.gnu.org/software/make/make.html

Optional:
