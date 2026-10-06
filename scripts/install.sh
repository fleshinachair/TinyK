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

# Target: the Move over SSH (key-based login as user "ableton"). Override when mDNS does not resolve, e.g.
#   MOVE_HOST=ableton@192.168.1.50 ./scripts/install.sh
MOVE_HOST="${MOVE_HOST:-ableton@move.local}"
DEST=/data/UserData/schwung/modules/sound_generators/TinyK

echo "Copying module to Move ($MOVE_HOST)..."
ssh "$MOVE_HOST" "mkdir -p $DEST"
scp $SRC_DIR/dsp.so $MOVE_HOST:$DEST/dsp.so.new
scp $SRC_DIR/module.json $SRC_DIR/ui.js $SRC_DIR/presets.json $SRC_DIR/help.json $MOVE_HOST:$DEST/
ssh "$MOVE_HOST" "mv -f $DEST/dsp.so.new $DEST/dsp.so"

# Optional .syx banks: added to $DEST/banks/ (banks already on the Move are left in place)
ssh "$MOVE_HOST" "mkdir -p $DEST/banks"
if ls $SRC_DIR/banks/*.syx >/dev/null 2>&1 || ls $SRC_DIR/banks/*.SYX >/dev/null 2>&1; then
    echo "Copying .syx banks..."
    scp $SRC_DIR/banks/*.[sS][yY][xX] $MOVE_HOST:$DEST/banks/
fi

# Set permissions
echo "Setting permissions..."
ssh "$MOVE_HOST" "chmod -R a+rw $DEST"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: $DEST/"
echo "Restart Schwung or reload track slot to load the new module."
