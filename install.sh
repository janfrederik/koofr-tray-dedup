#!/bin/sh
# Builds and installs the shim, and points Koofr's autostart entry at it.
# Idempotent: safe to re-run, e.g. after Koofr rewrites its autostart file.
set -eu

cd "$(dirname "$0")"
LIBDIR="$HOME/.local/lib/koofr-tray-dedup"
KOOFR="$HOME/.koofr-dist/storagegui"
DESKTOP="$HOME/.config/autostart/koofr.desktop"

make install PREFIX="$LIBDIR"

if [ ! -f "$DESKTOP" ]; then
    echo "No $DESKTOP; start Koofr with: env LD_LIBRARY_PATH=$LIBDIR $KOOFR" >&2
    exit 0
fi

# Koofr's own entry indents its keys, so match leading whitespace.
sed -i "s|^\([[:space:]]*\)Exec=.*|\1Exec=env LD_LIBRARY_PATH=$LIBDIR $KOOFR|" "$DESKTOP"
grep -n 'Exec=' "$DESKTOP"
echo "Installed. Restart Koofr (quit it, then log out/in or run the Exec line above)."
