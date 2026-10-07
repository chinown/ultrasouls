"""Animated first-person models for the mod, taken from your own copy of ULTRAKILL: the Piercer revolver
(held in V1's right arm) and the Feedbacker arm, each with its skeleton, skinned meshes and animation clips.

  python tools/uk_models.py pack [--install]   write build/ultrasouls_models.bin (--install copies it next to the exe)
  python tools/uk_models.py info               list nodes, meshes and clips

Nothing from ULTRAKILL is stored in this repository: the pack is built on your machine from your install.

Pack layout ("USMDL002", count, then per model):
  name[40]
  nodes:    count, then name[40], parent index (-1 for the root), rest pose (position 3, rotation xyzw, scale 3)
  textures: count, then name[40], width, height, RGBA
  meshes:   count, then name[40], texture index, vertex count, index count, bone count,
            vertices (position 3, normal 3, uv 2), bone slots (4 x u16), weights (4 x f32), indices (u32),
            bones (node index, then the bind matrix's top three rows, 12 f32)
  clips:    count, then name[40], frames per second, frame count, then for every frame, for every node,
            position 3, rotation xyzw, scale 3
  screens:  count, then name[40], node index, texture index (-1: plain colour), fill kind, colour rgba,
            four corners (bottom-left, top-left, top-right, bottom-right), each position 3 and uv 2

A screen is a flat panel fixed to one node: the little displays on the weapons (the Piercer's battery
monitor, the coin meters of the other two revolvers, the shotgun's core meter). Its corners are in the
node's own space. Fill kind is the edge a meter's filled part starts from: 0 for a panel that is not a
meter, 1 left, 2 right, 3 bottom, 4 top. The names tell the DLL which is which: "pierce.monitor",
"marksman.coin0" ... "sharp.coin2", "core.fill"; everything else is decoration ("marksman.bg1"). The sawblade launcher's: "magnet.sink0" to
"magnet.sink2" (a meter for each magnet), "magnet.ammo" (where the number of saws is written: a panel with no
picture), "overheat.fill" (the heat), "overheat.heatbg" (the frame round it, which takes the heat's colour) and
"overheat.sink0" (the heat sink).

Animation clips are stored in Unity's runtime form: each animated value is a curve, and curves come in
three kinds laid out one after the other (streamed: cubic segments keyed in time; dense: plain samples;
constant: one value). A binding names a transform by the CRC32 of its path below the Animator and says
whether its curves are a position (3), a rotation quaternion (4) or a scale (3). The clips are sampled
here at 60 frames per second, so the DLL only has to blend between two frames.
"""
import math
import os
import struct
import sys
import zlib

import numpy as np

import uk_assets
from uk_assets import follow, load_bundle, material_texture, prefab_nodes, safe
from uk_bundles import cab_index

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "..", "build")
PACK = os.path.join(BUILD, "ultrasouls_models.bin")
GAME_DIR = r"F:\SteamLibrary\steamapps\common\DARK SOULS REMASTERED"
SAMPLE_FPS = 60.0
CAB = uk_assets.REVOLVER_PREFAB[0]
MODELS = {"revolver": "Revolver Pierce", "feedbacker": "Arm Blue", "knuckleblaster": "Arm Red",
          "shotgun": "Shotgun Grenade", "revolver_alt": "Alternative Revolver Pierce",
          "railcannon": "Railcannon Electric", "whiplash": "Hook Arm",
          "sawlauncher": "Sawblade Launcher Magnet"}     # pack name -> prefab root object
# The displays on each weapon: model -> [(name prefix, the prefab they are read from)]. The three
# revolvers share one rig, so the Marksman's and the Sharpshooter's displays are fixed to the same bone of
# the one revolver model that is packed.
SCREENS = {"revolver": [("pierce", "Revolver Pierce"), ("marksman", "Revolver Ricochet"), ("sharp", "Revolver Twirl")],
           "revolver_alt": [("pierce", "Alternative Revolver Pierce"), ("marksman", "Alternative Revolver Ricochet"), ("sharp", "Alternative Revolver Twirl")],
           "shotgun": [("core", "Shotgun Grenade"), ("pump", "Shotgun Pump")],
           "sawlauncher": [("magnet", "Sawblade Launcher Magnet"), ("overheat", "Sawblade Launcher Overheat")]}
BATTERY = (("batteryFull", "battery_full"), ("batteryMid", "battery_mid"), ("batteryLow", "battery_low"))
# Meshes of ULTRAKILL's effects, packed as models of one fixed part: pack name -> (prefab root, object in it).
# Each is scaled so that its furthest point is 1 from its middle; the DLL gives it the size the effect has.
PROPS = {"fx_sphere": ("Explosion", "Sphere_8"),          # the explosion's ball of fire (its texture scrolls)
         "fx_sphere_super": ("Explosion Super", "Sphere_8"),   # the super explosion's, with its own redder picture
         "fx_shock": ("Explosion", "Sphere_8 (1)"),       # the faint shell that runs ahead of it
         "fx_coin": ("Coin", "Model"),                    # the Marksman's coin
         "fx_core": ("Grenade", "Grenade")}               # the shotgun's ejected core
# Rigid parts of a weapon that are not skinned to its skeleton: model -> [(prefab root, object in it)]. A part
# hangs from the model's node of its own name or, where the model has none, from the nearest node above it
# that the model has by name (the Overheat's second, glowing blade 'Blade (1)' sits on 'Blade', a tenth larger).
RIGID = {"sawlauncher": [("Sawblade Launcher Magnet", "Blade"), ("Sawblade Launcher Overheat", "Blade (1)")]}
# Things that fly through the world, each a prefab kept whole: in ULTRAKILL's units about its own root and
# in the root's axes, so the DLL only has to say where the root is and which way it points. An object with
# a Spin script keeps a node of its own (the saw's teeth turn on the hub).
#   a sawblade: a hub ('Cylinder', a ProBuilder mesh whose points are in the prefab itself, not in a mesh
#   asset) with a flat picture of the teeth on it ('Quad', Unity's own quad), lying in the root's x-z plane
#   the Attractor's magnet: the harpoon, pointing along the root's z
FLYERS = {"fx_saw": "NailAltFodder", "fx_saw_overheat": "NailAlt", "fx_saw_heated": "NailAltHeated", "fx_harpoon": "Harpoon"}


