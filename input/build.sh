#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
NDK=${ANDROID_NDK_HOME:-$HOME/Library/Android/sdk/ndk/27.2.12479018}
mkdir -p "$HERE/out"
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang" --target=aarch64-linux-android31 \
    -Wall -Wextra -O2 "$HERE/vmouse.c" -o "$HERE/out/vinput"
