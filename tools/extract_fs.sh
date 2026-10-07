#!/bin/sh
# Copies the firmware files the build reads into <work dir>/fs: Meta's HIDL interface libraries, gathered into
# fs/vendor/lib64 (where the vendor HAL stand-ins load them) from wherever this version keeps them (v54: vendor;
# later versions: system and system_ext), and the camera mux configuration. usage: extract_fs.sh <work dir with img/>
set -e
W=$1 D=/opt/homebrew/opt/e2fsprogs/sbin/debugfs
mkdir -p "$W/fs/vendor/lib64" "$W/fs/vendor/etc/cameramuxmode"
for src in system:/system/lib64 system_ext:/lib64 vendor:/lib64; do   # vendor last: its copy wins
    img=$W/img/${src%%:*}.img dir=${src#*:}
    for l in $($D -R "ls -p $dir" "$img" 2>/dev/null | awk -F/ '$6 ~ /^vendor\.oculus\..*@[0-9.]+\.so$/ {print $6}'); do
        $D -R "dump $dir/$l $W/fs/vendor/lib64/$l" "$img" >/dev/null 2>&1
    done
done
$D -R "dump /etc/cameramuxmode/configv2.json $W/fs/vendor/etc/cameramuxmode/configv2.json" "$W/img/vendor.img" >/dev/null 2>&1
ls "$W/fs/vendor/lib64"
