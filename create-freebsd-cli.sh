#!/bin/sh
set -e

# Script to create CLI-only rpi-imager
# This creates a minimal executable that only supports command-line operation

# Parse command line arguments
ARCH=$(uname -m)  # Default to current architecture
CLEAN_BUILD=1
QT_ROOT_ARG=""

usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  --arch=ARCH            Target architecture (amd64, aarch64, armv7l)"
    echo "  --qt-root=PATH         Path to Qt installation directory"
    echo "  --no-clean             Don't clean build directory"
    echo "  -h, --help             Show this help message"
    echo ""
    echo "This script creates an executable optimized for CLI-only operation:"
    echo "  - Forces CLI mode (no GUI components)"
    echo "  - Uses only Qt Core libraries"
    echo "  - Minimal size by excluding all GUI dependencies"
    echo "  - Perfect for headless/server environments"
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
if [ "$ARCH" != "amd64" ]; then
    echo "Error: Architecture must be amd64"
    exit 1
fi

echo "Building CLI-only executable for architecture: $ARCH"

# Extract project information from CMakeLists.txt
SOURCE_DIR="src/"
CMAKE_FILE="${SOURCE_DIR}CMakeLists.txt"

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

echo "Building $PROJECT_NAME version $GIT_VERSION (numeric: $PROJECT_VERSION) for CLI-only operation"

QT_VERSION=""
QT_DIR=""

# Check if Qt root is specified via command line argument (highest priority)
if [ -n "$QT_ROOT_ARG" ]; then
    echo "Using Qt from command line argument: $QT_ROOT_ARG"
    QT_DIR="$QT_ROOT_ARG"
# Check if Qt6_ROOT is explicitly set in environment
elif [ -n "$Qt6_ROOT" ]; then
    echo "Using Qt from Qt6_ROOT environment variable: $Qt6_ROOT"
    QT_DIR="$Qt6_ROOT"
# Auto-detect Qt installation in /opt/Qt (look for CLI-specific builds first)
else
    if [ -d "/opt/Qt" ]; then
        echo "Checking for Qt installations in /opt/Qt..."
        # Find the newest Qt6 version installed
        NEWEST_QT=$(find /opt/Qt -maxdepth 1 -type d -name "6.*" | sort -V | tail -n 1)
        if [ -n "$NEWEST_QT" ]; then
            QT_VERSION=$(basename "$NEWEST_QT")

            # Find appropriate compiler directory for the architecture
            # Priority: CLI-specific builds, then regular builds
            if [ "$ARCH" = "amd64" ]; then
                if [ -d "$NEWEST_QT/gcc_amd64_cli" ]; then
                    QT_DIR="$NEWEST_QT/gcc_amd64_cli"
                    echo "Found CLI-optimized Qt build"
                elif [ -d "$NEWEST_QT/gcc_amd64" ]; then
                    QT_DIR="$NEWEST_QT/gcc_amd64"
                    echo "Using regular Qt build (consider building CLI-optimized version)"
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

# If Qt not found, suggest building it
if [ -z "$QT_DIR" ]; then
    echo "Error: No suitable Qt installation found for $ARCH"

    if [ -f "./qt/build-qt-cli.sh" ]; then
        echo "You can build a CLI-optimized Qt using:"
        echo "  ./qt/build-qt-cli.sh --version=6.9.1"
        echo "Or specify the Qt location with:"
        echo "  $0 --qt-root=/path/to/qt"
    else
        echo "You can specify the Qt location with:"
        echo "  $0 --qt-root=/path/to/qt"
    fi

    exit 1
fi

# Check if Qt Version
if [ -f "$QT_DIR/bin/qmake" ]; then
    QT_VERSION=$("$QT_DIR/bin/qmake" -query QT_VERSION)
    echo "Qt version: $QT_VERSION"
fi

# Configuration
BUILD_TYPE="MinSizeRel"  # Optimize for size

# Location of AppDir and output file
APPDIR="$PWD/AppDir-cli-$ARCH"
OUTPUT_FILE="$PWD/Raspberry_Pi_Imager-${GIT_VERSION}-cli-${ARCH}"

# Set up build directory
BUILD_DIR="build-cli-$ARCH"

# Clean up previous builds if requested
if [ "$CLEAN_BUILD" -eq 1 ]; then
    echo "Cleaning previous build..."
    rm -rf "$APPDIR" "$BUILD_DIR"
fi

mkdir -p "$APPDIR"
mkdir -p "$BUILD_DIR"

echo "Building rpi-imager CLI-only for $ARCH..."
# Configure and build with CMake
cd "$BUILD_DIR"

# Set architecture-specific CMake flags
CMAKE_EXTRA_FLAGS=""
if [ "$ARCH" = "aarch64" ] && [ "$(uname -m)" = "amd64" ]; then
    # Cross-compiling from amd64 to aarch64
    echo "Cross-compiling from $(uname -m) to $ARCH"
    CMAKE_EXTRA_FLAGS="-DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_SYSTEM_PROCESSOR=aarch64"
