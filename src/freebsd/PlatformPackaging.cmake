# FreeBSD packaging: installation and runtime checks

if(BUILD_CLI_ONLY)
    message(STATUS "Building CLI-only version for FreeBSD")
else()
    # GUI build: install full desktop integration
    message(FATAL_ERROR "Building GUI version for FreeBSD")
endif()
