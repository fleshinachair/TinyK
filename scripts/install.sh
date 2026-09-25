#!/bin/bash
# Install schwung-mk-va module to Ableton Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ -d "dist/TinyK" ]; then
    SRC_DIR="dist/TinyK"
elif [ -d "dist/tinykorg" ]; then
    SRC_DIR="dist/tinykorg"
else
    echo "Error: dist/TinyK not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing TinyK Module ==="

DEST=/data/UserData/schwung/modules/sound_generators/TinyK

echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p $DEST"
scp $SRC_DIR/dsp.so ableton@move.local:$DEST/dsp.so.new
scp $SRC_DIR/module.json $SRC_DIR/ui.js $SRC_DIR/presets.json $SRC_DIR/help.json ableton@move.local:$DEST/
ssh ableton@move.local "mv -f $DEST/dsp.so.new $DEST/dsp.so"

# Set permissions
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw $DEST"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: $DEST/"
echo "Restart Schwung or reload track slot to load the new module."
