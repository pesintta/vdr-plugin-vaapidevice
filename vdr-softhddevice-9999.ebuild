# Copyright 1999-2012 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Header: $

EAPI="3"

inherit vdr-plugin

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
IUSE="vaapi vdpau alsa oss"

DEPEND=">=x11-libs/libxcb-1.7
		x11-libs/xcb-util
		x11-libs/xcb-util-wm
		x11-libs/xcb-util-wm
		x11-libs/xcb-util-keysyms
		x11-libs/xcb-util-renderutil
		x11-libs/libX11
		sys-devel/gettext
		sys-devel/make
		vdpau? ( x11-libs/libvdpau )
		vaapi? ( x11-libs/libva )
		alsa? ( media-libs/alsa-lib )
"

src_prepare() {
		vdr-plugin_src_prepare
}

src_compile() {
		local myconf

		myconf=""
		use vdpau && myconf="${myconf} -DUSE_VDPAU"
		use vaapi && myconf="${myconf} -DUSE_VAAPI"
		use alsa && myconf="${myconf} -DUSE_ALSA"
		use oss && myconf="${myconf} -DUSE_OSS"

		emake all CC="$(tc-getCC)" CFLAGS="${CFLAGS}" \
			LDFLAGS="${LDFLAGS}" CONFIG="${myconf}" LIBDIR="."
}

src_install() {
		vdr-plugin_src_install

		dodir /etc/vdr/plugins || die
		insinto /etc/vdr/plugins
		fowners -R vdr:vdr /etc/vdr || die
}
