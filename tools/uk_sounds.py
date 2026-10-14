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
           "RocketLauncher", "Cannonball", "PhysicalShockwave", "FireObjectPool")

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
    "nail_shot": ("Nailgun", "Nailgun Magnet", "muzzleFlash"),
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
    # the Firestarter's spray (its loop and the burst it starts with), its end, the click of an empty tank, and a fire's own sound
    "napalm_loop": ("RocketLauncher", "Rocket Launcher Napalm", "napalmMuzzleFlashSounds[0]"),
    "napalm_start": ("RocketLauncher", "Rocket Launcher Napalm", "napalmMuzzleFlashSounds[1]"),
    "napalm_stop": ("RocketLauncher", "Rocket Launcher Napalm", "napalmStopSound"),
    "napalm_empty": ("RocketLauncher", "Rocket Launcher Napalm", "napalmNoAmmoSound"),
    "fire_loop": ("FireObjectPool", "BloodsplatterManager", "simpleFirePrefab"),
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


def find_ffmpeg():
    import shutil
    here = "C:" + os.sep + "ffmpeg" + os.sep + "ffmpeg.exe"
    return shutil.which("ffmpeg") or (here if os.path.exists(here) else None)


# Dark Souls' own sounds, taken out of your own copy's sound bank as the pack is made (nothing of them is in the
# repository): the three of its critical hit. pack name -> the sample's name in sound/frpg_main.fsb
#   kill-chance   the low swell as a parry lands and the enemy is open
#   kill1         the weapon going in
#   kill-success  the hit of the critical itself
DS_SOUNDS = {"visc_parry": "kill-chance.wav", "visc_in": "kill1.wav", "visc_rip": "kill-success.wav"}


def _mp3_frame_len(h):
    b = struct.unpack(">I", h)[0]
    if (b >> 21) != 0x7FF:
        return 0
    ver, layer, br_i, sr_i, pad = (b >> 19) & 3, (b >> 17) & 3, (b >> 12) & 15, (b >> 10) & 3, (b >> 9) & 1
    if layer != 1 or br_i in (0, 15) or sr_i == 3 or ver == 1:
        return 0
    br = ([0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320] if ver == 3
          else [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160])[br_i] * 1000
    sr = [44100, 48000, 32000][sr_i] >> (0 if ver == 3 else 1 if ver == 2 else 2)
    return (144 if ver == 3 else 72) * br // sr + pad


def ds_sounds():
    """{pack name: (rate, channels, 16-bit PCM)} for DS_SOUNDS. The bank is an FSB4 of MP3 samples one after the
    other, each sample's frames padded out to four bytes; the padding is dropped and ffmpeg decodes the rest."""
    import subprocess
    import tempfile
    import wave
    path = os.path.join(GAME_DIR, "sound", "frpg_main.fsb")
    ffmpeg = find_ffmpeg()
    out = {}
    try:
        d = open(path, "rb").read()
    except OSError:
        print("SKIPPED Dark Souls' own sounds: %s is not there" % path)
        return out
    if d[:4] != b"FSB4" or not ffmpeg:
        print("SKIPPED Dark Souls' own sounds: %s" % ("ffmpeg is needed to decode them" if d[:4] == b"FSB4" else "the bank is not an FSB4"))
        return out
    count, headers = struct.unpack_from("<II", d, 4)
    pos, cur = 0x30, 0x30 + headers
    wanted = {v: k for k, v in DS_SOUNDS.items()}
    for _ in range(count):
        size = struct.unpack_from("<H", d, pos)[0]
        name = d[pos + 2:pos + 32].split(b"\0")[0].decode("latin1")
        length = struct.unpack_from("<I", d, pos + 36)[0]
        if name in wanted:
            frames = bytearray()
            p, end = cur, cur + length
            while p + 4 <= end:
                fl = _mp3_frame_len(d[p:p + 4])
                if not fl:
                    p += 1
                    continue
                frames += d[p:p + fl]
                p += fl
            mp3 = os.path.join(tempfile.gettempdir(), "ultrasouls_ds.mp3")
            wav = os.path.join(tempfile.gettempdir(), "ultrasouls_ds.wav")
            try:
                open(mp3, "wb").write(frames)
                subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", mp3, "-ar", "48000", "-ac", "1", "-c:a", "pcm_s16le", wav], check=True)
                with wave.open(wav, "rb") as w:
                    out[wanted[name]] = (w.getframerate(), w.getnchannels(), w.readframes(w.getnframes()))
            except Exception as e:
                print("SKIPPED %s of Dark Souls: %s" % (name, e))
            finally:
                for f in (mp3, wav):
                    if os.path.exists(f):
                        os.remove(f)
        cur += length
        pos += size
    return out


