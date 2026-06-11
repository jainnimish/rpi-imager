#!/bin/sh
set -e

# Parse command line arguments
ARCH=$(uname -m)  # Default to current architecture
CLEAN_BUILD=1
QT_ROOT_ARG=""

usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  --arch=ARCH        Target architecture (amd64, aarch64)"
    echo "  --qt-root=PATH     Path to Qt installation directory"
    echo "  --no-clean         Don't clean build directory"
    echo "  -h, --help         Show this help message"
    exit 1
}

for arg in "$@"; do
    case $arg in
        --arch=*)
            ARCH="${arg#*=}"
            ;;
        --qt-root=*)
            QT_ROOT_ARG="${arg#*=}"
            ;;
        --no-clean)
            CLEAN_BUILD=0
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $arg"
            usage
            ;;
    esac
done

# Resolve Qt root path argument if provided (expand ~ and convert to absolute path)
if [ -n "$QT_ROOT_ARG" ]; then
    # Expand tilde if present at the start
    case "$QT_ROOT_ARG" in
        "~"/*) QT_ROOT_ARG="$HOME/${QT_ROOT_ARG#\~/}" ;;
        "~")   QT_ROOT_ARG="$HOME" ;;
    esac
    # Convert to absolute path if it exists
    if [ -e "$QT_ROOT_ARG" ]; then
        QT_ROOT_ARG=$(cd "$QT_ROOT_ARG" && pwd)
    else
        echo "Warning: Specified Qt root path does not exist: $QT_ROOT_ARG"
        echo "Will attempt to use it anyway, but this may fail..."
    fi
fi

# Validate architecture
if [ "$ARCH" != "amd64" ] && [ "$ARCH" != "aarch64" ]; then
    echo "Error: Architecture must be one of: amd64, aarch64"
    exit 1
fi

echo "Building for architecture: $ARCH"

# Extract project information from CMakeLists.txt
SOURCE_DIR="src/"
CMAKE_FILE="${SOURCE_DIR}CMakeLists.txt"
CMAKE_INSTALL_PREFIX="/usr/local"

# Get version from git tag (same approach as CMake)
GIT_VERSION=$(git describe --tags --always --dirty 2>/dev/null || echo "0.0.0-unknown")

# Extract numeric version components for compatibility
# Match versions like: v1.2.3, 1.2.3, v1.2.3-extra, etc.
MAJOR=$(echo "$GIT_VERSION" | sed -n 's/^v\{0,1\}\([0-9]\{1,\}\)\.[0-9]\{1,\}\.[0-9]\{1,\}.*/\1/p')
MINOR=$(echo "$GIT_VERSION" | sed -n 's/^v\{0,1\}[0-9]\{1,\}\.\([0-9]\{1,\}\)\.[0-9]\{1,\}.*/\1/p')
PATCH=$(echo "$GIT_VERSION" | sed -n 's/^v\{0,1\}[0-9]\{1,\}\.[0-9]\{1,\}\.\([0-9]\{1,\}\).*/\1/p')

if [ -n "$MAJOR" ] && [ -n "$MINOR" ] && [ -n "$PATCH" ]; then
    PROJECT_VERSION="$MAJOR.$MINOR.$PATCH"
else
    MAJOR="0"
    MINOR="0"
    PATCH="0"
    PROJECT_VERSION="0.0.0"
    echo "Warning: Could not parse version from git tag: $GIT_VERSION"
fi

# Extract project name
PROJECT_NAME=$(grep "project(" "$CMAKE_FILE" | head -1 | sed 's/project(\([^[:space:]]*\).*/\1/' | tr '[:upper:]' '[:lower:]')

echo "Building $PROJECT_NAME version $GIT_VERSION (numeric: $PROJECT_VERSION)"

# Check for Qt installation
# Priority: 1. Command line argument, 2. Environment variable, 3. Auto-detection
QT_VERSION=""
QT_DIR=""

