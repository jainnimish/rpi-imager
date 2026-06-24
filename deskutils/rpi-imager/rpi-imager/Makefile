PORTNAME=       rpi-imager
DISTVERSION=    2.0.10
DISTVERSIONPREFIX= v
CATEGORIES=     deskutils

USE_GITHUB= yes
GH_ACCOUNT= jainnimish

WRKSRC_SUBDIR= src

MAINTAINER= njain15@protonmail.com
COMMENT=    Raspberry Pi Imaging Utility
WWW=        https://github.com/jainnimish/rpi-imager

LICENSE=    APACHE20
LICENSE_FILE= ${WRKDIR}/${PORTNAME}-${PORTVERSION}/license.txt

FLAVORS= default cli
FLAVOR?= ${FLAVORS:[1]}
cli_PKGNAMESUFFIX=-cli

LIB_DEPENDS=    libgnutls.so:security/gnutls \
                libcrypt.so:security/libxcrypt \
                libidn2.so:dns/libidn2 \
                libnettle.so:security/nettle

USES=      qt:6 cmake pkgconfig desktop-file-utils libarchive:run
USE_QT=    base imageformats:run

.if ${FLAVOR} == cli
CMAKE_ON= BUILD_CLI_ONLY
.else
USE_QT+= svg declarative
.endif

OPTIONS_DEFINE= LINGUIST
LINGUIST_DESC=  Enable Qt translation tool

.include <bsd.port.options.mk>

.if ${PORT_OPTIONS:MLINGUIST}
USE_QT+= tools:build
.endif

.include <bsd.port.mk>
