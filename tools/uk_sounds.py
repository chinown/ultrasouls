"""ULTRAKILL's sound effects for the mod, taken from your own copy of the game.

  python tools/uk_sounds.py refs             which clip each sound field of the player, revolver and coin names
  python tools/uk_sounds.py pack [--install] decode the clips the mod uses and write build/ultrasouls_sounds.bin
                                             (--install copies it next to the Dark Souls exe)

Nothing from ULTRAKILL is stored in this repository: the pack is built on your machine from your install.
Needs UnityPy with its FMOD support (pip install UnityPy fmod_toolkit).

Pack layout: "USSND001", count, then per sound: name[40], sample rate, channels, frame count, int16 PCM.
"""
import json
import os
import struct
import sys

import UnityPy

import uk_assets
from uk_assets import follow, load_bundle
from uk_bundles import cab_index

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "..", "build")
GAME_DIR = r"F:\SteamLibrary\steamapps\common\DARK SOULS REMASTERED"
CLASSES = ("NewMovement", "Revolver", "Coin", "GroundCheck", "RevolverBeam", "Punch", "WeaponCharges", "PlayerFootsteps", "Shotgun",
           "Grenade", "Railcannon", "RevolverAnimationReceiver", "HookArm", "Nailgun", "Nail", "Spin", "Harpoon", "TimeBomb", "DeathSequence", "TextAppearByLines", "LaughingSkull", "PlayOnAwakeTracker",
           "RocketLauncher", "Cannonball", "PhysicalShockwave")