# Check if Qt root is specified via command line argument (highest priority)
if [ -n "$QT_ROOT_ARG" ]; then
    echo "Using Qt from command line argument: $QT_ROOT_ARG"
    QT_DIR="$QT_ROOT_ARG"
    # Try to determine the version if possible
    if [ -f "$QT_DIR/bin/qmake" ]; then
        QT_VERSION=$("$QT_DIR/bin/qmake" -query QT_VERSION)
        echo "Qt version: $QT_VERSION"
    fi
# Check if Qt6_ROOT is explicitly set in environment
elif [ -n "$Qt6_ROOT" ]; then
    echo "Using Qt from Qt6_ROOT environment variable: $Qt6_ROOT"
    QT_DIR="$Qt6_ROOT"
    # Try to determine the version if possible
    if [ -f "$QT_DIR/bin/qmake" ]; then
        QT_VERSION=$("$QT_DIR/bin/qmake" -query QT_VERSION)
        echo "Qt version: $QT_VERSION"
    fi
# Auto-detect Qt installation in /opt/Qt
else
    if [ -d "/opt/Qt" ]; then
        echo "Checking for Qt installations in /opt/Qt..."
        # Find the newest Qt6 version installed
        NEWEST_QT=$(find /opt/Qt -maxdepth 1 -type d -name "6.*" | sort -V | tail -n 1)
        if [ -n "$NEWEST_QT" ]; then
            QT_VERSION=$(basename "$NEWEST_QT")

            # Find appropriate compiler directory for the architecture
            if [ "$ARCH" = "amd64" ]; then
                if [ -d "$NEWEST_QT/gcc_64" ]; then
                    QT_DIR="$NEWEST_QT/gcc_64"
                fi
            elif [ "$ARCH" = "aarch64" ]; then
                if [ -d "$NEWEST_QT/gcc_arm64" ]; then
                    QT_DIR="$NEWEST_QT/gcc_arm64"
                fi
            fi

            if [ -n "$QT_DIR" ]; then
                echo "Found Qt $QT_VERSION for $ARCH at $QT_DIR"
            else
                echo "Found Qt $QT_VERSION, but no binary directory for $ARCH"
                QT_VERSION=""
            fi
        fi
    fi
fi

# If Qt not found, suggest running build-qt.sh
if [ -z "$QT_DIR" ]; then
    echo "Error: No suitable Qt installation found for $ARCH"

    if [ -f "./qt/build-qt.sh" ]; then
        echo "You can build Qt using the provided script:"
        echo "  ./qt/build-qt.sh --version=6.9.1"
        echo "Or specify the Qt location with:"
        echo "  $0 --qt-root=/path/to/qt"
        echo "  export Qt6_ROOT=/path/to/qt"
    else
        echo "You can specify the Qt location with:"
        echo "  $0 --qt-root=/path/to/qt"
        echo "  export Qt6_ROOT=/path/to/qt"
    fi

    exit 1
fi

# Configuration
BUILD_TYPE="MinSizeRel"
QML_SOURCES_PATH="$PWD/src/qmlcomponents/"


# Set up build directory
BUILD_DIR="build-$ARCH"

# Clean up previous builds if requested
if [ "$CLEAN_BUILD" -eq 1 ]; then
    echo "Cleaning previous build..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "Building rpi-imager for $ARCH..."
# Configure and build with CMake
cd "$BUILD_DIR"

# Set architecture-specific CMake flags
CMAKE_EXTRA_FLAGS=""
if [ "$ARCH" = "aarch64" ] && [ "$(uname -m)" = "amd64" ]; then
    # Cross-compiling from amd64 to aarch64
    echo "Cross-compiling from $(uname -m) to $ARCH"
    # You may need to adjust these flags depending on your cross-compilation setup
    CMAKE_EXTRA_FLAGS="-DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_SYSTEM_PROCESSOR=aarch64"
fi

# Add Qt path to CMake flags
CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DQt6_ROOT=$QT_DIR"

# shellcheck disable=SC2086
cmake "../$SOURCE_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_INSTALL_PREFIX=$CMAKE_INSTALL_PREFIX $CMAKE_EXTRA_FLAGS
make -j"$(nproc)"
echo "Build completed successfully for $ARCH architecture."

# Install the binary
make install
