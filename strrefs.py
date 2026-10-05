"""List the string constants a function references (adrp+add pairs), for the function(s) that reference a given
string. Finds JSON keys a parser reads. usage: strrefs.py <lib.so> <objdump -d output> <string>"""
import re, struct, sys, bisect
lib, dis, needle = sys.argv[1], sys.argv[2], sys.argv[3].encode()
d = open(lib, 'rb').read()
def cstr(va):   # vaddr == file offset for the first segment of these libs
    if va >= len(d) or va < 0: return None
    e = d.find(b'\0', va)
    s = d[va:e]
    return s.decode('latin1') if 2 < len(s) < 300 and all(32 <= c < 127 for c in s) else None
target = d.find(needle)
lines = open(dis).read().split('\n')
# map instruction lines
pat_adrp = re.compile(r'^\s*([0-9a-f]+):\s+adrp\s+(x\d+), 0x([0-9a-f]+)')
pat_add = re.compile(r'^\s*([0-9a-f]+):\s+add\s+(x\d+), (x\d+), #0x([0-9a-f]+)')
refs = []   # (line index, string address)
pages = {}
for i, l in enumerate(lines):
    m = pat_adrp.match(l)
    if m: pages[m.group(2)] = (int(m.group(3), 16), i); continue
    m = pat_add.match(l)
    if m and m.group(3) in pages and i - pages[m.group(3)][1] < 12:
        refs.append((i, pages[m.group(3)][0] + int(m.group(4), 16)))
hits = [i for i, a in refs if a == target]
idx = [i for i, _ in refs]
addr_line = {}
pat_line = re.compile(r'^\s*([0-9a-f]+):')
for i, l in enumerate(lines):
    m = pat_line.match(l)
    if m: addr_line[int(m.group(1), 16)] = i
def bounds(h):
    s = h
    while s > 0 and not re.search(r'\bret$', lines[s - 1]) and h - s < 6000: s -= 1
    e = h
    while e < len(lines) - 1 and not re.search(r'\bret$', lines[e]) and e - h < 6000: e += 1
    return s, e
def collect(s, e, depth, seen, out):
    lo, hi = bisect.bisect_left(idx, s), bisect.bisect_right(idx, e)
    for i, a in refs[lo:hi]:
        t = cstr(a)
        if t and t not in out: out.append(t)
    if depth <= 0: return
    for i in range(s, e + 1):
        m = re.search(r'\s(?:bl|b)\s+0x([0-9a-f]+) <', lines[i])
        if m:
            a = int(m.group(1), 16)
            if a in addr_line and a not in seen:
                seen.add(a); cs, ce = bounds(addr_line[a] + 1); collect(addr_line[a], ce, depth - 1, seen, out)
DEPTH = int(sys.argv[4]) if len(sys.argv) > 4 else 2
for h in hits:
    s, e = bounds(h); out = []
    collect(s, e, DEPTH, set(), out)
    print(f'== ref at line {h}'); print(' | '.join(out))
raise SystemExit
for h in hits:
    # function bounds: nearest preceding/following 'ret' + prologue heuristics
    s = h
    while s > 0 and not re.search(r'\bret$', lines[s - 1]) and h - s < 6000: s -= 1
    e = h
    while e < len(lines) - 1 and not re.search(r'\bret$', lines[e]) and e - h < 6000: e += 1
    lo, hi = bisect.bisect_left(idx, s), bisect.bisect_right(idx, e + 400)
    strs = []
    for i, a in refs[lo:hi]:
        t = cstr(a)
        if t and t not in strs: strs.append(t)
    print(f'== ref at line {h} (function lines {s}-{e})'); print(' | '.join(strs))
