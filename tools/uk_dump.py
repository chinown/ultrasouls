"""Look inside ULTRAKILL's prefabs: the numbers its code reads from the editor rather than from C#.

  python tools/uk_dump.py find TEXT          objects whose name contains TEXT, with their components
  python tools/uk_dump.py show NAME [N]      every component of the object called NAME (the N-th match), with
                                             its fields; references are shown as the kind and name they point to
  python tools/uk_dump.py show NAME NAME...  the same for several objects
  python tools/uk_dump.py tree NAME [N]      the object's children, with their components
  python tools/uk_dump.py deep NAME [N]      the object and everything under it, each with every component's fields

Reads your own install; nothing is written.
"""
import os
import sys

import uk_assets  # noqa: F401  (sets up the bundle paths)
from uk_assets import follow, load_bundle
from uk_bundles import cab_index


def prefab_env():
    idx = cab_index()
    path = next(b for b in idx.values() if os.path.basename(b) == "gameprefabs_assets_all.bundle")
    return load_bundle(path)


def comp_name(c):
    if c is None:
        return "?"
    if c.type.name == "MonoBehaviour":
        try:
            s = follow(c, c.read_typetree().get("m_Script"))
            return s.read_typetree().get("m_ClassName") if s is not None else "MonoBehaviour"
        except Exception:
            return "MonoBehaviour"
    return c.type.name


def objects(env, test):
    out = []
    for o in env.objects:
        if o.type.name != "GameObject":
            continue
        try:
            t = o.read_typetree()
        except Exception:
            continue
        if test(t["m_Name"]):
            out.append((o, t))
    return out


def components(o, t):
    return [follow(o, c["component"]) for c in t["m_Component"]]


def describe(owner, v):
    """A reference as 'Kind name'."""
    if not v.get("m_PathID"):
        return "none"
    try:
        target = follow(owner, v)
    except Exception:
        target = None
    if target is None:
        return "(elsewhere)"
    try:
        tt = target.read_typetree()
    except Exception:
        return target.type.name
    name = tt.get("m_Name")
    if not name and "m_GameObject" in tt:
        go = follow(target, tt["m_GameObject"])
        name = go.read_typetree()["m_Name"] if go is not None else "?"
        return "%s on '%s'" % (comp_name(target), name)
    return "%s '%s'" % (target.type.name, name)


def show_value(owner, v, indent, depth=0):
    if isinstance(v, dict):
        if set(v.keys()) == {"m_FileID", "m_PathID"}:
            return describe(owner, v)
        if set(v.keys()) <= {"x", "y", "z", "w", "r", "g", "b", "a"}:
            return "(" + ", ".join("%.4g" % v[k] for k in v) + ")"
        if depth > 2:
            return "{...}"
        return "".join("\n%s  %s: %s" % (indent, k, show_value(owner, x, indent + "  ", depth + 1)) for k, x in v.items())
    if isinstance(v, list):
        if len(v) > 12:
            return "[%d items]" % len(v)
        return "[" + ", ".join(show_value(owner, x, indent + "  ", depth + 1) for x in v) + "]"
    if isinstance(v, float):
        return "%.5g" % v
    if isinstance(v, (bytes, bytearray)):
        return "<%d bytes>" % len(v)
    return repr(v)


SKIP = {"rolloffCustomCurve", "panLevelCustomCurve", "spreadCustomCurve", "reverbZoneMixCustomCurve", "m_ObjectHideFlags", "m_CorrespondingSourceObject", "m_PrefabInstance", "m_PrefabAsset", "m_EditorHideFlags",
        "m_EditorClassIdentifier", "m_Script", "m_GameObject", "m_Name"}


def cmd_find(text):
    env = prefab_env()
    for o, t in objects(env, lambda n: text.lower() in n.lower()):
        print("%-44s %s" % (t["m_Name"], ", ".join(comp_name(c) for c in components(o, t))))


_ENV = None


def pick(name, n):
    global _ENV
    env = _ENV = _ENV or prefab_env()
    found = objects(env, lambda x: x == name)
    if not found:
        sys.exit("no object called %r" % name)
    if len(found) > 1:
        print("(%d objects have this name; showing number %d)" % (len(found), n))
    return found[n]


def cmd_show(name, n=0, picked=None):
    o, t = picked or pick(name, n)
    print("'%s' active %s" % (t["m_Name"], t.get("m_IsActive")))
    for c in components(o, t):
        if c is None:
            continue
        print("  [%s]" % comp_name(c))
        try:
            ct = c.read_typetree()
        except Exception as e:
            print("    (unreadable: %s)" % e)
            continue
        for k, v in ct.items():
            if k in SKIP:
                continue
            print("    %s: %s" % (k, show_value(c, v, "    ")))


def cmd_tree(name, n=0):
    o, t = pick(name, n)

    def walk(go, gt, depth):
        comps = components(go, gt)
        print("%s%s%s  [%s]" % ("  " * depth, gt["m_Name"], "" if gt.get("m_IsActive") else " (inactive)",
                                ", ".join(comp_name(c) for c in comps if c is not None and c.type.name not in ("Transform", "RectTransform"))))
        for c in comps:
            if c is not None and c.type.name in ("Transform", "RectTransform"):
                for ch in c.read_typetree()["m_Children"]:
                    cto = follow(c, ch)
                    if cto is None:
                        continue
                    cgo = follow(cto, cto.read_typetree()["m_GameObject"])
                    if cgo is not None:
                        walk(cgo, cgo.read_typetree(), depth + 1)

    walk(o, t, 0)


def cmd_deep(name, n=0):
    o, t = pick(name, n)

    def walk(go, gt, path):
        print("==== " + path)
        cmd_show(None, picked=(go, gt))
        for c in components(go, gt):
            if c is not None and c.type.name in ("Transform", "RectTransform"):
                for ch in c.read_typetree()["m_Children"]:
                    cto = follow(c, ch)
                    if cto is None:
                        continue
                    cgo = follow(cto, cto.read_typetree()["m_GameObject"])
                    if cgo is not None:
                        cgt = cgo.read_typetree()
                        walk(cgo, cgt, path + " / " + cgt["m_Name"])

    walk(o, t, t["m_Name"])


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "find":
        cmd_find(a[1])
    elif len(a) >= 2 and a[0] == "show":
        if len(a) > 2 and not a[2].isdigit():          # several names: one load for all of them
            for name in a[1:]:
                cmd_show(name)
        else:
            cmd_show(a[1], int(a[2]) if len(a) > 2 else 0)
    elif len(a) >= 2 and a[0] == "tree":
        cmd_tree(a[1], int(a[2]) if len(a) > 2 else 0)
    elif len(a) >= 2 and a[0] == "deep":
        cmd_deep(a[1], int(a[2]) if len(a) > 2 else 0)
    else:
        sys.exit(__doc__)
