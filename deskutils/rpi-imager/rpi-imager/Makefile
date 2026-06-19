PORTNAME=       rpi-imager
DISTVERSION=    2.0.8
DISTVERSIONPREFIX= v
CATEGORIES=     deskutils

USE_GITHUB= yes
GH_ACCOUNT= jainnimish

WRKSRC_SUBDIR= src

MAINTAINER= njain15@protonmail.com
COMMENT=    Raspberry Pi Imaging Utility
WWW=        https://github.com/jainnimish/rpi-imager

LICENSE=       APACHE20
LICENSE_FILE=  ${WRKSRC}/license.txt

FLAVORS= default cli
FLAVOR?= ${FLAVORS:[1]}
cli_PKGNAMESUFFIX=-cli

LIB_DEPENDS=   libgnutls.so:security/gnutls \
			         libidn2.so:dns/libidn2 \
			         libnettle.so:security/nettle

USES=   qt:6 cmake pkgconfig desktop-file-utils
USE_QT=	core network imageformats_run buildtools_build

.if ${FLAVOR} == cli
CMAKE_ON= BUILD_CLI_ONLY
.else
USE_QT+= gui widgets svg quickcontrols2 declarative dbus
.endif

OPTIONS_DEFINE= LINGUIST
LINGUIST_DESC=  Enable Qt translation tool

.if ${PORT_OPTIONS:LINGUIST}
USE_QT+= linguisttools_build
.endif

.include <bsd.port.mk>
