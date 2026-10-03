"""Index ULTRAKILL's asset bundles: which bundle file holds which internal asset file ("CAB-...").

Objects in one bundle refer to objects in another by that internal name, so this is what lets a
reference (file id, path id) be followed across bundles. Reads only each bundle's directory.
Needs: pip install UnityPy   (for its lz4 dependency)
"""
import glob
import json
import os
import struct

import lz4.block

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\ULTRAKILL\ULTRAKILL_Data\StreamingAssets\aa\StandaloneWindows64"
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "uk_cab_index.json")


def _cstr(f):
    out = bytearray()
    while True:
        c = f.read(1)
        if not c or c == b"\0":
            return out.decode("utf-8", "replace")
        out += c


def bundle_nodes(path):
    """Names of the asset files inside one UnityFS bundle."""
    with open(path, "rb") as f:
        if _cstr(f) != "UnityFS":
            return []
        version = struct.unpack(">I", f.read(4))[0]
        _cstr(f)
        _cstr(f)
        _size, csize, usize, flags = struct.unpack(">qIII", f.read(20))
        if version >= 7:
            f.seek((f.tell() + 15) & ~15)
        if flags & 0x80:
            pos = f.tell()
            f.seek(-csize, os.SEEK_END)
            raw = f.read(csize)
            f.seek(pos)
        else:
            raw = f.read(csize)
    comp = flags & 0x3F
    if comp in (2, 3):
        info = lz4.block.decompress(raw, uncompressed_size=usize)
    elif comp == 0:
        info = raw
    else:
        return []
    p = 16
    nblocks = struct.unpack_from(">i", info, p)[0]
    p += 4 + nblocks * 10
    nnodes = struct.unpack_from(">i", info, p)[0]
    p += 4
    names = []
    for _ in range(nnodes):
        p += 20
        end = info.index(b"\0", p)
        names.append(info[p:end].decode("utf-8", "replace"))
        p = end + 1
    return names


def cab_index(refresh=False):
    """{asset file name (lower case): bundle path}"""
    if not refresh and os.path.exists(CACHE):
        return json.load(open(CACHE, encoding="utf-8"))
    index = {}
    for path in glob.glob(ROOT + r"\**\*.bundle", recursive=True):
        try:
            for name in bundle_nodes(path):
                if not name.endswith((".resS", ".resource")):
                    index[name.lower()] = path
        except Exception as e:   # a bundle in a format this reader does not handle
            print(f"{os.path.basename(path)}: {type(e).__name__}: {e}")
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    json.dump(index, open(CACHE, "w", encoding="utf-8"), indent=0)
    return index


if __name__ == "__main__":
    idx = cab_index(refresh=True)
    print(len(idx), "asset files in", len(set(idx.values())), "bundles")
    for name, path in list(idx.items())[:8]:
        print(" ", name, "->", os.path.basename(path))
