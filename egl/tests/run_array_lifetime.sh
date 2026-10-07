#!/bin/sh
# Exercise the installed stereo import shim, including deletion before fence completion.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
NDK=${ANDROID_NDK_HOME:-$HOME/Library/Android/sdk/ndk/27.2.12479018}
ADB=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}/platform-tools/adb
SERIAL=${EMUXR2_SERIAL:-emulator-5554}
TMP=$(mktemp -d /tmp/emuxr2-array-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang" --target=aarch64-linux-android31 -O2 \
    "$ROOT/egl/tests/array_lifetime.c" -o "$TMP/test" -lEGL -lGLESv3 -lnativewindow
"$ADB" -s "$SERIAL" push "$TMP/test" /data/local/tmp/emuxr2-array-lifetime >/dev/null
"$ADB" -s "$SERIAL" shell /data/local/tmp/emuxr2-array-lifetime
