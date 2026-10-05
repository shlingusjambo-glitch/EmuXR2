#!/bin/sh
# rebuild the emulator disk from work/ images and boot it headless; log -> boot.log
set -e
H=$(cd "$(dirname "$0")" && pwd); cd ~/MacVRFirmware; S=~/Library/Android/sdk/system-images/android-32/google_apis/arm64-v8a
pkill -f "qemu-system-aarch64.*horizon" 2>/dev/null || true; sleep 1
rm -f sysdir/system.img
python3 "$H/build_super.py" $S/system.img sysdir/system.img system=work/system.img system_ext=work/system_ext.img product=work/product.img vendor=emu/emu_vendor.img >/dev/null
python3 - <<'PY'
import hashlib, os, struct, re
p = os.path.expanduser('~/MacVRFirmware/sysdir/')
f = open(p + 'system.img', 'r+b'); f.seek(2048 * 512); v = bytearray(f.read(6208)); struct.pack_into('>I', v, 120, 3)
f.seek(2048 * 512); f.write(v); f.close(); t = open(p + 'VerifiedBootParams.textproto').read()
open(p + 'VerifiedBootParams.textproto', 'w').write(re.sub(r'digest=[0-9a-f]+', 'digest=' + hashlib.sha256(v).hexdigest(), t))
PY
export ANDROID_SDK_ROOT=~/Library/Android/sdk
nohup ~/Library/Android/sdk/emulator/emulator -avd horizon -sysdir ~/MacVRFirmware/sysdir -no-snapshot -no-boot-anim ${WIPE--wipe-data} -show-kernel ${WINDOW--no-window} -gpu host -logcat "*:I" -logcat-output ~/MacVRFirmware/logcat.txt $EMUARGS > boot.log 2>&1 &
echo booting
