# FreeBSD platform-specific sources and link settings

find_package(GnuTLS REQUIRED)

find_package(PkgConfig REQUIRED)

# Deal with non-local include dir packages
macro(get_package PREFIX PKG)
    pkg_check_modules(${PREFIX} REQUIRED ${PKG})
    set(${PREFIX}_INCLUDE_DIR ${${PREFIX}_INCLUDE_DIRS})
    set(${PREFIX}_LIBRARY ${${PREFIX}_LIBRARIES})
endmacro()

get_package(ZLIB zlib)
get_package(LIBLZMA liblzma)
get_package(LibArchive libarchive)
get_package(YESCRYPT libxcrypt)

# local include packages
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(NETTLE REQUIRED IMPORTED_TARGET nettle)
pkg_check_modules(LIBIDN2 REQUIRED IMPORTED_TARGET libidn2)

set(PLATFORM_SOURCES
    drivelist/drivelist_freebsd.cpp
    freebsd/stpanalyzer.h
    freebsd/stpanalyzer.cpp
    freebsd/acceleratedcryptographichash_gnutls.cpp
    freebsd/bootimgcreator_freebsd.cpp
    freebsd/secureboot_crypto_freebsd.cpp
    freebsd/file_operations_freebsd.cpp
    freebsd/platformquirks_freebsd.cpp
)

# Only include DBus-dependent and GUI components for non-CLI builds
if(NOT BUILD_CLI_ONLY)
    list(APPEND PLATFORM_SOURCES
        freebsd/freebsd_suspend_inhibitor.cpp
        freebsd/networkmanagerapi.h
        freebsd/networkmanagerapi.cpp
        freebsd/nativefiledialog_freebsd.cpp
        freebsd/urihandler_dbus.h
        freebsd/urihandler_dbus.cpp
    )
else()
    # Use stub implementations for CLI builds (no DBus dependency)
    list(APPEND PLATFORM_SOURCES
        freebsd/suspend_inhibitor_stub.cpp
        freebsd/wlancredentials_stub.cpp
    )
endif()

set(EXTRALIBS ${EXTRALIBS} GnuTLS::GnuTLS PkgConfig::ZSTD PkgConfig::NETTLE PkgConfig::LIBIDN2)

set(DEPENDENCIES "")
add_definitions(-DHAVE_GNUTLS)

# libusb requires libudev for rpiboot support
pkg_check_modules(UDEV REQUIRED libudev)
