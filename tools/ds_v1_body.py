"""V1's body as Dark Souls armour pieces, built from your own copies of both games.

  python tools/ds_v1_body.py build      fit V1's third-person model to the Dark Souls player skeleton and write
                                        the part files (body with the head, arms, legs) to build/v1_body/
  python tools/ds_v1_body.py preview    the same fit, drawn to build/v1_body/preview_*.png: in the skeleton's
                                        own pose and with some joints turned, to see that the parts follow
  python tools/ds_v1_body.py install    copy the built part files into the game's parts folder (new files, model
                                        id 9800: none of the game's own is replaced), if the game is not running
  python tools/ds_v1_body.py remove     take them out again

The files alone change nothing: tools/patch_v1.py points the "nothing equipped" rows of the body, arms and
legs at model 9800 (V1_BODY there), so a player with no armour on is drawn as V1. The head is part of the
body piece, and the body row hides the human head's face and hair, because the row for a bare head is
shared with every helmetless character in the game.

Nothing from either game is stored in this repository.

How the fit works. ULTRAKILL's 'v1_combined' is the body the game shows for the player in mirrors: one mesh,
'v1_mdl', made of rigid pieces, nearly every vertex tied to a single bone. Dark Souls' player is drawn as
armour pieces, each a model bound by bone NAME to the skeleton in chr/c0000 (bind pose: arms straight out,
facing -Z, left at +X). So every V1 piece is moved, as a whole, from where its bone holds it in V1's rest
pose (arms hanging, facing +Z, left at -X) to where the matching Dark Souls bone would hold it:
  - the whole model is turned half round and scaled so the hip joints are at the same height;
  - the trunk keeps its shape, stretched upward a little so the shoulders are at Dark Souls' height;
  - each arm and leg segment is laid along the Dark Souls bone it is given to, joint on joint, stretched or
    shortened along its length to fit (the legs keep V1's own narrower stance);
  - hands, feet, head and wings go with the segment they hang from.
The weights are then renamed: each V1 bone's share goes to the Dark Souls bone in BONE_MAP.
"""
import os
import struct
import sys

import numpy as np

import ds_flver as F
from dsparam import dcx_compress, dcx_decompress

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "build", "v1_body")

# V1 bone -> Dark Souls bone (left and right are filled in from the .L entries)
BONE_MAP = {
    "spine": "Pelvis", "spine.001": "Pelvis", "spine.002": "Spine", "spine.003": "Spine1", "spine.004": "Neck", "spine.005": "Neck",
    "spine.006": "Head",
    "shoulder.L": "L_Clavicle", "upper_arm.L": "L_UpperArm", "forearm.L": "L_Forearm", "hand.L": "L_Hand",
    "f_index.01.L": "L_Finger1", "f_index.02.L": "L_Finger11", "f_index.03.L": "L_Finger12", "thumb.01.L": "L_Finger0", "thumb.02.L": "L_Finger01",
    "thigh.L": "L_Thigh", "shin.L": "L_Calf", "foot.L": "L_Foot", "toe.L": "L_Toe0", "heel.02.L": "L_Foot",
}
for _k, _v in list(BONE_MAP.items()):
    if _k.endswith(".L"):
        BONE_MAP[_k[:-2] + ".R"] = "R_" + _v[2:]
for _w in range(1, 9):
    for _s in range(1, 4):
        BONE_MAP["wing_%d_%d" % (_w, _s)] = "Spine1"
# which part file a Dark Souls bone's vertices go into
PART_OF = {"Head": "BD", "Neck": "BD", "Spine": "BD", "Spine1": "BD", "L_Clavicle": "BD", "R_Clavicle": "BD", "Pelvis": "LG"}
for _side in "LR":
    for _b in ("UpperArm", "Forearm", "Hand", "Finger0", "Finger01", "Finger02", "Finger1", "Finger11", "Finger12", "Finger2", "Finger21", "Finger22"):
        PART_OF["%s_%s" % (_side, _b)] = "AM"
    for _b in ("Thigh", "Calf", "Foot", "Toe0"):
        PART_OF["%s_%s" % (_side, _b)] = "LG"
TEMPLATES = {"BD": "BD_M_0000", "AM": "AM_M_0000", "LG": "LG_M_0000"}     # the game's own piece each is modelled on (bone list, vertex layout)
MODEL_ID = 9800              # a model number none of the game's own parts has; "A": one file for both sexes
PARTS_DIR = os.path.join(F.GAME_DIR, "parts")


def out_names(part):
    """(file name, model name inside it) for the plain piece and for its hollowed twin, which the game asks for by "_M"."""
    base = "%s_A_%04d" % (part, MODEL_ID)
    return [(base + ".partsbnd.dcx", base, base), (base + "_M.partsbnd.dcx", base, base + "_M")]
