"""List the 64-bit libs a binary needs (recursively) that the Quest system lacks; they come from the emulator's AOSP system.
usage: missing_libs.py <quest system file list> <emulator ext4 image> <binary path in emulator image>..."""
import os, re, subprocess, sys, tempfile
D = '/opt/homebrew/opt/e2fsprogs/sbin/debugfs'
RE = os.path.expanduser('~/Library/Android/sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf')
have = {os.path.basename(l.strip()) for l in open(sys.argv[1]) if '/lib64/' in l}
img, tmp = sys.argv[2], tempfile.mkdtemp()
def needed(path):
    out = os.path.join(tmp, os.path.basename(path))
    subprocess.run([D, '-R', f'dump {path} {out}', img], capture_output=True)
    if not os.path.exists(out): return None
    return re.findall(r'\(NEEDED\)\s+Shared library: \[(\S+)\]', subprocess.run([RE, '-d', out], capture_output=True, text=True).stdout)
todo, missing = list(sys.argv[3:]), []
while todo:
    for n in needed(todo.pop()) or []:
        if n not in have and n not in missing:
            missing.append(n); todo.append('/system/lib64/' + n)
for m in missing: print(m)
