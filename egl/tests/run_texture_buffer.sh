#!/bin/sh
# Test the installed guest shim; build.sh must have built the host link libraries.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
FW=${EMUXR2_FIRMWARE:-$HOME/MacVRFirmware}
NDK=${ANDROID_NDK_HOME:-$HOME/Library/Android/sdk/ndk/27.2.12479018}
ADB=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}/platform-tools/adb
SERIAL=${EMUXR2_SERIAL:-emulator-5554}
TMP=$(mktemp -d /tmp/emuxr2-gles-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
for LIB in libEGL_angle libGLESv2_angle; do
    /opt/homebrew/opt/e2fsprogs/sbin/debugfs -R "dump /lib64/egl/$LIB.so $TMP/$LIB.so" "$FW/emu/emu_vendor.img" >/dev/null 2>&1
done
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang" --target=aarch64-linux-android31 \
    "$ROOT/egl/tests/texture_buffer.c" -o "$TMP/test" -L"$ROOT/egl/out" -L"$TMP" \
    -l:libEGL_macvr.so -l:libGLESv2_macvr.so -l:libEGL_angle.so -l:libGLESv2_angle.so \
    -Wl,-rpath,/vendor/lib64/egl
"$ADB" -s "$SERIAL" push "$TMP/test" /data/local/tmp/emuxr2-texture-buffer-test >/dev/null
"$ADB" -s "$SERIAL" shell /data/local/tmp/emuxr2-texture-buffer-test