TEX_SCALE = 4                # each texel becomes one 4 x 4 block, which DXT1 stores exactly


def bone_local(bn):
    rx, ry, rz = bn["r"]
    cx, sx, cy, sy, cz, sz = np.cos(rx), np.sin(rx), np.cos(ry), np.sin(ry), np.cos(rz), np.sin(rz)
    RX = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    RY = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    RZ = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    m = np.eye(4)
    m[:3, :3] = RY @ RZ @ RX                 # about X first, then Z, then Y (checked against the game's own arm mesh)
    m[:3, 3] = bn["t"]
    return m


def ds_skeleton():
    """(bones, {name: index}, world matrices) of the player's skeleton."""
    sk = F.flver_of(os.path.join(F.GAME_DIR, "chr", "c0000.chrbnd.dcx"))
    world = []
    for bn in sk.bones:
        m = bone_local(bn)
        world.append(world[bn["parent"]] @ m if bn["parent"] >= 0 else m)
    return sk.bones, {bn["name"]: i for i, bn in enumerate(sk.bones)}, world


def load_v1():
    """V1's mesh in its rest pose, turned to face the way Dark Souls' model does."""
    import uk_models as M
    from uk_assets import follow, material_texture, prefab_nodes
    from UnityPy.helpers.MeshHelper import MeshHandler
    nodes = prefab_nodes(M.find_root("v1_combined"))
    mdl = next(p for p in nodes if nodes[p]["name"] == "v1_mdl")
    smr = next(c for c in nodes[mdl]["components"] if c.type.name == "SkinnedMeshRenderer")
    t = smr.read_typetree()
    mesh = follow(smr, t["m_Mesh"]).read()
    h = MeshHandler(mesh)
    h.process()
    pos = np.array(h.m_Vertices, dtype=np.float64).reshape(-1, 3)
    nrm = np.array(h.m_Normals, dtype=np.float64).reshape(-1, 3)
    uv = np.array(h.m_UV0, dtype=np.float64).reshape(len(pos), -1)[:, :2].copy()
    uv[:, 1] = 1.0 - uv[:, 1]                                  # Unity's V runs bottom-up
    bi = np.array(h.m_BoneIndices, dtype=np.int64).reshape(len(pos), -1)
    bw = np.array(h.m_BoneWeights, dtype=np.float64).reshape(len(pos), -1)
    bw = bw / bw.sum(axis=1, keepdims=True)
    binds = [np.array([[getattr(b, "e%d%d" % (r, c)) for c in range(4)] for r in range(4)]) for b in mesh.m_BindPose]
    bone_ids = [b["m_PathID"] for b in t["m_Bones"]]
    names = [nodes[b]["name"] for b in bone_ids]
    world = [nodes[b]["world"] for b in bone_ids]
    hp = np.concatenate([pos, np.ones((len(pos), 1))], axis=1)
    rest = np.zeros((len(pos), 3))
    rest_n = np.zeros((len(pos), 3))
    for k in range(bi.shape[1]):
        for j in range(len(names)):
            sel = (bi[:, k] == j) & (bw[:, k] > 0)
            if sel.any():
                m = world[j] @ binds[j]
                rest[sel] += bw[sel, k:k + 1] * (hp[sel] @ m.T)[:, :3]
                rest_n[sel] += bw[sel, k:k + 1] * (nrm[sel] @ np.linalg.inv(m[:3, :3]))      # inverse transpose, as rows
    turn = np.array([-1.0, 1.0, -1.0])                         # half a turn about the vertical: faces -Z, left at +X
    rest *= turn
    rest_n *= turn
    rest_n /= np.maximum(np.linalg.norm(rest_n, axis=1, keepdims=True), 1e-9)
    joints = {n: world[j][:3, 3] * turn for j, n in enumerate(names)}
    subs, textures = [], []
    for si, tris in enumerate(h.get_triangles()):
        mat = follow(smr, t["m_Materials"][si])
        tex, _ = material_texture(mat)
        tname = tex.peek_name()
        if tname not in [x[0] for x in textures]:
            textures.append((tname, tex.read().image.convert("RGBA")))
        subs.append((tname, np.array(tris, dtype=np.int64).reshape(-1, 3)))
    return {"pos": rest, "nrm": rest_n, "uv": uv, "bi": bi, "bw": bw, "names": names, "joints": joints, "subs": subs, "textures": textures}


def rot_between(a, b):
    """The smallest rotation that turns unit vector a onto unit vector b."""
    v = np.cross(a, b)
    c = float(a @ b)
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx / (1.0 + c)


