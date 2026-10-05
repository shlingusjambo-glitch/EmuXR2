#!/bin/sh
# builds /vendor/bin/hw/macvr-hal against the platform libraries of the user's firmware (needs the NDK and the
# AOSP 12L headers in $FW/aosp). usage: build.sh <firmware work dir>
set -e
FW=$1; cd "$(dirname "$0")"; mkdir -p out lib
N=~/Library/Android/sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/darwin-x86_64
V=$FW/aosp/vndk D=/opt/homebrew/opt/e2fsprogs/sbin/debugfs
for l in libhidlbase libutils libcutils libc++ libbase; do [ -f lib/$l.so ] || $D -R "dump /system/apex/com.android.vndk.current/lib64/$l.so lib/$l.so" $FW/work/system.img >/dev/null 2>&1; done
for l in liblog; do [ -f lib/$l.so ] || $D -R "dump /system/lib64/$l.so lib/$l.so" $FW/work/system.img >/dev/null 2>&1; done
for l in $(cd $FW/fs/vendor/lib64 && ls vendor.oculus.*.so); do cp -f $FW/fs/vendor/lib64/$l lib/; done
RES=$($N/bin/clang -print-resource-dir)
INC="-nostdinc -isystem $V/external/libcxx/include -isystem $RES/include -isystem $FW/aosp/bionic_inc -isystem $V/bionic/libc/kernel/uapi -isystem $V/bionic/libc/kernel/uapi/asm-arm64 -isystem $V/bionic/libc/kernel/android/uapi
 -I$V/system/libfmq/base -I$V/external/libcxxabi/include -I$V/system/libhidl/base/include -I$V/system/libhidl/transport/include
 -I$V/system/core/libutils/include -I$V/system/core/libcutils/include -I$V/system/logging/liblog/include_vndk -I$V/system/libbase/include
 -I$V/system/libhwbinder/include -I$V/system/core/libsystem/include -I$V/system/libfmq/include -I$V/generated-headers/system/libhidl/transport/base/1.0/android.hidl.base@1.0_genc++_headers/gen
 -I$V/generated-headers/system/libhidl/transport/manager/1.0/android.hidl.manager@1.0_genc++_headers/gen"
$N/bin/clang++ --target=aarch64-linux-android31 -std=c++17 -O2 -fno-rtti -fPIE -pie -nostdinc++ -nostdlib++ -D__ANDROID_VNDK__ -Wno-unused-parameter $INC \
  -o out/macvr-hal *.cpp -Llib -l:libhidlbase.so -l:libutils.so -l:libcutils.so -l:liblog.so -l:libc++.so -l:libbase.so \
  -l:vendor.oculus.hardware.graphics.composer@1.0.so -l:vendor.oculus.hardware.graphics.composer@1.1.so -l:vendor.oculus.hardware.sensors_java@1.0.so -l:vendor.oculus.hardware.sensors@1.0.so -l:vendor.oculus.hardware.devicecert@1.0.so -Wl,--allow-shlib-undefined
ls -la out/macvr-hal

# Scope the Quest identity override to Meta processes using LD_PRELOAD.
$N/bin/clang++ --target=aarch64-linux-android31 -std=c++17 -O2 -fPIC -shared -nostdinc++ -nostdlib++ $INC \
  -o out/libdeviceid_macvr.so ../compat/deviceid.cpp -Llib -l:libc++.so -Wl,--allow-shlib-undefined

# DSP startup/memory adapter; compute operations return ENOSYS.
$N/bin/clang --target=aarch64-linux-android31 -D_GNU_SOURCE -shared -fPIC -O2 ../compat/hexagon.c -llog -o out/libhexagon.so
