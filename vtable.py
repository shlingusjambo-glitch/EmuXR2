"""Print a C++ class's virtual methods in vtable order, from a shared library's vtable relocations.
usage: vtable.py <lib.so> <mangled vtable symbol substring>"""
import re, subprocess, sys
B = '/Users/elywright/Library/Android/sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/darwin-x86_64/bin/'
lib, key = sys.argv[1], sys.argv[2]
syms = {}
for line in subprocess.run([B + 'llvm-nm', '-D', '--defined-only', '-S', lib], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4: syms.setdefault(int(p[0], 16), []).append((int(p[1], 16), p[3]))
vt = [(a, s, n) for a, l in syms.items() for s, n in l if n.startswith('_ZTV') and key in n]
dem = lambda n: subprocess.run([B + 'llvm-cxxfilt'], input=n, capture_output=True, text=True).stdout.strip()
rel = {}
for line in subprocess.run([B + 'llvm-readelf', '-r', '-W', lib], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'\s*([0-9a-f]+)\s+[0-9a-f]+\s+(R_AARCH64_\w+)\s+(?:([0-9a-f]+)\s+(\S+)\s+\+\s+([0-9a-f]+)|([0-9a-f]+))?', line)
    if not m: continue
    off = int(m.group(1), 16)
    if m.group(2) == 'R_AARCH64_RELATIVE': rel[off] = ('addr', int(m.group(6) or '0', 16))
    elif m.group(4): rel[off] = ('sym', m.group(4))
for a, s, n in vt:
    print('#', dem(n), hex(a), s)
    for i, off in enumerate(range(a + 16, a + s, 8)):
        k, v = rel.get(off, (None, None))
        name = v if k == 'sym' else next((nm for _, nm in syms.get(v, [])), hex(v) if v else '?')
        print(i, dem(name))
