# ultrasouls

Work-in-progress mod that brings ULTRAKILL V1's gameplay into Dark Souls Remastered.

No game files are included. You need your own copy of Dark Souls Remastered. Play offline while the mod is installed.

## What works so far

- **Params (`tools/patch_v1.py`)**: attacks cost no stamina, every melee weapon heals on hit, arrows and bolts fly fast and flat, and merchants sell them for 1 soul.
- **DLL (`dll/mod.cpp`)**: a `dinput8.dll` proxy. Stamina is locked at max and animations run faster. F5 switches to first person, which replaces Dark Souls' controls with V1's.
  - **Movement**: mouse look, run, jump, dash, slide and ground slam, driven through the game's own physics step so walls, floors and ceilings collide. The numbers come from ULTRAKILL's code and Player prefab (run 16.5 u/s, jump 28.08 u/s, gravity 40 u/s², dash 49.5 u/s for 0.2 s, slide 24 u/s, slam 100 u/s, 3 stamina bars at 70/s), converted at 1 unit = 0.5 m so V1's 3.5-unit capsule matches the character's height.
  - **Shooting**: with a bow or crossbow equipped, fire one normal shot; after that, holding the left mouse button fires every 0.5 s (V1's revolver rate) with no animation, from the eye along the crosshair. The projectile is still the game's arrow or bolt.
  - **Ammo**: after two normal shots the count of the equipped arrows or bolts is found and held at 99. Instant shots use none.
  - **HUD**: a crosshair and three stamina bars, drawn into the game's back buffer.

First person (F5 toggles it; third person plays as normal Dark Souls):

| Key | Action |
|---|---|
| Mouse / WASD | Look / move |
| Left mouse | Fire (hold to keep firing) |
| Space | Jump |
| Shift | Dash |
| Ctrl | Slide on the ground, slam in the air |
| F1 / F2 | Field of view |
| F3 / F4 | Eye height |
| F9 / F10 | Mouse sensitivity |
| F11 | Instant fire on/off |

Space, Shift, Ctrl and the left mouse button are hidden from the game while first person is on, so its own binds on them do nothing.

Always: F6 / F7 / F8 animation speed down / up / toggle.

The offsets and function addresses in `mod.cpp` are for the Steam exe with SHA-1 `9150CC63C617332ED3C2C66E7566ED67E3292DA0`. Each hook checks the code it replaces and stays off if it doesn't match.

## Layout

- `tools/dsparam.py` reads and patches `GameParam.parambnd.dcx` in place (DCX, BND3, PARAM, PARAMDEF), with no dependencies.
- `tools/patch_v1.py` applies the param changes. It patches from `backup/GameParam.parambnd.dcx.current`, a copy of your own unmodified file, plus `backup/paramdef.paramdefbnd.dcx` from the game's `paramdef` folder. The game path is set at the top of the script.
- `tools/inspect_params.py` lists the param tables and diffs two param files.
- `dll/probe.cpp` is a read-only DLL that logs the player's memory layout to `ultrasouls.log`.
- `tools/rtti.py` finds a class's virtual function table in the exe by name, for locating game code.
- `tools/uk_il.py` dumps the IL of a class from your own copy of ULTRAKILL, to read V1's movement constants (`pip install dnfile dncil`).
- `tools/uk_player.py` reads V1's saved values (walk speed, jump power, mass, capsule) from ULTRAKILL's asset bundles (`pip install UnityPy`).
- `tools/dis.sh` and `tools/xref.py` disassemble a range of the exe and find callers of an address. They read `backup/text.bin`, the exe's first `.text` section.

## Build

```bash
python tools/patch_v1.py --install
```

```bash
g++ -O2 -shared -static -o build/dinput8.dll dll/mod.cpp dll/dinput8.def -lpsapi -ld3d11 -ldxgi
```

Copy `build/dinput8.dll` next to `DarkSoulsRemastered.exe`. Delete it to uninstall.

## Roadmap

Shooting without needing a bow or a first normal shot, V1's hitscan revolver and the other weapons, wall jump and slam bounce, fall damage, coins and parry, style meter.
