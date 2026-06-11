#!/bin/sh
#
# Script to download and build Qt with GUI for FreeBSD
#
# POSIX-compliant shell script
#

set -e

# Source common configuration and functions
BASE_DIR="$(cd "$(dirname "$0")" >/dev/null 2>&1 && pwd)"
. "$BASE_DIR/qt-build-common.sh"

# Initialize common variables
init_common_variables

# Parse command line arguments
usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    print_common_usage_options
    exit 1
}

# Parse common arguments first
parse_common_args "$@"

# Parse script-specific arguments
for arg in $COMMON_REMAINING_ARGS; do
    case $arg in
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $arg"
            usage
            ;;
    esac
done

# Validate common inputs
validate_common_inputs

# Set architecture-specific prefix suffix
set_arch_prefix_suffix

# Print configuration
print_common_config "Qt desktop build"

# Install dependencies if not skipped
if [ "$SKIP_DEPENDENCIES" -eq 0 ]; then
    install_basic_freebsd_deps

    echo "Installing Qt base dependencies..."
    sudo pkg install -y \
        devel/evdev-proto textproc/py-cyclonedx-python-lib misc/py-spdx-tools \
        graphics/vulkan-headers x11/libICE x11/pixman \
        x11/libSM x11/libX11 \
        x11/libXau x11/libxcb-dev x11/libXcomposite x11/libXcursor \
        x11/libXdamage x11/libXdmcp x11/libXext \
        x11/libXi x11/libXfixes x11/libXinerama \
        x11/libXrandr x11/libXrender accessibility/at-spi2-core \
        devel/libb2 archivers/brotli \
        devel/dbus devel/double-conversion graphics/libdrm \
        devel/libevdev \
        graphics/graphite2 print/harfbuzz x11/libinput \
        devel/gettext-runtime \
        graphics/jpeg-turbo devel/libmtdev \
        graphics/png x11/libxkbcommon \
        graphics/vulkan-loader graphics/wayland \
        archivers/zstd print/cups \
        x11/xcb-util-cursor x11/xcb-util-wm \
        x11/xcb-util-image x11/xcb-util-keysyms \
        x11/xcb-util-renderutil x11/xcb-util graphics/libglvnd \
        graphics/cairo graphics/gdk-pixbuf2 devel/glib20 \
        x11-toolkits/gtk30 x11-toolkits/pango \
        multimedia/assimp security/nss audio/pulseaudio audio/alsa-lib \
        graphics/wayland-protocols

else
    print_skip_deps_message "Qt desktop"
fi

# Create build directories
create_build_directories

# Download Qt source code
download_qt_source

# Clean build directory if requested
clean_build_directory

# Configure and build Qt
cd "$BUILD_DIR"

# Build config options using helpers
CONFIG_OPTS="$(get_base_config_opts) -make libs $(get_common_skip_opts)"
CONFIG_OPTS="$CONFIG_OPTS $(get_build_type_opts)"

# Apply exclusions
echo "Applying exclusions for desktop build..."
apply_exclusions
CONFIG_OPTS="$CONFIG_OPTS $EXCLUSION_OPTS"

# Add CMake options
CONFIG_OPTS="$CONFIG_OPTS $(get_cmake_opts)"

# Run Qt configure
run_qt_configure "$CONFIG_OPTS"

# Build Qt
build_qt

# Install Qt
install_qt

# Create environment and toolchain files
create_qt_env_script
create_cmake_toolchain

# Print final usage instructions
print_usage_instructions "Qt desktop"