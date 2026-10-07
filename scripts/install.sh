#!/bin/bash
# Install the TinyK module to Ableton Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ -f "dist/tinyk/dsp.so" ]; then
    SRC_DIR="dist/tinyk"
else
    echo "Error: dist/tinyk not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing TinyK Module ==="

# Target: the Move over SSH (key-based login as user "ableton"). Override when mDNS does not resolve, e.g.
#   MOVE_HOST=ableton@192.168.1.50 ./scripts/install.sh
MOVE_HOST="${MOVE_HOST:-ableton@move.local}"
# The directory is the module id (lowercase): Set slot files store "tinyk" and the chain host loads
# sound_generators/<id>/dsp.so on boot, on a case-sensitive file system.
MODULES=/data/UserData/schwung/modules/sound_generators
DEST=$MODULES/tinyk
OLD=$MODULES/TinyK

echo "Copying module to Move ($MOVE_HOST)..."
ssh "$MOVE_HOST" "mkdir -p $DEST/banks"
# One-time migration from the old upper-case directory (id "TinyK", which slots could not restore on boot): keep
# its banks, then remove it so the module is not listed twice.
ssh "$MOVE_HOST" "if [ -d $OLD ]; then for f in $OLD/banks/*; do [ -f \"\$f\" ] && [ ! -e $DEST/banks/\"\$(basename \"\$f\")\" ] && cp \"\$f\" $DEST/banks/; done; rm -rf $OLD && echo 'Migrated $OLD -> $DEST'; fi"
scp $SRC_DIR/dsp.so $MOVE_HOST:$DEST/dsp.so.new
scp $SRC_DIR/module.json $SRC_DIR/ui.js $SRC_DIR/presets.json $SRC_DIR/help.json $SRC_DIR/release.json $MOVE_HOST:$DEST/
ssh "$MOVE_HOST" "mv -f $DEST/dsp.so.new $DEST/dsp.so"

# Banks: the compiled-in bank is the default, so nothing from the local banks/ folder is copied unless asked
# (INSTALL_BANKS=1). TinyK_Default.syx is never copied: it is the source of the built-in bank and would show
# as a duplicate. Banks already on the Move are left in place.
ssh "$MOVE_HOST" "mkdir -p $DEST/banks"
if [ "${INSTALL_BANKS:-0}" = "1" ]; then
    for f in banks/*.[sS][yY][xX]; do
        [ -e "$f" ] || continue
        case "$(basename "$f")" in [Tt]iny[Kk]_[Dd]efault.[sS][yY][xX]) continue ;; esac
        echo "Copying bank $(basename "$f")..."
        scp "$f" "$MOVE_HOST:$DEST/banks/"
    done
fi

# Set permissions
echo "Setting permissions..."
ssh "$MOVE_HOST" "chmod -R a+rw $DEST"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: $DEST/"
echo "Restart Schwung or reload track slot to load the new module."
