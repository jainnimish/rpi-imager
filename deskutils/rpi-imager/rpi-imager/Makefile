PORTNAME=       rpi-imager
DISTVERSION=    2.0.8
DISTVERSIONPREFIX= v
PORTREVISION=   1
CATEGORIES=     deskutils

USE_GITHUB= yes
GH_ACCOUNT= raspberrypi

WRKSRC_SUBDIR= src

MAINTAINER= rpi@FreeBSD.org
COMMENT=    Raspberry Pi Imaging Utility
WWW=        https://github.com/raspberrypi/rpi-imager

LICENSE=       APACHE20
LICENSE_FILE=  ${WRKSRC}/license.txt

FLAVORS= default gui
FLAVOR?= ${FLAVORS:[1]}
default_PKGNAMESUFFIX=-cli

BUILD_DEPENDS= git:devel/git
LIB_DEPENDS=   libgnutls.so:security/gnutls \
			         libidn2.so:dns/libidn2 \
			         libnettle.so:security/nettle

USES=   qt:6 cmake pkgconfig desktop-file-utils
USE_QT=	core network imageformats_run

.if ${FLAVOR} == gui
USE_QT+= gui widgets svg quickcontrols2 declarative dbus
.else
CMAKE_ON= BUILD_CLI_ONLY
.endif

OPTIONS_DEFINE= LINGUIST
LINGUIST_DESC=  Enable Qt translation tool

.if ${PORT_OPTIONS:LINGUIST}
USE_QT+= linguisttools_build
.endif

CMAKE_BUILD_TYPE= MinSizeRel

.include <bsd.port.mk>
