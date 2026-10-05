#!/bin/sh
# builds the vendor Vulkan HAL wrapper (vulkan.macvr.so) with the NDK
set -e
cd "$(dirname "$0")"; FW=${1:-$HOME/MacVRFirmware}; mkdir -p out
N=~/Library/Android/sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/darwin-x86_64/bin
$N/clang --target=aarch64-linux-android31 -fPIC -shared -O2 -Wall -Wno-unused-function -I$FW/aosp/hal_inc -I$FW/aosp/vndk/system/core/libcutils/include -I$FW/aosp/vndk/system/core/libsystem/include \
  -o out/vulkan.macvr.so -Wl,-soname,vulkan.macvr.so vk_macvr.c -llog -ldl -lnativewindow
ls -la out/vulkan.macvr.so
