"""Drive Dark Souls Remastered to test the mod without a person at the keyboard.

  python tools/game.py start                 launch through Steam and load the last save
  python tools/game.py shot NAME [SCALE]     screenshot -> build/shots/NAME.png and NAME_small.png
  python tools/game.py do CMD [CMD ...]      input commands, e.g. "key 0x74" "rel 300 0" "mdown 960 540" "wait 1.5" "mup"
  python tools/game.py clip NAME CMD [...]   the same input commands, recorded: frames -> build/shots/NAME_000.png ...
  python tools/game.py log [N]               last N lines of the mod's log
  python tools/game.py hp                    the player's health, read from the game's memory
  python tools/game.py quit [--keep]         save, return to the title screen and close the game, then put
                                             the save file back as it was before `start` (unless --keep)

Uses the universal-modder plugin's `um.win` (window capture through ffmpeg's gfxcapture, input through
SendInput). The click points are for a 1920x1080 window. The game must be set to start offline; `start`
stops if the "Game will start in offline mode" notice does not appear.

`start` copies the save file to build/save_backups first, and `quit` restores that copy, so a test
session (deaths, lost souls, used items, a moved character) leaves no trace in the save.

Input commands are WinDrive's: key <vk>, hold <vk> <ms>, click x y, mdown x y / mup, rel dx dy, type text.
Useful keys: F5 0x74, Space 0x20, Shift 0x10, Ctrl 0x11, W A S D 0x57 0x41 0x53 0x44, Esc 0x1B.
"""
import ctypes
import ctypes.wintypes
import glob
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(HERE, "..", "build", "shots")
GAME_DIR = r"F:\SteamLibrary\steamapps\common\DARK SOULS REMASTERED"
LOG = os.path.join(GAME_DIR, "ultrasouls.log")
EXE = "DarkSoulsRemastered.exe"
PROC = "DarkSoulsRemastered"
STEAM_APP = "570940"

BACKUPS = os.path.join(HERE, "..", "build", "save_backups")
MARKER = os.path.join(BACKUPS, "session.txt")      # holds the path of the copy made by the running session
KEEP_BACKUPS = 20


def save_file():
    """The most recently written DRAKS0005.sl2 under Documents/NBGI (Documents may be redirected)."""
    buf = ctypes.create_unicode_buffer(ctypes.wintypes.MAX_PATH)
    ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buf)      # CSIDL_PERSONAL
    found = glob.glob(os.path.join(buf.value, "NBGI", "DARK SOULS REMASTERED", "*", "DRAKS0005.sl2"))
    return max(found, key=os.path.getmtime) if found else None


def backup_save():
    src = save_file()
    if not src:
        sys.exit("save file not found: not starting a test session without a backup")
    os.makedirs(BACKUPS, exist_ok=True)
    dst = os.path.join(BACKUPS, time.strftime("DRAKS0005_%Y%m%d_%H%M%S.sl2"))
    shutil.copy2(src, dst)
    with open(MARKER, "w", encoding="utf-8") as f:
        f.write(dst + "\n" + src + "\n")
    old = sorted(glob.glob(os.path.join(BACKUPS, "DRAKS0005_*.sl2")))
    for path in old[:-KEEP_BACKUPS]:
        os.remove(path)
    print("save backed up to", dst)


def restore_save():
    if not os.path.exists(MARKER):
        print("no backup from this session: save left as the game wrote it")
        return
    dst, src = open(MARKER, encoding="utf-8").read().split("\n")[:2]
    time.sleep(6)                             # let the game's last write and Steam's sync finish
    shutil.copyfile(dst, src)                 # copyfile: the restored save gets a new timestamp
    os.remove(MARKER)
    print("save restored from", dst)


# click points, 1920x1080
OFFLINE_OK = (960, 660)
UNCLEAN_OK = (960, 770)        # "Last time, the game may not have been closed using Quit Game..." 
MENU_CONTINUE = (960, 788)
TAB_SYSTEM = (1621, 130)
SYSTEM_QUIT = (567, 488)
QUIT_OK = (758, 660)


