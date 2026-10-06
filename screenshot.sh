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
OUT="$DEST/$NAME.png"; RAW="$DEST/.$NAME.raw.png"; rm -f "$OUT" "$RAW"
perl -e 'alarm 20; exec @ARGV' "$ADB" emu screenrecord screenshot "$RAW" >/dev/null
# the emulator writes the file after replying: wait until its size has settled
prev=-1; for i in $(seq 1 50); do sleep 0.2; cur=$(stat -f %z "$RAW" 2>/dev/null || echo 0); [ "$cur" -gt 0 ] && [ "$cur" = "$prev" ] && break; prev=$cur; done
[ -s "$RAW" ] || exit 1
# the panel scans out portrait (eyes stacked); the emulator returns it either way round
w=$(sips -g pixelWidth "$RAW" | awk '/pixelWidth/{print $2}'); h=$(sips -g pixelHeight "$RAW" | awk '/pixelHeight/{print $2}')
if [ "$h" -gt "$w" ]; then sips -r 90 "$RAW" --out "$OUT" >/dev/null; else cp "$RAW" "$OUT"; fi
rm -f "$RAW"
echo "$OUT"
