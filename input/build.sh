#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
NDK=${ANDROID_NDK_HOME:-$HOME/Library/Android/sdk/ndk/27.2.12479018}
mkdir -p "$HERE/out"
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang" --target=aarch64-linux-android31 \
    -Wall -Wextra -O2 "$HERE/vmouse.c" -o "$HERE/out/vinput"
# In-guest Java helpers, run with app_process as root (javac + d8: the NDK's binder stub lacks AServiceManager):
# Injector (head poses into TrackingDataInjection) and Audio (the guest's sound).
# Hidden framework classes are compiled against stubs/ and resolved from the boot classpath at run time.
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@21}   # d8 needs it too
JDK=$JAVA_HOME
ANDROID_JAR=$(ls "$HOME/Library/Android/sdk/platforms/android-"*/android.jar | sort -V | tail -n 1)
D8=$(ls -d "$HOME/Library/Android/sdk/build-tools/"*/d8 | sort -V | tail -n 1)
for c in Injector Audio; do
    rm -rf "$HERE/out/$c" && mkdir -p "$HERE/out/$c"
    set -- "$HERE/$c.java"
    if [ "$c" = Injector ]; then set -- "$@" "$HERE/LatestPacketInput.java" "$HERE/HomeButton.java"; fi
    "$JDK/bin/javac" -source 8 -target 8 -nowarn -cp "$ANDROID_JAR" -sourcepath "$HERE/stub" -d "$HERE/out/$c" "$@"
    set -- "$HERE/out/$c/$c"*.class
    if [ "$c" = Injector ]; then set -- "$@" "$HERE/out/$c/LatestPacketInput.class" "$HERE/out/$c/HomeButton.class"; fi
    "$D8" --min-api 31 --lib "$ANDROID_JAR" --output "$HERE/out/$c" "$@"
    mv "$HERE/out/$c/classes.dex" "$HERE/out/$(echo $c | tr A-Z a-z).dex"
done
