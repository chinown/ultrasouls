"""Dark Souls Remastered's model files (FLVER, version 0x2000C), read and written.

  python tools/ds_flver.py check [N]      read every part model in your install, write it back and compare
                                          byte for byte (N: only the first N files)
  python tools/ds_flver.py info FILE      what is in one .partsbnd.dcx / .chrbnd.dcx: bones, materials, meshes, layouts

The layout is the one documented by the SoulsFormats project. A file is the header, then tables (dummies,
materials, bones, meshes, face sets, vertex buffers, buffer layouts, textures), then the tables' own
data (layout members, bounding boxes, bone index lists, strings), then the vertex and index data.
Nothing from the game is stored in this repository; the tool only works on your own files.
"""
import os
import struct
import sys

import numpy as np

from dsparam import Bnd3, dcx_decompress

GAME_DIR = r"F:\SteamLibrary\steamapps\common\DARK SOULS REMASTERED"

# vertex member types (how it is stored) and semantics (what it is)
T_FLOAT2, T_FLOAT3, T_FLOAT4, T_BYTE4A, T_BYTE4B, T_SHORT2, T_BYTE4C, T_UV, T_UVPAIR, T_SHORT_BONES, T_SHORT4A, T_SHORT4B, T_BYTE4E = (
    0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13, 0x15, 0x16, 0x18, 0x1A, 0x2E, 0x2F)
TYPE_SIZE = {T_FLOAT2: 8, T_FLOAT3: 12, T_FLOAT4: 16, T_BYTE4A: 4, T_BYTE4B: 4, T_SHORT2: 4, T_BYTE4C: 4, T_UV: 4, T_UVPAIR: 8, T_SHORT_BONES: 8,
             T_SHORT4A: 8, T_SHORT4B: 8, T_BYTE4E: 4}
S_POSITION, S_WEIGHTS, S_BONES, S_NORMAL, S_UV, S_TANGENT, S_BITANGENT, S_COLOR = 0, 1, 2, 3, 5, 6, 7, 10
UV_FACTOR = 1024.0


def _utf16(buf, off):
    end = off
    while buf[end:end + 2] != b"\0\0":
        end += 2
    return bytes(buf[off:end]).decode("utf-16-le")


