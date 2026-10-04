# ultrasouls

Work-in-progress mod that brings ULTRAKILL V1's gameplay into Dark Souls Remastered.

No game files are included, from either game. You need your own copy of Dark Souls Remastered, and your own copy of ULTRAKILL for the HUD and the revolver model, which are extracted from it on your machine. Play offline while the mod is installed.

## What works so far

- **Params (`tools/patch_v1.py`)**: attacks cost no stamina, every melee weapon heals on hit, arrows and bolts fly fast and flat, merchants sell them for 1 soul, and new rows are added for the revolver's two shots.
- **DLL (`dll/mod.cpp`, `dll/hud.cpp`)**: a `dinput8.dll` proxy. Stamina is locked at max and animations run faster. F5 switches to first person, which replaces Dark Souls' controls with V1's.
  - **Movement**: mouse look, run, jump, dash, slide and ground slam, driven through the game's own physics step so walls, floors and ceilings collide. The numbers come from ULTRAKILL's code and Player prefab (run 16.5 u/s, jump 28.08 u/s, gravity 40 u/s², dash 49.5 u/s for 0.2 s, slide 24 u/s, slam 100 u/s, 3 stamina bars at 70/s), converted at 1 unit = 0.5 m so V1's 3.5-unit capsule matches the character's height.
  - **Movement techs**, each from ULTRAKILL's `NewMovement` code: wall jump (up to 3 before landing, 24 u/s up and away from the wall), wall cling (holding towards a wall turns the fall into a slow slide down), slam jump (a jump within 0.4 s of a slam landing is higher the longer the slam fell), slide boost (a slide started within 0.2 s of a slam landing, or during a dash, starts up to 3x faster and wears off), slide-jump and dash-jump. A dash cancels a slam. Falls do no damage, but the map's kill planes still kill, and a fall with no floor under it puts the player back where they last stood after 30 m. Not done yet: slide-timed speed jumps (SSJ), slam storage, sliding in the air.
  - **Piercer revolver**: fired through the game's own projectile entry point, so nothing needs to be equipped. Left mouse fires every 0.5 s; holding right mouse charges for 0.57 s and releasing fires a shot that passes through enemies, then recharges for 2.5 s. Those timings are ULTRAKILL's. The damage (150 and 450 attack) is a tuning choice.
  - **Marksman revolver** (E or 1 switches variation): right mouse tosses a coin (four charges, one back every 4 s) with ULTRAKILL's throw (forward 20 u/s, up 15 u/s, plus the player's velocity). Shooting a coin sends the shot on 0.1 s later, to another coin (adding to its power) or to the nearest hostile character, at 2x a plain shot's damage plus 1x per extra coin; a coin shot during its flash or after 1 s splits to two targets. Tested in-game: the switch, the toss, hitting a coin, the HUD. The ricochet itself is not confirmed yet: until v0.37 it left from the player's eye instead of the coin (the game sets a projectile up after the shoot call returns), and the fix has not been seen hitting an enemy. The log records each ricochet and its target's health a second later. Simplified for now: coins do not collide with the world, they are drawn as flat gold rings rather than ULTRAKILL's coin model, the ricochet does not check line of sight, and it aims at the body rather than a weak point.
  - **HUD and viewmodel**: ULTRAKILL's weapon panel, health and stamina bars, crosshair and rings, rebuilt from the layout stored in the game's own HUD objects, plus the Piercer's model held in V1's arm. Checked with the offline test program below; newly added and still being tested in-game. The revolver is drawn in its idle pose with a procedural recoil; its real animations are not played yet. If the asset pack is missing, plain bars are drawn instead.

First person (F5 toggles it; third person plays as normal Dark Souls):

