#!/bin/sh
# Keep stdin open: m dx dy, d/u, c, r, w steps, k Linux-keycode 0/1.
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ADB=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}/platform-tools/adb
"$HERE/input/build.sh"
"$ADB" push "$HERE/input/out/vinput" /data/local/tmp/vinput >/dev/null
"$ADB" shell chmod 755 /data/local/tmp/vinput
exec "$ADB" shell -T /data/local/tmp/vinput
