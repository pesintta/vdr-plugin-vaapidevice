# Copyright 1999-2012 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Header: $

EAPI="4"

inherit eutils vdr-plugin-2

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
IUSE="vaapi vdpau alsa oss yaepg opengl debug"

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
"

src_prepare() {
		vdr-plugin-2_src_prepare
}

src_compile() {
		local myconf

		myconf="-DHAVE_PTHREAD_NAME"
		use vdpau && myconf="${myconf} -DUSE_VDPAU"
		use vaapi && myconf="${myconf} -DUSE_VAAPI"
		use alsa && myconf="${myconf} -DUSE_ALSA"
		use oss && myconf="${myconf} -DUSE_OSS"
		use debug && myconf="${myconf} -DDEBUG"

		#vdr-plugin-2_src_compile
		cd "${S}"

		BUILD_TARGETS=${BUILD_TARGETS:-${VDRPLUGIN_MAKE_TARGET:-all}}

		emake ${BUILD_PARAMS} CONFIG="${myconf}" \
				${BUILD_TARGETS} \
				LOCALEDIR="${TMP_LOCALE_DIR}" \
				LIBDIR="${S}" \
				TMPDIR="${T}" \
		|| die "emake failed"
}

src_install() {
		vdr-plugin-2_src_install
}
