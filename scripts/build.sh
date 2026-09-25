#!/usr/bin/env bash
# Build schwung-mk-va module for Ableton Move (ARM64)
#
# Compiles the 4-voice VA synth DSP engine and packages the module.
# Supports Docker cross-compilation or local cross-compilation toolchain.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="tinykorg-builder"

# Check if Docker is available and we're not already inside a container/cross environment
if command -v docker &>/dev/null && [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== tinyKorg Module Build (via Docker) ==="
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

echo "=== Building tinyKorg Module (ARM64 Cortex-A72) ==="
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
DIST_DIR="dist/TinyK"
mkdir -p "$DIST_DIR"

cp src/module.json "$DIST_DIR/module.json"
cp src/ui.js "$DIST_DIR/ui.js"
cp src/presets.json "$DIST_DIR/presets.json"
cp src/help.json "$DIST_DIR/help.json"
cp build/dsp.so "$DIST_DIR/dsp.so"
chmod +x "$DIST_DIR/dsp.so"

# Also create aliases for legacy variants
mkdir -p dist/tinykorg dist/schwung-mk-va dist/mk-va
cp -r "$DIST_DIR/"* dist/tinykorg/
cp -r "$DIST_DIR/"* dist/schwung-mk-va/
cp -r "$DIST_DIR/"* dist/mk-va/

# Create release tarballs
cd dist
tar -czvf TinyK.tar.gz TinyK/
tar -czvf tinykorg.tar.gz tinykorg/
tar -czvf schwung-mk-va.tar.gz schwung-mk-va/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output Directory: $DIST_DIR"
echo "Tarball: dist/TinyK.tar.gz"
if command -v file &>/dev/null; then
    echo "Binary inspection:"
    file build/dsp.so
fi
echo ""