def find_root(name):
    """(asset file, path id) of the prefab root object called `name` (a GameObject whose transform has no parent)."""
    env = load_bundle(cab_index()[CAB])
    inner = None
    for o in env.objects:
        if o.type.name != "GameObject":
            continue
        t = o.read_typetree()
        if t["m_Name"] != name:
            continue
        for c in t["m_Component"]:
            tr = follow(o, c["component"])
            if tr is not None and tr.type.name == "Transform":
                if tr.read_typetree()["m_Father"]["m_PathID"] == 0:
                    return (o.assets_file.name.lower(), o.path_id)
                if inner is None:
                    inner = (o.assets_file.name.lower(), o.path_id)
    # The whiplash is not a prefab of its own: its 'Hook Arm' object is part of the player's, under the
    # 'Punch' object the other arms are put under when they are equipped. The first one found is taken.
    if inner is not None:
        return inner
    raise SystemExit("prefab root not found: " + name)


def trs_of(local):
    p, r, s = local["m_LocalPosition"], local["m_LocalRotation"], local["m_LocalScale"]
    return [p["x"], p["y"], p["z"], r["x"], r["y"], r["z"], r["w"], s["x"], s["y"], s["z"]]


def streamed_frames(words):
    """[(time, [(curve index, (c0, c1, c2, c3))])]: at `time` each listed curve starts a new cubic segment."""
    buf = struct.pack("<%dI" % len(words), *words)
    off, frames = 0, []
    while off + 8 <= len(buf):
        time, n = struct.unpack_from("<fi", buf, off)
        off += 8
        keys = []
        for _ in range(n):
            idx, c0, c1, c2, c3 = struct.unpack_from("<i4f", buf, off)
            off += 20
            keys.append((idx, (c0, c1, c2, c3)))
        frames.append((time, keys))
    return frames


def sample_clip(clip_tree, paths, rest):
    """(fps, array [frame, node, 10]) for one AnimationClip. `paths` maps a path's CRC32 to a node index."""
    mc = clip_tree["m_MuscleClip"]
    data = mc["m_Clip"]["data"]
    st, de, co = data["m_StreamedClip"], data["m_DenseClip"], data["m_ConstantClip"]
    n_st, n_de = st["curveCount"], de["m_CurveCount"]
    frames_st = streamed_frames(st["data"])
    start, stop = mc["m_StartTime"], mc["m_StopTime"]
    n_frames = max(2, int(round((stop - start) * SAMPLE_FPS)) + 1)
    out = np.tile(np.array(rest, dtype=np.float64), (n_frames, 1, 1))
    bindings = clip_tree["m_ClipBindingConstant"]["genericBindings"]
    # the streamed curves are walked forward in time once, keeping each curve's current segment
    seg = [None] * n_st
    cursor = 0
    unknown = 0
    for f in range(n_frames):
        t = start + (stop - start) * f / (n_frames - 1)
        while cursor < len(frames_st) and frames_st[cursor][0] <= t:
            ftime, keys = frames_st[cursor]
            for idx, c in keys:
                if 0 <= idx < n_st:
                    seg[idx] = (ftime, c)
            cursor += 1

        def value(ci):
            if ci < n_st:
                s = seg[ci]
                if s is None:
                    return 0.0
                dt = t - s[0]
                if not math.isfinite(dt) or dt > 1e6:
                    return s[1][3]
                c0, c1, c2, c3 = s[1]
                return ((c0 * dt + c1) * dt + c2) * dt + c3
            if ci < n_st + n_de:
                k = ci - n_st
                pos = (t - de["m_BeginTime"]) * de["m_SampleRate"]
                i0 = min(max(int(pos), 0), de["m_FrameCount"] - 1)
                i1 = min(i0 + 1, de["m_FrameCount"] - 1)
                a, b = de["m_SampleArray"][i0 * n_de + k], de["m_SampleArray"][i1 * n_de + k]
                return a + (b - a) * (pos - i0)
            return co["data"][ci - n_st - n_de]

        ci = 0
        for b in bindings:
            attr = b["attribute"]
            width = 4 if attr == 2 else 3 if attr in (1, 3, 4) else 1
            node = paths.get(b["path"])
            if node is None:
                if f == 0:
                    unknown += 1
            elif attr == 1:
                out[f, node, 0:3] = [value(ci + j) for j in range(3)]
            elif attr == 2:
                q = np.array([value(ci + j) for j in range(4)])
                ln = np.linalg.norm(q)
                out[f, node, 3:7] = q / ln if ln > 1e-9 else [0, 0, 0, 1]
            elif attr == 3:
                out[f, node, 7:10] = [value(ci + j) for j in range(3)]
            ci += width
    if unknown:
        print("    (%d bindings name transforms that are not in the prefab)" % unknown)
    return SAMPLE_FPS, out


