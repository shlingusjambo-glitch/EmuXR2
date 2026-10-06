#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
SDK=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}
NDK=${ANDROID_NDK_HOME:-$SDK/ndk/27.2.12479018}
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home}
mkdir -p "$HERE/out/pointer-classes" "$HERE/out/pointer-dex"
"$JAVA_HOME/bin/javac" --release 8 -cp "$SDK/platforms/android-34/android.jar" -d "$HERE/out/pointer-classes" "$HERE/AbsolutePointer.java"
"$SDK/build-tools/34.0.0/d8" --min-api 31 --output "$HERE/out/pointer-dex" "$HERE/out/pointer-classes/AbsolutePointer.class"
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang" --target=aarch64-linux-android31 -Wall -Wextra -O2 "$HERE/system-input.c" -o "$HERE/out/system-input"
