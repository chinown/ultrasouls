"""Find call/jmp sites that target an address range. usage: python tools/xref.py <lo rva hex> [hi rva hex]"""
import struct, sys
d = open("backup/text.bin", "rb").read()
base = 0x1000
lo = int(sys.argv[1], 16); hi = int(sys.argv[2], 16) if len(sys.argv) > 2 else lo
out = {}
for op in (0xE8, 0xE9):
    i = d.find(bytes([op]))
    while i != -1 and i + 5 <= len(d):
        t = base + i + 5 + struct.unpack_from("<i", d, i + 1)[0]
        if lo <= t <= hi:
            out.setdefault(t, []).append(("call " if op == 0xE8 else "jmp ") + "%X" % (base + i))
        i = d.find(bytes([op]), i + 1)
for t in sorted(out):
    print("%X <- %s" % (t, ", ".join(out[t][:40])), "(%d)" % len(out[t]))
