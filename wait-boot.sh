#!/bin/sh
# wait until the emulator reports boot completed (adb calls time out instead of hanging), then $1 more seconds
A=~/Library/Android/sdk/platform-tools/adb
t() { perl -e 'alarm shift; exec @ARGV' "$@"; }
for i in $(seq 1 200); do [ "$(t 8 $A shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break; sleep 3; done
t 8 $A logcat -G 32M >/dev/null 2>&1; sleep ${1:-0}; echo "boot_completed=$(t 8 $A shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')"
