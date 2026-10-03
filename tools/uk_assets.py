"""Extract the ULTRAKILL assets the mod's HUD and viewmodel use, from your own copy of the game.

Nothing extracted is committed: it all goes under build/ (git-ignored) and into one pack file that
is copied next to the Dark Souls exe.

  python tools/uk_assets.py hud        sprites and fonts referenced by the HUD layout (run uk_hud_dump.py first)
  python tools/uk_assets.py pack [--install]   build the pack file: HUD art, font, and the revolver viewmodel
  python tools/uk_assets.py find NAME  list objects whose name contains NAME, across all bundles

Needs: pip install UnityPy
"""
import glob
import json
import os
import sys

import UnityPy

from uk_bundles import ROOT, cab_index

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "..", "build")
ASSETS = os.path.join(BUILD, "assets")

_envs = {}
_objects = {}


def load_bundle(path):
    """Environment for a bundle, plus an index of its objects by (asset file name, path id)."""
    if path not in _envs:
        print(f"loading {os.path.basename(path)} ({os.path.getsize(path) // 1000000} MB)", flush=True)
        env = UnityPy.load(path)
        _envs[path] = env
        _objects[path] = {(o.assets_file.name.lower(), o.path_id): o for o in env.objects}
    return _envs[path]


def resolve(ref):
    """Follow a {"file", "path_id"} reference from the HUD dump to the object it names."""
    cab = ref["file"].lower()
    bundle = cab_index().get(cab)
    if not bundle:
        return None
    load_bundle(bundle)
    return _objects[bundle].get((cab, ref["path_id"]))


def follow(obj, pptr):
    """Follow a PPtr found inside `obj`'s own data."""
    if not pptr or not pptr.get("m_PathID"):
        return None
    fid = pptr["m_FileID"]
    af = obj.assets_file
    return resolve({"file": af.externals[fid - 1].name if fid else af.name, "path_id": pptr["m_PathID"]})


def safe(name):
    return "".join(c if c.isalnum() or c in "-_." else "_" for c in name)


def hud_refs():
    d = json.load(open(os.path.join(BUILD, "uk_hud.json"), encoding="utf-8"))
    sprites, fonts = {}, {}

    def walk(n):
        for dr in n.get("draws", []):
            if "image" in dr and dr["image"].get("sprite"):
                r = dr["image"]["sprite"]
                sprites[(r["file"], r["path_id"])] = r
            if "text" in dr and dr["text"].get("font"):
                r = dr["text"]["font"]
                fonts[(r["file"], r["path_id"])] = r
        for c in n["children"]:
            walk(c)

    for t in d.values():
        walk(t["tree"])
    return list(sprites.values()), list(fonts.values())


def export_sprite(obj, out_dir):
    t = obj.read_typetree()
    name = t["m_Name"]
    img = obj.read().image
    path = os.path.join(out_dir, safe(name) + ".png")
    img.save(path)
    b = t.get("m_Border", {})
    return {"name": name, "path_id": obj.path_id, "file": os.path.basename(path), "w": img.width, "h": img.height,
            "border": [b.get("x", 0), b.get("y", 0), b.get("z", 0), b.get("w", 0)],   # left, bottom, right, top
            "ppu": t.get("m_PixelsToUnits", 100.0)}


def export_font(obj, out_dir):
    t = obj.read_typetree()
    name = t["m_Name"]
    face = t["m_FaceInfo"]
    atlas = follow(obj, t["m_AtlasTextures"][0])
    img = atlas.read().image
    path = os.path.join(out_dir, safe(name) + ".png")
    img.save(path)
    glyphs = {g["m_Index"]: g for g in t["m_GlyphTable"]}
    chars = {}
    for c in t["m_CharacterTable"]:
        g = glyphs.get(c["m_GlyphIndex"])
        if g is None:
            continue
        m, r = g["m_Metrics"], g["m_GlyphRect"]
        chars[c["m_Unicode"]] = {"x": r["m_X"], "y": r["m_Y"], "w": r["m_Width"], "h": r["m_Height"],
                                 "bx": m["m_HorizontalBearingX"], "by": m["m_HorizontalBearingY"], "adv": m["m_HorizontalAdvance"],
                                 "mw": m["m_Width"], "mh": m["m_Height"], "scale": g.get("m_Scale", 1.0)}
    return {"name": name, "path_id": obj.path_id, "file": os.path.basename(path), "atlas_w": img.width, "atlas_h": img.height,
            "point_size": face["m_PointSize"], "line_height": face["m_LineHeight"], "ascent": face["m_AscentLine"],
            "descent": face["m_DescentLine"], "baseline": face["m_Baseline"], "padding": t.get("m_AtlasPadding"),
            "render_mode": t.get("m_AtlasRenderMode"), "scale": face.get("m_Scale", 1.0), "chars": chars}