def _plugin():
    roots = sorted(glob.glob(os.path.expanduser(r"~\.claude\plugins\cache\universal-modder\universal-modder\*")))
    if not roots:
        sys.exit("universal-modder plugin not found")
    sys.path.insert(0, roots[-1])
    import um.win as win
    return win


win = _plugin()


def running():
    return win.pid_of(PROC)


def shot(name, scale=0.5):
    os.makedirs(SHOTS, exist_ok=True)
    return win.shot(os.path.join(SHOTS, name + ".png"), exe=EXE, scale=scale)


def _highlighted(path, point):
    """True if the menu's orange highlight bar is at `point` (it is about (99, 47, 14))."""
    from PIL import Image
    r, g, b = Image.open(path).convert("RGB").getpixel(point)
    return 80 <= r <= 135 and 30 <= g <= 70 and b <= 35


def log_lines():
    try:
        return open(LOG, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return []


def do(drive, commands):
    for c in commands:
        if c.startswith("wait "):
            time.sleep(float(c.split()[1]))
        else:
            print(c, "->", drive.cmd(c))


def clip(name, commands, fps=20, width=960):
    """Record the game window while a `do` sequence runs, then save the frames as SHOTS/NAME_000.png ...
    (for effects that are over in a fraction of a second: an explosion, a muzzle flash, pellets in flight)."""
    import subprocess
    os.makedirs(SHOTS, exist_ok=True)
    base = os.path.join(SHOTS, name)
    for old in glob.glob(base + "_[0-9][0-9][0-9].png"):
        os.remove(old)
    rec = win.Recorder(exe=EXE, out=base, fps=fps, audio=False)
    rec.start()
    time.sleep(0.6)                                # the recorder needs a moment before its first frame
    do(win.Drive(PROC), commands)
    rec.stop()
    subprocess.run([win.ffmpeg_win(), "-hide_banner", "-loglevel", "error", "-y", "-i", base + ".mkv", "-vf", "scale=%d:-2" % width,
                    base + "_%03d.png"], check=False)
    frames = sorted(glob.glob(base + "_[0-9][0-9][0-9].png"))
    print("%d frames at %d fps: %s ... %s" % (len(frames), fps, frames[0] if frames else "-", frames[-1] if frames else "-"))
    return frames


def start():
    if not running():
        backup_save()
        win.launch(STEAM_APP, [], steam=True)
        for _ in range(90):
            if running():
                break
            time.sleep(2)
        else:
            sys.exit("the game window did not appear")
        time.sleep(8)
    d = win.Drive(PROC)
    saw_offline = False
    for step in range(40):
        path = shot("_start", scale=None)
        if _highlighted(path, (OFFLINE_OK[0] - 110, OFFLINE_OK[1])):
            saw_offline = True
            do(d, ["focus", "click %d %d" % OFFLINE_OK])
        elif _highlighted(path, (MENU_CONTINUE[0] - 110, MENU_CONTINUE[1])):
            if not saw_offline:
                sys.exit("reached the main menu without the offline notice: not continuing")
            do(d, ["focus", "click %d %d" % MENU_CONTINUE])
            break
        elif _highlighted(path, (UNCLEAN_OK[0] - 110, UNCLEAN_OK[1])):
            do(d, ["focus", "click %d %d" % UNCLEAN_OK])   # checked last, so it can never stand in for the offline notice
        else:
            do(d, ["focus", "scanmode on", "key 0x0D"])   # logos and "press any button"
        time.sleep(3)
    else:
        sys.exit("could not get through the title screen")
    for _ in range(60):
        if any(l.startswith("physics step hook") for l in log_lines()):
            print("in game; mod hooks installed")
            try:
                protect(True)
            except Exception as e:
                print("could not set the no-damage flag:", e)
            return
        time.sleep(1)
    sys.exit("the save did not load (no hook line in the mod log)")


def _process():
    """(handle, exe base) of the running game, opened for reading and writing its memory."""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    k32.OpenProcess.restype = ctypes.wintypes.HANDLE
    psapi.EnumProcessModules.argtypes = [ctypes.wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p), ctypes.wintypes.DWORD,
                                         ctypes.POINTER(ctypes.wintypes.DWORD)]
    h = k32.OpenProcess(0x0010 | 0x0020 | 0x0008 | 0x0400, False, running())
    mods = (ctypes.c_void_p * 1)()
    need = ctypes.wintypes.DWORD()
    psapi.EnumProcessModules(h, mods, ctypes.sizeof(mods), ctypes.byref(need))
    return k32, h, mods[0]


