# FreeBSD platform-specific sources and link settings

find_package(GnuTLS REQUIRED)

# Find liburing for async I/O
# Uses pkg-config since liburing doesn't have a CMake config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING liburing)

if(LIBURING_FOUND)
    message(STATUS "Found liburing: ${LIBURING_VERSION}")
    add_definitions(-DHAVE_LIBURING)
else()
    message(WARNING "liburing not found - async I/O will be disabled. Install with: sudo apt install liburing-dev")
endif()

set(PLATFORM_SOURCES
    drivelist/drivelist_freebsd.cpp
    freebsd/stpanalyzer.h
    freebsd/stpanalyzer.cpp
    freebsd/acceleratedcryptographichash_gnutls.cpp
    freebsd/bootimgcreator_freebsd.cpp
    freebsd/rsakeyfingerprint_freebsd.cpp
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

set(EXTRALIBS ${EXTRALIBS} GnuTLS::GnuTLS idn2 nettle)

# Add liburing if available
if(LIBURING_FOUND)
    set(EXTRALIBS ${EXTRALIBS} ${LIBURING_LIBRARIES})
    include_directories(${LIBURING_INCLUDE_DIRS})
endif()

set(DEPENDENCIES "")
add_definitions(-DHAVE_GNUTLS)

# libusb requires libudev for rpiboot support
pkg_check_modules(UDEV REQUIRED libudev)