def cmd_hud():
    sprites, fonts = hud_refs()
    out_s, out_f = os.path.join(ASSETS, "sprites"), os.path.join(ASSETS, "fonts")
    os.makedirs(out_s, exist_ok=True)
    os.makedirs(out_f, exist_ok=True)
    meta = {"sprites": [], "fonts": []}
    for r in sprites:
        o = resolve(r)
        if o is None:
            print("unresolved sprite", r)
            continue
        m = export_sprite(o, out_s)
        meta["sprites"].append(m)
        print(f"sprite {m['name']!r} {m['w']}x{m['h']} border {m['border']} id ...{str(m['path_id'])[-6:]}")
    for r in fonts:
        o = resolve(r)
        if o is None:
            print("unresolved font", r)
            continue
        m = export_font(o, out_f)
        meta["fonts"].append(m)
        print(f"font {m['name']!r} atlas {m['atlas_w']}x{m['atlas_h']} point size {m['point_size']} glyphs {len(m['chars'])} mode {m['render_mode']} padding {m['padding']}")
    json.dump(meta, open(os.path.join(ASSETS, "hud_assets.json"), "w", encoding="utf-8"), indent=1)


def cmd_find(needle, types=("GameObject", "Mesh", "Texture2D", "Sprite", "Material", "AnimationClip")):
    needle = needle.lower()
    for path in sorted(glob.glob(ROOT + r"\**\*.bundle", recursive=True), key=os.path.getsize):
        base = os.path.basename(path).lower()
        if any(k in base for k in ("music", "sounds", "shaders")) or "scenes" in base:
            continue
        env = load_bundle(path)
        for o in env.objects:
            if o.type.name not in types:
                continue
            try:
                name = o.peek_name() or ""
            except Exception:
                continue
            if needle in name.lower():
                print(f"{os.path.basename(path)}  {o.type.name:14} {name!r}  file {o.assets_file.name} id {o.path_id}")
        _envs.pop(path, None)
        _objects.pop(path, None)


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "hud":
        cmd_hud()
    elif len(sys.argv) >= 3 and sys.argv[1] == "find":
        cmd_find(sys.argv[2])
    elif len(sys.argv) < 2 or sys.argv[1] != "pack":
        print(__doc__)


# ---------------------------------------------------------------- viewmodel

REVOLVER_PREFAB = ("cab-29dbe2ce332a0e8d30a4abde8b59a690", -8143948484576890295)   # "Revolver Pierce"


def _trs(t):
    """Unity local position / rotation (quaternion) / scale -> 4x4 matrix (column vectors)."""
    import numpy as np
    p, q, s = t["m_LocalPosition"], t["m_LocalRotation"], t["m_LocalScale"]
    x, y, z, w = q["x"], q["y"], q["z"], q["w"]
    r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = r * np.array([s["x"], s["y"], s["z"]])
    m[:3, 3] = [p["x"], p["y"], p["z"]]
    return m


def prefab_nodes(root_ref):
    """Walk a prefab: {transform path id: {name, world matrix, components, parent}} with the root's
    own local transform included (so 'world' is the space of the prefab's parent)."""
    cab, pid = root_ref
    bundle = cab_index()[cab]
    load_bundle(bundle)
    objs = _objects[bundle]
    nodes = {}

    def visit(go, parent_world, parent_id):
        gt = go.read_typetree()
        comps = [objs.get((cab, c["component"]["m_PathID"])) for c in gt["m_Component"]]
        tr = next(c for c in comps if c is not None and c.type.name in ("Transform", "RectTransform"))
        tt = tr.read_typetree()
        world = parent_world @ _trs(tt)
        nodes[tr.path_id] = {"name": gt["m_Name"], "world": world, "local": tt, "components": [c for c in comps if c is not None],
                             "parent": parent_id, "active": gt.get("m_IsActive", True)}
        for ch in tt["m_Children"]:
            cto = objs.get((cab, ch["m_PathID"]))
            if cto is not None:
                visit(objs[(cab, cto.read_typetree()["m_GameObject"]["m_PathID"])], world, tr.path_id)

    import numpy as np
    visit(objs[(cab, pid)], np.eye(4), None)
    return nodes


