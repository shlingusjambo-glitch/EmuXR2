"""Append (typepermissive X) for every domain type a CIL file declares itself (local emulator only)."""
import re, sys
for fn in sys.argv[1:]:
    s = open(fn).read()
    if '; macvr permissive' in s: continue
    types = set(re.findall(r'^\(type (\S+)\)', s, re.M))
    dom = set(re.search(r'\(typeattributeset domain \(([^)]*)\)', s).group(1).split())
    add = sorted(types & dom)
    open(fn, 'a').write('\n; macvr permissive\n' + ''.join(f'(typepermissive {t})\n' for t in add))
    print(fn, len(add))
