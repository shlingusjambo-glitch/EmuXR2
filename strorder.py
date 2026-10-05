"""Print, in code order, the string constants referenced inside the function that references <string>
(shows the order a parser looks keys up in). usage: strorder.py <lib> <objdump> <string>"""
import re, sys
lib, dis, needle = sys.argv[1], sys.argv[2], sys.argv[3].encode()
d = open(lib, 'rb').read(); target = d.find(needle)
def cstr(va):
    e = d.find(b'\0', va); s = d[va:e]
    return s.decode('latin1') if 1 < len(s) < 200 and all(32 <= c < 127 for c in s) else None
lines = open(dis).read().split('\n')
pages, refs = {}, []
for i, l in enumerate(lines):
    m = re.match(r'^\s*([0-9a-f]+):\s+adrp\s+(x\d+), 0x([0-9a-f]+)', l)
    if m: pages[m.group(2)] = (int(m.group(3), 16), i); continue
    m = re.match(r'^\s*([0-9a-f]+):\s+add\s+(x\d+), (x\d+), #0x([0-9a-f]+)', l)
    if m and m.group(3) in pages and i - pages[m.group(3)][1] < 12: refs.append((i, pages[m.group(3)][0] + int(m.group(4), 16)))
for h in [i for i, a in refs if a == target][:1]:
    s = h
    while s > 0 and not re.search(r'\bret$', lines[s - 1]) and h - s < 8000: s -= 1
    e = h
    while e < len(lines) - 1 and not (re.search(r'\bret$', lines[e]) and e - h > 50) and e - h < 8000: e += 1
    print(' > '.join(t for i, a in refs if s <= i <= e for t in [cstr(a)] if t))