| Key | Action |
|---|---|
| Mouse / WASD | Look / move |
| Left mouse | Fire (hold to keep firing) |
| Right mouse | Piercer: hold to charge, release to fire the piercing shot. Marksman: toss a coin |
| E or 1 | Switch between Piercer and Marksman (E is also Dark Souls' interact key, which still works) |
| Space | Jump |
| Shift | Dash |
| Ctrl | Slide on the ground (press, then hold), slam in the air |
| Space in the air, by a wall | Wall jump |
| F1 / F2 | Field of view |
| F3 / F4 | Eye height |
| F9 / F10 | Mouse sensitivity |
| F11 | Revolver on/off (off gives the mouse buttons back to the game) |
| Insert | Viewmodel on/off |
| Delete | Hide Dark Souls' own HUD in first person, on/off |
| Page Down | Hide the player's body in first person, on/off |

Space, Shift, Ctrl and both mouse buttons are hidden from the game while first person is on, so its own binds on them do nothing.

Always: F6 / F7 / F8 animation speed down / up / toggle.

Dark Souls' own HUD is switched off while first person is on, through the game's "HUD" option, and switched back on when you leave it. The player's body is hidden in first person through the game's own camouflage mechanism (the one the Hidden Body spell uses), called every frame with zero opacity; whether enemies react to the player any differently because of it has not been checked. Interaction prompts ("Rest at bonfire") still show with the HUD hidden.

V1 jumps far higher than anything Dark Souls' maps expect. Walls that look solid can be cleared, and behind some of them there is no floor, only the fall to a kill plane.

The offsets and function addresses in `mod.cpp` are for the Steam exe with SHA-1 `9150CC63C617332ED3C2C66E7566ED67E3292DA0`. Each hook checks the code it replaces and stays off if it doesn't match.

## Layout

Mod:

- `dll/mod.cpp` is the DLL: input, hooks, movement, shooting.
- `dll/hud.cpp`, `dll/hud.h` draw the HUD and viewmodel with Direct3D 11 from the asset pack.
- `dll/hud_test.cpp` draws the same HUD onto an off-screen target and writes BMP files, to check it without starting the game.
- `tools/patch_v1.py` applies the param changes. It patches from `backup/GameParam.parambnd.dcx.current`, a copy of your own unmodified file, plus `backup/paramdef.paramdefbnd.dcx` from the game's `paramdef` folder.
- `tools/dsparam.py` reads and patches `GameParam.parambnd.dcx` (DCX, BND3, PARAM, PARAMDEF) with no dependencies, and can add rows.
- `tools/game.py` launches the game, loads the last save, takes screenshots, sends input and quits, for testing a build unattended. It needs the universal-modder plugin, a 1920x1080 window, and the game set to start offline. `start` copies the save file to `build/save_backups` and `quit` puts that copy back, so a test session leaves the save untouched (`quit --keep` keeps what the game wrote).

ULTRAKILL assets (`pip install UnityPy`; everything they produce stays under `build/`, which is git-ignored):

- `tools/uk_bundles.py` indexes which bundle holds which internal asset file.
- `tools/uk_hud_dump.py` dumps the HUD's layout (rectangles, colours, sprites, text) from the Player object.
- `tools/uk_assets.py` extracts the HUD sprites and font, bakes the revolver's skinned meshes in their saved pose, and writes `ultrasouls_assets.bin`.

Reverse-engineering helpers:

- `tools/inspect_params.py` lists the param tables and diffs two param files.
- `dll/probe.cpp` is a read-only DLL that logs the player's memory layout to `ultrasouls.log`.
- `tools/rtti.py` finds a class's virtual function table in the exe by name.
- `tools/dis.sh` and `tools/xref.py` disassemble a range of the exe and find callers of an address. They read `backup/text.bin`, the exe's first `.text` section.
- `tools/decomp.py` decompiles functions of the exe to C with Ghidra (`python tools/decomp.py 2BC650`), into `build/decomp`. It needs Ghidra, Java 21 and a one-time analysis of the exe; the script's header says where it looks for them.
- `tools/uk_il.py` dumps the IL of a class from ULTRAKILL, to read V1's constants (`pip install dnfile dncil`). ILSpy's command-line tool gives the same code as C#, which is easier to read.
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

The revolver's real animations, the style meter, the other weapons, wall jump and slam bounce, fall damage, coins and parry.
