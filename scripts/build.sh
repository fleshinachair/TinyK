#!/usr/bin/env bash
# Build TinyK module for Ableton Move (ARM64)
#
# Compiles the 4-voice VA synth DSP engine and packages the module.
# Supports Docker cross-compilation or local cross-compilation toolchain.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="tinyk-builder"

# Check if Docker is available and we're not already inside a container/cross environment
if command -v docker &>/dev/null && [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== TinyK Module Build (via Docker) ==="
    echo ""

    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    echo "Running build in container..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual compilation (in Docker or using local cross-compiler) ===
cd "$REPO_ROOT"

echo "=== Building TinyK Module (ARM64 Cortex-A72) ==="
mkdir -p build

BUILD_SUCCESS=0

# Option 1: Use CMake if cross-compiler or toolchain is configured
if command -v cmake &>/dev/null && [ -n "$CROSS_PREFIX" ]; then
    echo "Configuring with CMake..."
    cmake -B build \
        -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        2>&1
    cmake --build build --target mk-va-plugin -j$(nproc 2>/dev/null || echo 4) 2>&1
    BUILD_SUCCESS=1
fi

# Option 2: Use aarch64-linux-gnu-gcc directly if available
if [ $BUILD_SUCCESS -eq 0 ] && command -v aarch64-linux-gnu-gcc &>/dev/null; then
    echo "Compiling with aarch64-linux-gnu-gcc..."
    aarch64-linux-gnu-gcc -O3 -fPIC -shared \
        -march=armv8-a -mtune=cortex-a72 \
        -Wall -Wextra \
        -Isrc -Isrc/dsp \
        src/dsp/dsp.c -lm \
        -Wl,--exclude-libs,ALL \
        -o build/dsp.so
    BUILD_SUCCESS=1
fi

# Option 3: Use Zig cross-compiler (local or in PATH)
if [ $BUILD_SUCCESS -eq 0 ]; then
    ZIG_BIN=""
    if [ -f "$REPO_ROOT/.toolchain/zig-windows-x86_64-0.13.0/zig.exe" ]; then
        ZIG_BIN="$REPO_ROOT/.toolchain/zig-windows-x86_64-0.13.0/zig.exe"
    elif command -v zig &>/dev/null; then
        ZIG_BIN="zig"
    fi

    if [ -n "$ZIG_BIN" ]; then
        echo "Compiling with Zig cross-compiler ($ZIG_BIN)..."
        "$ZIG_BIN" cc -target aarch64-linux-gnu.2.35 -mcpu=cortex_a72 \
            -O3 -fPIC -shared \
            -Wall -Wextra \
            -Isrc -Isrc/dsp \
            src/dsp/dsp.c -lm \
            -o build/dsp.so
        BUILD_SUCCESS=1
    fi
fi

if [ $BUILD_SUCCESS -eq 0 ]; then
    echo "Error: No suitable ARM64 cross-compiler found."
    echo "Please ensure docker, aarch64-linux-gnu-gcc, or zig is installed."
    exit 1
fi

# === Packaging ===
echo "Packaging module..."
# The directory is the module id: lowercase, as Schwung requires (^[a-z0-9][a-z0-9-]*$). Slot files store the id
# and the chain host loads sound_generators/<id>/dsp.so from it on boot; the release asset keeps its name.
MODULE_NAME="tinyk"
ASSET_NAME="TinyK.tar.gz"
DIST_DIR="dist/$MODULE_NAME"
# A fresh directory (dist/TinyK from older builds is the same folder on a case-insensitive disk and kept its case)
rm -rf "$DIST_DIR" dist/TinyK
mkdir -p "$DIST_DIR"

cp src/module.json "$DIST_DIR/module.json"
cp src/ui.js "$DIST_DIR/ui.js"
cp src/canvas.js "$DIST_DIR/canvas.js"   # the Arp Steps LED widget (Schwung param pages)
cp src/presets.json "$DIST_DIR/presets.json"
cp src/help.json "$DIST_DIR/help.json"
if [ -f src/dsp/presets.h ]; then
    cp src/dsp/presets.h "$DIST_DIR/presets.h"
elif [ -f src/presets.h ]; then
    cp src/presets.h "$DIST_DIR/presets.h"
fi
cp build/dsp.so "$DIST_DIR/dsp.so"
chmod +x "$DIST_DIR/dsp.so"

cp release.json "$DIST_DIR/release.json"

# The module scans <module>/banks/ for .syx banks at start-up. The package ships the folder EMPTY, so a
# release never redistributes third-party patch banks; scripts/install.sh copies your local banks/*.syx
# to the Move separately.
rm -rf "$DIST_DIR/banks"
mkdir -p "$DIST_DIR/banks"
printf 'Drop microKORG bank dumps (.syx, 128 programs) here; they appear on the Bank page after reloading TinyK.
' > "$DIST_DIR/banks/README.txt"

# Create release tarball (packaging ONLY TinyK.tar.gz, holding tinyk/)
rm -f "dist/$ASSET_NAME"
cd dist
tar -czvf "$ASSET_NAME" "$MODULE_NAME/"
cd ..
cp "dist/$ASSET_NAME" "./$ASSET_NAME"

echo ""
echo "=== Build Complete ==="
echo "Output Directory: $DIST_DIR"
echo "Tarball: dist/$ASSET_NAME (and ./$ASSET_NAME)"
if command -v file &>/dev/null; then
    echo "Binary inspection:"
    file build/dsp.so
fi
echo ""
