# Copyright 1999-2014 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Header: /var/cvsroot/gentoo-x86/x11-libs/libva-intel-driver/libva-intel-driver-9999.ebuild,v 1.13 2014/10/24 07:05:21 aballier Exp $

EAPI=5

SCM=""
if [ "${PV%9999}" != "${PV}" ] ; then # Live ebuild
	SCM=git-2
	EGIT_BRANCH=master
	EGIT_REPO_URI="git://anongit.freedesktop.org/git/vaapi/intel-driver"
fi

AUTOTOOLS_AUTORECONF="yes"
inherit autotools-multilib ${SCM}

DESCRIPTION="HW video decode support for Intel integrated graphics"
HOMEPAGE="http://www.freedesktop.org/wiki/Software/vaapi"
if [ "${PV%9999}" != "${PV}" ] ; then # Live ebuild
	SRC_URI=""
	S="${WORKDIR}/${PN}"
else
	SRC_URI="http://www.freedesktop.org/software/vaapi/releases/libva-intel-driver/${P}.tar.bz2"
fi

LICENSE="MIT"
SLOT="0"
#if [ "${PV%9999}" = "${PV}" ] ; then
	KEYWORDS="~amd64 ~x86 ~amd64-linux ~x86-linux"
#else
#	KEYWORDS=""
#fi
IUSE="+drm wayland X"

RDEPEND=">=x11-libs/libva-1.4[X?,wayland?,drm?,${MULTILIB_USEDEP}]
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
	epatch "${FILESDIR}/0001-vpp-fix-adaptive-filter-for-all-channels-flag-Haswel.patch"
	epatch "${FILESDIR}/0002-vpp-fix-AVS-coefficients-for-Broadwell.patch"
	epatch "${FILESDIR}/0003-vpp-factor-out-calculation-of-AVS-coefficients.patch"
	epatch "${FILESDIR}/0004-vpp-add-support-for-high-quality-scaling.patch"
	epatch "${FILESDIR}/0005-vpp-validate-AVS-filter-coefficients-for-debugging-p.patch"
	epatch "${FILESDIR}/0006-vpp-cache-calculation-of-AVS-coefficients.patch"
	epatch "${FILESDIR}/0007-vpp-drop-internal-postprocessing-I965_PP_xxx-flags.patch"
	epatch "${FILESDIR}/0008-vpp-enable-advanced-video-scaling-in-VPP-pipelines-t.patch"
}