class Flver:
    def __init__(self, data):
        b = self.raw = bytes(data)
        assert b[:6] == b"FLVER\0" and b[6:8] == b"L\0", "not a little-endian FLVER"
        (self.version, data_off, data_len, n_dummy, n_mat, n_bone, n_mesh, n_vb) = struct.unpack_from("<8i", b, 8)
        assert self.version == 0x2000C, hex(self.version)
        self.bb_min = struct.unpack_from("<3f", b, 0x28)
        self.bb_max = struct.unpack_from("<3f", b, 0x34)
        self.true_faces, self.total_faces = struct.unpack_from("<2i", b, 0x40)
        self.index_size, self.unicode, self.unk4a, _ = struct.unpack_from("<4B", b, 0x48)
        assert self.index_size == 16 and self.unicode == 1
        self.unk4c, n_fs, n_layout, n_tex = struct.unpack_from("<4i", b, 0x4C)
        self.unk5c, self.unk5d = b[0x5C], b[0x5D]
        self.unk68 = struct.unpack_from("<i", b, 0x68)[0]
        pos = 0x80
        self.dummies = [b[pos + i * 64:pos + i * 64 + 64] for i in range(n_dummy)]     # kept as they are
        pos += n_dummy * 64
        self.materials = []
        for i in range(n_mat):
            name_off, mtd_off, tex_count, tex_index, flags, gx_off, unk18, zero = struct.unpack_from("<8i", b, pos)
            assert gx_off == 0 and zero == 0
            self.materials.append({"name": _utf16(b, name_off), "mtd": _utf16(b, mtd_off), "flags": flags, "unk18": unk18, "tex_count": tex_count,
                                   "tex_index": tex_index, "textures": []})
            pos += 32
        self.bones = []
        for i in range(n_bone):
            t = struct.unpack_from("<3f", b, pos)
            name_off = struct.unpack_from("<i", b, pos + 12)[0]
            r = struct.unpack_from("<3f", b, pos + 16)
            parent, child = struct.unpack_from("<2h", b, pos + 28)
            s = struct.unpack_from("<3f", b, pos + 32)
            nxt, prev = struct.unpack_from("<2h", b, pos + 44)
            bmin = struct.unpack_from("<3f", b, pos + 48)
            unk3c = struct.unpack_from("<i", b, pos + 60)[0]
            bmax = struct.unpack_from("<3f", b, pos + 64)
            assert b[pos + 76:pos + 128] == bytes(52)
            self.bones.append({"name": _utf16(b, name_off), "t": t, "r": r, "s": s, "parent": parent, "child": child, "next": nxt, "prev": prev,
                               "bb_min": bmin, "bb_max": bmax, "unk3c": unk3c})
            pos += 128
        self.meshes = []
        for i in range(n_mesh):
            (dynamic, material, z0, z1, default_bone, bone_count, bb_off, bone_off, fs_count, fs_off, vb_count, vb_off) = struct.unpack_from("<12i", b, pos)
            assert z0 == 0 and z1 == 0 and dynamic in (0, 1)
            m = {"dynamic": dynamic, "material": material, "default_bone": default_bone,
                 "bone_indices": list(struct.unpack_from("<%di" % bone_count, b, bone_off)),
                 "bb": struct.unpack_from("<6f", b, bb_off) if bb_off else None,
                 "_fs": list(struct.unpack_from("<%di" % fs_count, b, fs_off)), "_vb": list(struct.unpack_from("<%di" % vb_count, b, vb_off))}
            self.meshes.append(m)
            pos += 48
        face_sets = []
        for i in range(n_fs):
            flags, strip, cull, unk06, count, off, length, z0, isz, z1 = struct.unpack_from("<IBBhiiiiii", b, pos)
            assert z0 == 0 and z1 == 0 and isz == 0 and length == count * 2
            idx = np.frombuffer(b, dtype="<u2", count=count, offset=data_off + off).copy()
            face_sets.append({"flags": flags, "strip": strip, "cull": cull, "unk06": unk06, "indices": idx})
            pos += 32
        vbuffers = []
        for i in range(n_vb):
            buf_index, layout, vsize, vcount, z0, z1, length, off = struct.unpack_from("<8i", b, pos)
            assert z0 == 0 and z1 == 0 and length == vsize * vcount
            vbuffers.append({"buffer_index": buf_index, "layout": layout, "size": vsize, "count": vcount,
                             "data": b[data_off + off:data_off + off + length]})
            pos += 32
        self.layouts = []
        for i in range(n_layout):
            count, z0, z1, off = struct.unpack_from("<4i", b, pos)
            members, so = [], 0
            for k in range(count):
                unk00, struct_off, typ, sem, index = struct.unpack_from("<5i", b, off + k * 20)
                assert struct_off == so, (struct_off, so)
                members.append({"unk00": unk00, "type": typ, "semantic": sem, "index": index})
                so += TYPE_SIZE[typ]
            self.layouts.append(members)
            pos += 16
        textures = []
        for i in range(n_tex):
            path_off, type_off = struct.unpack_from("<2i", b, pos)
            sx, sy = struct.unpack_from("<2f", b, pos + 8)
            unk10, unk11, z0, z1 = struct.unpack_from("<4B", b, pos + 16)
            unk14, unk18, unk1c = struct.unpack_from("<3f", b, pos + 20)
            textures.append({"path": _utf16(b, path_off), "type": _utf16(b, type_off), "scale": (sx, sy), "unk10": unk10, "unk11": unk11, "unk14": unk14,
                             "unk18": unk18, "unk1c": unk1c})
            pos += 32
        for m in self.materials:
            m["textures"] = textures[m["tex_index"]:m["tex_index"] + m["tex_count"]]
        for m in self.meshes:
            m["face_sets"] = [face_sets[i] for i in m.pop("_fs")]
            m["vbuffers"] = [vbuffers[i] for i in m.pop("_vb")]

    # ---- writing
    def write(self):
        out = bytearray(0x80)
        fills = {}

        def reserve(key):
            fills[key] = len(out)
            out.extend(b"\0\0\0\0")

        def fill(key, value):
            struct.pack_into("<i", out, fills.pop(key), value)

        def pad(n):
            while len(out) % n:
                out.append(0)

        def wstr(s):
            out.extend(s.encode("utf-16-le") + b"\0\0")

        for d in self.dummies:
            out.extend(d)
        for i, m in enumerate(self.materials):
            reserve("mname%d" % i)
            reserve("mmtd%d" % i)
            out.extend(struct.pack("<i", len(m["textures"])))
            reserve("mtex%d" % i)
            out.extend(struct.pack("<iiii", m["flags"], 0, m["unk18"], 0))
        for i, bn in enumerate(self.bones):
            out.extend(struct.pack("<3f", *bn["t"]))
            reserve("bname%d" % i)
            out.extend(struct.pack("<3f2h3f2h3fi3f", *bn["r"], bn["parent"], bn["child"], *bn["s"], bn["next"], bn["prev"], *bn["bb_min"], bn["unk3c"],
                                   *bn["bb_max"]))
            out.extend(bytes(52))
        for i, m in enumerate(self.meshes):
            out.extend(struct.pack("<6i", m["dynamic"], m["material"], 0, 0, m["default_bone"], len(m["bone_indices"])))
            reserve("mbb%d" % i)
            reserve("mbones%d" % i)
            out.extend(struct.pack("<i", len(m["face_sets"])))
            reserve("mfs%d" % i)
            out.extend(struct.pack("<i", len(m["vbuffers"])))
            reserve("mvb%d" % i)
        k = 0
        for m in self.meshes:
            for fs in m["face_sets"]:
                out.extend(struct.pack("<IBBhi", fs["flags"], fs["strip"], fs["cull"], fs["unk06"], len(fs["indices"])))
                reserve("fsv%d" % k)
                out.extend(struct.pack("<4i", len(fs["indices"]) * 2, 0, 0, 0))
                k += 1
        k = 0
        for m in self.meshes:
            for j, vb in enumerate(m["vbuffers"]):
                size = sum(TYPE_SIZE[x["type"]] for x in self.layouts[vb["layout"]])
                assert len(vb["data"]) % size == 0
                count = len(vb["data"]) // size
                out.extend(struct.pack("<7i", vb.get("buffer_index", j), vb["layout"], size, count, 0, 0, size * count))
                reserve("vbo%d" % k)
                k += 1
        for i, lay in enumerate(self.layouts):
            out.extend(struct.pack("<3i", len(lay), 0, 0))
            reserve("lay%d" % i)
        k = 0
        for i, m in enumerate(self.materials):
            fill("mtex%d" % i, k)
            for t in m["textures"]:
                reserve("tpath%d" % k)
                reserve("ttype%d" % k)
                out.extend(struct.pack("<2f4B3f", *t["scale"], t["unk10"], t["unk11"], 0, 0, t["unk14"], t["unk18"], t["unk1c"]))
                k += 1
        pad(16)
        for i, lay in enumerate(self.layouts):
            fill("lay%d" % i, len(out))
            so = 0
            for x in lay:
                out.extend(struct.pack("<5i", x["unk00"], so, x["type"], x["semantic"], x["index"]))
                so += TYPE_SIZE[x["type"]]
        pad(16)
        for i, m in enumerate(self.meshes):
            if m["bb"] is None:
                fill("mbb%d" % i, 0)
            else:
                fill("mbb%d" % i, len(out))
                out.extend(struct.pack("<6f", *m["bb"]))
        pad(16)
        start = len(out)
        for i, m in enumerate(self.meshes):
            if not m["bone_indices"]:
                fill("mbones%d" % i, start)
            else:
                fill("mbones%d" % i, len(out))
                out.extend(struct.pack("<%di" % len(m["bone_indices"]), *m["bone_indices"]))
        pad(16)
        k = 0
        for i, m in enumerate(self.meshes):
            fill("mfs%d" % i, len(out))
            for _ in m["face_sets"]:
                out.extend(struct.pack("<i", k))
                k += 1
        pad(16)
        k = 0
        for i, m in enumerate(self.meshes):
            fill("mvb%d" % i, len(out))
            for _ in m["vbuffers"]:
                out.extend(struct.pack("<i", k))
                k += 1
        pad(16)
        pad(16)                                   # (the GX lists would be here: this version has none)
        k = 0
        for i, m in enumerate(self.materials):
            fill("mname%d" % i, len(out))
            wstr(m["name"])
            fill("mmtd%d" % i, len(out))
            wstr(m["mtd"])
            for t in m["textures"]:
                fill("tpath%d" % k, len(out))
                wstr(t["path"])
                fill("ttype%d" % k, len(out))
                wstr(t["type"])
                k += 1
        pad(16)
        for i, bn in enumerate(self.bones):
            fill("bname%d" % i, len(out))
            wstr(bn["name"])
        pad(32)
        data_start = len(out)
        kf = kv = 0
        true_faces = total_faces = 0
        for m in self.meshes:
            for fs in m["face_sets"]:
                pad(16)
                fill("fsv%d" % kf, len(out) - data_start)
                idx = np.asarray(fs["indices"], dtype="<u2")
                out.extend(idx.tobytes())
                kf += 1
                if fs["strip"]:
                    a, b_, c = idx[:-2].astype(np.int64), idx[1:-1].astype(np.int64), idx[2:].astype(np.int64)
                    live = (a != 0xFFFF) & (b_ != 0xFFFF) & (c != 0xFFFF)
                    total_faces += int(live.sum())
                    if not fs["flags"] & 0x80000000:
                        true_faces += int((live & (a != b_) & (b_ != c) & (c != a)).sum())
                else:
                    total_faces += len(idx) // 3
                    true_faces += len(idx) // 3
            for vb in m["vbuffers"]:
                pad(16)
                fill("vbo%d" % kv, len(out) - data_start)
                out.extend(vb["data"])
                kv += 1
        # (the game's own files start the data on a multiple of 32, line each block inside it up on 16 and
        # end with the last block, unpadded: found by writing them back and comparing)
        assert not fills, fills
        n_fs = sum(len(m["face_sets"]) for m in self.meshes)
        n_vb = sum(len(m["vbuffers"]) for m in self.meshes)
        n_tex = sum(len(m["textures"]) for m in self.materials)
        struct.pack_into("<6s2s8i", out, 0, b"FLVER\0", b"L\0", self.version, data_start, len(out) - data_start, len(self.dummies), len(self.materials),
                         len(self.bones), len(self.meshes), n_vb)
        struct.pack_into("<6f2i4B4i2B", out, 0x28, *self.bb_min, *self.bb_max, true_faces, total_faces, 16, 1, self.unk4a, 0, self.unk4c, n_fs,
                         len(self.layouts), n_tex, self.unk5c, self.unk5d)
        struct.pack_into("<i", out, 0x68, self.unk68)
        return bytes(out)

    # ---- vertices as arrays
    def vertices(self, mesh):
        """{semantic or (semantic, n-th of it): array} for a mesh's first vertex buffer."""
        vb = mesh["vbuffers"][0]
        lay = self.layouts[vb["layout"]]
        size = sum(TYPE_SIZE[x["type"]] for x in lay)
        raw = np.frombuffer(vb["data"], dtype=np.uint8).reshape(-1, size)
        out, off, seen = {}, 0, {}
        for x in lay:
            n = TYPE_SIZE[x["type"]]
            part = np.ascontiguousarray(raw[:, off:off + n])
            t, sem = x["type"], x["semantic"]
            if t in (T_FLOAT2, T_FLOAT3, T_FLOAT4):
                val = part.view("<f4").astype(np.float64)
            elif t in (T_SHORT2, T_UV, T_UVPAIR):
                val = part.view("<i2").astype(np.float64) / UV_FACTOR
            elif t == T_SHORT_BONES:
                val = part.view("<u2").astype(np.int64)
            elif t in (T_SHORT4A, T_SHORT4B):
                val = part.view("<i2").astype(np.float64) / 32767.0
            elif sem == S_BONES:
                val = part.astype(np.int64)
            elif sem == S_WEIGHTS:
                val = part.view("i1").astype(np.float64) / 127.0
            elif sem == S_COLOR:
                val = part.astype(np.float64) / 255.0
            else:                                   # normals and tangents in bytes: 127 is zero
                val = (part.astype(np.float64) - 127.0) / 127.0
            k = seen.get(sem, 0)
            seen[sem] = k + 1
            out[sem if k == 0 else (sem, k)] = val
            off += n
        return out


