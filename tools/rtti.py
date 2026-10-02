"""Find MSVC RTTI vtables in DarkSoulsRemastered.exe by class name.

usage: python tools/rtti.py ChrCam ChrAimCam ...
Prints each class's vtable RVA and its virtual function RVAs.
"""
import struct
import sys

EXE = r"F:\SteamLibrary\steamapps\common\DARK SOULS REMASTERED\DarkSoulsRemastered.exe"
BASE = 0x140000000


class Pe:
    def __init__(self, path):
        self.d = d = open(path, "rb").read()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt = pe + 24
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            s = opt + optsz + i * 40
            name = d[s:s + 8].rstrip(b"\0").decode(errors="replace")
            vs, va, rs, ro = struct.unpack_from("<IIII", d, s + 8)
            self.secs.append((name, va, vs, ro, rs))

    def off(self, rva):
        for _, va, vs, ro, rs in self.secs:
            if va <= rva < va + rs:
                return rva - va + ro
        return None

    def rva(self, off):
        for _, va, vs, ro, rs in self.secs:
            if ro <= off < ro + rs:
                return off - ro + va
        return None

    def sec(self, name, index=0):
        return [s for s in self.secs if s[0] == name][index]


def find_vtables(pe, cls):
    d = pe.d
    name = (".?AV%s@NS_FRPG@@" % cls).encode() + b"\0"
    pos = d.find(name)
    if pos < 0:
        return []
    td_rva = pe.rva(pos - 0x10)   # TypeDescriptor: vtable ptr, spare ptr, name
    _, rva, vs, ro, rs = pe.sec(".rdata")
    out = []
    # RTTICompleteObjectLocator: sig(1), offset, cdOffset, typeDesc rva, classDesc rva, self rva
    needle = struct.pack("<I", td_rva)
    i = d.find(needle, ro, ro + rs)
    while i != -1:
        col = i - 0xC
        if struct.unpack_from("<I", d, col)[0] == 1 and struct.unpack_from("<I", d, col + 0x14)[0] == pe.rva(col):
            col_va = BASE + pe.rva(col)
            j = d.find(struct.pack("<Q", col_va), ro, ro + rs)
            while j != -1:
                out.append((pe.rva(j + 8), struct.unpack_from("<I", d, col + 4)[0]))
                j = d.find(struct.pack("<Q", col_va), j + 1, ro + rs)
        i = d.find(needle, i + 1, ro + rs)
    return out


def vfuncs(pe, vt_rva, limit=40):
    text = pe.sec(".text")
    fns = []
    o = pe.off(vt_rva)
    for k in range(limit):
        v = struct.unpack_from("<Q", pe.d, o + k * 8)[0] - BASE
        if not (text[1] <= v < text[1] + text[2] or 0x2019000 <= v < 0x2019000 + 0x1182000):
            break
        fns.append(v)
    return fns


if __name__ == "__main__":
    pe = Pe(EXE)
    for cls in sys.argv[1:]:
        for vt, offset in find_vtables(pe, cls):
            fns = vfuncs(pe, vt)
            print(f"{cls}: vtable exe+{vt:X} (subobject offset {offset:#x}), {len(fns)} funcs: " + " ".join(f"{f:X}" for f in fns))