# sound name in the pack -> (class, object the component is on, field). A field that names a prefab is
# followed to the first AudioSource under it. Volumes and pitches are set by the DLL, from ULTRAKILL's code.
WANTED = {
    "jump": ("NewMovement", "Player", "jumpSound"),
    "dash": ("NewMovement", "Player", "dodgeSound"),
    "dash_jump": ("NewMovement", "Player", "dashJumpSound"),
    "wall_jump_final": ("NewMovement", "Player", "finalWallJump"),
    "landing": ("NewMovement", "Player", "landingSound"),
    "landing_heavy": ("NewMovement", "Player", "impactDust"),
    "slide_stop": ("NewMovement", "Player", "slideStopSound"),
    "stamina_fail": ("NewMovement", "Player", "staminaFailSound"),
    "shot_piercer": ("Revolver", "Revolver Pierce", "gunShots[0]"),
    "shot_marksman": ("Revolver", "Revolver Ricochet", "gunShots[0]"),
    "shot_super": ("Revolver", "Revolver Pierce", "superGunSound"),
    "pierce_charge": ("Revolver", "Revolver Pierce", "chargeEffect"),
    "pierce_recharging": ("Revolver", "Revolver Pierce", "chargingSound"),
    "pierce_ready": ("Revolver", "Revolver Pierce", "chargedSound"),
    "weapon_draw": ("Revolver", "Revolver Pierce", "m_GameObject"),
    "coin_toss": ("Coin", "Coin", "m_GameObject"),
    "coin_flash": ("Coin", "Coin", "flash"),
    "coin_hit": ("Coin", "Coin", "coinHitSound"),
    # the default surface's four footsteps, and the scraping loops of its slide and wall-cling effects
    "footstep_1": ("PlayerFootsteps", "Agent", "footsteps[0]"),
    "footstep_2": ("PlayerFootsteps", "Agent", "footsteps[1]"),
    "footstep_3": ("PlayerFootsteps", "Agent", "footsteps[2]"),
    "footstep_4": ("PlayerFootsteps", "Agent", "footsteps[3]"),
    "slide_loop": ("PlayerFootsteps", "Agent", "set.slideParticles"),
    "wall_scrape": ("PlayerFootsteps", "Agent", "set.wallScrapeParticles"),
    # the Feedbacker ("Arm Blue")
    "punch_swing": ("Punch", "Arm Blue", "m_GameObject"),
    "punch_hit": ("Punch", "Arm Blue", "normalHit"),
    "punch_hit_heavy": ("Punch", "Arm Blue", "heavyHit"),
    # the Knuckleblaster ("Arm Red") and its blast wave
    "knuckle_swing": ("Punch", "Arm Red", "aud"),
    "knuckle_hit": ("Punch", "Arm Red", "normalHit"),
    "knuckle_hit_heavy": ("Punch", "Arm Red", "heavyHit"),
    "knuckle_blast": ("Punch", "Arm Red", "blastWave"),
    "knuckle_eject": ("Punch", "Arm Red", "shellEjector"),            # the shells thrown out after a blast
    "punch_projectile": ("Punch", "Arm Blue", "specialHit"),          # a punched projectile: the projectile boost
    # the Core Eject shotgun
    "shotgun_shot": ("Shotgun", "Shotgun Grenade", "shootSound"),
    "shotgun_click": ("Shotgun", "Shotgun Grenade", "clickSound"),
    "shotgun_smack": ("Shotgun", "Shotgun Grenade", "smackSound"),
    "shotgun_core": ("Shotgun", "Shotgun Grenade", "grenadeSoundBubble"),
    "shotgun_charge": ("Shotgun", "Shotgun Grenade", "chargeSoundBubble"),
    "explosion": ("Shotgun", "Shotgun Grenade", "explosion"),
    "explosion_super": ("Grenade", "Grenade", "superExplosion"),      # a core shot in the air
    # the Sharpshooter
    "shot_sharpshooter": ("Revolver", "Revolver Twirl", "gunShots[0]"),
    "twirl_shot": ("Revolver", "Revolver Twirl", "twirlShotSound"),
    "twirl_loop": ("Revolver", "Revolver Twirl", "chargeEffect"),
    "ricochet": ("RevolverBeam", "Revolver Beam Sharp", "ricochetSound"),
    # the alternate ("Slab") revolvers
    "shot_slab": ("Revolver", "Alternative Revolver Pierce", "gunShots[0]"),
    "shot_super_alt": ("Revolver", "Alternative Revolver Pierce", "superGunSound"),
    "slab_click": ("RevolverAnimationReceiver", "Revolver_Rerigged_Alternate", "click"),   # the hammer coming back, 0.89 s after a shot
    # the Pump Charge shotgun
    "pump1": ("Shotgun", "Shotgun Pump", "pump1sound"),
    "pump2": ("Shotgun", "Shotgun Pump", "pump2sound"),
    "pump_charge": ("Shotgun", "Shotgun Pump", "pumpChargeSound"),
    "pump_warning": ("Shotgun", "Shotgun Pump", "warningBeep"),      # pumped three times: it will go off in the hand
    # the Railcannon
    "rail_fire": ("Railcannon", "Railcannon Electric", "fireSound"),
    "rail_fire_malicious": ("Railcannon", "Railcannon Malicious", "fireSound"),
    "rail_hum": ("Railcannon", "Railcannon Electric", "fullCharge"),
    "rail_charged": ("WeaponCharges", "Guns", "railCannonFullChargeSound"),
    # the whiplash ("Hook Arm", part of the player's own prefab)
    "hook_throw": ("HookArm", "Hook Arm", "throwSound"),
    "hook_throw_loop": ("HookArm", "Hook Arm", "throwLoop"),
    "hook_hit": ("HookArm", "Hook Arm", "hitSound"),                  # the hook going into an enemy
    "hook_pull": ("HookArm", "Hook Arm", "pullSound"),
    "hook_pull_loop": ("HookArm", "Hook Arm", "pullLoop"),
    "hook_pull_done": ("HookArm", "Hook Arm", "pullDoneSound"),
    "hook_catch": ("HookArm", "Hook Arm", "catchSound"),              # the hook back in the hand
    "hook_woosh": ("HookArm", "Hook Arm", "wooshSound"),
    "hook_clink": ("HookArm", "Hook Arm", "clinkSparks"),             # the hook meeting a wall
    # the Sawblade Launcher
    "saw_shot": ("Nailgun", "Sawblade Launcher Magnet", "muzzleFlash"),
    "saw_shot_super": ("Nailgun", "Sawblade Launcher Magnet", "muzzleFlash2"),
    "saw_snap": ("Nailgun", "Sawblade Launcher Magnet", "snapSound"),
    "saw_magnet": ("Nailgun", "Sawblade Launcher Magnet", "magnetShotSound"),
    "saw_no_ammo": ("Nailgun", "Sawblade Launcher Magnet", "noAmmoSound"),
    "saw_last_shot": ("Nailgun", "Sawblade Launcher Magnet", "lastShotSound"),
    "saw_bounce": ("Nail", "NailAlt", "environmentHitSound"),
    "saw_hit": ("Nail", "NailAlt", "enemyHitSound"),
    "saw_break": ("Nail", "NailAlt", "sawBreakEffect"),               # a saw breaking ('BreakParticleMetalSaw')
    "saw_chainsaw": ("Nail", "NailAltHeated", "stoppedAud"),          # the heated saw's own looping noise
    "saw_spin": ("Spin", "Blade", "m_GameObject"),                    # the blade in the launcher's jaws, looping
    "harpoon_stop": ("Harpoon", "Harpoon", "environmentHitSound"),    # the magnet's harpoon going into a wall
    "harpoon_pierce": ("Harpoon", "Harpoon", "enemyHitSound"),        # and into an enemy
    "magnet_beep": ("TimeBomb", "Harpoon", "beepLight"),              # the magnet's beep
    # dying: the sequence's own sound, and the two its lines of text come with
    "death_sequence": ("DeathSequence", "DeathSequence", "m_GameObject"),
    "death_error": ("TextAppearByLines", "Text (TMP)", "errorSound"),
    "death_warning": ("TextAppearByLines", "Text (TMP)", "warningSound"),
    # the screen after it: its hum, the click it comes up with, and the skull's laugh
    "death_buzz": ("PlayOnAwakeTracker", "BlackScreen", "m_GameObject"),
    "death_tv_off": ("PlayOnAwakeTracker", "ISeeYou", "m_GameObject"),
    "death_laugh": ("LaughingSkull", "LaughingSkull", "m_GameObject"),
    # the rocket launcher: its shot, the clunk of its reload, the Freezeframe's clock (stopping, starting, ticking
    # and winding back up) and the hum of the S.R.S. Cannon charging
    "rocket_fire": ("RocketLauncher", "Rocket Launcher Freeze", "m_GameObject"),
    "rocket_clunk": ("RocketLauncher", "Rocket Launcher Freeze", "clunkSound"),
    "rocket_freeze": ("RocketLauncher", "Rocket Launcher Freeze", "timerFreezeSound"),
    "rocket_unfreeze": ("RocketLauncher", "Rocket Launcher Freeze", "timerUnfreezeSound"),
    "rocket_tick": ("RocketLauncher", "Rocket Launcher Freeze", "timerTickSound"),
    "rocket_wind": ("RocketLauncher", "Rocket Launcher Freeze", "timerWindupSound"),
    "srs_charge": ("RocketLauncher", "Rocket Launcher Cannonball", "chargeSound"),
    # a rocket in flight, one that has hung frozen for a second, and one going off against a wall
    "rocket_loop": ("Grenade", "Rocket", "m_GameObject"),
    "rocket_levelup": ("Grenade", "Rocket", "levelUpEffect"),
    "explosion_harmless": ("Grenade", "Rocket", "harmlessExplosion"),
    # the cannonball: bouncing off an enemy, breaking, shot in the air, and the wave where it lands
    "cannonball_bounce": ("Cannonball", "Cannonball", "bounceSound"),
    "cannonball_break": ("Cannonball", "Cannonball", "breakEffect"),
    "explosion_big": ("Cannonball", "Cannonball", "interruptionExplosion"),
    "shockwave": ("PhysicalShockwave", "PhysicalShockwavePlayer", "soundEffect"),
}


