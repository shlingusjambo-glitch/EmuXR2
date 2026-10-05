"""For each BnHw<X>::_hidl_<method> server stub in a HIDL interface lib, list the Parcel read/write calls it makes
(reads = arguments, writes after the status = return values). usage: hidlsig.py <lib.so> <BnHwName>"""
import re, subprocess, sys
B = '/Users/elywright/Library/Android/sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/darwin-x86_64/bin/'
lib, cls = sys.argv[1], sys.argv[2]
out = subprocess.run([B + 'llvm-objdump', '-d', '-C', '--no-show-raw-insn', lib], capture_output=True, text=True).stdout
cur = None; calls = {}
for line in out.splitlines():
    m = re.match(r'^[0-9a-f]+ <(.*)>:$', line)
    if m:
        name = m.group(1); cur = name if f'{cls}::_hidl_' in name else None
        if cur: calls[cur] = []
        continue
    if cur:
        m = re.search(r'\bbl\s+(?:0x)?[0-9a-f]+ <(.*)>', line)
        if m:
            t = m.group(1)
            t = re.sub(r'@plt$', '', t)
            k = re.search(r'Parcel::(read|write)\w*|writeEmbeddedToParcel|readEmbeddedFromParcel|::_hidl_\w+|(?<=::)[a-zA-Z]+\(', t)
            m2 = re.search(r'6Parcel(\d+)', t)
            if m2: n = int(m2.group(1)); i = m2.end(); calls[cur].append(t[i:i + n])
            elif 'Embedded' in t or 'Parcel::' in t: calls[cur].append(re.sub(r'\(.*', '', t.split('::')[-1]))
for k, v in calls.items():
    print(re.sub(r'\(.*', '', k.split('::_hidl_')[1]), ' '.join(v))
