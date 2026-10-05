"""Read 8-byte words at virtual addresses of an ELF file (implicit relocation addends, globals).
usage: peek.py <elf> <vaddr>..."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<Q', d, 0x20); phn, = struct.unpack_from('<H', d, 0x38)
segs = [struct.unpack_from('<IIQQQQQQ', d, phoff + 56 * i) for i in range(phn)]
def word(va):
    for t, _, off, vaddr, _, fsz, msz, _ in segs:
        if t == 1 and vaddr <= va < vaddr + fsz: return struct.unpack_from('<Q', d, off + va - vaddr)[0]
for a in sys.argv[2:]:
    w = word(int(a, 16)); print(a, hex(w) if w is not None else None)