def script_class(obj, tree):
    s = follow(obj, tree.get("m_Script"))
    return s.read_typetree().get("m_ClassName") if s is not None else None


def clip_of(owner, pptr, depth=0):
    """(clip object, volume, pitch, description) for a PPtr to an AudioClip, or to a prefab holding an AudioSource."""
    target = follow(owner, pptr)
    if target is None:
        return None
    kind = target.type.name
    if kind == "AudioClip":
        return target, 1.0, 1.0, "clip"
    if kind in ("Transform", "RectTransform") and depth < 3:
        # a field that names a transform (the Knuckleblaster's shell ejector): its object's AudioSource
        return clip_of(target, target.read_typetree()["m_GameObject"], depth + 1)
    if kind == "AudioSource":
        t = target.read_typetree()
        clip = follow(target, t.get("m_audioClip"))
        return (clip, t.get("m_Volume", 1.0), t.get("m_Pitch", 1.0), "source") if clip is not None else None
    if kind == "GameObject" and depth < 3:
        t = target.read_typetree()
        comps = [follow(target, c["component"]) for c in t["m_Component"]]
        for c in comps:
            if c is not None and c.type.name == "AudioSource":
                r = clip_of(target, {"m_FileID": 0, "m_PathID": c.path_id}, depth + 1)
                if r:
                    return r[0], r[1], r[2], "prefab " + t["m_Name"]
        for c in comps:                       # no AudioSource on the root: look one level down
            if c is not None and c.type.name in ("Transform", "RectTransform"):
                for ch in c.read_typetree()["m_Children"]:
                    cto = follow(c, ch)
                    if cto is None:
                        continue
                    r = clip_of(cto, cto.read_typetree()["m_GameObject"], depth + 1)
                    if r:
                        return r
    if kind == "MonoBehaviour" and depth < 2:
        return None
    return None


