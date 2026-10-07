#!/bin/sh
# Boot the emulator from the already-built disk (fast path: no image rebuild).
# Rebuild first with boot.sh after changing hal/, egl/, vk/ or compat/; this
# script only boots what boot.sh last assembled. Log -> ~/MacVRFirmware/boot.log
set -eu
# no emulator window by default: drawing it on the Mac costs games ~20% fps (Ape Sprint 25 -> 30); WINDOW=1 shows it
[ "${WINDOW:-0}" = 1 ] && HEADLESS= || HEADLESS=1
# The host composes and keeps display buffers in Vulkan (no GL copy of every buffer the guest renders: Ape Sprint
# 30 -> 54 fps); the display then streams over gRPC (token-protected) since its shared-memory recorder needs GL.
MACVR_EMUARGS="-feature GuestUsesAngle,VulkanNativeSwapchain -grpc 8554 -grpc-use-token"
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FW=~/MacVRFirmware
[ -f "$FW/sysdir/system.img" ] || { echo "no sysdir/system.img: run boot.sh once first" >&2; exit 1; }
pids=$(pgrep -f 'qemu-system-aarch64.*-avd horizon( |$)' || true)
if [ -n "$pids" ]; then
    echo "stopping old emulator ($pids)"
    perl -e 'alarm 15; exec @ARGV' "$HOME/Library/Android/sdk/platform-tools/adb" -s "${EMUXR2_SERIAL:-emulator-5554}" shell sync >/dev/null 2>&1 || true
    # shellcheck disable=SC2086
    kill $pids 2>/dev/null || true
    sleep 3
    for pid in $pids; do kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true; done
    sleep 1
fi
export ANDROID_SDK_ROOT=~/Library/Android/sdk
# shellcheck disable=SC2086
nohup ~/Library/Android/sdk/emulator/emulator -avd horizon -sysdir "$FW/sysdir" -no-snapshot -no-boot-anim ${WIPE-} -show-kernel ${HEADLESS:+-no-window} -gpu ${GPU-host} -no-metrics -crash-report-mode never -logcat "*:I" -logcat-output "$FW/logcat.txt" $MACVR_EMUARGS ${EMUARGS:-} > "$FW/boot.log" 2>&1 &
echo "booting (HEADLESS=${HEADLESS:-0}); waiting for the guest"
exec "$H/wait-boot.sh" 20
