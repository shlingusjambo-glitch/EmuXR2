#!/bin/sh
# builds /vendor/bin/hw/macvr-hal against the platform libraries of the user's firmware (needs the NDK and the
# AOSP 12L headers in $FW/aosp). usage: build.sh <firmware work dir>
set -e
FW=$1; cd "$(dirname "$0")"; mkdir -p out lib
N=~/Library/Android/sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/darwin-x86_64
V=$FW/aosp/vndk D=/opt/homebrew/opt/e2fsprogs/sbin/debugfs
for l in libhidlbase libutils libcutils libc++ libbase libfmq; do [ -f lib/$l.so ] || $D -R "dump /system/apex/com.android.vndk.current/lib64/$l.so lib/$l.so" $FW/work/system.img >/dev/null 2>&1; done
for l in liblog; do [ -f lib/$l.so ] || $D -R "dump /system/lib64/$l.so lib/$l.so" $FW/work/system.img >/dev/null 2>&1; done
rm -f lib/vendor.oculus.*.so; for l in $(cd $FW/fs/vendor/lib64 && ls vendor.oculus.*.so); do cp -f $FW/fs/vendor/lib64/$l lib/; done
# Meta revises some interfaces in place (same version number, new method layout): pick the layout this firmware's library
# has (see oculus_sensors.h, oculus_composer.h), and the device certificate stand-in only while the firmware has that HAL
has() { $N/bin/llvm-nm -D --defined-only "lib/$1" 2>/dev/null | grep -q "$2"; }
DEFS="-DMACVR_SENSORS_REV=$(has vendor.oculus.hardware.sensors@1.0.so applyMuxModeByName && echo 2 || echo 1)"
DEFS="$DEFS -DMACVR_COMPOSER_REV=$(has vendor.oculus.hardware.graphics.composer@1.1.so setBroMatrix && echo 2 || echo 1)"
SRC=$(ls *.cpp | grep -v devicecert) CERT=
if [ -f lib/vendor.oculus.hardware.devicecert@1.0.so ]; then DEFS="$DEFS -DMACVR_DEVICECERT=1" SRC="$SRC devicecert.cpp" CERT=-l:vendor.oculus.hardware.devicecert@1.0.so; fi
echo "macvr-hal: $DEFS"
RES=$($N/bin/clang -print-resource-dir)
INC="-nostdinc -isystem $V/external/libcxx/include -isystem $RES/include -isystem $FW/aosp/bionic_inc -isystem $V/bionic/libc/kernel/uapi -isystem $V/bionic/libc/kernel/uapi/asm-arm64 -isystem $V/bionic/libc/kernel/android/uapi
 -I$V/system/libfmq/base -I$V/external/libcxxabi/include -I$V/system/libhidl/base/include -I$V/system/libhidl/transport/include
 -I$V/system/core/libutils/include -I$V/system/core/libcutils/include -I$V/system/logging/liblog/include_vndk -I$V/system/libbase/include
 -I$V/system/libhwbinder/include -I$V/system/core/libsystem/include -I$V/system/libfmq/include -I$V/generated-headers/system/libhidl/transport/base/1.0/android.hidl.base@1.0_genc++_headers/gen
 -I$V/generated-headers/system/libhidl/transport/manager/1.0/android.hidl.manager@1.0_genc++_headers/gen"
$N/bin/clang++ --target=aarch64-linux-android31 -std=c++17 -O2 -fno-rtti -fPIE -pie -nostdinc++ -nostdlib++ -D__ANDROID_VNDK__ -Wno-unused-parameter $DEFS $INC \
  -o out/macvr-hal $SRC -Llib -l:libhidlbase.so -l:libutils.so -l:libcutils.so -l:liblog.so -l:libc++.so -l:libbase.so -l:libfmq.so \
  -l:vendor.oculus.hardware.graphics.composer@1.0.so -l:vendor.oculus.hardware.graphics.composer@1.1.so -l:vendor.oculus.hardware.sensors_java@1.0.so -l:vendor.oculus.hardware.sensors@1.0.so $CERT -Wl,--allow-shlib-undefined
ls -la out/macvr-hal

# Scope the Quest identity override to Meta processes using LD_PRELOAD.
$N/bin/clang++ --target=aarch64-linux-android31 -std=c++17 -O2 -fPIC -shared -nostdinc++ -nostdlib++ $INC \
  -o out/libdeviceid_macvr.so ../compat/deviceid.cpp -Llib -l:libc++.so -Wl,--allow-shlib-undefined

# DSP startup/memory adapter; compute operations return ENOSYS.
$N/bin/clang --target=aarch64-linux-android31 -D_GNU_SOURCE -shared -fPIC -O2 ../compat/hexagon.c -llog -o out/libhexagon.so

