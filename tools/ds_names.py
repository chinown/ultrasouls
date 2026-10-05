"""Boss names for the mod's boss health bar, read from your own copy of Dark Souls Remastered.

  python tools/ds_names.py [--install]   write build/ultrasouls_names.txt (--install copies it next to the exe)

Each line is an NpcParam row id, a tab, and a boss's name. The game's NPC name text
(msg/ENGLISH/item.msgbnd.dcx) lists the bosses under the first four digits of their NpcParam rows (2250
is the Taurus Demon, rows 225000 on; 2230, 2231 and 2232 are the three demons that share one model), so
every row whose id divided by 100 has a name there gets it. The DLL reads a character's NpcParam id from
memory, and a character with a line here is a boss. Where the game reuses a boss's row for an ordinary
enemy later on, that enemy gets a bar too. Nothing from the game is stored in this repository.
"""
import os
import shutil
import struct
import sys

from dsparam import Bnd3, GameParam, dcx_decompress

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
GAME_DIR = r"F:\SteamLibrary\steamapps\common\DARK SOULS REMASTERED"
OUT = os.path.join(ROOT, "build", "ultrasouls_names.txt")


def fmg_strings(data):
    """{id: text} of one FMG text file (the Dark Souls 1 layout: id ranges, then offsets to UTF-16 strings)."""
    groups, count, offsets = struct.unpack_from("<III", data, 0x0C)
    out = {}
    for g in range(groups):
        index, first, last = struct.unpack_from("<III", data, 0x1C + g * 12)
        for k in range(last - first + 1):
            off = struct.unpack_from("<I", data, offsets + (index + k) * 4)[0]
            if not off:
                continue
            end = off
            while data[end:end + 2] != b"\0\0":
                end += 2
            text = data[off:end].decode("utf-16-le", "replace").strip()
            if text:
                out[first + k] = text
    return out


def main(install=False):
    bnd = Bnd3(dcx_decompress(open(os.path.join(GAME_DIR, "msg", "ENGLISH", "item.msgbnd.dcx"), "rb").read()))
    names = {}
    for e in bnd.entries:                          # the base game's file, then the one the DLC adds to it
        if "NPC_name_" in e.get("name", ""):
            names.update(fmg_strings(bnd.data(e)))
    g = GameParam(os.path.join(ROOT, "backup", "GameParam.parambnd.dcx.current"), os.path.join(ROOT, "backup", "paramdef.paramdefbnd.dcx"))
    npc = g["NpcParam"]
    lines = []
    for rid in npc.order:
        model = rid // 100
        if model >= 2000 and model in names:       # below 2000 the text is the human characters' names
            lines.append("%d\t%s" % (rid, names[model]))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print("wrote", os.path.normpath(OUT), len(lines), "boss rows of", len(npc.order), "characters")
    if install:
        shutil.copy(OUT, os.path.join(GAME_DIR, "ultrasouls_names.txt"))
        print("installed next to the exe")


if __name__ == "__main__":
    main("--install" in sys.argv[1:])