def _read(k32, h, addr, n):
    k32.ReadProcessMemory.argtypes = [ctypes.wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        raise OSError("read failed at %x" % addr)
    return buf.raw


# The player object is [[exe+1C77E50]+0x68]; its second flag word is at +0x524 (DSR-Gadget's ChrFlags2 at
# 0x514, moved by 0x10 in this build, as health is). Bit 0x40 is "no damage".
PLAYER_FLAGS2 = 0x524
FLAG_NO_DAMAGE = 0x40


def protect(on=True):
    """Make the player take no damage for this session, so a character left standing among enemies while a
    test script runs is still alive at the end. The flag lives in memory only, and the save is restored anyway."""
    import struct
    k32, h, base = _process()
    world = struct.unpack("<Q", _read(k32, h, base + 0x1C77E50, 8))[0]
    player = struct.unpack("<Q", _read(k32, h, world + 0x68, 8))[0]
    flags = struct.unpack("<I", _read(k32, h, player + PLAYER_FLAGS2, 4))[0]
    new = (flags | FLAG_NO_DAMAGE) if on else (flags & ~FLAG_NO_DAMAGE)
    k32.WriteProcessMemory.argtypes = [ctypes.wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    got = ctypes.c_size_t()
    ok = k32.WriteProcessMemory(h, ctypes.c_void_p(player + PLAYER_FLAGS2), struct.pack("<I", new), 4, ctypes.byref(got))
    hp = struct.unpack("<ii", _read(k32, h, player + 0x3E8, 8))
    print("player flags %08x -> %08x (%s); hp %d/%d" % (flags, new, "written" if ok else "WRITE FAILED", hp[0], hp[1]))


def player_hp():
    import struct
    k32, h, base = _process()
    world = struct.unpack("<Q", _read(k32, h, base + 0x1C77E50, 8))[0]
    player = struct.unpack("<Q", _read(k32, h, world + 0x68, 8))[0]
    return struct.unpack("<ii", _read(k32, h, player + 0x3E8, 8))


def first_person_on():
    state = False
    for l in log_lines():
        if l.startswith("first person on"):
            state = True
        elif l.startswith("first person off"):
            state = False
    return state


def quit_game(keep=False):
    pid = running()
    if not pid:
        print("not running")
        if not keep:
            restore_save()
        return
    d = win.Drive(PROC)
    if first_person_on():
        do(d, ["focus", "key 0x74", "wait 1"])       # menus need the cursor, which first person holds
    do(d, ["focus", "key 0x1B", "wait 1.5", "click %d %d" % TAB_SYSTEM, "wait 1.5", "click %d %d" % SYSTEM_QUIT, "wait 1.5",
           "click %d %d" % QUIT_OK, "wait 12"])
    path = shot("_quit", scale=None)
    from PIL import Image
    if max(Image.open(path).convert("RGB").getpixel((960, 840))) < 150:
        sys.exit("did not reach the title screen; leaving the game running")
    win.kill(pid)
    for _ in range(20):                      # the DLL stays locked until the process is really gone
        if not running():
            break
        time.sleep(0.5)
    time.sleep(1.5)
    print("saved, returned to the title screen and closed")
    if keep:
        if os.path.exists(MARKER):
            os.remove(MARKER)
        print("save kept as the game wrote it")
    else:
        restore_save()


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "start":
        start()
    elif cmd == "shot" and len(sys.argv) >= 3:
        print(shot(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 0.5))
    elif cmd == "do":
        do(win.Drive(PROC), sys.argv[2:])
    elif cmd == "clip" and len(sys.argv) >= 4:
        clip(sys.argv[2], sys.argv[3:])
    elif cmd == "log":
        print("\n".join(log_lines()[-(int(sys.argv[2]) if len(sys.argv) > 2 else 20):]))
    elif cmd == "hp":
        print("player hp %d/%d" % player_hp())
    elif cmd == "quit":
        quit_game(keep="--keep" in sys.argv[2:])
    else:
        print(__doc__)
