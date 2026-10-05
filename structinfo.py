"""Offsets of the hidl_string members of a HIDL struct (recursing into nested structs), from its generated
writeEmbeddedToParcel in the interface library. A zero-filled struct is then valid once empty hidl_strings are
constructed at those offsets. usage: structinfo.py <objdump -d -C output> <namespace::Struct>..."""
import re, sys
dis = open(sys.argv[1]).read().split('\n\n')
funcs = {}
for blk in dis:
    m = re.search(r'<[\w:]*writeEmbeddedToParcel\(([\w:]+) const&', blk)
    if m and m.group(1) not in funcs: funcs[m.group(1)] = blk
def strings(name, base=0, depth=0):
    blk = funcs.get(name)
    if not blk or depth > 8: return []
    out, off, src = [], 0, None
    lines = blk.splitlines()
    m = re.search(r'mov\s+(x\d+), x0\n', blk); src = m.group(1) if m else None
    for l in lines:
        a = re.search(r'add\s+x0, (x\d+), #0x([0-9a-f]+)', l)
        if a and a.group(1) == src: off = int(a.group(2), 16)
        if re.search(r'mov\s+x0, (x\d+)$', l.strip()) and re.search(r'mov\s+x0, ' + (src or 'zz') + r'$', l.strip()): off = 0
        b = re.search(r'\s(?:bl|b)\s+(?:0x)?[0-9a-f]+ <(.*)>', l)
        if b:
            t = b.group(1)
            if 'writeEmbeddedToParcel' in t and 'hidl_string' in t: out.append(base + off)
            else:
                n = re.search(r'writeEmbeddedToParcel\(([\w:]+) const&', t)
                if n and n.group(1) != name: out += strings(n.group(1), base + off, depth + 1)
            off = 0
    return out
for s in sys.argv[2:]:
    print(s, strings(s))
