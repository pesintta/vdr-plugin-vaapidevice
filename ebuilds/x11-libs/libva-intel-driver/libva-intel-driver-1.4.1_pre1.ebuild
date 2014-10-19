# Copyright 1999-2014 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Header: /var/cvsroot/gentoo-x86/x11-libs/libva-intel-driver/libva-intel-driver-9999.ebuild,v 1.12 2014/06/18 21:02:16 mgorny Exp $

EAPI=5


SCM=git-2
EGIT_COMMIT="f11176415ec26eb5960ba6841d2d9c22f2cabc60"
EGIT_REPO_URI="git://anongit.freedesktop.org/vaapi/intel-driver"

AUTOTOOLS_AUTORECONF="yes"
inherit autotools-multilib ${SCM}

DESCRIPTION="HW video decode support for Intel integrated graphics"
HOMEPAGE="http://www.freedesktop.org/wiki/Software/vaapi"
SRC_URI=""
S="${WORKDIR}/${PN}"

LICENSE="MIT"
SLOT="0"
KEYWORDS="amd64"
IUSE="+drm wayland +X"

RDEPEND=">=x11-libs/libva-1.4.0[X?,wayland?,drm?,${MULTILIB_USEDEP}]
	!<x11-libs/libva-1.0.15[video_cards_intel]
	>=x11-libs/libdrm-2.4.46[video_cards_intel,${MULTILIB_USEDEP}]
	wayland? ( >=media-libs/mesa-9.1.6[egl,${MULTILIB_USEDEP}] >=dev-libs/wayland-1.0.6[${MULTILIB_USEDEP}] )"

DEPEND="${RDEPEND}
	virtual/pkgconfig"

DOCS=( AUTHORS NEWS README )

multilib_src_configure() {
	local myeconfargs=(
		$(use_enable drm)
		$(use_enable wayland)
		$(use_enable X x11)
	)
	autotools-utils_src_configure
}

src_unpack() {
	git-2_src_unpack
	cd "${S}"
	git cherry-pick f4b0f97..ed378b9
}