def encode_vertices(layout, arrays):
    """The reverse of Flver.vertices: bytes for one vertex buffer."""
    count = len(arrays[S_POSITION])
    parts, seen = [], {}
    for x in layout:
        t, sem = x["type"], x["semantic"]
        k = seen.get(sem, 0)
        seen[sem] = k + 1
        val = np.asarray(arrays[sem if k == 0 else (sem, k)])
        if t in (T_FLOAT2, T_FLOAT3, T_FLOAT4):
            raw = val.astype("<f4")
        elif t in (T_SHORT2, T_UV, T_UVPAIR):
            raw = np.clip(np.round(val * UV_FACTOR), -32768, 32767).astype("<i2")
        elif t == T_SHORT_BONES:
            raw = val.astype("<u2")
        elif t in (T_SHORT4A, T_SHORT4B):
            raw = np.clip(np.round(val * 32767.0), -32767, 32767).astype("<i2")
        elif sem == S_BONES:
            raw = val.astype(np.uint8)
        elif sem == S_WEIGHTS:
            raw = np.clip(np.round(val * 127.0), -127, 127).astype("i1")
        elif sem == S_COLOR:
            raw = np.clip(np.round(val * 255.0), 0, 255).astype(np.uint8)
        else:
            raw = np.clip(np.round(val * 127.0 + 127.0), 0, 255).astype(np.uint8)
        parts.append(np.ascontiguousarray(raw).view(np.uint8).reshape(count, TYPE_SIZE[t]))
    return np.concatenate(parts, axis=1).tobytes()


