"""Sanity-check the parser and diff the current GameParam against the 2023 .bak."""
import os
import sys
from dsparam import GameParam

HERE = os.path.dirname(os.path.abspath(__file__))
BK = os.path.join(HERE, "..", "backup")
cur = GameParam(os.path.join(BK, "GameParam.parambnd.dcx.current"), os.path.join(BK, "paramdef.paramdefbnd.dcx"))
old = GameParam(os.path.join(BK, "GameParam.parambnd.dcx.bak2023"), os.path.join(BK, "paramdef.paramdefbnd.dcx"))

print("== params (rows, def row size, detected row size) ==")
for stem, p in cur.params.items():
    flag = "" if p.pdef.row_size == p.detected_row_size else "  <-- SIZE MISMATCH"
    print(f"{stem:32} {p.param_type:28} rows={len(p.rows):5} def={p.pdef.row_size:4} det={p.detected_row_size:4}{flag}")
missing = [s for s in cur.bnd.stems() if s not in cur.params]
print("no paramdef for:", missing)

if "--diff" in sys.argv:
    print("\n== diff current vs bak2023 ==")
    for stem, p in cur.params.items():
        if stem not in old.params:
            print(f"[{stem}] only in current")
            continue
        q = old.params[stem]
        added = [r for r in p.order if r not in q.rows]
        removed = [r for r in q.order if r not in p.rows]
        changed = []
        for r in p.order:
            if r in q.rows:
                a, b = p.row(r), q.row(r)
                d = {k: (b[k], a[k]) for k in a if a[k] != b[k]}
                if d:
                    changed.append((r, d))
        if added or removed or changed:
            print(f"[{stem}] added={len(added)} removed={len(removed)} changed={len(changed)}")
            for r in added[:40]:
                print(f"   + {r} {p.names.get(r, '')}")
            for r in removed[:10]:
                print(f"   - {r} {q.names.get(r, '')}")
            for r, d in changed[:25]:
                items = list(d.items())
                print(f"   ~ {r} {p.names.get(r, '')}: " + ", ".join(f"{k} {v[0]}->{v[1]}" for k, v in items[:8]) + (" ..." if len(items) > 8 else ""))
