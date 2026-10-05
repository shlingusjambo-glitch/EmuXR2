#!/bin/sh
# summarize the last boot's logcat: our shims' messages, crash reasons, crashing processes
L=${1:-$HOME/MacVRFirmware/logcat.txt}
grep -E "MacVR-(VK|EGL|GLES)|macvr-hal" "$L" | sed -E 's/^[0-9-]+ [0-9:.]+ +[0-9]+ +[0-9]+ //' | grep -vE "device 0x|external image|export 0x|bind 0x|import 0x" | sort | uniq -c | sort -rn | head -${N:-12} | cut -c1-200
echo "-- aborts"; grep -E "Abort message" "$L" | sed 's/.*Abort message//' | sort | uniq -c | sort -rn | head -${N:-12} | cut -c1-200
echo "-- crashed"; grep -E "Cmdline" "$L" | sed 's/.*Cmdline//' | sort | uniq -c | sort -rn | head -${N:-12}
