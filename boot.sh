#!/bin/sh
# rebuild the emulator disk from work/ images and boot with a live window; log -> boot.log
set -e
# no emulator window by default: drawing it on the Mac costs games ~20% fps (Ape Sprint 25 -> 30); WINDOW=1 shows it
[ "${WINDOW:-0}" = 1 ] && HEADLESS= || HEADLESS=1
# The host composes and keeps display buffers in Vulkan (no GL copy of every buffer the guest renders: Ape Sprint
# 30 -> 54 fps); the display then streams over gRPC (token-protected) since its shared-memory recorder needs GL.
MACVR_EMUARGS="-feature GuestUsesAngle,VulkanNativeSwapchain -grpc 8554 -grpc-use-token"
H=$(cd "$(dirname "$0")" && pwd); cd ~/MacVRFirmware; S=~/Library/Android/sdk/system-images/android-32/google_apis/arm64-v8a
# Wait for the old VM to release its disk and AVD locks before rebuilding.
pids=$(pgrep -f 'qemu-system-aarch64.*-avd horizon( |$)' || true)
if [ -n "$pids" ]; then
    # Flush guest filesystem writes before terminating QEMU. Otherwise recent settings
    # and directory changes can disappear even though their adb commands succeeded.
    perl -e 'alarm 15; exec @ARGV' "$HOME/Library/Android/sdk/platform-tools/adb" -s "${EMUXR2_SERIAL:-emulator-5554}" shell sync >/dev/null 2>&1 || true
    kill $pids 2>/dev/null || true
    for attempt in 1 2 3 4 5 6 7 8 9 10; do
        alive=0
        for pid in $pids; do kill -0 "$pid" 2>/dev/null && alive=1; done
        [ "$alive" = 0 ] && break
        sleep 1
    done
    for pid in $pids; do kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true; done
    sleep 1
fi
rm -f sysdir/system.img
# the framebuffer the streamer reads: a stale one (a killed emulator never removes it) can't be resized for a new panel
python3 -c "import ctypes; ctypes.CDLL(None).shm_unlink(b'videmulator5554')"
python3 "$H/build_super.py" $S/system.img sysdir/system.img system=work/system.img system_ext=work/system_ext.img product=work/product.img vendor=emu/emu_vendor.img >/dev/null
python3 - <<'PY'
import hashlib, os, struct, re
p = os.path.expanduser('~/MacVRFirmware/sysdir/')
f = open(p + 'system.img', 'r+b'); f.seek(2048 * 512); v = bytearray(f.read(6208)); struct.pack_into('>I', v, 120, 3)
f.seek(2048 * 512); f.write(v); f.close(); t = open(p + 'VerifiedBootParams.textproto').read()
open(p + 'VerifiedBootParams.textproto', 'w').write(re.sub(r'digest=[0-9a-f]+', 'digest=' + hashlib.sha256(v).hexdigest(), t))
PY
export ANDROID_SDK_ROOT=~/Library/Android/sdk
# AVD tuning: the address-space graphics transport (a ring buffer; the default pipe makes every Vulkan call a
# blocking round trip), a 72 Hz display like the Quest's, 6 vCPUs (Meta's runtime expects big cores 4+), and a
# 2560x1600 panel: the stream's capture is drawn on it, 1280x1600 per eye (the shape of Horizon's eye images)
AVD=~/.android/avd/horizon.avd/config.ini
for kv in "hw.gltransport=asg" "hw.lcd.vsync=72" "hw.cpu.ncore=6" "hw.lcd.width=1600" "hw.lcd.height=2560"; do
    k=${kv%%=*}; v=${kv#*=}
    if grep -q "^$k" "$AVD"; then sed -i '' "s/^$k.*/$k = $v/" "$AVD"; else echo "$k = $v" >> "$AVD"; fi
done
if [ "${FOREGROUND:-0}" = 1 ]; then
    exec ~/Library/Android/sdk/emulator/emulator -avd horizon -sysdir ~/MacVRFirmware/sysdir -no-snapshot -no-boot-anim ${WIPE-} ${HEADLESS:+-no-window} -gpu ${GPU-host} -no-metrics -crash-report-mode never -logcat "*:W" -logcat-output ~/MacVRFirmware/logcat.txt $MACVR_EMUARGS $EMUARGS
fi
nohup ~/Library/Android/sdk/emulator/emulator -avd horizon -sysdir ~/MacVRFirmware/sysdir -no-snapshot -no-boot-anim ${WIPE-} -show-kernel ${HEADLESS:+-no-window} -gpu ${GPU-host} -no-metrics -crash-report-mode never -logcat "*:W" -logcat-output ~/MacVRFirmware/logcat.txt $MACVR_EMUARGS $EMUARGS > boot.log 2>&1 &
echo booting
