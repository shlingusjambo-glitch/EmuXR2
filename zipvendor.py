"""Stream one logical partition out of an emulator system-image zip without unpacking the whole disk.
usage: zipvendor.py <image zip> <partition> <out>"""
import io, os, sys, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lpdump import read_super
BASE = 4096 * 512   # super starts at GPT sector 4096 in emulator images
z = zipfile.ZipFile(sys.argv[1])
name = next(n for n in z.namelist() if n.endswith('/system.img'))
f = z.open(name)
head = f.read(BASE + 4096 * 8)
m = read_super(io.BytesIO(head), BASE)
part = next(p for p in m['parts'] if p[0] == sys.argv[2])
exts = m['exts'][part[2]:part[2] + part[3]]
pos = len(head)
with open(sys.argv[3], 'wb') as out:
    for nsec, kind, start, _ in exts:   # linear extents, in order
        off = BASE + start * 512; assert off >= pos, "extents out of order"
        while pos < off: pos += len(f.read(min(1 << 20, off - pos)))
        left = nsec * 512
        while left: b = f.read(min(1 << 20, left)); out.write(b); left -= len(b); pos += len(b)
print(sys.argv[2], os.path.getsize(sys.argv[3]))