def audio_fields(obj, tree):
    """Every field of a component that leads to a clip: {field: (clip, volume, pitch, how)}."""
    out = {}
    for key, val in tree.items():
        items = val if isinstance(val, list) else [val]
        for i, v in enumerate(items):
            if isinstance(v, dict) and "m_PathID" in v and v["m_PathID"]:
                try:
                    r = clip_of(obj, v)
                except Exception:
                    r = None
                if r:
                    out[key + ("[%d]" % i if isinstance(val, list) else "")] = r
    return out


def scan(bundles):
    found = {}
    for path in bundles:
        env = load_bundle(path)
        for o in env.objects:
            if o.type.name != "MonoBehaviour":
                continue
            try:
                t = o.read_typetree()
            except Exception:
                continue
            cls = script_class(o, t)
            if cls not in CLASSES:
                continue
            go = follow(o, t.get("m_GameObject"))
            go_name = go.read_typetree()["m_Name"] if go is not None else "?"
            fields = audio_fields(o, t)
            if cls == "PlayerFootsteps" and "footstepSet" in t:
                # the footstep set holds, per surface type, the effect spawned while sliding or clinging;
                # surface 0 is the default one, and the effect's own AudioSource has the looping clip
                fs = follow(o, t["footstepSet"])
                if fs is not None:
                    ft = fs.read_typetree()
                    for key in ("slideParticles", "wallScrapeParticles"):
                        for e in ft.get(key, []):
                            if e.get("<SurfaceType>k__BackingField") == 0:
                                r = clip_of(fs, e.get("<particle>k__BackingField"))
                                if r:
                                    fields["set." + key] = r
            if fields:
                found.setdefault((cls, go_name), {}).update(fields)
    return found


def bundles_for_refs():
    # the weapon and coin prefabs, and a scene that holds the Player object
    idx = cab_index()
    names = ("gameprefabs_assets_all.bundle", "specialscenes_scenes_earlyaccessend.bundle")
    return sorted({b for b in idx.values() if os.path.basename(b) in names})


