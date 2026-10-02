# ultrasouls

Work-in-progress mod that brings ULTRAKILL V1's gameplay into Dark Souls Remastered.

No game files are included. You need your own copy of Dark Souls Remastered. Play offline while the mod is installed.

## What works so far

- **Params (`tools/patch_v1.py`)**: attacks cost no stamina, every melee weapon heals on hit, arrows and bolts fly fast and flat.
- **DLL (`dll/mod.cpp`)**: a `dinput8.dll` proxy that locks stamina at max and adds an animation speed multiplier (F6/F7 adjust, F8 toggle). The jump on J is an experiment.

## Layout

- `tools/dsparam.py` reads and patches `GameParam.parambnd.dcx` in place (DCX, BND3, PARAM, PARAMDEF), with no dependencies.
- `tools/patch_v1.py` applies the param changes. It patches from `backup/GameParam.parambnd.dcx.current`, a copy of your own unmodified file, plus `backup/paramdef.paramdefbnd.dcx` from the game's `paramdef` folder. The game path is set at the top of the script.
- `tools/inspect_params.py` lists the param tables and diffs two param files.
- `dll/probe.cpp` is a read-only DLL that logs the player's memory layout to `ultrasouls.log`.

## Build

```bash
python tools/patch_v1.py --install
```

```bash
g++ -O2 -shared -static -o build/dinput8.dll dll/mod.cpp dll/dinput8.def -lpsapi
```

Copy `build/dinput8.dll` next to `DarkSoulsRemastered.exe`. Delete it to uninstall.

## Roadmap

First-person camera, V1 movement controller (dash, slide, jump, wall jump, slam), hitscan guns, coins and parry, HUD and style meter.
