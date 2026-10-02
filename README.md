# ultrasouls

Work-in-progress mod that brings ULTRAKILL V1's gameplay into Dark Souls Remastered.

No game files are included. You need your own copy of Dark Souls Remastered. Play offline while the mod is installed.

## What works so far

- **Params (`tools/patch_v1.py`)**: attacks cost no stamina, every melee weapon heals on hit, arrows and bolts fly fast and flat.
- **DLL (`dll/mod.cpp`)**: a `dinput8.dll` proxy. Stamina is locked at max and animations run faster. A jump goes through the game's own physics step, so walls and ceilings stop it. First-person mode puts the view at the character's head with a wider field of view, drives movement from WASD relative to the view, and makes the body face where you look. Mouse look in first person is the newest part and is still being tested.

| Key | Action |
|---|---|
| F5 | First person on/off |
| F1 / F2 | Field of view |
| F3 / F4 | Eye height |
| F9 / F10 | Mouse sensitivity |
| F6 / F7 / F8 | Animation speed down / up / toggle |
| J | Jump |

The offsets and function addresses in `mod.cpp` are for the Steam exe with SHA-1 `9150CC63C617332ED3C2C66E7566ED67E3292DA0`. Each hook checks the code it replaces and stays off if it doesn't match.

## Layout

- `tools/dsparam.py` reads and patches `GameParam.parambnd.dcx` in place (DCX, BND3, PARAM, PARAMDEF), with no dependencies.
- `tools/patch_v1.py` applies the param changes. It patches from `backup/GameParam.parambnd.dcx.current`, a copy of your own unmodified file, plus `backup/paramdef.paramdefbnd.dcx` from the game's `paramdef` folder. The game path is set at the top of the script.
- `tools/inspect_params.py` lists the param tables and diffs two param files.
- `dll/probe.cpp` is a read-only DLL that logs the player's memory layout to `ultrasouls.log`.
- `tools/rtti.py` finds a class's virtual function table in the exe by name, for locating game code.
- `tools/uk_il.py` dumps the IL of a class from your own copy of ULTRAKILL, to read V1's movement constants (`pip install dnfile dncil`).

## Build

```bash
python tools/patch_v1.py --install
```

```bash
g++ -O2 -shared -static -o build/dinput8.dll dll/mod.cpp dll/dinput8.def -lpsapi
```

Copy `build/dinput8.dll` next to `DarkSoulsRemastered.exe`. Delete it to uninstall.

## Roadmap

V1's real movement numbers, then dash, slide, wall jump and slam, hitscan guns, coins and parry, HUD and style meter.
