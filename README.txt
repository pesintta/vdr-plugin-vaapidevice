@file README.txt		@brief A software HD output device for VDR

Copyright (c) 2011 by Johns.  All Rights Reserved.

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

    o Video VA-API/VA-API
    o planned: Video VA-API/Opengl
    o planned: Video CPU/Xv
    o planned: Video CPU/Opengl
    o planned: Software Deinterlacer
    o Audio FFMpeg/Analog
    o Audio FFMpeg/Digital
    o planned: HDMI/SPDIF Passthrough

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
	cd vdr-softhddevice
	make VDRDIR=<path-to-your-vdr-files> LIBDIR=.

Setup: /etc/vdr/setup.conf
	Following is supported:

	softhddevice.Deinterlace = 0
	0 = bob, 1 = weave, 2 = temporal, 3 = temporal_spatial, 4 = software
	(only 0, 1 supported)

	softhddevice.MakePrimary = 1
	0 = no change, 1 make softhddevice primary at start

	softhddevice.Scaling = 0
	0 = normal, 1 = fast, 2 = HQ, 3 = anamorphic

Requires:
---------
	media-video/ffmpeg
		Complete solution to record, convert and stream audio and
		video. Includes libavcodec.
		http://ffmpeg.org
	media-libs/alsa-lib
		Advanced Linux Sound Architecture Library
		http://www.alsa-project.org
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
    or untested
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
		Only versions >= 0.3.8 are supported

	x11-libs/libX11
		X.Org X11 library
		http://xorg.freedesktop.org

	GNU Make 3.xx
		http://www.gnu.org/software/make/make.html

Optional:
