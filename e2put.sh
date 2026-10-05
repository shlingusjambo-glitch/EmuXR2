#!/bin/sh
# e2put.sh <ext4 img> <local file> <path in image> [selinux label]: replace or add one file (root:root 0644)
D=/opt/homebrew/opt/e2fsprogs/sbin/debugfs; img=$1 src=$2 dst=$3 lab=${4:-u:object_r:system_file:s0}
old=$($D -R "stat $dst" "$img" 2>/dev/null | sed -n 's/.*security.selinux (..) = "\([^"\\]*\).*/\1/p')
[ -n "$old" ] && lab=$old
printf '%s\0' "$lab" > /tmp/.e2put.lab
$D -w -f - "$img" >/dev/null 2>&1 <<CMD
rm $dst
write $src $dst
set_inode_field $dst mode 0100644
set_inode_field $dst uid 0
set_inode_field $dst gid 0
ea_set -f /tmp/.e2put.lab $dst security.selinux
CMD
sz=$($D -R "stat $dst" "$img" 2>/dev/null | sed -n 's/.*Size: \([0-9]*\).*/\1/p' | head -1)
[ "$sz" = "$(wc -c < "$src" | tr -d ' ')" ] || { echo "e2put: failed to write $dst into $img" >&2; exit 1; }
echo "$dst $sz"
