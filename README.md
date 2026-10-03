# ultrasouls

Work-in-progress mod that brings ULTRAKILL V1's gameplay into Dark Souls Remastered.

No game files are included, from either game. You need your own copy of Dark Souls Remastered, and your own copy of ULTRAKILL for the HUD and the revolver model, which are extracted from it on your machine. Play offline while the mod is installed.

## What works so far

- **Params (`tools/patch_v1.py`)**: attacks cost no stamina, every melee weapon heals on hit, arrows and bolts fly fast and flat, merchants sell them for 1 soul, and new rows are added for the revolver's two shots.
- **DLL (`dll/mod.cpp`, `dll/hud.cpp`)**: a `dinput8.dll` proxy. Stamina is locked at max and animations run faster. F5 switches to first person, which replaces Dark Souls' controls with V1's.
  - **Movement**: mouse look, run, jump, dash, slide and ground slam, driven through the game's own physics step so walls, floors and ceilings collide. The numbers come from ULTRAKILL's code and Player prefab (run 16.5 u/s, jump 28.08 u/s, gravity 40 u/s², dash 49.5 u/s for 0.2 s, slide 24 u/s, slam 100 u/s, 3 stamina bars at 70/s), converted at 1 unit = 0.5 m so V1's 3.5-unit capsule matches the character's height.
  - **Piercer revolver**: fired through the game's own projectile entry point, so nothing needs to be equipped. Left mouse fires every 0.5 s; holding right mouse charges for 0.57 s and releasing fires a shot that passes through enemies, then recharges for 2.5 s. Those timings are ULTRAKILL's. The damage (150 and 450 attack) is a tuning choice.
  - **HUD and viewmodel**: ULTRAKILL's weapon panel, health and stamina bars, crosshair and rings, rebuilt from the layout stored in the game's own HUD objects, plus the Piercer's model held in V1's arm. Checked with the offline test program below; newly added and still being tested in-game. The revolver is drawn in its idle pose with a procedural recoil; its real animations are not played yet. If the asset pack is missing, plain bars are drawn instead.

First person (F5 toggles it; third person plays as normal Dark Souls):

| Key | Action |
|---|---|
| Mouse / WASD | Look / move |
| Left mouse | Fire (hold to keep firing) |
| Right mouse | Hold to charge, release to fire the piercing shot |
| Space | Jump |
| Shift | Dash |
| Ctrl | Slide on the ground, slam in the air |
| F1 / F2 | Field of view |
| F3 / F4 | Eye height |
| F9 / F10 | Mouse sensitivity |
| F11 | Revolver on/off (off gives the mouse buttons back to the game) |
| Home | Viewmodel on/off |
| Insert | Switch how shots are requested (item-style is the default and the one that works) |

Space, Shift, Ctrl and both mouse buttons are hidden from the game while first person is on, so its own binds on them do nothing.

Always: F6 / F7 / F8 animation speed down / up / toggle.

Not handled yet: Dark Souls' own HUD and the player's body are still drawn in first person (turn the HUD off in the game's options for now).

The offsets and function addresses in `mod.cpp` are for the Steam exe with SHA-1 `9150CC63C617332ED3C2C66E7566ED67E3292DA0`. Each hook checks the code it replaces and stays off if it doesn't match.

## Layout

Mod:

- `dll/mod.cpp` is the DLL: input, hooks, movement, shooting.
- `dll/hud.cpp`, `dll/hud.h` draw the HUD and viewmodel with Direct3D 11 from the asset pack.
- `dll/hud_test.cpp` draws the same HUD onto an off-screen target and writes BMP files, to check it without starting the game.
- `tools/patch_v1.py` applies the param changes. It patches from `backup/GameParam.parambnd.dcx.current`, a copy of your own unmodified file, plus `backup/paramdef.paramdefbnd.dcx` from the game's `paramdef` folder.
- `tools/dsparam.py` reads and patches `GameParam.parambnd.dcx` (DCX, BND3, PARAM, PARAMDEF) with no dependencies, and can add rows.

ULTRAKILL assets (`pip install UnityPy`; everything they produce stays under `build/`, which is git-ignored):

- `tools/uk_bundles.py` indexes which bundle holds which internal asset file.
- `tools/uk_hud_dump.py` dumps the HUD's layout (rectangles, colours, sprites, text) from the Player object.
- `tools/uk_assets.py` extracts the HUD sprites and font, bakes the revolver's skinned meshes in their saved pose, and writes `ultrasouls_assets.bin`.

Reverse-engineering helpers:

- `tools/inspect_params.py` lists the param tables and diffs two param files.
- `dll/probe.cpp` is a read-only DLL that logs the player's memory layout to `ultrasouls.log`.
- `tools/rtti.py` finds a class's virtual function table in the exe by name.
- `tools/dis.sh` and `tools/xref.py` disassemble a range of the exe and find callers of an address. They read `backup/text.bin`, the exe's first `.text` section.
- `tools/uk_il.py` dumps the IL of a class from ULTRAKILL, to read V1's constants (`pip install dnfile dncil`).
- `tools/uk_player.py` reads V1's saved values (walk speed, jump power, mass, capsule) from ULTRAKILL's bundles.

The game folders are set at the top of `patch_v1.py`, `uk_bundles.py` and `uk_assets.py`.

## Build

Params:

```bash
python tools/patch_v1.py --install
```

Asset pack, from your ULTRAKILL install (the last command copies it next to the Dark Souls exe):

```bash
python tools/uk_hud_dump.py
```

```bash
python tools/uk_assets.py hud
```

```bash
python tools/uk_assets.py pack --install
```

DLL:

```bash
g++ -O2 -shared -static -o build/dinput8.dll dll/mod.cpp dll/hud.cpp dll/dinput8.def -lpsapi -ld3d11 -ldxgi
```

Copy `build/dinput8.dll` next to `DarkSoulsRemastered.exe`. Delete it and `ultrasouls_assets.bin` to uninstall.

Offline HUD check:

```bash
g++ -O2 -static -o build/hud_test.exe dll/hud_test.cpp dll/hud.cpp -ld3d11 -ldxgi
```

```bash
build/hud_test.exe build/ultrasouls_assets.bin build/hud_test
```

## Roadmap

Hide Dark Souls' HUD and the player's body in first person, the revolver's real animations, the style meter, the other weapons, wall jump and slam bounce, fall damage, coins and parry.
