#!/bin/sh
# Capture what the compositor scans out (the emulator's physical 1080x1920 panel, eyes stacked as on a Quest 2)
# and turn it upright: both eyes side by side. Usage: ./screenshot.sh [name] [directory]
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST=${2:-$H/screenshots}
NAME=${1:-$(date +%Y%m%d-%H%M%S)}
case "$NAME" in *[!a-zA-Z0-9_-]*) echo 'Use a simple screenshot name.' >&2; exit 1;; esac
mkdir -p "$DEST"
ADB=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}/platform-tools/adb
OUT="$DEST/$NAME.png"; rm -f "$OUT"
perl -e 'alarm 20; exec @ARGV' "$ADB" emu screenrecord screenshot "$OUT" >/dev/null
[ -s "$OUT" ] || exit 1
sips -r 90 "$OUT" >/dev/null
echo "$OUT"
