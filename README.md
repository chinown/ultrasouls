# ultrasouls

Work-in-progress mod that brings ULTRAKILL V1's gameplay into Dark Souls Remastered.

No game files are included, from either game. You need your own copy of Dark Souls Remastered, and your own copy of ULTRAKILL for the HUD, the models, the animations and the sounds, which are extracted from it on your machine. Play offline while the mod is installed.

## What works so far

- **Params (`tools/patch_v1.py`)**: attacks cost no stamina, every melee weapon heals on hit, arrows and bolts fly fast and flat, merchants sell them for 1 soul, and new rows are added for everything the DLL fires: the revolver's shots, coin ricochets, the punches, shotgun pellets and explosions.
- **DLL (`dll/mod.cpp`, `dll/hud.cpp`)**: a `dinput8.dll` proxy. Stamina is locked at max and animations run faster. F5 switches to first person, which replaces Dark Souls' controls with V1's.
  - **Movement**: mouse look, run, jump, dash, slide and ground slam, driven through the game's own physics step so walls, floors and ceilings collide. The numbers come from ULTRAKILL's code and Player prefab (run 16.5 u/s, jump 28.08 u/s, gravity 40 u/s², dash 49.5 u/s for 0.2 s, slide 24 u/s, slam 100 u/s, 3 stamina bars at 70/s), converted at 1 unit = 0.5 m so V1's 3.5-unit capsule matches the character's height.
  - **Movement techs**, each from ULTRAKILL's `NewMovement` code: wall jump (up to 3 before landing, 24 u/s up and away from the wall), wall cling (holding towards a wall turns the fall into a slow slide down), slam jump (a jump within 0.4 s of a slam landing is higher the longer the slam fell), slide boost (a slide started within 0.2 s of a slam landing, or during a dash, starts up to 3x faster and wears off), slide-jump and dash-jump. A dash cancels a slam. Falls do no damage, but the map's kill planes still kill, and a fall with no floor under it puts the player back where they last stood after 30 m. Not done yet: slide-timed speed jumps (SSJ), slam storage, sliding in the air.
  - **Animated models**: the revolver in V1's right arm, the Feedbacker, the Knuckleblaster and the shotgun are drawn with their own skeletons and animation clips, taken from your ULTRAKILL install by `tools/uk_models.py`. The revolver idles, plays one of its three shot clips at random when fired and its draw clip when it is selected; the arms play their punches and the coin flip, and the Feedbacker its hook when it punches a shotgun shot or a coin on (a punch or parry on an enemy stays a jab); the shotgun plays its draw, its fire-and-reload and its core-throw clips. The revolver's shot clips are played at 80% of their travel from the idle pose, a tuning choice against ULTRAKILL's look. Clips cut straight from one to the next (no blending yet). All three revolver variations use the Piercer's model. Each part of a model is drawn with its own material's texture and colour, faces turned away from the eye are not drawn, and textures repeat (the shotgun's run past their edge). Dark Souls Remastered draws 60 frames a second and the models are posed afresh for each one, between the clips' samples, so the animations are as smooth as the game's frame rate allows. Without the model pack the revolver is drawn in its fixed pose.
  - **Feedbacker** (F): a punch with the damage of one revolver shot within 4 u of the eye, with ULTRAKILL's punch stamina (two charges, refilling at 1.25 per second) and cooldown. It is an unseen short projectile fired through the same path as the revolver. **Parry**: a punch that lands on an enemy in the middle of an attack does twice the damage with a heavy stagger, flashes the screen, refills health, stamina and punch stamina, and scores PARRY. "In the middle of an attack" means one of the enemy's animation slots holds an animation numbered 3000 to 3999, which is how Dark Souls numbers attacks. Tested in game on hollows: melee swings (3000, 3003, 3010) and firebomb throws (3008) were all parried. A parry holds the frame for 0.25 s under a white sheet at 49%, which then fades over a tenth of a second, as ULTRAKILL's time stop and its ParryFlash image do; so do a projectile boost, a core shot in the air, and a coin's shot that catches an enemy in mid-attack (INTERRUPTION) (`parry_freeze=0` in `ultrasouls.ini` leaves a plain flash instead). **Coin punch**: a punch on a coin sends it at the nearest enemy it can see for the coin's power as damage and FISTFUL OF DOLLAR, and the coin comes off the enemy flying straight up with one more power, to be shot or punched again; with no enemy in sight it comes off the first surface along the view. Not done: parrying projectiles in flight. The freeze and the coin punch were written while the game could not be run and have only been checked offline.
  - **Knuckleblaster** (G): only one arm can be out at a time (the other cannot start a punch until the first is ready again); costs 1.5 punch stamina, 2.5 times the damage, knocks the enemy off its feet and back (seen in game: a hollow thrown about 4 m). If G is still held 0.42 s into the punch (the clip's BlastCheck event) it lets off a blast wave: a 3 m wide shove 1 m ahead, and the shells are thrown out with their reload sound 0.78 s later. The wave's damage on enemies has not been measured.
  - **Shotgun, Core Eject** (2): left mouse fires twelve pellets, each turned up to 10 degrees off the view, at 75 u/s; ready again 1.33 s later, or 0.44 s after switching weapons, which cuts the reload short as in ULTRAKILL. A shot into an attacking enemy within 4 u parries it. Holding right mouse winds the core up for up to a second; letting go throws it, and it explodes on the first thing it touches (6 u across, 3.5 times a revolver shot). A core shot in the air with the revolver, or by a shot coming off a coin, goes off as the larger explosion. A Feedbacker punch within 0.15 s of a shot is a **projectile boost**: one pellet is sent on along the view and explodes where it lands. ULTRAKILL's code gives it 100 u/s; here it flies at 250 u/s, a tuning choice against how instant the original feels. Tested in game: pellets, the point-blank parry (PARRY then OVERKILL), a core killing an enemy, a boosted pellet (PROJECTILE BOOST, then EXPLODED). Dark Souls subtracts a flat defence from every hit, so a pellet has 0.57 of the revolver's attack value to land about a quarter of its damage on a basic hollow. Explosions do not hurt or launch the player.
  - **Sharpshooter** (third revolver variation): holding right mouse spins the gun; the charge rises to 100 in 1.33 s. Letting go, or pressing fire, with 25 or more spends one of three charges (one back every 6.7 s) on a shot that passes through enemies and bounces off surfaces, once per 25 of charge up to three times. Each bounce leaves 0.1 s later along the mirrored direction, or at a living enemy within 5 u of that line that the bounce point can see. While it spins the hand goes over to the gun's own Twirl pose, the gun lying on its side and turning about the finger, more so the longer it is held. Tested in game: the spin (before that pose was added), a shot damaging an enemy, three bounces between walls. The aimed bounce has not been seen hitting an enemy yet.
  - **Alternate ("Slab") revolvers** (X swaps the variation in the hand between its standard and its alternate form; the choice is kept for each variation and saved as `slab=` in `ultrasouls.ini`), from `Revolver`'s `altVersion` code and the three "Alternative Revolver" prefabs: the alternate model with its own clips. A shot is 2.5 times the standard one (the 'Revolver Beam Alternative': 1.25, up to twice on one enemy) and the gun is ready again 1.19 s later, when its Shoot clip's ReadyGun event comes. Put away before the hammer comes back (0.89 s) and drawn again within 2 s it comes out with the slow draw (1.15 s) instead of the quick one (0.33 s). A shot on a coin in its split window reloads it at once (0.47 s). Piercer: the charged shot is 5 times a standard shot through everything on the line, and comes back at half the rate (5 s). Sharpshooter: the spin winds up ten times as fast, and the shot needs and spends all three charges, which come back at 35 a second; it turns end over end where the standard one turns like a wheel. Marksman: the same coins. Built and checked in offline renders; not yet run in the game.
  - **Shotgun, Pump Charge** (2 again, or E): right mouse pumps it, up to three times. Fired unpumped it sends 10 pellets in two thirds of the spread; after one pump 16 in the full spread; after two, 24 in twice the spread; after three it has no pellets and goes off as an explosion one unit ahead, half again the size of a core's (in ULTRAKILL that one hurts V1 too; here it does not). The meter on the gun fills a third per pump, turns from green to red, and blinks with a warning beep at three. Not yet run in the game.
  - **Railcannon** (4; again, or E, for the other variation): one charge for both variations, full 16 s after a shot, shown by the five lights on the gun coming on one after another and a hum when it is full. Left mouse fires. Electric: a beam through every enemy on the line for 8 times a revolver shot. Malicious: 2 times a revolver shot to the first thing on the line and an explosion 13.5 u across there. Either beam is passed on by a coin and sets off a core. Right mouse zooms to half the field of view, with the mouse slowed to match. The Screwdriver and the shotgun's Sawed-On are not in yet. Not yet run in the game.
  - **Style meter**: ULTRAKILL's ranks (D to ULTRAKILL) with its meter sizes and drain speeds, and the bonus list. Hits and kills are found by watching nearby enemies' health after the player's attacks. Scored so far: hits, KILL, BIG KILL (bosses), DOUBLE / TRIPLE / MULTIKILL, RICOSHOT, PARRY, PROJECTILE BOOST, EXPLODED, OVERKILL (a shotgun kill at arm's length), DISRESPECT. Not done: freshness (weapons going stale), losing style when hurt, HEADSHOT and the other bonuses tied to things Dark Souls has no equivalent for.
  - **Effects**, rebuilt from ULTRAKILL's own effect prefabs with their meshes, textures and numbers (`tools/uk_dump.py` reads them, `tools/uk_models.py` packs the meshes):
    - the revolver's beam: a white line 0.25 u wide that thins to nothing in a sixth of a second, ending at the first surface or enemy, with the 60 hit particles thrown off that spot;
    - explosions: the low-polygon ball of fire with its scrolling texture, the faint shell that runs ahead of it, a spreading ring and white sparks;
    - coins: the game's coin model at its own size, turning end over end, with its flash and a short trail; a coin that meets a floor or wall is spent;
    - the shotgun's core: the game's own canister with the skull's face, tumbling;
    - pellets as small yellow balls with a white trail, and five sparks where each lands;
    - the streaks of air on a dash and while sliding;
    - a muzzle flash, and the Piercer's charge glowing at the muzzle behind the gun.
    They are drawn over the finished picture, without the game's depth, so walls are respected only roughly: a coin, a core or an explosion is left out while the eye has no clear line to it (the game's sight ray decides), a tracer is drawn only along the stretches that were in sight when it was made, and bursts of particles are not started out of sight. An explosion half behind a wall is still drawn whole. Dark Souls' own sparks where a shot lands are switched off in the params. Smoke, lights and blood are not brought over. The weapon bobs while walking as ULTRAKILL's does. This set was written while the game could not be run; it has been checked in offline renders only.
  - **Displays on the weapons**: the Piercer's monitor shows its battery (charging, then red and yellow while it refills), the Marksman's shows its four coins as discs that come back like a clock hand going round, the Sharpshooter's its three shots as bars that fill, and the shotgun's side meter shows the core (full, turning red while wound up, empty until the gun is ready again). The panels' places, sizes and pictures are read from the weapons' prefabs.
  - **HUD motion**: as in ULTRAKILL's `NewMovement`, the HUD panels lean a little against the player's velocity and the weapon about three times as much (`hud_motion=0` in `ultrasouls.ini` turns it off).
  - **Healing from blood**: a hit on an enemy within 2.5 m gives back 10% of full health, a kill within 3.5 m 30% more, a shotgun pellet's or an explosion's hit 3% each (the pellets that landed are estimated from the distance). These are the amounts and reaches of ULTRAKILL's blood splatters, on V1's 100 health.
  - **Boss health bar**: a boss within 60 m gets ULTRAKILL's bar across the top with its name once it has turned on the player, and keeps it until it dies or is left behind. "Turned on the player" is inferred, not read from the game: the boss has lost health, is in an attack animation, or is doing anything but standing idle with the player in plain sight. Which characters are bosses, and their names, come from the game's own text through `tools/ds_names.py`.
  - **No fall damage** in first person: while the mod controls the time in the air, the game's "on the ground" flag is held set, so the game never registers a fall. Kill planes still kill. Not seen in game when written.
  - **Camera tilt**, from ULTRAKILL's `CameraController`: the view rolls 1 degree towards the side being strafed to, 5 degrees while dashing or sliding, eased in and out the way the original does it. A dash or slide straight ahead narrows the field of view by 5%, one straight back widens it by 10%.
  - **Piercer revolver**: fired through the game's own projectile entry point, so nothing needs to be equipped. Left mouse fires every 0.5 s; holding right mouse charges for 0.57 s and releasing fires a shot that passes through enemies, then recharges for 2.5 s. Those timings are ULTRAKILL's. The damage (150 and 450 attack) is a tuning choice. The charged shot also hits coins and cores.
  - **Marksman revolver** (E, or 1 again, goes to the next variation): right mouse tosses a coin (four charges, one back every 4 s) with ULTRAKILL's throw (forward 20 u/s, up 15 u/s, plus the player's velocity). Shooting a coin sends the shot on 0.1 s later, to another coin (adding to its power) or to the nearest hostile character, at 2x a plain shot's damage plus 1x per extra coin; a coin shot during its flash or after 1 s splits to two targets. Tested in-game: the switch, the toss, hitting a coin, the HUD, ricochets killing enemies, and the line-of-sight check. A coin only goes for enemies it can see (the game's own ray cast decides), idle ones included; with none in sight the shot leaves in a random direction, as in ULTRAKILL. Simplified for now: coins do not collide with the world (one that drops 15 m below where it was thrown is removed), they are drawn as flat gold rings rather than ULTRAKILL's coin model, and the ricochet aims at the body rather than a weak point.
  - **Sounds**: jump, wall jump, dash, dash-jump, landing (light and heavy), slide stop, out-of-stamina, both revolvers' shots, the charged shot with its rising charge and refill ticking, weapon switch, coin toss, coin flash and coin hit. The clips are decoded from your own ULTRAKILL install by `tools/uk_sounds.py`; the volumes and pitches are the ones ULTRAKILL's code sets. Mixed in software and fed to the Windows audio engine (WASAPI, shared mode). Each of ULTRAKILL's audio sources is a channel that plays one sound at a time, so a new shot cuts the last shot's tail. Footsteps follow ULTRAKILL's timer (faster with speed, four clips, never the same twice running), and the slide and the wall cling have their scraping loops. The arms, the shotgun (shot, core launch, charge, click, smacks), explosions, the Sharpshooter's spin and shot and the ricochet have theirs too. Not done yet: the falling wind, the revolver's enemy hit sound, surface-specific footsteps. Without the sound pack the mod is silent.
  - **HUD**: ULTRAKILL's weapon panel (with each weapon's and variation's own icon and colour), health and stamina bars, the fist icon (the picture and colour of the arm last used, filled by punch stamina), the crosshair with its health and stamina arcs, and the style panel, rebuilt from the layout stored in the game's own HUD objects. If the asset pack is missing, plain bars are drawn instead.
  - **Characters that are not there**: Dark Souls keeps some characters in its list that are not in the world (Undead Burg has two). The mod's targeting skips them by a flag bit found by comparing them with real enemies; see the comment at `OFF_CHR_FLAGS` in `mod.cpp` for the evidence.