def made_sounds():
    """Sounds of your own, which are neither game's: the files in reference/sounds (a folder that is not in the
    repository) go into the pack. A 16-bit WAV is read as it is; anything else (MP3, OGG, M4A) is decoded by
    ffmpeg if there is one (on the PATH or at C:\ffmpeg\ffmpeg.exe). Silence at the front is cut off, so a sound
    starts when it is asked for. The pack name is the file's name, or the one names.txt in that folder gives it
    (a line each: FILE NAME). The mod plays these if they are there:
      visc_parry   as a parry stuns an enemy, opening it to the visceral attack
      visc_in      as the hand goes in
      visc_rip     as it is torn out
    {name: (rate, channels, 16-bit PCM)}"""
    import glob
    import shutil
    import subprocess
    import tempfile
    import wave
    import numpy as np
    folder = os.path.join(HERE, "..", "reference", "sounds")
    names = {}
    try:
        for line in open(os.path.join(folder, "names.txt"), encoding="utf-8"):
            parts = line.split()
            if len(parts) >= 2 and not line.lstrip().startswith("#"):
                names[parts[0].lower()] = parts[1].lower()
    except OSError:
        pass
    ffmpeg = find_ffmpeg()
    out = {}
    for path in sorted(glob.glob(os.path.join(folder, "*"))):
        base = os.path.basename(path).lower()
        ext = os.path.splitext(base)[1]
        if ext not in (".wav", ".mp3", ".ogg", ".m4a", ".flac"):
            continue
        name = names.get(base, os.path.splitext(base)[0])[:39]
        if base not in names and not name.startswith("visc_"):
            continue                             # (a file nobody has given a part to stays out of the pack)
        tmp = None
        try:
            src = path
            if ext != ".wav":
                if not ffmpeg:
                    print("SKIPPED %s: ffmpeg is needed for anything but WAV" % path)
                    continue
                tmp = os.path.join(tempfile.gettempdir(), "ultrasouls_snd.wav")
                subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", path, "-ar", "48000", "-c:a", "pcm_s16le", tmp], check=True)
                src = tmp
            with wave.open(src, "rb") as w:
                if w.getsampwidth() != 2 or w.getnchannels() not in (1, 2):
                    print("SKIPPED %s: it has to be 16-bit, mono or stereo" % path)
                    continue
                rate, channels = w.getframerate(), w.getnchannels()
                pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, channels)
            loud = np.abs(pcm).max(axis=1)
            first = int(np.argmax(loud > 0.03 * max(int(loud.max()), 1)))
            first = max(0, first - rate // 200)
            out[name] = (rate, channels, pcm[first:].tobytes())
        except Exception as e:
            print("SKIPPED %s: %s" % (path, e))
        finally:
            if tmp and os.path.exists(tmp):
                os.remove(tmp)
    return out


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
    for name, (rate, channels, pcm) in ds_sounds().items():
        frames = len(pcm) // (2 * channels)
        entries.append((name, rate, channels, frames, pcm))
        print("%-18s %6d Hz, %d ch, %.2f s (Dark Souls' own %s, from your copy)" % (name, rate, channels, frames / rate, DS_SOUNDS[name]))
    for name, (rate, channels, pcm) in made_sounds().items():
        entries = [e for e in entries if e[0] != name]
        frames = len(pcm) // (2 * channels)
        entries.append((name, rate, channels, frames, pcm))
        print("%-18s %6d Hz, %d ch, %.2f s (your own, from reference/sounds)" % (name, rate, channels, frames / rate))
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
