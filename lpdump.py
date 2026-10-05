import struct, sys
def read_super(f, base):
    f.seek(base + 4096); g = f.read(4096)
    magic, ssz = struct.unpack('<II', g[:8]); assert magic == 0x616c4467, hex(magic)
    mmax, slots, lbs = struct.unpack('<III', g[40:52])
    f.seek(base + 4096 * 3); h = f.read(256)
    m, maj, mnr, hsz = struct.unpack('<IHHI', h[:12]); assert m == 0x414C5030
    tsz, = struct.unpack('<I', h[44:48])
    descs = [struct.unpack('<III', h[80 + 12 * i: 92 + 12 * i]) for i in range(4)]
    f.seek(base + 4096 * 3 + hsz); t = f.read(tsz)
    tab = lambda d: [t[d[0] + i * d[2]: d[0] + (i + 1) * d[2]] for i in range(d[1])]
    parts = [(p[:36].rstrip(b'\0').decode(), *struct.unpack('<IIII', p[36:52])) for p in tab(descs[0])]
    exts = [struct.unpack('<QIQI', e[:24]) for e in tab(descs[1])]
    groups = [(g[:36].rstrip(b'\0').decode(), *struct.unpack('<IQ', g[36:48])) for g in tab(descs[2])]
    bdevs = [struct.unpack('<QIIQ', b[:24]) + (b[24:60].rstrip(b'\0').decode(),) for b in tab(descs[3])]
    return dict(mmax=mmax, slots=slots, lbs=lbs, ver=(maj, mnr), hsz=hsz, parts=parts, exts=exts, groups=groups, bdevs=bdevs)
if __name__ == '__main__':
    print(read_super(open(sys.argv[1], 'rb'), int(sys.argv[2]) if len(sys.argv) > 2 else 0))