def cmd_refs():
    found = scan(bundles_for_refs())
    for (cls, go_name), fields in sorted(found.items()):
        print("%s on '%s'" % (cls, go_name))
        for f, (clip, vol, pitch, how) in sorted(fields.items()):
            print("    %-28s %-34s vol %.2f pitch %.2f (%s)" % (f, clip.read_typetree()["m_Name"], vol, pitch, how))


def decode(clip):
    """(sample rate, channels, int16 PCM bytes) of an AudioClip, through UnityPy's FMOD decoder."""
    import io
    import wave
    data = clip.read()
    wavs = data.samples
    if not wavs:
        raise ValueError("no samples decoded")
    raw = next(iter(wavs.values()))
    with wave.open(io.BytesIO(raw)) as w:
        if w.getsampwidth() != 2:
            raise ValueError("unexpected sample width %d" % w.getsampwidth())
        return w.getframerate(), w.getnchannels(), w.readframes(w.getnframes())


def made_sounds():
    """Sounds that are neither game's: made here from sines and noise. {name: (rate, 16-bit mono PCM)}
    parry_clang: the ring of a parry that opens an enemy to the visceral attack (a struck-metal clang over a
    low thump: a handful of partials that are not multiples of each other, each dying away at its own rate)."""
    import numpy as np
    rate = 48000
    t = np.arange(int(rate * 1.1)) / rate
    rng = np.random.default_rng(7)
    clang = sum(a * np.sin(2 * np.pi * f * t + p) * np.exp(-d * t)
                for f, a, d, p in ((523, 0.50, 5.0, 0.0), (1244, 0.42, 6.5, 1.0), (1987, 0.34, 8.0, 2.1), (2890, 0.26, 10.0, 0.4),
                                   (4310, 0.18, 13.0, 1.7), (6120, 0.10, 17.0, 2.9)))
    strike = rng.standard_normal(len(t)) * np.exp(-90.0 * t) * 0.9
    thump = np.sin(2 * np.pi * (95.0 * t - 60.0 * t * t).clip(0)) * np.exp(-14.0 * t) * 0.8
    x = clang + strike + thump
    x *= np.minimum(1.0, t / 0.002)
    x = x / np.abs(x).max() * 0.9
    return {"parry_clang": (rate, (x * 32767).astype("<i2").tobytes())}


def cmd_pack(install=False):
    found = scan(bundles_for_refs())
    out = os.path.join(BUILD, "ultrasouls_sounds.bin")
    entries = []
    for name, (cls, go_name, field) in WANTED.items():
        fields = found.get((cls, go_name), {})
        if field not in fields:
            print("MISSING %-18s %s.%s on '%s'" % (name, cls, field, go_name))
            continue
        clip = fields[field][0]
        try:
            rate, channels, pcm = decode(clip)
        except Exception as e:
            print("FAILED  %-18s %s" % (name, e))
            continue
        frames = len(pcm) // (2 * channels)
        entries.append((name, rate, channels, frames, pcm))
        print("%-18s %6d Hz, %d ch, %.2f s" % (name, rate, channels, frames / rate))
    for name, (rate, pcm) in made_sounds().items():
        entries.append((name, rate, 1, len(pcm) // 2, pcm))
        print("%-18s %6d Hz, 1 ch, %.2f s (made here)" % (name, rate, len(pcm) / 2 / rate))
    with open(out, "wb") as f:
        f.write(b"USSND001" + struct.pack("<I", len(entries)))
        for name, rate, channels, frames, pcm in entries:
            f.write(name.encode("ascii").ljust(40, b"\0")[:40] + struct.pack("<III", rate, channels, frames) + pcm)
    print("wrote", os.path.normpath(out), os.path.getsize(out), "bytes,", len(entries), "of", len(WANTED), "sounds")
    if install:
        import shutil
        shutil.copy(out, os.path.join(GAME_DIR, "ultrasouls_sounds.bin"))
        print("installed next to the exe")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "refs":
        cmd_refs()
    elif cmd == "pack":
        cmd_pack("--install" in sys.argv[2:])
    else:
        sys.exit(__doc__)
