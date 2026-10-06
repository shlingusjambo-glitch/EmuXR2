#!/bin/sh
# Start the Quest streamer in the foreground (Ctrl-C stops it): pushes the
# guest helpers, streams the compositor to the Quest over adb reverse, and
# injects the headset + controller tracking back into the guest.
# Env: EMUXR2_CONTROLLERS=0 to skip controller injection (default: on),
#      EMUXR2_SERIAL to pick a non-default emulator, ANDROID_SERIAL for adb.
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
A=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}/platform-tools/adb
PY=${EMUXR2_PYTHON:-$H/.venv/bin/python3}
[ -x "$PY" ] || PY=python3
command -v "$PY" >/dev/null || { echo "python3 not on PATH" >&2; exit 1; }
"$PY" -c "import av, numpy" 2>/dev/null || { echo "Missing streamer dependencies. Run: python3 -m venv .venv && .venv/bin/python3 -m pip install av numpy" >&2; exit 1; }
echo "waiting for the emulator"
for i in $(seq 1 60); do
    "$A" -s "${EMUXR2_SERIAL:-emulator-5554}" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' | grep -q '^1$' && break
    sleep 2
done
"$A" -s "${EMUXR2_SERIAL:-emulator-5554}" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' | grep -q '^1$' || { echo "emulator not up: run start-emulator.sh first" >&2; exit 1; }
Q=$("$A" devices | awk '$2=="device" && $1 !~ /emulator/ {print $1; exit}')
[ -n "${Q:-}" ] && echo "quest: $Q" || echo "no quest yet: streamer waits for it on :9945"
cd "$H/bridge"
exec "$PY" stream.py
