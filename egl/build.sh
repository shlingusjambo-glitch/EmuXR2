#!/bin/sh
# builds the vendor EGL shim (needs the NDK); stubs link to ANGLE's libraries from the emulator vendor image
set -e
IMG=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); cd "$(dirname "$0")"; N=~/Library/Android/sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/darwin-x86_64/bin
CC="$N/clang --target=aarch64-linux-android31 -fPIC -shared -O2 -Wl,-rpath,\$ORIGIN"
mkdir -p out; D=/opt/homebrew/opt/e2fsprogs/sbin/debugfs
for l in libEGL_angle libGLESv1_CM_angle libGLESv2_angle; do $D -R "dump /lib64/egl/$l.so out/$l.so" "$IMG" >/dev/null 2>&1; done
$CC -o out/libEGL_macvr.so -Wl,-soname,libEGL_macvr.so egl_macvr.c -Lout -l:libEGL_angle.so -llog -ldl
echo 'void macvr_gles_v1(void){}' | $CC -x c -o out/libGLESv1_CM_macvr.so -Wl,-soname,libGLESv1_CM_macvr.so - -Lout -l:libGLESv1_CM_angle.so
$CC -o out/libGLESv2_macvr.so -Wl,-soname,libGLESv2_macvr.so gles_macvr.c -Lout -l:libGLESv2_angle.so -llog -ldl
rm out/lib*_angle.so; ls out