def open_bnd(path):
    return Bnd3(dcx_decompress(open(path, "rb").read()))


def flver_of(path):
    b = open_bnd(path)
    for e in b.entries:
        if e["name"].lower().endswith(".flver"):
            return Flver(b.data(e))
    raise KeyError("no model in " + path)


def cmd_check(limit=None):
    parts = os.path.join(GAME_DIR, "parts")
    names = sorted(n for n in os.listdir(parts) if n.endswith(".partsbnd.dcx"))
    if limit:
        names = names[:limit]
    ok = bad = 0
    types = {}
    for n in names:
        try:
            b = open_bnd(os.path.join(parts, n))
            for e in b.entries:
                if not e["name"].lower().endswith(".flver"):
                    continue
                src = b.data(e)
                f = Flver(src)
                same = f.write() == src
                for m in f.meshes:
                    for vb in m["vbuffers"]:
                        lay = f.layouts[vb["layout"]]
                        key = tuple((x["semantic"], x["type"]) for x in lay)
                        types[key] = types.get(key, 0) + 1
                        if encode_vertices(lay, f.vertices(m)) != vb["data"]:
                            same = False
                ok += same
                bad += not same
                if not same:
                    print("  differs:", n)
        except Exception as ex:
            bad += 1
            print("  failed:", n, repr(ex)[:150])
    print("%d models read and written back identical, %d not" % (ok, bad))
    for key, count in sorted(types.items(), key=lambda kv: -kv[1]):
        print("  %5d vertex buffers laid out as %s" % (count, " ".join("%d:%02X" % x for x in key)))


