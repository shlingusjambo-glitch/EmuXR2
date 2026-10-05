#!/bin/sh
# Capture the actual guest display. Usage: ./screenshot.sh [name] [directory]
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST=${2:-$H/screenshots}
NAME=${1:-$(date +%Y%m%d-%H%M%S)}
case "$NAME" in *[!a-zA-Z0-9_-]*) echo 'Use a simple screenshot name.' >&2; exit 1;; esac
mkdir -p "$DEST"
ADB=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}/platform-tools/adb
"$ADB" exec-out screencap -p > "$DEST/$NAME.png.tmp"
[ -s "$DEST/$NAME.png.tmp" ] || { rm "$DEST/$NAME.png.tmp"; exit 1; }
mv "$DEST/$NAME.png.tmp" "$DEST/$NAME.png"
echo "$DEST/$NAME.png"