elif [ "$ARCH" = "armv7l" ] && [ "$(uname -m)" = "amd64" ]; then
    # Cross-compiling from amd64 to armv7l
    echo "Cross-compiling from $(uname -m) to $ARCH"
    CMAKE_EXTRA_FLAGS="-DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_SYSTEM_PROCESSOR=arm"
fi

# Add Qt path to CMake flags
CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DQt6_ROOT=$QT_DIR"

# Build CLI-only version
CMAKE_EXTRA_FLAGS="$CMAKE_EXTRA_FLAGS -DBUILD_CLI_ONLY=ON"

# shellcheck disable=SC2086
cmake "../$SOURCE_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_INSTALL_PREFIX=/usr/local $CMAKE_EXTRA_FLAGS
make -j"$(nproc)"

echo "Creating CLI-only AppDir..."
# Install to AppDir
make DESTDIR="$APPDIR" install
cd ..

# Create the AppRun file for CLI operation
cat > "$APPDIR/AppRun" << 'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "${0}")")"

# Set up paths
export PATH="${HERE}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH}"

# Execute the CLI-only binary directly (no --cli flag needed, it's built-in)
exec "${HERE}/usr/local/bin/rpi-imager-cli" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# Manual Qt deployment for CLI-only (minimal)
echo "Deploying minimal Qt dependencies for CLI-only operation..."

# Copy only essential Qt libraries (CLI-only)
mkdir -p "$APPDIR/usr/lib"
if [ -f "$QT_DIR/lib/libQt6Core.so" ] && [ -f "$QT_DIR/lib/libQt6Network.so" ]; then
    cp "$QT_DIR/lib/libQt6Core.so"* "$APPDIR/usr/lib/"
    cp "$QT_DIR/lib/libQt6Network.so"* "$APPDIR/usr/lib/" 2>/dev/null || true
elif [ -f "$QT_DIR/libQt6Core.so" ] && [ -f "$QT_DIR/libQt6Network.so" ]; then
    # If system Qt libs were used, there will be no lib subdirectory
    cp "$QT_DIR/libQt6Core.so"* "$APPDIR/usr/lib/"
    cp "$QT_DIR/libQt6Network.so"* "$APPDIR/usr/lib/" 2>/dev/null || true
else
    echo "Failed to find Qt6 library locations"
    return 1
fi

# Note: No GUI libraries (QtGui, QtQuick, QtWidgets, QtSvg, etc.)

# Copy minimal plugins (no GUI plugins needed)
mkdir -p "$APPDIR/usr/plugins/platforms"
# Note: No platform plugins needed for CLI-only operation

# Copy only essential network plugins
mkdir -p "$APPDIR/usr/plugins"
cp -r "$QT_DIR/plugins/tls" "$APPDIR/usr/plugins/" 2>/dev/null || true

# CLI-specific optimizations
echo "Applying CLI-only optimizations..."

# Remove any GUI-related files that might have been included
rm -rf "$APPDIR/usr/qml" 2>/dev/null || true
rm -rf "$APPDIR/usr/plugins/platforms" 2>/dev/null || true
rm -rf "$APPDIR/usr/plugins/imageformats" 2>/dev/null || true
rm -rf "$APPDIR/usr/plugins/iconengines" 2>/dev/null || true
rm -rf "$APPDIR/usr/share/fonts" 2>/dev/null || true
rm -rf "$APPDIR/usr/translations" 2>/dev/null || true

# Remove GUI libraries if they somehow got included
rm -f "$APPDIR/usr/lib/libQt6Gui.so"* 2>/dev/null || true
rm -f "$APPDIR/usr/lib/libQt6Quick.so"* 2>/dev/null || true
rm -f "$APPDIR/usr/lib/libQt6Qml.so"* 2>/dev/null || true
rm -f "$APPDIR/usr/lib/libQt6Widgets.so"* 2>/dev/null || true
rm -f "$APPDIR/usr/lib/libQt6Svg.so"* 2>/dev/null || true
rm -f "$APPDIR/usr/lib/libQt"*"QuickControls"*.so* 2>/dev/null || true

# Remove development files to save space
find "$APPDIR" -name "*.a" -delete 2>/dev/null || true
find "$APPDIR" -name "*.la" -delete 2>/dev/null || true
find "$APPDIR" -name "*.prl" -delete 2>/dev/null || true

# Strip binaries to reduce size
find "$APPDIR" -type f -executable -exec strip {} \; 2>/dev/null || true

echo "Stripping shared libraries..."
SAVED_DIR="$PWD"
cd "$APPDIR"
SO_FILES=$(find . -name "*.so*" 2>/dev/null || true)
if [ -n "$SO_FILES" ]; then
    echo "$SO_FILES"
    # shellcheck disable=SC2086
    strip --strip-unneeded $SO_FILES 2>/dev/null || true
fi
cd "$SAVED_DIR"

echo "Creating CLI-only app..."

echo "Creating self-contained tarball..."
tar czf "${OUTPUT_FILE}.tar.gz" -C "$APPDIR" .
echo "Created compressed archive: ${OUTPUT_FILE}.tar.gz"