def cmd_info(path):
    b = open_bnd(path)
    for e in b.entries:
        print("%4d %8d %s" % (e.get("id", -1), e["size"], e["name"]))
    f = flver_of(path)
    print("bounds", np.round(f.bb_min, 3), np.round(f.bb_max, 3), "unk4a %d 4c %d 5c %d 5d %d 68 %d" % (f.unk4a, f.unk4c, f.unk5c, f.unk5d, f.unk68),
          "dummies", len(f.dummies))
    for i, bn in enumerate(f.bones):
        print("  bone %2d %-22s parent %2d t %s r %s s %s" % (i, bn["name"], bn["parent"], np.round(bn["t"], 3), np.round(bn["r"], 3), np.round(bn["s"], 2)))
    for i, m in enumerate(f.materials):
        print("  material %d %r mtd %r flags %x unk18 %d" % (i, m["name"], m["mtd"], m["flags"], m["unk18"]))
        for t in m["textures"]:
            print("      %-24s %r scale %s unk %d %d %.2f %.2f %.2f" % (t["type"], t["path"], t["scale"], t["unk10"], t["unk11"], t["unk14"], t["unk18"], t["unk1c"]))
    for i, lay in enumerate(f.layouts):
        print("  layout %d: %s" % (i, ", ".join("sem %d type %02X idx %d unk %d" % (x["semantic"], x["type"], x["index"], x["unk00"]) for x in lay)))
    for i, m in enumerate(f.meshes):
        v = f.vertices(m)
        print("  mesh %d material %d dynamic %d default bone %d, %d bones in its list, %d vertices, layout %d" % (
            i, m["material"], m["dynamic"], m["default_bone"], len(m["bone_indices"]), len(v[S_POSITION]), m["vbuffers"][0]["layout"]))
        for fs in m["face_sets"]:
            print("      face set flags %08x strip %d cull %d unk06 %d: %d indices" % (fs["flags"], fs["strip"], fs["cull"], fs["unk06"], len(fs["indices"])))
        print("      position box %s to %s" % (np.round(v[S_POSITION].min(axis=0), 3), np.round(v[S_POSITION].max(axis=0), 3)))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["check"]:
        cmd_check(int(a[1]) if len(a) > 1 else None)
    elif a[:1] == ["info"] and len(a) > 1:
        cmd_info(a[1])
    else:
        sys.exit(__doc__)
