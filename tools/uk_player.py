"""Find V1's saved movement values in ULTRAKILL's asset bundles.

Looks for the NewMovement component by its first serialized field (walkSpeed = 750) and prints the
floats after it, plus the Rigidbody and collider on the same GameObject.
Needs: pip install UnityPy
usage: python tools/uk_player.py [bundle name substring]
"""
import glob
import os
import struct
import sys

import UnityPy

ROOT = r"C:\Program Files (x86)\Steam\steamapps\common\ULTRAKILL\ULTRAKILL_Data\StreamingAssets\aa\StandaloneWindows64"
SKIP = ("music", "sounds", "textures", "models", "shaders")
NEEDLE = struct.pack("<f", 750.0)


def scan(path):
    env = UnityPy.load(path)
    found = False
    for obj in env.objects:
        if obj.type.name != "MonoBehaviour":
            continue
        raw = obj.get_raw_data()
        i = raw.find(NEEDLE)
        if i == -1 or i > 0x40 or i % 4:
            continue
        floats = struct.unpack_from("<8f", raw, i)
        # walkSpeed, airAcceleration, jumpPower, wallJumpPower are all positive and round
        if not all(1.0 <= v <= 100000.0 for v in floats[:4]):
            continue
        found = True
        print(f"{os.path.basename(path)}: MonoBehaviour path_id {obj.path_id}, {len(raw)} bytes, first field at {i:#x}")
        print("  floats:", [round(v, 4) for v in floats])
        go_path = struct.unpack_from("<q", raw, 4)[0]   # m_GameObject PPtr: file id (int32), path id (int64)
        go = obj.assets_file.objects.get(go_path)
        if go is None:
            print("  GameObject not in this file")
            continue
        tree = go.read_typetree()
        print("  GameObject:", tree.get("m_Name"))
        for comp in tree.get("m_Component", []):
            ref = comp["component"]
            c = obj.assets_file.objects.get(ref["m_PathID"])
            if c is None or c.type.name not in ("Rigidbody", "CapsuleCollider", "BoxCollider", "SphereCollider", "Transform"):
                continue
            t = c.read_typetree()
            keep = {k: v for k, v in t.items() if k in ("m_Mass", "m_Drag", "m_UseGravity", "m_Radius", "m_Height", "m_Center", "m_Size", "m_LocalScale", "m_LocalPosition")}
            print("  ", c.type.name, keep)
    return found


def main():
    want = sys.argv[1].lower() if len(sys.argv) > 1 else ""
    paths = sorted(glob.glob(ROOT + r"\**\*.bundle", recursive=True), key=os.path.getsize)
    for p in paths:
        name = os.path.basename(p).lower()
        if want and want not in name:
            continue
        if not want and any(s in name for s in SKIP):
            continue
        try:
            if scan(p) and not want:
                break
        except Exception as e:   # some bundles need features UnityPy lacks; keep going
            print(f"{os.path.basename(p)}: {type(e).__name__}: {e}")


if __name__ == "__main__":
    main()