First person (F5 toggles it; third person plays as normal Dark Souls):

| Key | Action |
|---|---|
| Mouse / WASD | Look / move |
| Left mouse | Fire (hold to keep firing) |
| Right mouse | Piercer: hold to charge, release to fire the piercing shot. Marksman: toss a coin. Sharpshooter: hold to spin, release to fire the bouncing shot. Shotgun: hold to wind the core up, release to throw it |
| 1 | Revolver, always starting on the Marksman; pressed again, the next variation (Piercer, then Sharpshooter, then round again) |
| 2 | Shotgun, starting on the Core Eject; pressed again, the Pump Charge |
| 4 | Railcannon, starting on the Electric; pressed again, the Malicious |
| X | Revolver: swap the variation in the hand between standard and alternate ("Slab") |
| E | Next variation of the weapon in the hand (E is also Dark Souls' interact key, which still works) |
| Q | Revolver, straight to the Sharpshooter (Q still reaches Dark Souls too: its menus close with it) |
| Space | Jump |
| Shift | Dash |
| Ctrl | Slide on the ground (press, then hold), slam in the air |
| Space in the air, by a wall | Wall jump |
| F | Feedbacker punch; parries an attacking enemy, boosts a shotgun shot, sends a coin at an enemy |
| G | Knuckleblaster punch; held, it adds the blast wave |
| F1 / F2 | Field of view |
| F3 / F4 | Eye height |
| F9 / F10 | Mouse sensitivity |
| F11 | Revolver on/off (off gives the mouse buttons back to the game) |
| - / = | Sound effects volume down / up (starts at 20%) |
| F12 | Save the last 30 s of the mod's sound output, and the list of sounds started, next to the exe |
| Insert | Viewmodel on/off |
| Delete | Hide Dark Souls' own HUD in first person, on/off |
| Page Down | Hide the player's body in first person, on/off |
| Page Up | Camera tilt on/off |
| Numpad 5 | Test aid: turn the view to the nearest enemy in plain sight (again within 3 s: the next) |
| Numpad 4 | Test aid: put the player next to the nearest living enemy |

Space, Shift, Ctrl, F, G, X and both mouse buttons are hidden from the game while first person is on, so its own binds on them do nothing. The game reads the keyboard twice, through DirectInput and through `GetAsyncKeyState` (which its menus use: G opened the gesture menu until that was filtered too), and both are covered.

Always: F6 / F7 / F8 animation speed down / up / toggle.

The default mouse sensitivity is ULTRAKILL's at its setting 10 (0.05 degrees per mouse count); F9 / F10 change it. Mouse sensitivity, field of view, eye height, sound volume and the camera tilt switch are written to `ultrasouls.ini` next to the exe whenever a key changes them, and read back at the next start. Two more lines can be set there by hand: `parry_freeze=0` and `hud_motion=0`.

Dark Souls' own HUD is switched off while first person is on, through the game's "HUD" option, and switched back on when you leave it. The player's body is hidden in first person through the game's own camouflage mechanism (the one the Hidden Body spell uses), called every frame with zero opacity; whether enemies react to the player any differently because of it has not been checked. Interaction prompts ("Rest at bonfire") still show with the HUD hidden.

Dark Souls confines the Windows cursor to whatever window is in front, even from the background, which broke the cursor lock of other games running beside it (ULTRAKILL's, on a second monitor). The mod drops those calls unless Dark Souls itself is in front.

V1 jumps far higher than anything Dark Souls' maps expect. Walls that look solid can be cleared, and behind some of them there is no floor, only the fall to a kill plane.

The offsets and function addresses in `mod.cpp` are for the Steam exe with SHA-1 `9150CC63C617332ED3C2C66E7566ED67E3292DA0`. Each hook checks the code it replaces and stays off if it doesn't match.

## Layout

Mod:

- `dll/mod.cpp` is the DLL: input, hooks, movement, shooting.
- `dll/hud.cpp`, `dll/hud.h` draw the HUD and viewmodel with Direct3D 11 from the asset pack.
- `dll/sound.cpp`, `dll/sound.h` mix and play the sound effects from the sound pack.
- `dll/sound_test.cpp` plays a scripted sequence through the same mixer into a WAV file, to measure levels and breaks without the game.
- `dll/sound_rt_test.cpp` runs the real output path outside the game at an inaudible volume and reports how much audio the device took and any dropouts.
- `dll/hud_test.cpp` draws the same HUD onto an off-screen target and writes BMP files, to check it without starting the game.
- `tools/patch_v1.py` applies the param changes. It patches from `backup/GameParam.parambnd.dcx.current`, a copy of your own unmodified file, plus `backup/paramdef.paramdefbnd.dcx` from the game's `paramdef` folder.
- `tools/dsparam.py` reads and patches `GameParam.parambnd.dcx` (DCX, BND3, PARAM, PARAMDEF) with no dependencies, and can add rows.
- `tools/ds_names.py` writes `ultrasouls_names.txt`, the bosses' names from the game's own text, for the boss health bar.
- `tools/game.py` launches the game, loads the last save, takes screenshots, sends input, records short clips as frames (`clip`) and quits, for testing a build unattended. It needs the universal-modder plugin, a 1920x1080 window, and the game set to start offline. `start` copies the save file to `build/save_backups` and `quit` puts that copy back, so a test session leaves the save untouched (`quit --keep` keeps what the game wrote).

ULTRAKILL assets (`pip install UnityPy`; everything they produce stays under `build/`, which is git-ignored):

- `tools/uk_bundles.py` indexes which bundle holds which internal asset file.
- `tools/uk_hud_dump.py` dumps the HUD's layout (rectangles, colours, sprites, text) from the Player object.
- `tools/uk_models.py` exports the skeletons, skinned meshes and animation clips (sampled at 60 frames per second) of the revolver, both arms and the shotgun into `ultrasouls_models.bin`, and prints each clip's events with their times.
- `tools/uk_sounds.py` finds the clips named by the sound fields of the player, the weapons, the arms and the coin (`refs`) and writes `ultrasouls_sounds.bin` (`pack`).
- `tools/uk_dump.py` prints a prefab's components and field values (`find`, `show`, `tree`): the numbers ULTRAKILL's code reads from the editor, such as a projectile's speed or an explosion's size.
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
python tools/uk_sounds.py pack --install
python tools/uk_models.py pack --install
python tools/ds_names.py --install
```

DLL:

```bash
g++ -O2 -shared -static -o build/dinput8.dll dll/mod.cpp dll/hud.cpp dll/sound.cpp dll/dinput8.def -lpsapi -ld3d11 -ldxgi -lole32
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

The other weapons and variations (the shotgun's Pump Charge and Sawed-On, nailgun, railcannon, rocket launcher), the Whiplash, explosions that launch the player, coins that collide with the world, parrying projectiles, slam damage, slide-timed speed jumps and slam storage, style freshness.