def fit(v1):
    """Where every vertex goes in Dark Souls' bind pose: (positions, normals, {V1 bone: (anchor, matrix, target anchor)})."""
    J = v1["joints"]
    bones, index, W = ds_skeleton()
    D = {n: W[i][:3, 3] for n, i in index.items()}
    foot = [j for j, n in enumerate(v1["names"]) if n.startswith(("foot", "toe"))]
    on_foot = np.isin(v1["bi"][:, 0], foot)
    sole = v1["pos"][on_foot, 1].min()
    hip_v, hip_d = J["thigh.L"], D["L_Thigh"]
    s = hip_d[1] / (hip_v[1] - sole)
    sh_v, sh_d = J["upper_arm.L"][1], D["L_UpperArm"][1]
    k = (sh_d - hip_d[1]) / ((sh_v - hip_v[1]) * s)            # the trunk's upward stretch
    tf = {}
    trunk_a = np.array([0.0, hip_v[1], hip_v[2]])
    trunk_A = np.array([0.0, hip_d[1], hip_d[2]])
    trunk_M = np.diag([s, s * k, s])
    for n in v1["names"]:
        if n in ("spine", "spine.001", "spine.002", "spine.003") or n.startswith(("shoulder", "wing")):
            tf[n] = (trunk_a, trunk_M, trunk_A)
    neck_a = np.array([0.0, sh_v, hip_v[2]])
    neck_A = np.array([0.0, sh_d, hip_d[2]])
    for n in ("spine.004", "spine.005", "spine.006"):
        tf[n] = (neck_a, np.eye(3) * s, neck_A)

    def segment(a, b, A, B):
        dv, dd = (b - a) / np.linalg.norm(b - a), (B - A) / np.linalg.norm(B - A)
        along = np.linalg.norm(B - A) / np.linalg.norm(b - a)
        R = rot_between(dv, dd)
        return R, R @ (np.eye(3) * s + (along - s) * np.outer(dv, dv)), along / s

    report = []
    for side, sd in ((".L", "L_"), (".R", "R_")):
        R, M, st = segment(J["upper_arm" + side], J["forearm" + side], D[sd + "UpperArm"], D[sd + "Forearm"])
        tf["upper_arm" + side] = (J["upper_arm" + side], M, D[sd + "UpperArm"])
        report.append(("upper_arm" + side, st))
        R, M, st = segment(J["forearm" + side], J["hand" + side], D[sd + "Forearm"], D[sd + "Hand"])
        tf["forearm" + side] = (J["forearm" + side], M, D[sd + "Forearm"])
        report.append(("forearm" + side, st))
        for n in ("hand", "f_index.01", "f_index.02", "f_index.03", "thumb.01", "thumb.02"):
            tf[n + side] = (J["hand" + side], R * s, D[sd + "Hand"])
        # the legs keep V1's own stance: its hips are narrower than the Dark Souls skeleton's, and a leg
        # swings the same about a pivot that is only further in or out along the swing's own axis
        def leg(joint, ds_joint):
            return np.array([J[joint + side][0] * s, D[sd + ds_joint][1], D[sd + ds_joint][2]])
        A_th, A_ca, A_ft = leg("thigh", "Thigh"), leg("shin", "Calf"), leg("foot", "Foot")
        R, M, st = segment(J["thigh" + side], J["shin" + side], A_th, A_ca)
        tf["thigh" + side] = (J["thigh" + side], M, A_th)
        report.append(("thigh" + side, st))
        R, M, st = segment(J["shin" + side], J["foot" + side], A_ca, A_ft)
        tf["shin" + side] = (J["shin" + side], M, A_ca)
        report.append(("shin" + side, st))
        s_foot = A_ft[1] / (J["foot" + side][1] - sole)        # so that the sole stands on the ground
        for n in ("foot", "toe", "heel.02"):
            tf[n + side] = (J["foot" + side], np.eye(3) * s_foot, A_ft)
    pos = np.zeros_like(v1["pos"])
    nrm = np.zeros_like(v1["nrm"])
    for j, n in enumerate(v1["names"]):
        a, M, A = tf[n]
        Mn = np.linalg.inv(M).T
        for c in range(v1["bi"].shape[1]):
            sel = (v1["bi"][:, c] == j) & (v1["bw"][:, c] > 0)
            if sel.any():
                w = v1["bw"][sel, c:c + 1]
                pos[sel] += w * (A + (v1["pos"][sel] - a) @ M.T)
                nrm[sel] += w * (v1["nrm"][sel] @ Mn.T)
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
    info = {"scale": s, "trunk": k, "segments": report, "sole": sole}
    return pos, nrm, info


