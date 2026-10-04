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
CLASSES = ("NewMovement", "Revolver", "Coin", "GroundCheck", "RevolverBeam", "Punch", "WeaponCharges", "PlayerFootsteps")

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
