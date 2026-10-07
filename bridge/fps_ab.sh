#!/bin/sh
# fps_ab.sh "<emulator args>" : cold boot with those emulator args and benchmark (fps_bench.py)
cd "$(dirname "$0")/.."; A="$HOME/Library/Android/sdk/platform-tools/adb -s emulator-5554"
EMUARGS="$1" ./boot.sh >/dev/null; sleep 20
for i in $(seq 60); do [ "$($A shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break; sleep 5; done
sleep 40; cd bridge && echo "args: $1" && .venv/bin/python fps_bench.py 40 2>&1 | tail -1
