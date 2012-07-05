# Copyright 1999-2012 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Header: $

EAPI="3"

inherit eutils vdr-plugin

if [[ ${PV} == "9999" ]] ; then
		inherit git-2
		EGIT_REPO_URI="git://projects.vdr-developer.org/vdr-plugin-softhddevice.git"
else
		SRC_URI="http://projects.vdr-developer.org/attachments/download/838/${P}.tgz"
fi


DESCRIPTION="A software and GPU emulated HD output device plugin for VDR."
HOMEPAGE="http://projects.vdr-developer.org/projects/show/plg-softhddevice"
SRC_URI=""

LICENSE="AGPL-3"
SLOT="0"
KEYWORDS="~x86 ~amd64"
IUSE="vaapi vdpau alsa oss yaepg opengl jpeg"

DEPEND=">=x11-libs/libxcb-1.8
		x11-libs/xcb-util
		x11-libs/xcb-util-wm
		x11-libs/xcb-util-keysyms
		x11-libs/xcb-util-renderutil
		x11-libs/libX11
		opengl? ( virtual/opengl )
		>=virtual/ffmpeg-0.7
		sys-devel/gettext
		sys-devel/make
		dev-util/pkgconfig
		yaepg? ( >=media-video/vdr-1.7.23[yaepg] )
		!yaepg? ( >=media-video/vdr-1.7.23 )
		vdpau? ( x11-libs/libvdpau virtual/ffmpeg[vdpau] )
		vaapi? ( x11-libs/libva virtual/ffmpeg[vaapi] )
		alsa? ( media-libs/alsa-lib )
		oss? ( sys-kernel/linux-headers )
		jpeg? ( virtual/jpeg )
"

src_prepare() {
		vdr-plugin_src_prepare
}

src_compile() {
		local myconf

		myconf="-DHAVE_PTHREAD_NAME"
		use vdpau && myconf="${myconf} -DUSE_VDPAU"
		use vaapi && myconf="${myconf} -DUSE_VAAPI"
		use alsa && myconf="${myconf} -DUSE_ALSA"
		use oss && myconf="${myconf} -DUSE_OSS"
		use jpeg && myconf="${myconf} -DUSE_JPEG"

		emake all CC="$(tc-getCC)" CFLAGS="${CFLAGS}" \
			LDFLAGS="${LDFLAGS}" CONFIG="${myconf}" LIBDIR="." || die
}

src_install() {
		vdr-plugin_src_install

		dodoc README.txt

		#dodir /etc/vdr/plugins || die
		#insinto /etc/vdr/plugins
		#fowners -R vdr:vdr /etc/vdr || die

		#insinto /etc/conf.d
		#doins vdr.softhddevice
}
