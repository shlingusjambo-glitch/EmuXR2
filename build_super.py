"""Rebuild the emulator's GPT disk (vbmeta + super) with the user's own Quest partitions.
usage: build_super.py <emulator system.img> <out disk> name=img ... (images not named keep the emulator's)"""
import hashlib, os, shutil, struct, subprocess, sys
from lpdump import read_super
SECT, BASE = 512, 4096 * 512   # super partition starts at GPT sector 4096 in emulator images

def sparsify(f):
    """Deallocate only complete zero ranges in our generated APFS disk, preserving all logical bytes."""
    if sys.platform != 'darwin': return
    import fcntl
    position = f.tell()
    block = 1 << 20
    zeros = bytes(block)
    f.flush(); f.seek(0)
    offset = 0
    try:
        while data := f.read(block):
            if data == zeros:
                # Darwin F_PUNCHHOLE, fpunchhole_t: flags, reserved, offset, length.
                fcntl.fcntl(f.fileno(), 99, struct.pack('IIqq', 0, 0, offset, block))
            offset += len(data)
    finally:
        f.seek(position)

def main(src, out, repl):
    subprocess.run(['cp', '-c', src, out], check=True)   # APFS clone: only rewritten blocks take space
    f = open(out, 'r+b'); m = read_super(f, BASE)
    old = {p[0]: m['exts'][p[2]] for p in m['parts']}
    pos, exts = 2048, []
    for name, *_ in m['parts']:
        n = os.path.getsize(repl[name]) // SECT if name in repl else old[name][0]
        exts.append((name, n, pos)); pos += (n + 2047) // 2048 * 2048
    assert pos * SECT <= m['bdevs'][0][3], 'super too small'
    # copy the kept partitions first (their old extents may overlap the new layout): buffer them
    keep = {}
    for name, n, _ in exts:
        if name not in repl:
            f.seek(BASE + old[name][2] * SECT); keep[name] = f.read(n * SECT)
    for name, n, start in exts:
        f.seek(BASE + start * SECT)
        if name in keep: f.write(keep[name])
        else:
            with open(repl[name], 'rb') as g: shutil.copyfileobj(g, f, 1 << 24)
    # metadata: same header and tables, only extent sizes/positions change
    f.seek(BASE + 4096 * 3); hdr = bytearray(f.read(m['hsz']))
    tsz, = struct.unpack('<I', hdr[44:48]); f.seek(BASE + 4096 * 3 + m['hsz']); tab = bytearray(f.read(tsz))
    eoff, _, esz = struct.unpack('<III', hdr[92:104])
    for name, pi, *_ in [(p[0], i) for i, p in enumerate(m['parts'])]:
        idx = m['parts'][pi][2]; n, start = next((e[1], e[2]) for e in exts if e[0] == name)
        struct.pack_into('<QIQI', tab, eoff + idx * esz, n, 0, start, 0)
    hdr[48:80] = hashlib.sha256(tab).digest()
    hdr[12:44] = b'\0' * 32; hdr[12:44] = hashlib.sha256(hdr).digest()
    for s in range(m['slots'] * 2):   # primary slots then backups
        f.seek(BASE + 4096 * 3 + s * m['mmax']); f.write(hdr); f.write(tab)
    sparsify(f)
    f.close()
    for name, n, start in exts: print(f'{name}: {n * SECT >> 20} MB at sector {start}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], dict(a.split('=', 1) for a in sys.argv[3:]))
