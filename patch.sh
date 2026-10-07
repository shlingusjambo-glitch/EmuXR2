#!/bin/sh
# Build the patched partition images from the user's own firmware (img/) and the emulator image.
# Runs locally; nothing here ships Meta files. usage: patch.sh <work dir with img/ fs-list> <emulator system.img>
set -e
W=$1 S=$2 H=$(cd "$(dirname "$0")" && pwd)
E2=/opt/homebrew/opt/e2fsprogs/sbin D=$E2/debugfs
cd "$W"; mkdir -p work emu aosp
unshare() {   # $1 image, $2 size (MiB; at least the original's plus 32), $3 original: grow, drop shared_blocks so files
    # can be rewritten safely, then restore any file e2fsck mangled (a file that repeats one block thousands of times)
    # from the original
    m=$(( $(stat -f %z "$3") / 1048576 + 32 )); [ "$m" -gt "$2" ] || m=$2
    truncate -s "${m}M" "$1"; $E2/resize2fs "$1" >/dev/null 2>&1
    for i in 1 2 3; do $E2/e2fsck -fy -E unshare_blocks "$1" >/dev/null 2>&1 && break; done
    l="$1.damaged"; [ -f "$l" ] || python3 "$H/fsdiff.py" "$3" "$1" > "$l"
    while read -r f; do $D -R "dump $f work/.r" "$3" >/dev/null 2>&1; put "$1" work/.r "$f"; done < "$l"
}
put() { "$H/e2put.sh" "$@" >/dev/null; }
x() { $D -R "dump $2 $3" "$1" >/dev/null 2>&1; }
# Quest partitions (the user's)
rm -f work/*.img; for p in system system_ext product; do cp -c img/$p.img work/$p.img; done
unshare work/system.img 2100 img/system.img
unshare work/system_ext.img 120 img/system_ext.img
# emulator AOSP partitions (Google's, used as the hardware layer)
rm -f emu/emu_vendor.img emu/emu_system.img
dd if="$S" of=emu/emu_vendor.img bs=512 skip=$((4096+5607424)) count=281280 2>/dev/null
cp -c emu/emu_vendor.img emu/emu_vendor.orig; unshare emu/emu_vendor.img 600 emu/emu_vendor.orig
# system: debuggable, adb without auth
x work/system.img /system/build.prop work/build.prop
sed -i '' -e 's/^ro.adb.secure=1/ro.adb.secure=0/' -e 's/^ro.debuggable=0/ro.debuggable=1/' -e 's/^ro.secure=1/ro.secure=0/' work/build.prop
put work/system.img work/build.prop /system/build.prop
# the Quest's device compatibility matrix requires its Qualcomm display HALs; the emulator's vendor has none of them,
# and an unmet matrix makes Android show a focus-stealing "internal problem with your device" dialog
x work/system.img /system/etc/vintf/compatibility_matrix.device.xml work/cm.device.xml
sed -i '' 's/optional="false"/optional="true"/' work/cm.device.xml
put work/system.img work/cm.device.xml /system/etc/vintf/compatibility_matrix.device.xml
# SELinux: every domain permissive (Quest platform policy + emulator vendor policy don't line up)
x work/system.img /system/etc/selinux/plat_sepolicy.cil work/plat_sepolicy.cil
x emu/emu_vendor.img /etc/selinux/vendor_sepolicy.cil work/vendor_sepolicy.cil
x work/system_ext.img /etc/selinux/system_ext_sepolicy.cil work/system_ext_sepolicy.cil
python3 "$H/permissive.py" work/plat_sepolicy.cil work/vendor_sepolicy.cil work/system_ext_sepolicy.cil >/dev/null
put work/system_ext.img work/system_ext_sepolicy.cil /etc/selinux/system_ext_sepolicy.cil
put work/system.img work/plat_sepolicy.cil /system/etc/selinux/plat_sepolicy.cil
put emu/emu_vendor.img work/vendor_sepolicy.cil /etc/selinux/vendor_sepolicy.cil
# vendor props: flattened APEX like the Quest, its zygote layout, no hardware CAC writeback
x emu/emu_vendor.img /build.prop work/vbuild.prop
sed -i '' -e 's/^ro.apex.updatable=true/ro.apex.updatable=false/' -e 's/^ro.zygote=zygote64$/ro.zygote=zygote64_stub32/' work/vbuild.prop
cat "$H/vendor.prop" >> work/vbuild.prop
# the system's fingerprint, or Android warns "There's an internal problem with your device" and that dialog keeps focus
x work/system.img /system/build.prop work/sbuild.prop
FP=$(sed -n 's/^ro.system.build.fingerprint=//p' work/sbuild.prop)
sed -i '' "s|^ro.vendor.build.fingerprint=.*|ro.vendor.build.fingerprint=$FP|" work/vbuild.prop
put emu/emu_vendor.img work/vbuild.prop /build.prop
python3 "$H/bridge/flat_mesh.py" work/emuxr2-mesh.bin
put emu/emu_vendor.img work/emuxr2-mesh.bin /etc/emuxr2-mesh.bin u:object_r:vendor_configs_file:s0
# AOSP binaries taken from the emulator's own system partition (extracted once, cached in aosp/)
AOSP="bin/mediaserver bin/drmserver lib64/libmediaplayerservice.so lib64/libresourcemanagerservice.so lib64/libstagefright_httplive.so"
for f in $AOSP; do [ -f aosp/$(basename $f) ] || NEED=1; done
if [ -n "$NEED" ]; then
    dd if="$S" of=emu/emu_system.img bs=512 skip=$((4096+2048)) count=1723432 2>/dev/null
    for f in $AOSP; do x emu/emu_system.img /system/$f aosp/$(basename $f); done; rm -f emu/emu_system.img
fi
# Apple silicon can't run 32-bit ARM: swap the Quest's 32-bit media daemons for AOSP's 64-bit ones
for b in mediaserver drmserver; do put work/system.img aosp/$b /system/bin/$b; $D -w -R "set_inode_field /system/bin/$b mode 0100755" work/system.img >/dev/null 2>&1; done
for l in libmediaplayerservice.so libresourcemanagerservice.so libstagefright_httplive.so; do put work/system.img aosp/$l /system/lib64/$l u:object_r:system_lib_file:s0; done
[ -x "$H/patch-extra.sh" ] && . "$H/patch-extra.sh"
$E2/e2fsck -fn work/system.img >/dev/null 2>&1 && $E2/e2fsck -fn emu/emu_vendor.img >/dev/null 2>&1 && echo patched