def skinned_meshes(nodes):
    """Bake every SkinnedMeshRenderer in the prefab into the prefab parent's space, in the pose the
    prefab is saved in. Returns [{name, positions, normals, uvs, indices, material object}]."""
    import numpy as np
    out = []
    for node in nodes.values():
        for c in node["components"]:
            if c.type.name != "SkinnedMeshRenderer":
                continue
            t = c.read_typetree()
            mesh_obj = follow(c, t["m_Mesh"])
            mesh = mesh_obj.read()
            from UnityPy.helpers.MeshHelper import MeshHandler
            h = MeshHandler(mesh)
            h.process()
            pos = np.array(h.m_Vertices, dtype=np.float64).reshape(-1, 3)
            nrm = np.array(h.m_Normals, dtype=np.float64).reshape(-1, 3) if h.m_Normals else np.zeros_like(pos)
            uv = np.array(h.m_UV0, dtype=np.float64).reshape(len(pos), -1)[:, :2] if h.m_UV0 else np.zeros((len(pos), 2))
            # Bone influences: 1, 2 or 4 per vertex. With one bone per vertex there are no weights at all.
            bone_i = np.array(h.m_BoneIndices, dtype=np.int64).reshape(len(pos), -1)
            if h.m_BoneWeights:
                bone_w = np.array(h.m_BoneWeights, dtype=np.float64).reshape(len(pos), -1)
            else:
                bone_w = np.zeros(bone_i.shape)
                bone_w[:, 0] = 1.0
            binds = [np.array([[getattr(b, f"e{r}{col}") for col in range(4)] for r in range(4)], dtype=np.float64) for b in mesh.m_BindPose]
            bones = [nodes[b["m_PathID"]]["world"] for b in t["m_Bones"]]
            skin = [bones[i] @ binds[i] for i in range(len(bones))]
            hp = np.concatenate([pos, np.ones((len(pos), 1))], axis=1)
            out_p = np.zeros((len(pos), 3))
            out_n = np.zeros((len(pos), 3))
            for k in range(bone_i.shape[1]):
                w = bone_w[:, k:k + 1]
                m = np.array([skin[i] for i in bone_i[:, k]])                 # (n, 4, 4)
                out_p += w * np.einsum("nij,nj->ni", m, hp)[:, :3]
                out_n += w * np.einsum("nij,nj->ni", m[:, :3, :3], nrm)
            ln = np.linalg.norm(out_n, axis=1, keepdims=True)
            out_n = out_n / np.where(ln > 1e-9, ln, 1.0)
            tris = []
            for sub in h.get_triangles():
                tris.extend(sub)
            idx = np.array(tris, dtype=np.int64).reshape(-1)
            mat = follow(c, t["m_Materials"][0]) if t["m_Materials"] else None
            out.append({"name": node["name"], "positions": out_p, "normals": out_n, "uvs": uv, "indices": idx, "material": mat,
                        "enabled": bool(t.get("m_Enabled", 1)) and node["active"]})
    return out


def material_texture(mat):
    """(texture object, colour) for a material's main texture."""
    if mat is None:
        return None, [1, 1, 1, 1]
    t = mat.read_typetree()
    props = t["m_SavedProperties"]
    tex = None
    for name, env in props["m_TexEnvs"]:
        if name == "_MainTex":
            tex = follow(mat, env["m_Texture"])
    color = [1, 1, 1, 1]
    for name, col in props["m_Colors"]:
        if name == "_Color":
            color = [col["r"], col["g"], col["b"], col["a"]]
    return tex, color


# ---------------------------------------------------------------- pack file

PACK = os.path.join(BUILD, "ultrasouls_assets.bin")
GAME_DIR = r"F:\SteamLibrary\steamapps\common\DARK SOULS REMASTERED"


def _name(s):
    b = s.encode("ascii", "replace")[:39]
    return b + b"\0" * (40 - len(b))