def build_model(name, root_name):
    root = find_root(root_name)
    nodes = prefab_nodes(root)
    order = list(nodes.keys())                               # depth first, parents before children
    index = {pid: i for i, pid in enumerate(order)}
    node_rows = [(nodes[pid]["name"], index.get(nodes[pid]["parent"], -1), trs_of(nodes[pid]["local"])) for pid in order]
    rest = [row[2] for row in node_rows]

    # The clips name each transform by the CRC32 of its path below some object. That object is not always
    # the one whose Animator holds the controller: the Feedbacker's controller sits on the prefab root,
    # but its clips' paths start at the inner 'Feedbacker' object, which has an Animator of its own.
    # So every object with an Animator is tried and the one whose paths match the most bindings is used.
    controller, animators = None, []
    for pid in order:
        for c in nodes[pid]["components"]:
            if c.type.name == "Animator":
                animators.append(pid)
                ctrl = follow(c, c.read_typetree().get("m_Controller"))
                if ctrl is not None and controller is None:
                    controller = ctrl

    def paths_below(base):
        found = {0: index[base]}

        def walk(pid, prefix):
            for child in order:
                if nodes[child]["parent"] == pid:
                    path = prefix + nodes[child]["name"]
                    found[zlib.crc32(path.encode("utf-8")) & 0xFFFFFFFF] = index[child]
                    walk(child, path + "/")
        walk(base, "")
        return found

    paths = {}
    if controller is not None:
        first = None
        for cp in controller.read_typetree().get("m_AnimationClips", []):
            first = follow(controller, cp)
            if first is not None:
                break
        wanted = {b["path"] for b in first.read_typetree()["m_ClipBindingConstant"]["genericBindings"]} if first is not None else set()
        best = -1
        # (the whiplash's start at 'GreenArmFinal', which has no Animator at all: every object is tried,
        # those with an Animator first)
        for base in animators + [pid for pid in order if pid not in animators]:
            cand = paths_below(base)
            score = len(wanted & set(cand))
            if score > best:
                best, paths = score, cand
                base_name = nodes[base]["name"]
        print("    animation paths start at '%s' (%d of %d bindings matched)" % (base_name, best, len(wanted)))

    def shown(pid):
        # an object is drawn only if it and everything above it, short of the prefab root, is active
        while pid is not None and nodes[pid]["parent"] is not None:
            if not nodes[pid]["active"]:
                return False
            pid = nodes[pid]["parent"]
        return True

    textures, tex_index, meshes = [], {}, []
    from PIL import Image
    from UnityPy.helpers.MeshHelper import MeshHandler
    for pid in order:
        node = nodes[pid]
        for c in node["components"]:
            if c.type.name != "SkinnedMeshRenderer":
                continue
            t = c.read_typetree()
            if not t.get("m_Enabled", 1) or not shown(pid):
                continue
            mesh = follow(c, t["m_Mesh"]).read()
            h = MeshHandler(mesh)
            h.process()
            pos = np.array(h.m_Vertices, dtype=np.float64).reshape(-1, 3)
            nrm = np.array(h.m_Normals, dtype=np.float64).reshape(-1, 3) if h.m_Normals else np.zeros_like(pos)
            uv = np.array(h.m_UV0, dtype=np.float64).reshape(len(pos), -1)[:, :2] if h.m_UV0 else np.zeros((len(pos), 2))
            uv = uv.copy()
            uv[:, 1] = 1.0 - uv[:, 1]                         # Unity's V runs bottom-up, the stored image is top-down
            bone_i = np.array(h.m_BoneIndices, dtype=np.int64).reshape(len(pos), -1)
            if h.m_BoneWeights:
                bone_w = np.array(h.m_BoneWeights, dtype=np.float64).reshape(len(pos), -1)
            else:
                bone_w = np.zeros(bone_i.shape)
                bone_w[:, 0] = 1.0
            slots = np.zeros((len(pos), 4), dtype="<u2")
            weights = np.zeros((len(pos), 4), dtype="<f4")
            k = min(4, bone_i.shape[1])
            slots[:, :k] = bone_i[:, :k]
            weights[:, :k] = bone_w[:, :k]
            total = weights.sum(axis=1, keepdims=True)
            weights = weights / np.where(total > 1e-9, total, 1.0)
            binds = [np.array([[getattr(b, f"e{r}{col}") for col in range(4)] for r in range(4)], dtype=np.float64) for b in mesh.m_BindPose]
            bones = [(index[b["m_PathID"]], binds[i][:3, :].reshape(-1)) for i, b in enumerate(t["m_Bones"])]
            # One mesh entry per look: the parts of a mesh are grouped by their material's texture and
            # colour, and a coloured material gets its own tinted copy of the texture. (Until v0.63 every
            # part was drawn with the first material's texture: the shotgun's black screen and its orange
            # heat sinks came out in the gun's own brown.)
            looks = {}
            for si, sub in enumerate(h.get_triangles()):
                smat = follow(c, t["m_Materials"][si]) if si < len(t["m_Materials"]) else None
                # A part drawn with an additive particle material only shows while the game lights it up
                # (the shotgun's heat glow: its tint's alpha is driven by code), so it is left out.
                if smat is not None and any(n == "_TintColor" for n, _ in smat.read_typetree()["m_SavedProperties"]["m_Colors"]):
                    print("    %s: part %d (%s) left out" % (node["name"], si, smat.read_typetree()["m_Name"]))
                    continue
                tex, color = material_texture(smat)
                rgb = tuple(int(round(min(max(x, 0.0), 1.0) * 255)) for x in color[:3])
                tname = (tex.peek_name() if tex else "white") + ("" if rgb == (255, 255, 255) else "_%02x%02x%02x" % rgb)
                if tname not in tex_index:
                    img = (tex.read().image if tex else Image.new("RGBA", (4, 4), (255, 255, 255, 255))).convert("RGBA")
                    if rgb != (255, 255, 255):
                        r, g, bl, al = img.split()
                        img = Image.merge("RGBA", (r.point(lambda v: v * rgb[0] // 255), g.point(lambda v: v * rgb[1] // 255), bl.point(lambda v: v * rgb[2] // 255), al))
                    tex_index[tname] = len(textures)
                    textures.append((tname, img.width, img.height, img.tobytes()))
                looks.setdefault(tname, []).extend(sub)
                # The lights on a gun: ULTRAKILL's shader adds the material's _EmissiveTex, times a colour
                # the game sets (the variation's) and an intensity. The picture is packed beside the main
                # one under the same name with "__emissive" after it, which is how the DLL finds it.
                if smat is not None and tname + "__emissive" not in tex_index:
                    emis = None
                    for pname, env in smat.read_typetree()["m_SavedProperties"]["m_TexEnvs"]:
                        if pname == "_EmissiveTex":
                            emis = follow(smat, env["m_Texture"])
                    if emis is not None:
                        eimg = emis.read().image.convert("RGBA")
                        tex_index[tname + "__emissive"] = len(textures)
                        textures.append((tname + "__emissive", eimg.width, eimg.height, eimg.tobytes()))
                        print("    %s: lights from %s %dx%d" % (node["name"], emis.peek_name(), eimg.width, eimg.height))
                used = sorted({i for tri in sub for i in tri})
                lo, hi = pos[used].min(axis=0), pos[used].max(axis=0)
                print("    %s part %d: %s, %d triangles, box %s to %s" % (node["name"], si, tname, len(sub), np.round(lo, 3), np.round(hi, 3)))
            verts = np.concatenate([pos, nrm, uv], axis=1).astype("<f4")
            for tname, tris in looks.items():
                meshes.append((node["name"], tex_index[tname], verts, slots, weights.astype("<f4"), np.array(tris, dtype="<u4").reshape(-1), bones))

    clips = []
    base_paths = {}
    if controller is not None:
        seen = set()
        for cp in controller.read_typetree().get("m_AnimationClips", []):
            clip = follow(controller, cp)
            if clip is None:
                continue
            tree = clip.read_typetree()
            cname = tree["m_Name"]
            if cname in seen or tree["m_MuscleClip"]["m_StopTime"] > 6.0:      # the 12 s shop idle is not needed
                continue
            # two clips share the name "Idle" on the arm: the first one listed is the animated arm's
            seen.add(cname)
            # Each clip is matched to the object its own paths start at. One controller can hold clips made
            # for different objects: the Knuckleblaster's Hook is written against another base than its
            # Punch, and sampled against the wrong one it left the arm frozen in its rest pose.
            wanted = {b["path"] for b in tree["m_ClipBindingConstant"]["genericBindings"]}
            clip_paths, score = paths, len(wanted & set(paths))
            for base in animators + [pid for pid in order if pid not in animators]:
                cand = base_paths.get(base)
                if cand is None:
                    cand = base_paths[base] = paths_below(base)
                if len(wanted & set(cand)) > score:
                    clip_paths, score = cand, len(wanted & set(cand))
            if score * 2 < len(wanted):
                # a clip for some other object that only shares the controller: left out, so nothing can play it
                print("    %s: only %d of %d bindings matched; left out" % (cname, score, len(wanted)))
                continue
            fps, frames = sample_clip(tree, clip_paths, rest)
            clips.append((cname, fps, frames.astype("<f4")))
            # the clip's events are where ULTRAKILL's code is called from mid-animation (a gun being ready
            # again, a sound); the DLL's timings are copied from this list
            events = ["%s %.2fs" % (e["functionName"], e["time"]) for e in tree.get("m_Events", [])]
            if events:
                print("    %s events: %s" % (cname, ", ".join(events)))
    return name, node_rows, textures, meshes, clips


def _script(c):
    """(class name, fields) of a script component, or (None, None)."""
    if c.type.name != "MonoBehaviour":
        return None, None
    try:
        t = c.read_typetree()
        s = follow(c, t.get("m_Script"))
        return (s.read_typetree().get("m_ClassName") if s is not None else None), t
    except Exception:
        return None, None


def build_screens(prefix, root_name, node_rows, textures, tex_index):
    """The displays of one weapon prefab, as [(name, node index, texture index, fill kind, rgba, corners 4x5)]."""
    from PIL import Image
    from UnityPy.helpers.MeshHelper import MeshHandler
    nodes = prefab_nodes(find_root(root_name))
    model_names = [row[0] for row in node_rows]
    sizes = {}

    def rect_size(pid):
        if pid not in sizes:
            t = nodes[pid]["local"]
            if "m_SizeDelta" not in t:
                sizes[pid] = None
            else:
                par = rect_size(nodes[pid]["parent"]) if nodes[pid]["parent"] is not None else None
                pw, ph = par if par else (0.0, 0.0)
                sizes[pid] = (pw * (t["m_AnchorMax"]["x"] - t["m_AnchorMin"]["x"]) + t["m_SizeDelta"]["x"],
                              ph * (t["m_AnchorMax"]["y"] - t["m_AnchorMin"]["y"]) + t["m_SizeDelta"]["y"])
        return sizes[pid]

    worlds = {}

    def world_of(pid):
        """A node's transform in the prefab's space. A RectTransform's saved local position only has its
        z right: x and y are worked out from its anchors when the game loads it, so they are here too."""
        if pid not in worlds:
            node = nodes[pid]
            t = node["local"]
            if "m_SizeDelta" not in t:
                worlds[pid] = node["world"]
            else:
                par = node["parent"]
                psize = rect_size(par) if par is not None else None
                x0 = y0 = pw = ph = 0.0
                if psize:
                    pp = nodes[par]["local"]["m_Pivot"]
                    pw, ph = psize
                    x0, y0 = -pp["x"] * pw, -pp["y"] * ph
                amin, amax, piv, pos = t["m_AnchorMin"], t["m_AnchorMax"], t["m_Pivot"], t["m_AnchoredPosition"]
                fixed = dict(t)
                fixed["m_LocalPosition"] = {"x": x0 + (amin["x"] + (amax["x"] - amin["x"]) * piv["x"]) * pw + pos["x"],
                                            "y": y0 + (amin["y"] + (amax["y"] - amin["y"]) * piv["y"]) * ph + pos["y"],
                                            "z": t["m_LocalPosition"]["z"]}
                worlds[pid] = (world_of(par) if par is not None else np.eye(4)) @ uk_assets._trs(fixed)
        return worlds[pid]

    def anchor_of(pid):
        # the nearest plain transform above that the packed model also has (by name)
        p = nodes[pid]["parent"]
        while p is not None:
            if "m_SizeDelta" not in nodes[p]["local"] and nodes[p]["name"] in model_names:
                return p
            p = nodes[p]["parent"]
        return None

    def shown(pid):
        while pid is not None and nodes[pid]["parent"] is not None:
            if not nodes[pid]["active"]:
                return False
            pid = nodes[pid]["parent"]
        return True

    def add_texture(tex, name):
        if name not in tex_index:
            img = tex.read().image.convert("RGBA")
            tex_index[name] = len(textures)
            textures.append((name, img.width, img.height, img.tobytes()))
        return tex_index[name]

    # which Image is which meter, from the weapon's own fields
    special, weapon, ammo_text = {}, None, None
    for pid, node in nodes.items():
        for c in node["components"]:
            cls, t = _script(c)
            if cls == "Nailgun":
                for i, ref in enumerate(t.get("heatSinkImages", [])):
                    special[ref["m_PathID"]] = ("sink%d" % i, None)
                ammo_text = t.get("ammoText", {}).get("m_PathID")
            if cls == "Revolver":
                weapon = (c, t)
                for i, ref in enumerate(t.get("coinPanels", [])):
                    special[ref["m_PathID"]] = ("coin%d" % i, None)
            elif cls == "Slider":
                # a slider moves its fill's right-hand anchor; the fill area is what a full meter covers
                fill = t.get("m_FillRect", {}).get("m_PathID")
                kind = {0: 1, 1: 2, 2: 3, 3: 4}.get(t.get("m_Direction", 0), 1)
                for q, n2 in nodes.items():
                    if q == fill:
                        for c2 in n2["components"]:
                            if _script(c2)[0] == "Image":
                                special[c2.path_id] = ("fill", kind)
    out, plain = [], 0
    for pid, node in nodes.items():
        if not shown(pid):
            continue
        anchor = anchor_of(pid)
        if anchor is None:
            continue
        to_anchor = np.linalg.inv(nodes[anchor]["world"]) @ world_of(pid)
        node_index = model_names.index(nodes[anchor]["name"])
        for c in node["components"]:
            cls, t = _script(c)
            if cls == "Image" and rect_size(pid) and t.get("m_Enabled", 1):
                w, h = rect_size(pid)
                piv = node["local"]["m_Pivot"]
                x0, y0 = -piv["x"] * w, -piv["y"] * h
                local = [(x0, y0, 0, 0), (x0, y0 + h, 0, 1), (x0 + w, y0 + h, 1, 1), (x0 + w, y0, 1, 0)]
                col = t["m_Color"]
                name, kind = special.get(c.path_id, (None, None))
                if name is None:
                    name, plain = "bg%d" % plain, plain + 1
                if kind is None:
                    kind = 0
                    if t.get("m_Type") == 3:              # a filled image: which edge it empties from
                        kind = {(0, 0): 1, (0, 1): 2, (1, 0): 3, (1, 1): 4}.get((t.get("m_FillMethod"), t.get("m_FillOrigin")), 1)
                corners = []
                for x, y, u, v in local:
                    p = to_anchor @ np.array([x, y, 0.0, 1.0])
                    corners.append([p[0], p[1], p[2], u, 1.0 - v])
                # the picture the panel is drawn with, if it has one of the game's own (Unity's built-in
                # ones are not in the bundles and stay plain rectangles)
                tex = -1
                sprite = follow(c, t["m_Sprite"]) if t.get("m_Sprite", {}).get("m_PathID") else None
                if sprite is not None and sprite.peek_name() == "HeatsinkMeter" and name.startswith("bg"):
                    name = "heatbg"                       # (the launcher's heat slider sits in it: Nailgun's sliderBg)
                if sprite is not None:
                    try:
                        tex = add_texture(sprite, "ui_" + sprite.peek_name())
                    except Exception:
                        tex = -1
                out.append((prefix + "." + name, node_index, tex, kind, [col["r"], col["g"], col["b"], col["a"]], corners))
                print("    screen %-18s on %-14s %.3g x %.3g units, colour (%.2f %.2f %.2f %.2f), fill kind %d" % (
                    prefix + "." + name, nodes[anchor]["name"], w, h, col["r"], col["g"], col["b"], col["a"], kind))
            elif cls == "Text" and ammo_text is not None and c.path_id == ammo_text and rect_size(pid):
                # where the launcher writes how many saws it has: the text's own box, and the size of its letters in it
                w, h = rect_size(pid)
                piv = node["local"]["m_Pivot"]
                x0, y0 = -piv["x"] * w, -piv["y"] * h
                corners = []
                for x, y, u, v in [(x0, y0, 0, 0), (x0, y0 + h, 0, 1), (x0 + w, y0 + h, 1, 1), (x0 + w, y0, 1, 0)]:
                    p = to_anchor @ np.array([x, y, 0.0, 1.0])
                    corners.append([p[0], p[1], p[2], u, 1.0 - v])
                col = t["m_Color"]
                out.append((prefix + ".ammo", node_index, -1, 0, [col["r"], col["g"], col["b"], t["m_FontData"]["m_FontSize"] / h], corners))
                print("    screen %-18s on %-14s %.3g x %.3g units, letters %d high, colour (%.2f %.2f %.2f)" % (
                    prefix + ".ammo", nodes[anchor]["name"], w, h, t["m_FontData"]["m_FontSize"], col["r"], col["g"], col["b"]))
            elif c.type.name == "MeshRenderer" and weapon is not None and (c.path_id == weapon[1].get("screenMR", {}).get("m_PathID") or node["name"] == "Monitor"):
                # (the alternate revolver's is called 'Monitor (1)': the weapon's own screenMR field says which it is)
                mf = next((m for m in node["components"] if m.type.name == "MeshFilter"), None)
                mref = follow(mf, mf.read_typetree()["m_Mesh"]) if mf is not None else None
                if mref is not None:
                    hnd = MeshHandler(mref.read())
                    hnd.process()
                    pos = np.array(hnd.m_Vertices, dtype=np.float64).reshape(-1, 3)
                    uv = np.array(hnd.m_UV0, dtype=np.float64).reshape(len(pos), -1)[:, :2]
                else:
                    # the mesh is Unity's built-in Quad, which is not in any bundle: one unit square in XY
                    pos = np.array([[-0.5, -0.5, 0], [-0.5, 0.5, 0], [0.5, 0.5, 0], [0.5, -0.5, 0]], dtype=np.float64)
                    uv = np.array([[0, 0], [0, 1], [1, 1], [1, 0]], dtype=np.float64)
                if len(pos) != 4:
                    print("    (the monitor is not a 4-corner panel: %d vertices; skipped)" % len(pos))
                    continue
                order = [min(range(4), key=lambda i, tu=tu, tv=tv: (uv[i][0] - tu) ** 2 + (uv[i][1] - tv) ** 2) for tu, tv in ((0, 0), (0, 1), (1, 1), (1, 0))]
                corners = []
                for i in order:
                    p = to_anchor @ np.array([pos[i][0], pos[i][1], pos[i][2], 1.0])
                    corners.append([p[0], p[1], p[2], uv[i][0], 1.0 - uv[i][1]])
                wt = weapon[1]
                first = -1
                for field, tname in BATTERY:
                    tex = follow(weapon[0], wt[field])
                    if tex is not None:
                        k = add_texture(tex, tname)
                        first = k if first < 0 else first
                for i, ref in enumerate(wt.get("batteryCharges", [])):
                    tex = follow(weapon[0], ref)
                    if tex is not None:
                        add_texture(tex, "battery_charge%d" % (i + 1))
                mat = follow(c, c.read_typetree()["m_Materials"][0])
                _tex, color = material_texture(mat)
                out.append((prefix + ".monitor", node_index, first, 0, list(color), corners))
                print("    screen %-18s on %-14s textured, material colour %s" % (prefix + ".monitor", nodes[anchor]["name"], [round(x, 2) for x in color]))
    return out


def build_prop(name, root_name, part_name):
    """One rigid mesh as a model: a single node, and every vertex fully on its one bone."""
    from PIL import Image
    from UnityPy.helpers.MeshHelper import MeshHandler
    nodes = prefab_nodes(find_root(root_name))
    pid = next(p for p, n in nodes.items() if n["name"] == part_name and any(c.type.name == "MeshFilter" for c in n["components"]))
    node = nodes[pid]
    mf = next(c for c in node["components"] if c.type.name == "MeshFilter")
    mr = next(c for c in node["components"] if c.type.name == "MeshRenderer")
    mesh = follow(mf, mf.read_typetree()["m_Mesh"]).read()
    h = MeshHandler(mesh)
    h.process()
    pos = np.array(h.m_Vertices, dtype=np.float64).reshape(-1, 3)
    nrm = np.array(h.m_Normals, dtype=np.float64).reshape(-1, 3) if h.m_Normals else np.zeros_like(pos)
    uv = np.array(h.m_UV0, dtype=np.float64).reshape(len(pos), -1)[:, :2].copy() if h.m_UV0 else np.zeros((len(pos), 2))
    uv[:, 1] = 1.0 - uv[:, 1]
    # the part's own proportions (the coin is a cylinder squashed flat), then unit size
    s = node["local"]["m_LocalScale"]
    pos = pos * np.array([s["x"], s["y"], s["z"]])
    # its real size, for the DLL's constants: the furthest point from its middle, in the prefab's units
    world_pts = (node["world"] @ np.concatenate([np.array(h.m_Vertices, dtype=np.float64).reshape(-1, 3), np.ones((len(pos), 1))], axis=1).T).T[:, :3]
    real = np.linalg.norm(world_pts - (world_pts.max(axis=0) + world_pts.min(axis=0)) / 2, axis=1).max()
    pos = pos - (pos.max(axis=0) + pos.min(axis=0)) / 2
    pos = pos / np.linalg.norm(pos, axis=1).max()
    tris = []
    for sub in h.get_triangles():
        tris.extend(sub)
    mat = follow(mr, mr.read_typetree()["m_Materials"][0])
    tex, color = material_texture(mat)
    img = (tex.read().image if tex else Image.new("RGBA", (4, 4), (255, 255, 255, 255))).convert("RGBA")
    textures = [(tex.peek_name() if tex else "white", img.width, img.height, img.tobytes())]
    verts = np.concatenate([pos, nrm, uv], axis=1).astype("<f4")
    slots = np.zeros((len(pos), 4), dtype="<u2")
    weights = np.zeros((len(pos), 4), dtype="<f4")
    weights[:, 0] = 1.0
    bind = np.eye(4)[:3, :].reshape(-1)
    meshes = [(part_name, 0, verts, slots, weights, np.array(tris, dtype="<u4").reshape(-1), [(0, bind)])]
    node_rows = [(name, -1, [0, 0, 0, 0, 0, 0, 1, 1, 1, 1])]
    print("    prop %-10s %d vertices, texture %s %dx%d, material colour %s, real radius %.4f units" % (
        name, len(pos), textures[0][0], img.width, img.height, [round(c, 2) for c in color], real))
    return name, node_rows, textures, meshes, []


def geometry_of(node):
    """(positions, normals, uvs with v downwards, triangle indices) of the mesh an object is drawn with, or None.
    The mesh is the MeshFilter's; failing that the object's ProBuilder data; failing that Unity's built-in quad."""
    from UnityPy.helpers.MeshHelper import MeshHandler
    mf = next((c for c in node["components"] if c.type.name == "MeshFilter"), None)
    ref = mf.read_typetree()["m_Mesh"] if mf is not None else None
    mesh = follow(mf, ref) if mf is not None else None
    if mesh is not None:
        h = MeshHandler(mesh.read())
        h.process()
        pos = np.array(h.m_Vertices, dtype=np.float64).reshape(-1, 3)
        nrm = np.array(h.m_Normals, dtype=np.float64).reshape(-1, 3) if h.m_Normals else np.zeros_like(pos)
        uv = np.array(h.m_UV0, dtype=np.float64).reshape(len(pos), -1)[:, :2].copy() if h.m_UV0 else np.zeros((len(pos), 2))
        tris = [i for sub in h.get_triangles() for tri in sub for i in tri]
    else:
        pb = next((t for cls, t in (_script(c) for c in node["components"]) if cls == "ProBuilderMesh"), None)
        if pb is not None:
            pos = np.array([[p["x"], p["y"], p["z"]] for p in pb["m_Positions"]], dtype=np.float64)
            uv = np.array([[p["x"], p["y"]] for p in pb.get("m_Textures0", [])], dtype=np.float64)
            if len(uv) != len(pos):
                uv = np.zeros((len(pos), 2))
            tris = [i for f in pb["m_Faces"] for i in f["m_Indexes"]]
            nrm = np.zeros_like(pos)
            for a, b, c in np.array(tris).reshape(-1, 3):
                n = np.cross(pos[b] - pos[a], pos[c] - pos[a])
                ln = np.linalg.norm(n)
                if ln > 1e-12:
                    nrm[[a, b, c]] += n / ln
            ln = np.linalg.norm(nrm, axis=1, keepdims=True)
            nrm = nrm / np.where(ln > 1e-9, ln, 1.0)
        elif ref is not None and ref.get("m_PathID") == 10210:      # the quad among Unity's default resources
            pos = np.array([[-0.5, -0.5, 0], [0.5, -0.5, 0], [-0.5, 0.5, 0], [0.5, 0.5, 0]], dtype=np.float64)
            uv = np.array([[0, 0], [1, 0], [0, 1], [1, 1]], dtype=np.float64)
            nrm = np.tile(np.array([0.0, 0.0, -1.0]), (4, 1))
            tris = [0, 2, 1, 2, 3, 1]
        else:
            return None
    uv = uv.copy()
    uv[:, 1] = 1.0 - uv[:, 1]
    return pos, nrm, uv, np.array(tris, dtype="<u4")


def add_look(mat, textures, tex_index):
    """The index, in `textures`, of the picture a material draws with, tinted by the material's colour."""
    from PIL import Image
    tex, color = material_texture(mat) if mat is not None else (None, (1, 1, 1, 1))
    rgb = tuple(int(round(min(max(x, 0.0), 1.0) * 255)) for x in color[:3])
    tname = (tex.peek_name() if tex else "white") + ("" if rgb == (255, 255, 255) else "_%02x%02x%02x" % rgb)
    if tname not in tex_index:
        img = (tex.read().image if tex else Image.new("RGBA", (4, 4), (255, 255, 255, 255))).convert("RGBA")
        if rgb != (255, 255, 255):
            r, g, bl, al = img.split()
            img = Image.merge("RGBA", (r.point(lambda v: v * rgb[0] // 255), g.point(lambda v: v * rgb[1] // 255), bl.point(lambda v: v * rgb[2] // 255), al))
        tex_index[tname] = len(textures)
        textures.append((tname, img.width, img.height, img.tobytes()))
    return tex_index[tname], tname, color


def _rigid_mesh(mname, tex, pos, nrm, uv, tris, bone_node):
    verts = np.concatenate([pos, nrm, uv], axis=1).astype("<f4")
    slots = np.zeros((len(pos), 4), dtype="<u2")
    weights = np.zeros((len(pos), 4), dtype="<f4")
    weights[:, 0] = 1.0
    return (mname, tex, verts, slots, weights, tris, [(bone_node, np.eye(4)[:3, :].reshape(-1))])


def _apply(m, pos, nrm):
    p = (m @ np.concatenate([pos, np.ones((len(pos), 1))], axis=1).T).T[:, :3]
    n = (m[:3, :3] @ nrm.T).T
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    return p, n / np.where(ln > 1e-9, ln, 1.0)


def rigid_parts(name, node_rows, textures, meshes):
    """Adds a model's RIGID parts to `meshes`, each wholly on one node."""
    names = [row[0] for row in node_rows]
    tex_index = {t[0]: i for i, t in enumerate(textures)}
    for root_name, part_name in RIGID.get(name, []):
        nodes = prefab_nodes(find_root(root_name))
        pid = next(p for p, n in nodes.items() if n["name"] == part_name and any(c.type.name == "MeshRenderer" for c in n["components"]))
        geo = geometry_of(nodes[pid])
        if geo is None:
            print("    %s: no mesh; left out" % part_name)
            continue
        pos, nrm, uv, tris = geo
        anchor = pid
        while anchor is not None and nodes[anchor]["name"] not in names:
            anchor = nodes[anchor]["parent"]
        if anchor is None:
            print("    %s: nothing of the model above it; left out" % part_name)
            continue
        pos, nrm = _apply(np.linalg.inv(nodes[anchor]["world"]) @ nodes[pid]["world"], pos, nrm)
        mr = next(c for c in nodes[pid]["components"] if c.type.name == "MeshRenderer")
        mat = follow(mr, mr.read_typetree()["m_Materials"][0])
        tex, tname, color = add_look(mat, textures, tex_index)
        meshes.append(_rigid_mesh(part_name, tex, pos, nrm, uv, tris, names.index(nodes[anchor]["name"])))
        print("    rigid part %-10s on %-8s %d vertices, %s, material colour %s, box %s to %s" % (
            part_name, nodes[anchor]["name"], len(pos), tname, [round(c, 3) for c in color], np.round(pos.min(axis=0), 3), np.round(pos.max(axis=0), 3)))


def _quat(r):
    """x y z w of a rotation matrix."""
    tr = r[0, 0] + r[1, 1] + r[2, 2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        q = [(r[2, 1] - r[1, 2]) / s, (r[0, 2] - r[2, 0]) / s, (r[1, 0] - r[0, 1]) / s, 0.25 * s]
    elif r[0, 0] > r[1, 1] and r[0, 0] > r[2, 2]:
        s = math.sqrt(1.0 + r[0, 0] - r[1, 1] - r[2, 2]) * 2
        q = [0.25 * s, (r[0, 1] + r[1, 0]) / s, (r[0, 2] + r[2, 0]) / s, (r[2, 1] - r[1, 2]) / s]
    elif r[1, 1] > r[2, 2]:
        s = math.sqrt(1.0 + r[1, 1] - r[0, 0] - r[2, 2]) * 2
        q = [(r[0, 1] + r[1, 0]) / s, 0.25 * s, (r[1, 2] + r[2, 1]) / s, (r[0, 2] - r[2, 0]) / s]
    else:
        s = math.sqrt(1.0 + r[2, 2] - r[0, 0] - r[1, 1]) * 2
        q = [(r[0, 2] + r[2, 0]) / s, (r[1, 2] + r[2, 1]) / s, 0.25 * s, (r[1, 0] - r[0, 1]) / s]
    return q


def build_flyer(name, root_name):
    """A whole prefab of rigid parts as one model, in the game's units about the root and in the root's axes."""
    nodes = prefab_nodes(find_root(root_name))
    root = next(p for p, n in nodes.items() if n["parent"] is None)
    rw = nodes[root]["world"]
    rot = rw[:3, :3] / np.linalg.norm(rw[:3, :3], axis=0)
    to_root = np.eye(4)                                      # undoes the root's place and turn, but not its size
    to_root[:3, :3] = rot.T
    to_root[:3, 3] = -rot.T @ rw[:3, 3]
    node_rows = [(name, -1, [0, 0, 0, 0, 0, 0, 1, 1, 1, 1])]
    textures, tex_index, meshes = [], {}, []
    for pid, node in nodes.items():
        p, on = pid, True
        while p is not None and nodes[p]["parent"] is not None:
            on = on and nodes[p]["active"]
            p = nodes[p]["parent"]
        mr = next((c for c in node["components"] if c.type.name == "MeshRenderer"), None)
        if not on or mr is None or not mr.read_typetree().get("m_Enabled", 1):
            continue
        geo = geometry_of(node)
        if geo is None:
            continue
        pos, nrm, uv, tris = geo
        m = to_root @ node["world"]
        mat = follow(mr, mr.read_typetree()["m_Materials"][0])
        tex, tname, color = add_look(mat, textures, tex_index)
        spin = next((t for cls, t in (_script(c) for c in node["components"]) if cls == "Spin"), None)
        bone = 0
        if spin is not None:
            # its own node, placed and turned as the object is; its size goes into the points
            scale = np.linalg.norm(m[:3, :3], axis=0)
            q = _quat(m[:3, :3] / scale)
            bone = len(node_rows)
            node_rows.append((node["name"], 0, [m[0, 3], m[1, 3], m[2, 3], q[0], q[1], q[2], q[3], 1, 1, 1]))
            pos = pos * scale
            d = spin["spinDirection"]
            print("    %s spins about its own (%g, %g, %g) at %g degrees a second" % (node["name"], d["x"], d["y"], d["z"], spin["speed"]))
        else:
            pos, nrm = _apply(m, pos, nrm)
        meshes.append(_rigid_mesh(node["name"], tex, pos, nrm, uv, tris, bone))
        box = pos if spin is None else _apply(m, pos / scale, nrm)[0]
        print("    %-10s %-10s %d vertices, %s, material colour %s, box %s to %s" % (
            name, node["name"], len(pos), tname, [round(c, 3) for c in color], np.round(box.min(axis=0), 3), np.round(box.max(axis=0), 3)))
    if not meshes:
        raise SystemExit("nothing to draw in " + root_name)
    return name, node_rows, textures, meshes, []


def _name(s):
    return s.encode("ascii", "replace").ljust(40, b"\0")[:40]


def cmd_pack(install=False):
    out = bytearray(b"USMDL002")
    out += struct.pack("<I", len(MODELS) + len(PROPS) + len(FLYERS))
    for name, root_name in list(MODELS.items()) + list(PROPS.items()) + list(FLYERS.items()):
        if name in PROPS:
            name, node_rows, textures, meshes, clips = build_prop(name, *root_name)
        elif name in FLYERS:
            name, node_rows, textures, meshes, clips = build_flyer(name, root_name)
        else:
            name, node_rows, textures, meshes, clips = build_model(name, root_name)
            rigid_parts(name, node_rows, textures, meshes)
        tex_index = {t[0]: i for i, t in enumerate(textures)}
        screens = []
        for prefix, prefab in SCREENS.get(name, []):
            screens += build_screens(prefix, prefab, node_rows, textures, tex_index)
        out += _name(name)
        out += struct.pack("<I", len(node_rows))
        for nname, parent, trs in node_rows:
            out += _name(nname) + struct.pack("<i10f", parent, *trs)
        out += struct.pack("<I", len(textures))
        for tname, w, h, data in textures:
            out += _name(tname) + struct.pack("<II", w, h) + data
        out += struct.pack("<I", len(meshes))
        for mname, tex, verts, slots, weights, idx, bones in meshes:
            out += _name(mname) + struct.pack("<iIII", tex, len(verts), len(idx), len(bones))
            out += verts.tobytes() + slots.tobytes() + weights.tobytes() + idx.tobytes()
            for node, bind in bones:
                out += struct.pack("<i12f", node, *bind)
        out += struct.pack("<I", len(clips))
        for cname, fps, frames in clips:
            out += _name(cname) + struct.pack("<fI", fps, frames.shape[0]) + frames.tobytes()
        out += struct.pack("<I", len(screens))
        for sname, node, tex, kind, rgba, corners in screens:
            out += _name(sname) + struct.pack("<iii4f", node, tex, kind, *rgba)
            for corner in corners:
                out += struct.pack("<5f", *corner)
        print("%-11s %d nodes, %d textures, %d meshes (%s), clips: %s" % (
            name, len(node_rows), len(textures), len(meshes), ", ".join("%s %d verts" % (m[0], len(m[2])) for m in meshes),
            ", ".join("%s %.2fs" % (c[0], (c[2].shape[0] - 1) / c[1]) for c in clips)))
    os.makedirs(BUILD, exist_ok=True)
    open(PACK, "wb").write(out)
    print("wrote", os.path.normpath(PACK), len(out) // 1024, "KB")
    if install:
        import shutil
        shutil.copy(PACK, os.path.join(GAME_DIR, "ultrasouls_models.bin"))
        print("installed next to the exe")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "pack":
        cmd_pack("--install" in sys.argv[2:])
    elif cmd == "info":
        for name, root_name in MODELS.items():
            name, node_rows, textures, meshes, clips = build_model(name, root_name)
            print(name, "nodes:", [(i, n[0], n[1]) for i, n in enumerate(node_rows)])
            print("  meshes:", [(m[0], len(m[2]), len(m[6])) for m in meshes], "clips:", [(c[0], c[2].shape) for c in clips])
    else:
        sys.exit(__doc__)
