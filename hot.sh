#!/bin/sh
# dev loop: bind-mount a freshly built library over its installed path on the running emulator (lost on reboot)
# usage: hot.sh <local file> <device path>
A=~/Library/Android/sdk/platform-tools/adb; n=$(date +%s)
$A root >/dev/null 2>&1; $A wait-for-device
$A shell mkdir -p /data/local/tmp/hot
$A push "$1" /data/local/tmp/hot/$n.so >/dev/null && $A shell "chmod 755 /data/local/tmp/hot/$n.so; while umount -l $2 2>/dev/null; do :; done; mount -o bind /data/local/tmp/hot/$n.so $2" && echo "hot: $2"