def ds_weights(v1):
    """For every vertex, up to four (Dark Souls bone name, weight), heaviest first."""
    out = []
    for i in range(len(v1["pos"])):
        acc = {}
        for c in range(v1["bi"].shape[1]):
            w = v1["bw"][i, c]
            if w > 0:
                name = BONE_MAP[v1["names"][v1["bi"][i, c]]]
                acc[name] = acc.get(name, 0.0) + w
        top = sorted(acc.items(), key=lambda kv: -kv[1])[:4]
        total = sum(w for _, w in top)
        out.append([(n, w / total) for n, w in top])
    return out


# ---- textures: DXT1 in a DDS file, in a TPF
def dxt1(img):
    """An image as DXT1 blocks. Each 4 x 4 block gets its brightest and darkest colours as the two ends and
    every pixel the nearest of the four colours along the line between them; a block of one colour (all of
    the full-size picture: every texel of V1's texture is blown up to exactly one block) is stored exactly,
    but for the 5:6:5 rounding."""
    a = np.asarray(img.convert("RGB"), dtype=np.int64)
    h, w = a.shape[:2]
    if h % 4 or w % 4:                                         # the smallest levels: fill out to whole blocks
        full = np.zeros((h + -h % 4, w + -w % 4, 3), dtype=np.int64)
        full[:h, :w] = a
        full[h:, :w] = a[h - 1:h, :]
        full[:, w:] = full[:, w - 1:w]
        a, (h, w) = full, full.shape[:2]
    blocks = a.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(h // 4, w // 4, 16, 3)
    lum = blocks @ np.array([2, 5, 1])
    hi = np.take_along_axis(blocks, lum.argmax(axis=2)[..., None, None], axis=2)[:, :, 0]
    lo = np.take_along_axis(blocks, lum.argmin(axis=2)[..., None, None], axis=2)[:, :, 0]

    def pack(c):
        return ((c[..., 0] >> 3) << 11) | ((c[..., 1] >> 2) << 5) | (c[..., 2] >> 3)

    def unpack(v):
        r, g, bl = (v >> 11) & 31, (v >> 5) & 63, v & 31
        return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (bl << 3) | (bl >> 2)], axis=-1)

    c0, c1 = pack(hi), pack(lo)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)   # c0 > c1 is the four-colour mode; equal ends: every index 0
    e0, e1 = unpack(c0), unpack(c1)
    pal = np.stack([e0, e1, (2 * e0 + e1) // 3, (e0 + 2 * e1) // 3], axis=2)              # blocks x 4 x 3
    dist = ((blocks[:, :, :, None, :] - pal[:, :, None, :, :]) ** 2).sum(axis=4)         # blocks x 16 x 4
    idx = dist.argmin(axis=3)
    idx[c0 == c1] = 0
    bits = (idx << (2 * np.arange(16))).sum(axis=2)
    out = np.zeros((h // 4, w // 4), dtype=[("c0", "<u2"), ("c1", "<u2"), ("bits", "<u4")])
    out["c0"], out["c1"], out["bits"] = c0, c1, bits
    return out.tobytes()


def dds_dxt1(img):
    """A DXT1 .dds with the full chain of smaller levels, as every texture of the game's own has."""
    from PIL import Image
    w, h = img.size
    levels, cur = [], img.convert("RGB")
    while True:
        levels.append(dxt1(cur))
        if cur.width == 1 and cur.height == 1:
            break
        cur = cur.resize((max(cur.width // 2, 1), max(cur.height // 2, 1)), Image.BOX)
    head = bytearray(128)
    struct.pack_into("<4s7I", head, 0, b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, h, w, len(levels[0]), 0, len(levels))
    struct.pack_into("<2I4s", head, 76, 32, 0x4, b"DXT1")       # pixel format: size, FOURCC flag
    struct.pack_into("<I", head, 108, 0x1000 | 0x8 | 0x400000)  # caps: texture, complex, with smaller levels
    return bytes(head) + b"".join(levels)


def tpf(textures):
    """[(name, dds bytes)] as a PC texture pack, laid out as the game's own are."""
    out = bytearray(16 + 20 * len(textures))
    names = []
    for name, _ in textures:
        names.append(len(out))
        out += name.encode("shift_jis") + b"\0"
    while len(out) % 4:
        out.append(0)
    start = len(out)
    for i, (name, dds) in enumerate(textures):
        struct.pack_into("<IiBBBBIi", out, 16 + 20 * i, len(out), len(dds), 0, 0, 0, 0, names[i], 0)   # format 0: DXT1
        out += dds
        while len(out) % 4:
            out.append(0)
    struct.pack_into("<4sii4B", out, 0, b"TPF\0", len(out) - start, len(textures), 0, 3, 2, 0)
    return bytes(out)


def bnd3(template, files):
    """A part archive holding `files` [(id, name, bytes)], with the header fields of the game's own."""
    assert template[:4] == b"BND3"
    head = bytearray(template[:0x20])
    table = bytearray()
    names = bytearray()
    name_base = 0x20 + 24 * len(files)
    name_offs = []
    for _, name, _ in files:
        name_offs.append(name_base + len(names))
        names += name.encode("shift_jis") + b"\0"
    data_base = name_base + len(names)
    data_base += -data_base % 16
    blob = bytearray()
    for i, (fid, name, data) in enumerate(files):
        off = data_base + len(blob)
        table += struct.pack("<IiIiIi", 0x40, len(data), off, fid, name_offs[i], len(data))
        blob += data
        if i + 1 < len(files):
            blob += bytes(-len(blob) % 16)        # (the last file is not padded: found by rebuilding the game's own archives)
    struct.pack_into("<i", head, 0x10, len(files))
    struct.pack_into("<i", head, 0x14, name_base + len(names))
    out = bytes(head) + bytes(table) + bytes(names)
    out += bytes(data_base - len(out))
    return out + bytes(blob)


# ---- the part models
def tangent_for(n):
    """Any unit vector square to the normal (the bump maps used are flat, so only its being square matters)."""
    ref = np.where(np.abs(n[:, 1:2]) < 0.9, np.array([[0.0, 1.0, 0.0]]), np.array([[1.0, 0.0, 0.0]]))
    t = np.cross(ref, n)
    return t / np.maximum(np.linalg.norm(t, axis=1, keepdims=True), 1e-9)


def build_parts(v1, pos, nrm, weights, flip_winding):
    """{part: (Flver, [texture names used])}"""
    out = {}
    part_of_vertex = [PART_OF[w[0][0]] for w in weights]
    for part, tname in TEMPLATES.items():
        path = os.path.join(F.GAME_DIR, "parts", tname + ".partsbnd.dcx")
        f = F.flver_of(path)
        # Every piece carries a bone table of its own, and the game takes the pose its vertices are bound in
        # from that table, not from the skeleton: in the game's own pieces the table is right (to a few
        # millimetres) for exactly the bones the piece's meshes use, and nonsense for the rest (the bare
        # body's "Head" sits at the feet). The first build kept the bare pieces' tables and gave the body
        # piece V1's head, bound to that Head: in the game the head was nowhere to be seen. So the table is
        # now the skeleton's own, whole, with its true parents (as the files of an armour mod found in the
        # folder have it), and every bone in it is right.
        f.bones = [dict(bn) for bn in ds_skeleton()[0]]
        bone_index = {bn["name"]: i for i, bn in enumerate(f.bones)}
        layout = next(i for i, lay in enumerate(f.layouts) if any(x["semantic"] == F.S_WEIGHTS for x in lay) and any(x["semantic"] == F.S_TANGENT for x in lay))
        materials, meshes, used_tex = [], [], []
        lo, hi = np.full(3, 1e9), np.full(3, -1e9)
        for tex_name, tris in v1["subs"]:
            # a triangle goes to the part most of its corners are in
            votes = np.array([[part_of_vertex[i] == part for i in tri] for tri in tris]).sum(axis=1) if len(tris) else np.zeros(0)
            mine = tris[votes >= 2]
            if not len(mine):
                continue
            # one mesh for every material; meshes of the same texture are merged
            slot = next((m for m in meshes if m["_tex"] == tex_name), None)
            if slot is None:
                slot = {"_tex": tex_name, "_tris": []}
                meshes.append(slot)
            slot["_tris"].append(mine)
        built = []
        for slot in meshes:
            tris = np.concatenate(slot["_tris"])
            vids = np.unique(tris)
            remap = {int(v): i for i, v in enumerate(vids)}
            names = sorted({n for v in vids for n, _ in weights[int(v)]})
            assert all(n in bone_index for n in names), [n for n in names if n not in bone_index]
            assert len(names) <= 38, len(names)
            local = {n: i for i, n in enumerate(names)}
            bi = np.zeros((len(vids), 4), dtype=np.int64)
            bw = np.zeros((len(vids), 4))
            for row, v in enumerate(vids):
                for c, (n, w) in enumerate(weights[int(v)]):
                    bi[row, c] = local[n]
                    bw[row, c] = w
            n4 = np.concatenate([nrm[vids], np.zeros((len(vids), 1))], axis=1)
            t4 = np.concatenate([tangent_for(nrm[vids]), np.ones((len(vids), 1))], axis=1)
            arrays = {F.S_POSITION: pos[vids], F.S_BONES: bi, F.S_WEIGHTS: bw, F.S_NORMAL: n4, F.S_TANGENT: t4,
                      F.S_COLOR: np.ones((len(vids), 4)), F.S_UV: v1["uv"][vids]}
            idx = np.array([[remap[int(i)] for i in tri] for tri in tris], dtype=np.int64)
            if flip_winding:
                idx = idx[:, ::-1]
            # the plain "one picture, lit" material, written as the game's own bare-head piece (HD_M_0000) has it
            mat = {"name": "V1_" + slot["_tex"], "mtd": "N:\\FRPG\\data\\INTERROOT_x64\\mtd\\parts\\P[D].mtd",
                   "flags": 0x106, "unk18": 0, "textures": [
                       {"type": "g_Diffuse", "path": slot["_tex"], "scale": (1.0, 1.0), "unk10": 1, "unk11": 1, "unk14": 0.0, "unk18": 0.0, "unk1c": 0.0},
                       {"type": "g_Specular", "path": "v1_flat_s", "scale": (1.0, 1.0), "unk10": 1, "unk11": 1, "unk14": 0.0, "unk18": 0.0, "unk1c": 0.0},
                       {"type": "g_Bumpmap", "path": "v1_flat_n", "scale": (1.0, 1.0), "unk10": 1, "unk11": 1, "unk14": 0.0, "unk18": 0.0, "unk1c": 0.0},
                       {"type": "g_DetailBumpmap", "path": "", "scale": (1.0, 1.0), "unk10": 0, "unk11": 0, "unk14": 0.0, "unk18": 0.0, "unk1c": 0.0}]}
            p = pos[vids]
            lo, hi = np.minimum(lo, p.min(axis=0)), np.maximum(hi, p.max(axis=0))
            built.append({"dynamic": 1, "material": len(materials), "default_bone": bone_index[names[0]], "bone_indices": [bone_index[n] for n in names],
                          "bb": tuple(p.min(axis=0)) + tuple(p.max(axis=0)),
                          "face_sets": [{"flags": 0, "strip": 0, "cull": 1, "unk06": 0, "indices": idx.reshape(-1).astype("<u2")}],
                          "vbuffers": [{"layout": layout, "data": F.encode_vertices(f.layouts[layout], arrays)}]})
            materials.append(mat)
            if slot["_tex"] not in used_tex:
                used_tex.append(slot["_tex"])
        f.materials = materials
        f.meshes = built
        if built:
            f.bb_min, f.bb_max = tuple(lo), tuple(hi)
        out[part] = (f, used_tex)
    return out


def winding_differs(v1, pos, nrm):
    """Whether Dark Souls' triangles run the other way round from Unity's: the game's own body mesh is
    asked which way its triangles face compared with its normals, and V1's mesh is asked the same."""
    f = F.flver_of(os.path.join(F.GAME_DIR, "parts", "BD_M_0000.partsbnd.dcx"))
    m = f.meshes[0]
    v = f.vertices(m)
    idx = m["face_sets"][0]["indices"].astype(np.int64)
    p, n = v[F.S_POSITION], v[F.S_NORMAL][:, :3]
    sign = 0.0
    flip = False
    for i in range(len(idx) - 2):
        a, b, c = idx[i], idx[i + 1], idx[i + 2]
        if 0xFFFF in (a, b, c):
            flip = False if c == 0xFFFF else flip      # a strip starts again after the marker
            if a == 0xFFFF:
                flip = False
            continue
        if a != b and b != c and a != c:
            g = np.cross(p[b] - p[a], p[c] - p[a])
            if flip:
                g = -g
            sign += float(np.sign(g @ (n[a] + n[b] + n[c])))
        flip = not flip
    tris = np.concatenate([t for _, t in v1["subs"]])
    g = np.cross(pos[tris[:, 1]] - pos[tris[:, 0]], pos[tris[:, 2]] - pos[tris[:, 0]])
    mine = float(np.sign((g * (nrm[tris[:, 0]] + nrm[tris[:, 1]] + nrm[tris[:, 2]])).sum(axis=1)).sum())
    return (sign > 0) != (mine > 0), sign, mine


def cmd_build():
    from PIL import Image
    v1 = load_v1()
    pos, nrm, info = fit(v1)
    weights = ds_weights(v1)
    flip, theirs, mine = winding_differs(v1, pos, nrm)
    print("scale %.4f (V1's units to metres), trunk stretched %.3f upward; segments along their length: %s" % (
        info["scale"], info["trunk"], ", ".join("%s %.2f" % x for x in info["segments"])))
    print("fitted model: %s to %s" % (np.round(pos.min(axis=0), 3), np.round(pos.max(axis=0), 3)))
    print("triangle winding: Dark Souls' own %+.0f, V1's %+.0f -> %s" % (theirs, mine, "turned round" if flip else "kept"))
    parts = build_parts(v1, pos, nrm, weights, flip)
    os.makedirs(OUT, exist_ok=True)
    tex = {}
    for name, img in v1["textures"]:
        big = img.resize((img.width * TEX_SCALE, img.height * TEX_SCALE), Image.NEAREST)
        tex[name] = dds_dxt1(big)
    tex["v1_flat_s"] = dds_dxt1(Image.new("RGB", (4, 4), (24, 24, 24)))
    tex["v1_flat_n"] = dds_dxt1(Image.new("RGB", (4, 4), (128, 128, 255)))
    for part, (f, used) in parts.items():
        tname = TEMPLATES[part]
        src = open(os.path.join(F.GAME_DIR, "parts", tname + ".partsbnd.dcx"), "rb").read()
        template = dcx_decompress(src)
        model = f.write()
        check = F.Flver(model)                                   # it must at least read back
        assert check.write() == model
        pack = tpf([(n, tex[n]) for n in used + ["v1_flat_s", "v1_flat_n"]])
        for file_name, folder, model_name in out_names(part):
            files = [(100, "N:\\FRPG\\data\\INTERROOT_x64\\parts\\%s\\%s.tpf" % (folder, folder), pack),
                     (200, "N:\\FRPG\\data\\INTERROOT_x64\\parts\\%s\\%s.flver" % (folder, model_name), model)]
            raw = bnd3(template, files)
            back = F.Bnd3(raw)                                   # the archive must read back, with the model in it unchanged
            assert [back.data(e) for e in back.entries] == [pack, model]
            packed = dcx_compress(src, raw)
            assert dcx_decompress(packed) == raw
            open(os.path.join(OUT, file_name), "wb").write(packed)
        print("%s: %d meshes, %d vertices, %d triangles, bones per mesh %s, textures %s, %d KB" % (
            out_names(part)[0][0], len(f.meshes), sum(len(m["vbuffers"][0]["data"]) // 40 for m in f.meshes), sum(len(m["face_sets"][0]["indices"]) // 3 for m in f.meshes),
            [len(m["bone_indices"]) for m in f.meshes], used, len(raw) // 1024))
    return v1, pos, nrm, weights, flip


# ---- a picture of the result, without the game
def render(pos, nrm, uv, tris_by_tex, textures, view, size=(520, 720), box=((-0.95, 0.95), (-0.05, 1.85))):
    from PIL import Image
    w, h = size
    img = np.zeros((h, w, 3), dtype=np.float64) + np.array([0.33, 0.34, 0.37])
    depth = np.full((h, w), -1e9)
    # view: a rotation about the vertical; the eye looks along -Z of the turned model (so 0 is from the front)
    c, s = np.cos(view), np.sin(view)
    R = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    p = pos @ R.T
    n = nrm @ R.T
    (x0, x1), (y0, y1) = box
    sx = (p[:, 0] - x0) / (x1 - x0) * w
    sy = (y1 - p[:, 1]) / (y1 - y0) * h
    light = np.array([0.3, 0.5, -0.8])
    light /= np.linalg.norm(light)
    for tname, tris in tris_by_tex:
        tex = np.asarray(textures[tname].convert("RGB"), dtype=np.float64) / 255.0
        th, tw = tex.shape[:2]
        for a, b, cc in tris:
            xs, ys = sx[[a, b, cc]], sy[[a, b, cc]]
            minx, maxx = int(max(np.floor(xs.min()), 0)), int(min(np.ceil(xs.max()), w - 1))
            miny, maxy = int(max(np.floor(ys.min()), 0)), int(min(np.ceil(ys.max()), h - 1))
            if maxx < minx or maxy < miny:
                continue
            den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(den) < 1e-9:
                continue
            gx, gy = np.meshgrid(np.arange(minx, maxx + 1) + 0.5, np.arange(miny, maxy + 1) + 0.5)
            l0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / den
            l1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / den
            l2 = 1.0 - l0 - l1
            inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
            if not inside.any():
                continue
            z = -(l0 * p[a, 2] + l1 * p[b, 2] + l2 * p[cc, 2])       # nearer to the eye is larger
            sub = depth[miny:maxy + 1, minx:maxx + 1]
            win = inside & (z > sub)
            if not win.any():
                continue
            u = l0 * uv[a, 0] + l1 * uv[b, 0] + l2 * uv[cc, 0]
            v = l0 * uv[a, 1] + l1 * uv[b, 1] + l2 * uv[cc, 1]
            col = tex[np.clip((v % 1.0 * th).astype(int), 0, th - 1), np.clip((u % 1.0 * tw).astype(int), 0, tw - 1)]
            nn = l0[..., None] * n[a] + l1[..., None] * n[b] + l2[..., None] * n[cc]
            nn /= np.maximum(np.linalg.norm(nn, axis=2, keepdims=True), 1e-9)
            shade = 0.45 + 0.55 * np.clip(-(nn @ light) * -1.0, 0, 1)
            sub[win] = z[win]
            img[miny:maxy + 1, minx:maxx + 1][win] = (col * shade[..., None])[win]
    return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))


def posed(pos, nrm, weights, turns):
    """The fitted mesh with some Dark Souls bones turned: {bone name: (axis, degrees)} in the model's own axes."""
    bones, index, W = ds_skeleton()
    local = [bone_local(bn) for bn in bones]
    for name, (axis, deg) in turns.items():
        i = index[name]
        a = np.radians(deg)
        axis = np.asarray(axis, dtype=np.float64)
        K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
        Rw = np.eye(3) + np.sin(a) * K + (1 - np.cos(a)) * K @ K        # about a world axis through the joint
        parent = W[bones[i]["parent"]][:3, :3] if bones[i]["parent"] >= 0 else np.eye(3)
        local[i] = local[i].copy()
        local[i][:3, :3] = parent.T @ Rw @ parent @ local[i][:3, :3]
    new = []
    for i, bn in enumerate(bones):
        new.append(new[bn["parent"]] @ local[i] if bn["parent"] >= 0 else local[i])
    skin = [new[i] @ np.linalg.inv(W[i]) for i in range(len(bones))]
    p = np.zeros_like(pos)
    n = np.zeros_like(nrm)
    for v in range(len(pos)):
        for name, w in weights[v]:
            m = skin[index[name]]
            p[v] += w * (m[:3, :3] @ pos[v] + m[:3, 3])
            n[v] += w * (m[:3, :3] @ nrm[v])
    return p, n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)


def cmd_preview():
    from PIL import Image
    v1, pos, nrm, weights, flip = cmd_build()
    textures = dict(v1["textures"])
    tris = [(t, x) for t, x in v1["subs"]]
    shots = [("bind", pos, nrm)]
    # arms brought down to the sides, one knee and one elbow bent, the head turned: the pieces should follow their joints
    down = {"L_UpperArm": ((0, 0, 1), -75), "R_UpperArm": ((0, 0, 1), 75), "R_Forearm": ((0, 1, 0), -70), "L_Thigh": ((1, 0, 0), 55),
            "L_Calf": ((1, 0, 0), -70), "Head": ((0, 1, 0), 35), "Spine1": ((1, 0, 0), -10)}
    p2, n2 = posed(pos, nrm, weights, down)
    shots.append(("posed", p2, n2))
    sheets = []
    for name, p, n in shots:
        row = [render(p, n, v1["uv"], tris, textures, a) for a in (0.0, np.pi / 2, np.pi, np.pi * 0.25)]
        sheet = Image.new("RGB", (sum(r.width for r in row), row[0].height))
        x = 0
        for r in row:
            sheet.paste(r, (x, 0))
            x += r.width
        path = os.path.join(OUT, "preview_%s.png" % name)
        sheet.save(path)
        print("wrote", os.path.normpath(path))


def game_running():
    import subprocess
    return "darksoulsremastered" in subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()


def cmd_install(remove=False):
    import shutil
    if game_running():
        sys.exit("Dark Souls is running: nothing was changed.")
    for part in TEMPLATES:
        for file_name, _, _ in out_names(part):
            dst = os.path.join(PARTS_DIR, file_name)
            if remove:
                if os.path.exists(dst):
                    os.remove(dst)
                    print("removed", dst)
                continue
            src = os.path.join(OUT, file_name)
            if not os.path.exists(src):
                sys.exit("not built yet: run 'build' first (%s is missing)" % file_name)
            shutil.copyfile(src, dst)
            print("installed", dst)
    # the params decide whether the pieces are worn: they are rebuilt to match what is in the folder now
    import subprocess
    subprocess.run([sys.executable, os.path.join(HERE, "patch_v1.py"), "--install"], check=True)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "build":
        cmd_build()
    elif cmd == "preview":
        cmd_preview()
    elif cmd in ("install", "remove"):
        cmd_install(remove=cmd == "remove")
    else:
        sys.exit(__doc__)