def _rgba(img):
    """RGBA bytes, top row first. Alpha-only art (all pixels black) becomes white, as Unity's UI shader treats it."""
    img = img.convert("RGBA")
    data = bytearray(img.tobytes())
    if not any(data[i] or data[i + 1] or data[i + 2] for i in range(0, len(data), 4)):
        for i in range(0, len(data), 4):
            data[i] = data[i + 1] = data[i + 2] = 255
    return bytes(data)


def cmd_pack(install=False):
    """Everything the DLL draws, in one file:
         textures: name[40], w, h, RGBA
         sprites:  name[40], texture index, border (left, bottom, right, top) px, pixels per unit
         fonts:    name[40], texture index, point size, line height, ascent, descent, baseline, padding, atlas w, h, glyphs
                   glyph: code point, atlas x, y(from top), w, h, bearing x, bearing y, advance
         meshes:   name[40], texture index, vertex count, index count, vertices (pos3 normal3 uv2), indices u32
    """
    import struct
    from PIL import Image
    meta = json.load(open(os.path.join(ASSETS, "hud_assets.json"), encoding="utf-8"))
    textures, sprites, fonts, meshes = [], [], [], []

    def add_texture(name, img):
        textures.append((name, img.width, img.height, _rgba(img)))
        return len(textures) - 1

    for s in meta["sprites"]:
        img = Image.open(os.path.join(ASSETS, "sprites", s["file"]))
        sprites.append((s["name"], add_texture(s["name"], img), s["border"], s["ppu"]))
    for f in meta["fonts"]:
        img = Image.open(os.path.join(ASSETS, "fonts", f["file"]))
        glyphs = [(int(code), g["x"], f["atlas_h"] - (g["y"] + g["h"]), g["w"], g["h"], g["bx"], g["by"], g["adv"]) for code, g in f["chars"].items()]
        fonts.append((f["name"], add_texture(f["name"], img), f, glyphs))

    nodes = prefab_nodes(REVOLVER_PREFAB)
    tex_index = {}
    os.makedirs(os.path.join(ASSETS, "models"), exist_ok=True)
    for m in skinned_meshes(nodes):
        tex, _color = material_texture(m["material"])
        tname = tex.peek_name() if tex else "white"
        if tname not in tex_index:
            img = tex.read().image if tex else Image.new("RGBA", (4, 4), (255, 255, 255, 255))
            img.save(os.path.join(ASSETS, "models", safe(tname) + ".png"))
            tex_index[tname] = add_texture(tname, img)
        import numpy as np
        uv = m["uvs"].copy()
        uv[:, 1] = 1.0 - uv[:, 1]          # Unity's V runs bottom-up, the stored image is top-down
        verts = np.concatenate([m["positions"], m["normals"], uv], axis=1).astype("<f4")
        meshes.append((m["name"], tex_index[tname], verts, m["indices"].astype("<u4")))

    out = bytearray(b"USPACK01")
    out += struct.pack("<I", len(textures))
    for name, w, h, data in textures:
        out += _name(name) + struct.pack("<II", w, h) + data
    out += struct.pack("<I", len(sprites))
    for name, tex, border, ppu in sprites:
        out += _name(name) + struct.pack("<I5f", tex, *border, ppu)
    out += struct.pack("<I", len(fonts))
    for name, tex, f, glyphs in fonts:
        out += _name(name) + struct.pack("<I8fI", tex, f["point_size"], f["line_height"], f["ascent"], f["descent"], f["baseline"],
                                         f["padding"], f["atlas_w"], f["atlas_h"], len(glyphs))
        for g in glyphs:
            out += struct.pack("<I7f", *g)
    out += struct.pack("<I", len(meshes))
    for name, tex, verts, idx in meshes:
        out += _name(name) + struct.pack("<III", tex, len(verts), len(idx)) + verts.tobytes() + idx.tobytes()
    open(PACK, "wb").write(out)
    print(f"{len(textures)} textures, {len(sprites)} sprites, {len(fonts)} fonts, {len(meshes)} meshes -> {os.path.normpath(PACK)} ({len(out) // 1024} KB)")
    if install:
        import shutil
        shutil.copyfile(PACK, os.path.join(GAME_DIR, "ultrasouls_assets.bin"))
        print("copied to", GAME_DIR)


if __name__ == "__main__" and len(sys.argv) >= 2 and sys.argv[1] == "pack":
    cmd_pack(install="--install" in sys.argv)
