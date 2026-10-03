"""Dump the layout of ULTRAKILL's in-game HUD from the Player object in a scene bundle.

Prints every node under the chosen roots with its RectTransform and what it draws (image sprite
reference, colour, fill; text, size, colour), so the HUD can be rebuilt from the game's own numbers.
usage: python tools/uk_hud_dump.py [root name ...]     (default: GunCanvas StyleCanvas Crosshair)
Needs: pip install UnityPy
"""
import json
import os
import sys

import UnityPy

from uk_bundles import ROOT

SCENE = os.path.join(ROOT, "specialscenes_scenes_earlyaccessend.bundle")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "uk_hud.json")


def vec(v, keys="xyzw"):
    return [round(v[k], 4) for k in keys if k in v]


def main():
    wanted = sys.argv[1:] or ["GunCanvas", "StyleCanvas", "Crosshair"]
    env = UnityPy.load(SCENE)
    objs = {o.path_id: o for o in env.objects}
    go = {}
    for o in env.objects:
        if o.type.name == "GameObject":
            t = o.read_typetree()
            go[o.path_id] = {"name": t["m_Name"], "active": bool(t.get("m_IsActive", True)),
                             "components": [c["component"]["m_PathID"] for c in t["m_Component"]]}
    tr = {}
    for o in env.objects:
        if o.type.name in ("Transform", "RectTransform"):
            tr[o.path_id] = o.read_typetree()

    def ref(p, owner):
        # a reference is (file id, path id); file id 0 is the owner's own file, n is its n-th external
        if not p or not p.get("m_PathID"):
            return None
        fid = p["m_FileID"]
        af = owner.assets_file
        return {"file": af.externals[fid - 1].name if fid else af.name, "path_id": p["m_PathID"]}

    def node(pid, depth):
        t = tr[pid]
        g = go[t["m_GameObject"]["m_PathID"]]
        n = {"name": g["name"], "active": g["active"], "depth": depth,
             "pos": vec(t["m_LocalPosition"]), "rot": vec(t["m_LocalRotation"]), "scale": vec(t["m_LocalScale"])}
        if "m_AnchorMin" in t:
            n.update(anchor_min=vec(t["m_AnchorMin"]), anchor_max=vec(t["m_AnchorMax"]),
                     anchored=vec(t["m_AnchoredPosition"]), size=vec(t["m_SizeDelta"]), pivot=vec(t["m_Pivot"]))
        draws = []
        for cid in g["components"]:
            c = objs.get(cid)
            if c is None or c.type.name not in ("MonoBehaviour", "Canvas", "Camera", "CanvasGroup"):
                continue
            try:
                ct = c.read_typetree()
            except Exception:
                continue
            if c.type.name == "Canvas":
                draws.append({"canvas": {k: ct.get(k) for k in ("m_RenderMode", "m_PlaneDistance", "m_SortingOrder")}})
            elif c.type.name == "Camera":
                draws.append({"camera": {"fov": ct.get("field of view"), "near": ct.get("near clip plane"), "ortho": ct.get("orthographic")}})
            elif c.type.name == "CanvasGroup":
                draws.append({"group_alpha": ct.get("m_Alpha")})
            elif "m_Sprite" in ct:
                draws.append({"image": {"sprite": ref(ct["m_Sprite"], c), "color": vec(ct["m_Color"], "rgba"), "type": ct.get("m_Type"),
                                        "fill_method": ct.get("m_FillMethod"), "fill_amount": ct.get("m_FillAmount"),
                                        "fill_origin": ct.get("m_FillOrigin"), "enabled": ct.get("m_Enabled"),
                                        "material": ref(ct.get("m_Material"), c), "preserve": ct.get("m_PreserveAspect"),
                                        "ppu_mult": ct.get("m_PixelsPerUnitMultiplier"), "fill_center": ct.get("m_FillCenter"),
                                        "clockwise": ct.get("m_FillClockwise")}})
            elif "m_text" in ct:
                draws.append({"text": {"text": ct["m_text"], "size": ct.get("m_fontSize"), "color": vec(ct.get("m_fontColor", {}), "rgba"),
                                       "align": ct.get("m_textAlignment"), "h": ct.get("m_HorizontalAlignment"), "v": ct.get("m_VerticalAlignment"),
                                       "font": ref(ct.get("m_fontAsset"), c), "enabled": ct.get("m_Enabled"), "style": ct.get("m_fontStyle"),
                                       "autosize": ct.get("m_enableAutoSizing"), "min": ct.get("m_fontSizeMin"), "max": ct.get("m_fontSizeMax")}})
            elif "m_Texture" in ct:
                draws.append({"rawimage": {"texture": ref(ct["m_Texture"], c), "color": vec(ct["m_Color"], "rgba"), "uv": ct.get("m_UVRect")}})
            else:
                keys = [k for k in ct if not k.startswith("m_")]
                if keys:
                    small = {k: ct[k] for k in keys if isinstance(ct[k], (int, float, bool, str))}
                    draws.append({"script": {"fields": keys[:14], "values": small}})
        if draws:
            n["draws"] = draws
        n["children"] = [node(c["m_PathID"], depth + 1) for c in t["m_Children"] if c["m_PathID"] in tr]
        return n

    out = {}
    for pid, t in tr.items():
        name = go[t["m_GameObject"]["m_PathID"]]["name"]
        if name in wanted and name not in out:
            # also record the chain of parents, for world-space canvases
            chain, cur = [], t
            while cur["m_Father"]["m_PathID"] in tr:
                cur = tr[cur["m_Father"]["m_PathID"]]
                g = go[cur["m_GameObject"]["m_PathID"]]
                entry = {"name": g["name"], "pos": vec(cur["m_LocalPosition"]), "rot": vec(cur["m_LocalRotation"]), "scale": vec(cur["m_LocalScale"])}
                for cid in g["components"]:
                    c = objs.get(cid)
                    if c is not None and c.type.name == "Camera":
                        ct = c.read_typetree()
                        entry["camera"] = {"fov": ct.get("field of view"), "near": ct.get("near clip plane"), "far": ct.get("far clip plane")}
                chain.append(entry)
            out[name] = {"parents": chain, "tree": node(pid, 0)}
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    json.dump(out, open(OUT, "w", encoding="utf-8"), indent=1)

    def show(n):
        pad = "  " * n["depth"]
        line = f"{pad}{n['name']}{'' if n['active'] else ' (off)'}"
        if "size" in n:
            line += f"  anchors {n['anchor_min']}-{n['anchor_max']} at {n['anchored']} size {n['size']} pivot {n['pivot']}"
        if n["scale"] != [1.0, 1.0, 1.0]:
            line += f" scale {n['scale']}"
        if n["rot"] != [0.0, 0.0, 0.0, 1.0]:
            line += f" rot {n['rot']}"
        print(line)
        for d in n.get("draws", []):
            print(f"{pad}    - {json.dumps(d, ensure_ascii=False)[:330]}")
        for c in n["children"]:
            show(c)

    for name, d in out.items():
        print(f"===== {name}; parents: " + " <- ".join(f"{p['name']} pos {p['pos']} rot {p['rot']} scale {p['scale']} {p.get('camera', '')}" for p in d["parents"]))
        show(d["tree"])


if __name__ == "__main__":
    main()
