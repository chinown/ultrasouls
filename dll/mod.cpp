// ULTRASOULS v0.12: dinput8.dll proxy.
//   - F5: first person on/off: mouse look, WASD movement relative to the view, body faces the view
//       F1/F2 field of view, F3/F4 eye height, F9/F10 mouse sensitivity
//   - stamina is held at max (rolls and sprinting are free)
//   - animation speed multiplier (movement comes from animation, so this is also move speed)
//       F6 slower, F7 faster, F8 toggle on/off
//   - J: jump driven through the game's own physics step (velocity), so walls and ceilings collide
//   - K: old jump that writes the height directly (no collision; kept for comparison)
// Log: ultrasouls.log next to the exe.
#include <windows.h>
#include <psapi.h>
#include <d3d11_1.h>
#include <dxgi.h>

#include "hud.h"
#include "sound.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>

// Verified against the probe log (exe 1.0.0.0, WorldChrBase at exe+1C77E50)
static const uintptr_t OFF_PLAYER = 0x68;       // WorldChr -> player
static const uintptr_t OFF_HP = 0x3E8;          // int, max at +4
static const uintptr_t OFF_STAMINA = 0x3F8;     // int, max at +4
static const uintptr_t OFF_MAP = 0x68;          // player -> ChrMapData
static const uintptr_t OFF_MAP_ANIM = 0x18;     // ChrMapData -> ChrAnimData
static const uintptr_t OFF_MAP_POS = 0x28;      // ChrMapData -> ChrPosData
static const uintptr_t OFF_POS_X = 0x10;
static const uintptr_t OFF_POS_Y = 0x14;        // up
static const uintptr_t OFF_POS_Z = 0x18;
static const uintptr_t OFF_ANIM_SPEED = 0xA8;   // confirmed in v0.1

// From disassembly of the exe. ChrPosData::Step(this, hkStepInfo*, gravity*) copies the position into
// the Havok character proxy and integrates it with collision; it is the only writer of the position.
static const uintptr_t RVA_STEP = 0x2BC650;
static const uint8_t STEP_PROLOGUE[8] = {0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x60};
static const uintptr_t OFF_POS_PROXY = 0x38;    // ChrPosData -> physics character wrapper
static const uintptr_t OFF_PROXY_VEL = 0x60;    // wrapper velocity, xyz floats (unverified: v0.7 tests this)

static const float JUMP_V0 = 9.0f;   // m/s
static const float JUMP_G = 20.0f;   // m/s^2 while rising; the game's own gravity handles the fall
static const float AIR_CARRY = 1.3f; // horizontal speed kept in the air, as a multiple of ground speed at takeoff

static HMODULE g_real = nullptr;
static FILE *g_log = nullptr;

static void logf(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// ---- mouse deltas, for a view the game's follow camera cannot drag around.
// Two possible sources: the game's DirectInput mouse device (we are its dinput8.dll) or raw input
// messages to its window. Both are counted; the camera uses whichever one is producing data.
static volatile LONG g_di_dx = 0, g_di_dy = 0, g_di_events = 0;
static volatile LONG g_raw_dx = 0, g_raw_dy = 0, g_raw_events = 0;
static void *g_mouse_dev = nullptr;
enum { MOUSE_DEVS = 64 };
static void *volatile g_mouse_devs[MOUSE_DEVS];   // recent devices created with the mouse GUID
static volatile LONG g_mouse_dev_n = 0, g_di_state_calls = 0, g_di_data_calls = 0;
static volatile LONG g_cur_dx = 0, g_cur_dy = 0, g_cur_events = 0;   // third source: the Windows cursor

static bool is_mouse_dev(void *dev) {
    for (int i = 0; i < MOUSE_DEVS; i++)
        if (g_mouse_devs[i] == dev) return true;
    return false;
}
static GUID g_dev_guids[8];
static volatile LONG g_dev_n = 0;
static const GUID GUID_SYS_MOUSE = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

typedef HRESULT(WINAPI *CreateDeviceFn)(void *self, const GUID &guid, void **dev, void *outer);
typedef HRESULT(WINAPI *GetStateFn)(void *self, DWORD cb, void *data);
typedef HRESULT(WINAPI *GetDataFn)(void *self, DWORD cb_obj, void *rgdod, DWORD *in_out, DWORD flags);
static CreateDeviceFn g_orig_create_device = nullptr;
static const GUID GUID_SYS_KEYBOARD = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// While first person is on, the game must not see the keys ULTRAKILL uses (Space roll, Shift and Ctrl
// bindings), so they are cleared from the keyboard state before it reaches the game. Our own controls
// read the keys through GetAsyncKeyState, which this does not touch.
static volatile bool g_block_keys = false, g_block_lmb = false;
static const uint8_t BLOCKED_KEYS[] = {0x39, 0x2A, 0x36, 0x1D, 0x9D, 0x21, 0x22};   // DIK_SPACE, L/R SHIFT, L/R CONTROL, F, G

// The game has a second keyboard reader: a table of 236 virtual keys polled with GetAsyncKeyState
// (exe+CA06A0), which is what its menus listen to. With only DirectInput filtered, G still opened the
// gesture menu. Its import slot is pointed at a filter that reports the same keys as up.
static const uintptr_t RVA_IAT_ASYNC_KEY = 0x2017C14;
static const int BLOCKED_VKS[] = {VK_SPACE, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, 'F', 'G'};
typedef SHORT (WINAPI *AsyncKeyFn)(int);
static AsyncKeyFn g_orig_async_key = nullptr;

static SHORT WINAPI my_async_key(int vk) {
    if (g_block_keys)
        for (int k : BLOCKED_VKS)
            if (vk == k) return 0;
    return g_orig_async_key(vk);
}

static bool hook_async_key() {
    void **slot = (void **)((uintptr_t)GetModuleHandleA(nullptr) + RVA_IAT_ASYNC_KEY);
    void *real = (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "GetAsyncKeyState");
    if (*slot == (void *)my_async_key) return true;
    if (*slot != real) return false;              // not the slot this was written for: leave it alone
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return false;
    g_orig_async_key = (AsyncKeyFn)real;
    *slot = (void *)my_async_key;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return true;
}

// The game confines the Windows cursor to the client area of whatever window is in front (exe+CA0B40
// asks for the foreground window, not its own) and keeps doing it from the background. With ULTRAKILL
// in front on one of two monitors that replaced ULTRAKILL's own cursor lock, and its cursor wandered off
// to the other monitor. The game's ClipCursor import goes through this, which drops the call unless one
// of this process's windows is the one in front.
static const uintptr_t RVA_IAT_CLIP_CURSOR = 0x2017B94;
typedef BOOL (WINAPI *ClipCursorFn)(const RECT *);
static ClipCursorFn g_orig_clip_cursor = nullptr;
static volatile LONG g_clips_dropped = 0;
static BOOL WINAPI my_clip_cursor(const RECT *rc) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) {
        InterlockedIncrement(&g_clips_dropped);
        return TRUE;
    }
    return g_orig_clip_cursor(rc);
}
static bool hook_clip_cursor() {
    void **slot = (void **)((uintptr_t)GetModuleHandleA(nullptr) + RVA_IAT_CLIP_CURSOR);
    void *real = (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "ClipCursor");
    if (*slot == (void *)my_clip_cursor) return true;
    if (*slot != real) return false;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return false;
    g_orig_clip_cursor = (ClipCursorFn)real;
    *slot = (void *)my_clip_cursor;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return true;
}

// Devices of different kinds may have different vtables, so the originals are kept per vtable.
struct DevVtable {
    void **vt;
    GetStateFn get_state;
    GetDataFn get_data;
};
static DevVtable g_dev_vts[4];
static volatile LONG g_dev_vt_n = 0;
static volatile LONG g_keys_blocked = 0;

static const DevVtable *find_vt(void *dev) {
    void **vt = *(void ***)dev;
    for (LONG i = 0; i < g_dev_vt_n; i++)
        if (g_dev_vts[i].vt == vt) return &g_dev_vts[i];
    return nullptr;
}

static void patch_vtable(void *obj, int index, void *replacement, void **original) {
    void **vt = *(void ***)obj;
    if (vt[index] == replacement) return;
    DWORD old;
    if (!VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &old)) return;
    *original = vt[index];
    vt[index] = replacement;
    VirtualProtect(&vt[index], sizeof(void *), old, &old);
}

static HRESULT WINAPI my_get_state(void *self, DWORD cb, void *data) {
    const DevVtable *o = find_vt(self);
    if (!o) return E_FAIL;
    HRESULT hr = o->get_state(self, cb, data);
    InterlockedIncrement(&g_di_state_calls);
    // v0.12 matched on the device pointer and saw nothing: the game creates dozens of devices.
    // The state size identifies a mouse instead (DIMOUSESTATE is 16 bytes, DIMOUSESTATE2 is 20).
    if (SUCCEEDED(hr) && data && (cb == 16 || cb == 20)) {
        // DIMOUSESTATE starts with LONG lX, lY
        InterlockedExchangeAdd(&g_di_dx, ((LONG *)data)[0]);
        InterlockedExchangeAdd(&g_di_dy, ((LONG *)data)[1]);
        InterlockedIncrement(&g_di_events);
        if (g_block_lmb) ((uint8_t *)data)[12] = ((uint8_t *)data)[13] = 0;   // rgbButtons[0..1] follow lX, lY, lZ
    }
    if (SUCCEEDED(hr) && data && cb == 256 && g_block_keys) {
        uint8_t *keys = (uint8_t *)data;
        for (uint8_t k : BLOCKED_KEYS)
            if (keys[k]) { keys[k] = 0; InterlockedIncrement(&g_keys_blocked); }
    }
    return hr;
}

static HRESULT WINAPI my_get_data(void *self, DWORD cb_obj, void *rgdod, DWORD *in_out, DWORD flags) {
    const DevVtable *o = find_vt(self);
    if (!o) return E_FAIL;
    HRESULT hr = o->get_data(self, cb_obj, rgdod, in_out, flags);
    InterlockedIncrement(&g_di_data_calls);
    if (SUCCEEDED(hr) && is_mouse_dev(self) && rgdod && in_out && !(flags & 1) && cb_obj >= 8) {
        // DIDEVICEOBJECTDATA starts with DWORD dwOfs, dwData; offsets 0 and 4 are the X and Y axes
        for (DWORD i = 0; i < *in_out; i++) {
            const DWORD *e = (const DWORD *)((const uint8_t *)rgdod + i * cb_obj);
            if (e[0] == 0) InterlockedExchangeAdd(&g_di_dx, (LONG)e[1]);
            if (e[0] == 4) InterlockedExchangeAdd(&g_di_dy, (LONG)e[1]);
        }
        if (*in_out) InterlockedIncrement(&g_di_events);
    } else if (SUCCEEDED(hr) && rgdod && in_out && cb_obj >= 8 && g_block_keys && !is_mouse_dev(self)) {
        // buffered keyboard events: dwOfs is the key code; turn blocked keys into "released"
        for (DWORD i = 0; i < *in_out; i++) {
            DWORD *e = (DWORD *)((uint8_t *)rgdod + i * cb_obj);
            for (uint8_t k : BLOCKED_KEYS)
                if (e[0] == k && (e[1] & 0x80)) { e[1] = 0; InterlockedIncrement(&g_keys_blocked); }
        }
    }
    return hr;
}

static void hook_device(void *dev) {
    if (find_vt(dev) || g_dev_vt_n >= 4) return;
    // record the originals before redirecting, so a call arriving mid-patch always finds them
    DevVtable &d = g_dev_vts[g_dev_vt_n];
    d.vt = *(void ***)dev;
    d.get_state = (GetStateFn)d.vt[9];
    d.get_data = (GetDataFn)d.vt[10];
    InterlockedIncrement(&g_dev_vt_n);
    void *unused;
    patch_vtable(dev, 9, (void *)my_get_state, &unused);
    patch_vtable(dev, 10, (void *)my_get_data, &unused);
}

static HRESULT WINAPI my_create_device(void *self, const GUID &guid, void **dev, void *outer) {
    HRESULT hr = g_orig_create_device(self, guid, dev, outer);
    LONG n = InterlockedIncrement(&g_dev_n) - 1;
    if (n < 8) g_dev_guids[n] = guid;
    if (SUCCEEDED(hr) && dev && *dev && IsEqualGUID(guid, GUID_SYS_MOUSE)) {
        g_mouse_dev = *dev;
        g_mouse_devs[(InterlockedIncrement(&g_mouse_dev_n) - 1) % MOUSE_DEVS] = *dev;
        hook_device(*dev);
    }
    if (SUCCEEDED(hr) && dev && *dev && IsEqualGUID(guid, GUID_SYS_KEYBOARD)) hook_device(*dev);
    return hr;
}

static void watch_dinput(void *dinput) {
    patch_vtable(dinput, 3, (void *)my_create_device, (void **)&g_orig_create_device);
}

static WNDPROC g_orig_wndproc = nullptr;
static LRESULT CALLBACK my_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            InterlockedExchangeAdd(&g_raw_dx, ri.data.mouse.lLastX);
            InterlockedExchangeAdd(&g_raw_dy, ri.data.mouse.lLastY);
            InterlockedIncrement(&g_raw_events);
        }
    }
    return CallWindowProcW(g_orig_wndproc, wnd, msg, wp, lp);
}

static BOOL CALLBACK find_game_window(HWND wnd, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(wnd) || GetWindow(wnd, GW_OWNER)) return TRUE;
    *(HWND *)out = wnd;
    return FALSE;
}

static HWND g_game_wnd_early = nullptr;
static bool watch_raw_input() {
    HWND wnd = nullptr;
    EnumWindows(find_game_window, (LPARAM)&wnd);
    if (!wnd) return false;
    g_game_wnd_early = wnd;
    g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(wnd, GWLP_WNDPROC, (LONG_PTR)my_wndproc);
    return g_orig_wndproc != nullptr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID *out, LPUNKNOWN outer) {
    typedef HRESULT(WINAPI *Fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    if (!g_real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\dinput8.dll");
        g_real = LoadLibraryA(path);
    }
    Fn fn = g_real ? (Fn)GetProcAddress(g_real, "DirectInput8Create") : nullptr;
    HRESULT hr = fn ? fn(inst, ver, iid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) watch_dinput(*out);
    return hr;
}

static bool safe_read(uintptr_t addr, void *out, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n, &got) && got == n;
}
static bool safe_write(uintptr_t addr, const void *in, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 && WriteProcessMemory(GetCurrentProcess(), (LPVOID)addr, in, n, &got) && got == n;
}
template <typename T> static bool rd(uintptr_t addr, T &v) { return safe_read(addr, &v, sizeof(T)); }
template <typename T> static bool wr(uintptr_t addr, T v) { return safe_write(addr, &v, sizeof(T)); }

static double real_s() {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}
// The mod's own clock: real time less the time spent in a parry's freeze, so coins, timers and
// animations stand still for it too.
static volatile double g_frozen_s = 0;
static volatile float g_freeze_request = 0;      // seconds; asked for by a parry, carried out when the frame is presented
// Two switches that only exist in ultrasouls.ini: parry_freeze=0 leaves a plain flash in place of the
// freeze, hud_motion=0 is ULTRAKILL's "reduce HUD motion".
static volatile bool g_parry_freeze = true, g_hud_motion = true;
static double now_s() {
    return real_s() - g_frozen_s;
}

// ---- physics step hook: runs on the game's thread, once per character per frame
typedef void (*StepFn)(void *self, void *step_info, void *gravity);
static StepFn g_orig_step = nullptr;
static volatile uintptr_t g_player_pos = 0;
static volatile uintptr_t g_player_chr = 0;        // the player character object (WorldChr + 0x68)
static volatile bool g_vjump = false;       // velocity jump in progress
static volatile bool g_vel_ok = true;       // the velocity field was confirmed against measured speed in v0.6
static volatile bool g_ctrl = false;        // first-person movement controller active (set by the camera hook)
static volatile float g_view_fwd[2] = {0, 1}, g_view_right[2] = {1, 0};   // horizontal view axes (x, z), unit length
static float g_vx = 0, g_vz = 0;            // controller's horizontal velocity
static const uintptr_t OFF_POS_YAW = 0x04;
static const uintptr_t OFF_POS_FORCE_STEP = 0x1FA;
static const uintptr_t OFF_POS_AIR_TIME = 0x1B4;   // seconds since the grounded flag was last set (read from the disassembly, unverified)
static float g_vy = 0;                      // controller's vertical velocity while airborne
static bool g_air = false;
static int g_air_steps = 0;                 // physics steps since takeoff
static volatile LONG g_landings = 0, g_bonks = 0;

// Height of the Havok character itself (what Step integrates), via the phantom's transform.
static bool proxy_y(uintptr_t proxy, float &y) {
    uintptr_t phantom = *(uintptr_t *)(proxy + 0x80);
    uintptr_t body = phantom ? *(uintptr_t *)(phantom + 0x30) : 0;
    if (!body) return false;
    y = *(const float *)(body + 0x34);
    return true;
}
static volatile bool g_jump_req = false;

// V1's movement, read from ULTRAKILL (NewMovement.Move/Jump, the Player prefab, physics settings):
// walkSpeed 750, jumpPower 90, airAcceleration 6000, mass 100, gravity 40, fixed step 0.008 s,
// capsule 3.5 units tall. One unit is taken as 0.5 m so V1's capsule matches the character's height.
static const float UK_UNIT = 0.5f;
static const float V1_STEP = 0.008f;
static const float V1_RUN_SPEED = 750.0f * V1_STEP * 2.75f * UK_UNIT;                    // 16.5 u/s -> 8.25 m/s
static const float V1_JUMP_SPEED = 90.0f * 1500.0f * 2.6f * V1_STEP / 100.0f * UK_UNIT;  // 28.08 u/s -> 14.04 m/s
static const float V1_SLIDE_JUMP_SPEED = V1_JUMP_SPEED * 2.0f / 2.6f;                    // 21.6 u/s -> 10.8 m/s
static const float V1_DASH_JUMP_SPEED = V1_JUMP_SPEED * 1.5f / 2.6f;                     // 16.2 u/s -> 8.1 m/s
static const float V1_GRAVITY = 40.0f * UK_UNIT;                                         // 20 m/s^2
static const float V1_AIR_ACCEL = 6000.0f / 100.0f * UK_UNIT;                            // 60 u/s^2 -> 30 m/s^2
// NewMovement.Dodge / TryDash / TryStartSlam / Update:
static const float V1_DASH_SPEED = V1_RUN_SPEED * 3.0f;                                  // 49.5 u/s -> 24.75 m/s
static const float V1_DASH_TIME = 25 * V1_STEP;                                         // boostLeft 100, -4 per step: 0.2 s
static const float V1_SLIDE_SPEED = 750.0f * V1_STEP * 4.0f * UK_UNIT;                  // 24 u/s -> 12 m/s
static const float V1_SLIDE_STEER = 5.0f * UK_UNIT;                                     // sideways input while sliding
static const float V1_SLAM_SPEED = 100.0f * UK_UNIT;                                    // 100 u/s down -> 50 m/s
static const float V1_BOOST_REGEN = 70.0f;                                              // stamina per second, 300 max, dash costs 100

static float g_boost = 300.0f, g_dash_left = 0, g_dash_x = 0, g_dash_z = 0, g_slide_x = 0, g_slide_z = 0;
static bool g_sliding = false, g_slamming = false, g_prev_shift = false, g_prev_ctrl = false;
static volatile LONG g_dashes = 0, g_slides = 0, g_slams = 0;

// NewMovement.WallJump / Cling / FixedUpdate / TryStartSlam and GroundCheck (decompiled C#):
static const float V1_WALL_JUMP_SPEED = 2000.0f * 150.0f * V1_STEP / 100.0f * UK_UNIT;   // 24 u/s -> 12 m/s, both up and away from the wall
static const float V1_WALL_JUMP_COOLDOWN = 0.1f;
static const int V1_WALL_JUMPS = 3;                                                     // until the next landing
static const float V1_SLAM_JUMP_WINDOW = 0.1f + 0.306f;                                 // superJumpChance, then extraJumpChance
static const float V1_SLAM_MIN_AIR = 0.1f;                                              // fallTime > 0.5, which grows by 5 per second
static const float V1_FALL_SPEED_MAX = 100.0f * UK_UNIT;                                // 50 m/s
static const float V1_PRESLIDE_UNIT = 24.0f * UK_UNIT;                                  // speed / 24 u/s is the slide's multiplier
static const float V1_PRESLIDE_DELAY = 0.2f;
static const float WALL_GRACE = 0.1f;        // ours: how long a wall contact stays usable (V1's wall check is a wider trigger volume)

static float g_slam_force = 0, g_slam_window = 0, g_slam_time = 0, g_air_time = 0;
static float g_wall_nx = 0, g_wall_nz = 0, g_wall_dist = 0, g_wall_age = 1e9f, g_wall_cd = 0, g_cling_fade = 0;
static float g_preslide = 0, g_preslide_delay = 0;
static int g_wall_jumps = 0;
static volatile LONG g_wall_jump_count = 0, g_slam_jump_count = 0, g_clings = 0;
static volatile int g_seen_manifold = 0;
static volatile float g_slide_mult0 = 1.0f;     // the multiplier the current slide started with (for the log)

// hkpCharacterProxy keeps the contact points its last integrate found in m_manifold, an hkArray at +0x20.
// Each hkpRootCdPoint is 0x40 bytes: the contact position, then the surface normal with the distance in w.
// A wall is a contact whose normal is close to horizontal. Returns the nearest one.
static const uintptr_t OFF_PROXY_MANIFOLD = 0x20;
static volatile float g_dbg_integrated = 0;     // how far the last integrate moved the player vertically
static volatile bool g_integrated_seen = false;   // set by the re-check hook during the current Step
static volatile LONG g_pull_fixes = 0;
// Position of the Havok character itself, via the phantom's transform.
static bool proxy_pos(uintptr_t proxy, float *out) {
    uintptr_t phantom = *(uintptr_t *)(proxy + 0x80);
    uintptr_t body = phantom ? *(uintptr_t *)(phantom + 0x30) : 0;
    if (!body) return false;
    memcpy(out, (const void *)(body + 0x30), 12);
    return true;
}
static const float RESCUE_DROP = 30.0f;          // metres below the last place stood on
// test aid: a move of the player asked for by the per-frame code, carried out in the player's next step
static volatile bool g_teleport_pending = false;
static float g_teleport_delta[3];
static float g_safe_pos[3], g_ground_time = 0;
static bool g_have_safe = false;
static volatile LONG g_rescues = 0;
enum { MAX_WALLS = 6 };
static float g_walls[MAX_WALLS][2];          // horizontal normals of every wall touched in the last step
static int g_wall_count = 0;
static bool wall_contact(uintptr_t proxy, float &nx, float &nz, float &dist) {
    const float *pts = *(const float **)(proxy + OFF_PROXY_MANIFOLD);
    int n = *(const int *)(proxy + OFF_PROXY_MANIFOLD + 8);
    g_seen_manifold = n;
    g_wall_count = 0;
    if (!pts || n <= 0 || n > 64) return false;
    bool found = false;
    float best = 1e9f;
    for (int i = 0; i < n; i++) {
        const float *nrm = pts + i * 16 + 4;
        if (fabsf(nrm[1]) > 0.35f) continue;             // floors, ceilings and slopes one can stand on
        float h = sqrtf(nrm[0] * nrm[0] + nrm[2] * nrm[2]);
        if (h < 0.5f || h > 1.5f) continue;
        if (g_wall_count < MAX_WALLS) {
            g_walls[g_wall_count][0] = nrm[0] / h;
            g_walls[g_wall_count][1] = nrm[2] / h;
            g_wall_count++;
        }
        if (nrm[3] > best) continue;
        best = nrm[3];
        nx = nrm[0] / h;
        nz = nrm[2] / h;
        dist = nrm[3];
        found = true;
    }
    return found;
}

static int key_down(int vk) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
}
static double g_vjump_t0 = 0;
static float g_carry = 0, g_dir_x = 0, g_dir_z = 0;
static volatile float g_seen_vel[3];        // what the game asked for this frame, before we touch it
static volatile LONG g_steps = 0, g_steps_alt = 0;
static StepFn g_orig_step_alt = nullptr;
static volatile float g_seen_snap = 0;
static volatile uint8_t g_seen_grounded = 0;

// Used by Step itself (same calls, same order): lock the physics world, set the proxy position, unlock.
static const uintptr_t RVA_PHYS_WORLD = 0x1C74870;     // global; lock object at [+0x28]
static const uintptr_t RVA_PHYS_LOCK = 0x2AF810;
static const uintptr_t RVA_PHYS_UNLOCK = 0x2AF820;
static const uintptr_t RVA_PROXY_SET_POS = 0xA79AE0;   // (hkpCharacterProxy*, hkVector4*)
static const uintptr_t OFF_POS_SNAP_A = 0xC4;          // vertical offsets Step adds after integrating (guess: ground snap)
static const uintptr_t OFF_POS_SNAP_B = 0x154;
static const uintptr_t OFF_POS_GROUNDED = 0x32;
static const uintptr_t OFF_POS_NO_ADJUST = 0x1F8;

static void try_instant_fire();

// The game steps a character through one of two functions with the same arguments, chosen by a flag
// on the character (ChrPosData+0x6D). Hooking only the first meant the controller did not run on
// frames that used the second, which showed up as input lag (a jump waited for the next 'full' step).
// ---- other characters. Step runs for every active character, so the Havok character objects seen here
// are the list of who is around. [HavokChara+0x250] points 0x4F8 into the owning ChrIns (measured on the
// player's own objects). ChrIns+0xD8 is the team: 4 on the hollow player; Dark Souls' own values are
// 6 enemy, 7 boss, 8 friendly NPC, 9 NPC turned hostile.
// An enemy that is idle or has not noticed the player is stepped rarely or not at all after it loads, so
// an entry is kept for as long as its objects still point at each other (ChrIns -> ChrCtrl at +0x68 ->
// HavokChara at +0x28), not for how recently it was stepped. v0.37 only counted characters stepped in the
// last 0.25 s, and coins ignored enemies that were standing near but not fighting.
struct Tracked { uintptr_t havok; double seen; };
enum { MAX_TRACKED = 256 };
static Tracked g_tracked[MAX_TRACKED];
static const uintptr_t OFF_HAVOK_OWNER = 0x250, OWNER_DELTA = 0x4F8, OFF_CHR_TEAM = 0xD8, OFF_CHR_HP = 0x3E8;
static const uintptr_t RVA_VT_ENEMY = 0x1322E68;
static const float TARGET_HEIGHT = 1.2f;         // ours: aim point above an enemy's feet (V1 aims at the weak point)
static void track_character(uintptr_t havok) {
    double t = now_s();
    int spare = -1;
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (g_tracked[i].havok == havok) { g_tracked[i].seen = t; return; }
        if (spare < 0 || (g_tracked[spare].havok && (!g_tracked[i].havok || g_tracked[i].seen < g_tracked[spare].seen))) spare = i;
    }
    g_tracked[spare] = {havok, t};               // an empty slot, or else the one stepped longest ago
}
struct Target { float p[3], dist; uintptr_t chr; int team, hp; bool present; };
// Some characters are in the game's list without being in the world: Undead Burg has two (390 health)
// that cannot be seen, never move and took nothing from an explosion 0.7 m away. In the flag word at
// +0x2A8, bit 0x20 was clear on exactly those two and set on every other character there, alive or dead,
// so it is read as "is in the world". Without this, coins and punches picked the unseen ones as targets.
static const uintptr_t OFF_CHR_FLAGS = 0x2A8;
static const uint32_t CHR_FLAG_PRESENT = 0x20;
// Living hostile characters within `max_dist` of `from`, nearest first. `all` lists every character instead.
static int find_targets(const float *from, Target *out, int max_n, float max_dist, bool all = false) {
    uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr);
    int n = 0;
    for (int i = 0; i < MAX_TRACKED; i++) {
        uintptr_t havok = g_tracked[i].havok, owner = 0, vt = 0, ctrl = 0, back = 0;
        if (!havok || havok == g_player_pos) continue;
        Target c{};
        if (!safe_read(havok + OFF_HAVOK_OWNER, &owner, 8) || owner < 0x10000) { g_tracked[i].havok = 0; continue; }
        c.chr = owner - OWNER_DELTA;
        if (!safe_read(c.chr + 0x68, &ctrl, 8) || ctrl < 0x10000 || !safe_read(ctrl + 0x28, &back, 8) || back != havok) {
            g_tracked[i].havok = 0;              // the character is gone
            continue;
        }
        if (!safe_read(c.chr, &vt, 8) || !safe_read(c.chr + OFF_CHR_TEAM, &c.team, 4) || !safe_read(c.chr + OFF_CHR_HP, &c.hp, 4)) continue;
        if (!safe_read(havok + 0x10, c.p, 12)) continue;
        uint32_t flags = 0;
        c.present = safe_read(c.chr + OFF_CHR_FLAGS, &flags, 4) && (flags & CHR_FLAG_PRESENT);
        if (!all && (vt != exe + RVA_VT_ENEMY || !(c.team == 6 || c.team == 7 || c.team == 9) || c.hp <= 0 || !c.present)) continue;
        c.p[1] += TARGET_HEIGHT;
        float dx = c.p[0] - from[0], dy = c.p[1] - from[1], dz = c.p[2] - from[2];
        c.dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (c.dist > max_dist) continue;
        int k;                                   // keep the nearest max_n, sorted
        if (n < max_n) k = n++;
        else if (c.dist < out[max_n - 1].dist) k = max_n - 1;
        else continue;
        out[k] = c;
        for (; k > 0 && out[k].dist < out[k - 1].dist; k--) { Target tmp = out[k]; out[k] = out[k - 1]; out[k - 1] = tmp; }
    }
    return n;
}

// ULTRAKILL's AudioSources, each of which plays one clip at a time: the player's (jump, dash), the ground
// check's (landing), the revolver's (shots) and the revolver's screen (refill ticking, ready beep).
// v0.42 let every sound overlap, so the two-second bass tails of the shots piled up on each other.
enum { CH_FREE = 0, CH_PLAYER = 1, CH_GROUND = 2, CH_GUN = 3, CH_SCREEN = 4, CH_FEET = 5 };
static bool g_air_jumped = false;                // the current time in the air began with a jump

// Footsteps (PlayerFootsteps): a timer starts at 1 and runs down at 3 per second at 15 u/s or more,
// proportionally slower below that; each time it empties a step plays (one of four clips, never the same
// twice running, volume 0.25, pitch 0.9 to 1.1). The slide and the wall cling each have a looping scrape.
static float frand(float lo, float hi);
static float g_footstep_timer = 1.0f;
static int g_last_footstep = -1;
static void footstep_update(float speed_ms, float dt) {
    float u = speed_ms / UK_UNIT;
    g_footstep_timer -= (u > 15.0f ? 15.0f : u) / 15.0f * dt * 3.0f;
    if (g_footstep_timer > 0) return;
    g_footstep_timer = 1.0f;
    int n = rand() % 4;
    if (n == g_last_footstep) n = (n + 1) % 4;
    g_last_footstep = n;
    static const char *const names[4] = {"footstep_1", "footstep_2", "footstep_3", "footstep_4"};
    sound_play(names[n], 0.25f, frand(0.9f, 1.1f), false, CH_FEET);
}
static void scrape_loops(bool sliding, bool clinging) {
    static int slide_voice = 0, cling_voice = 0;
    if (sliding && !slide_voice) slide_voice = sound_play("slide_loop", 1.0f, 1.0f, true);
    if (!sliding && slide_voice) { sound_stop(slide_voice); slide_voice = 0; }
    if (clinging && !cling_voice) cling_voice = sound_play("wall_scrape", 0.7f, 1.0f, true);
    if (!clinging && cling_voice) { sound_stop(cling_voice); cling_voice = 0; }
}
static float frand(float lo, float hi) { return lo + (hi - lo) * (float)rand() / (float)RAND_MAX; }

static void step_common(void *self, void *step_info, void *gravity, StepFn orig) {
    track_character((uintptr_t)self);
    uintptr_t proxy = 0;
    bool rising = false, ctrl_air = false, have_y0 = false;
    float ctrl_dt = 0, ctrl_vy = 0, y0 = 0;
    if ((uintptr_t)self == g_player_pos && g_player_pos) {
        proxy = *(uintptr_t *)((uintptr_t)self + OFF_POS_PROXY);
        if (proxy) {
            float *vel = (float *)(proxy + OFF_PROXY_VEL);
            g_seen_vel[0] = vel[0];
            g_seen_vel[1] = vel[1];
            g_seen_vel[2] = vel[2];
            InterlockedIncrement(&g_steps);
            if (orig == g_orig_step_alt) InterlockedIncrement(&g_steps_alt);
            if (g_ctrl) {
                // First-person movement: WASD sets the horizontal velocity directly, relative to the view.
                // On the ground it is instant; in the air the current momentum is steered, and kept if no key is held.
                float dt = *(float *)((uintptr_t)step_info + 8);
                // The grounded flag drops out for a frame or two on uneven ground, which in v0.12 left the
                // character sliding on after the keys were released. The game's own air timer smooths that.
                bool flag = *(uint8_t *)((uintptr_t)self + OFF_POS_GROUNDED) != 0;
                float air_time = *(float *)((uintptr_t)self + OFF_POS_AIR_TIME);
                if (g_wall_cd > 0) g_wall_cd -= dt;
                g_wall_age += dt;
                if (g_slam_window > 0 && (g_slam_window -= dt) <= 0) g_slam_force = 0;
                float speed_before = sqrtf(g_vx * g_vx + g_vy * g_vy + g_vz * g_vz);
                if (g_jump_req && !g_air) {
                    // NewMovement.Jump: a slide-jump is lower and keeps the slide speed; a dash-jump is lower
                    // still and keeps the dash speed only if a stamina bar can be paid, else drops to run speed.
                    // Just after a slam lands, the jump is 3x plus what the slam built up, instead of 2.6x.
                    // the jump sound is higher and louder for a slam jump (pitch 2, volume 0.85)
                    g_air_jumped = true;
                    bool slam_jump = !g_sliding && g_dash_left <= 0 && g_slam_window > 0;
                    sound_play("jump", slam_jump ? 0.85f : 0.75f, slam_jump ? 2.0f : 1.0f, false, CH_PLAYER);
                    if (g_sliding) {
                        g_vy = V1_SLIDE_JUMP_SPEED;
                    } else if (g_dash_left > 0) {
                        g_vy = V1_DASH_JUMP_SPEED;
                        if (g_boost >= 100.0f) {
                            g_boost -= 100.0f;
                            sound_play("dash_jump", 1.0f, 1.5f);
                        } else {
                            g_vx = g_dash_x * V1_RUN_SPEED;
                            g_vz = g_dash_z * V1_RUN_SPEED;
                            sound_play("stamina_fail", 0.6f, 0.5f);
                        }
                    } else if (g_slam_window > 0) {
                        g_vy = V1_JUMP_SPEED / 2.6f * (g_slam_force < 5.5f ? 3.0f + (g_slam_force - 1.0f) : 12.5f);
                        InterlockedIncrement(&g_slam_jump_count);
                    } else {
                        g_vy = V1_JUMP_SPEED;
                    }
                    g_slam_window = 0;
                    g_wall_cd = 0.2f;                   // jumpCooldown after a ground jump
                    g_dash_left = 0;
                    g_sliding = false;
                    g_air = true;
                    g_air_steps = 0;
                    g_air_time = 0;
                } else if (g_jump_req && g_air && g_wall_age < WALL_GRACE && g_wall_jumps < V1_WALL_JUMPS && g_wall_cd <= 0) {
                    // NewMovement.WallJump: all speed is dropped, then the same push up and away from the wall.
                    // It also ends a dash, and (unlike V1's 'slam storage') a slam.
                    g_wall_jumps++;
                    g_wall_cd = V1_WALL_JUMP_COOLDOWN;
                    g_vx = g_wall_nx * V1_WALL_JUMP_SPEED;
                    g_vz = g_wall_nz * V1_WALL_JUMP_SPEED;
                    g_vy = V1_WALL_JUMP_SPEED;
                    g_dash_left = 0;
                    g_slamming = false;
                    InterlockedIncrement(&g_wall_jump_count);
                    sound_play("jump", 0.75f, 1.0f + 0.25f * g_wall_jumps, false, CH_PLAYER);       // each wall jump sounds higher
                    if (g_wall_jumps == V1_WALL_JUMPS) sound_play("wall_jump_final", 0.75f, 1.0f, false, CH_GROUND);
                } else if (!g_air && !flag && air_time >= 0.15f) {
                    g_air = true;                       // walked off a ledge
                    g_air_jumped = false;
                    g_air_steps = 0;
                    g_air_time = 0;
                    g_vy = vel[1] < 0 ? vel[1] : 0;
                }
                g_jump_req = false;
                g_air_time = g_air ? g_air_time + dt : 0;

                float f = (float)(key_down('W') - key_down('S')), s = (float)(key_down('D') - key_down('A'));
                float wx = g_view_fwd[0] * f + g_view_right[0] * s, wz = g_view_fwd[1] * f + g_view_right[1] * s;
                float wl = sqrtf(wx * wx + wz * wz);
                if (wl > 1.0f) { wx /= wl; wz /= wl; }
                // direction for a dash or slide: the keys held, or straight ahead if none
                float dir_x = wl > 0.01f ? wx / wl : g_view_fwd[0], dir_z = wl > 0.01f ? wz / wl : g_view_fwd[1];

                bool shift = key_down(VK_SHIFT) != 0, ctrl = key_down(VK_CONTROL) != 0;
                bool shift_edge = shift && !g_prev_shift, ctrl_edge = ctrl && !g_prev_ctrl;
                g_prev_shift = shift;
                g_prev_ctrl = ctrl;

                // stamina: 3 bars of 100, refilling at 70/s except while sliding

                bool was_sliding = g_sliding;
                if (shift_edge && g_boost < 100.0f) sound_play("stamina_fail", 0.6f, 0.5f);
                if (shift_edge && g_boost >= 100.0f) {
                    // TryDash: also ends a slide or a slam
                    sound_play("dash", 1.0f, 1.0f, false, CH_PLAYER);
                    g_boost -= 100.0f;
                    g_dash_left = V1_DASH_TIME;
                    g_dash_x = dir_x;
                    g_dash_z = dir_z;
                    g_sliding = false;
                    g_slamming = false;
                    InterlockedIncrement(&g_dashes);
                }
                if (ctrl_edge && g_air && !g_slamming && g_air_time >= V1_SLAM_MIN_AIR) {
                    g_slamming = true;          // ground slam: straight down, no horizontal speed
                    g_slam_force = 1.0f;        // slamForce: 1, +5 per second of falling
                    g_slam_time = 0;
                    g_dash_left = 0;
                    InterlockedIncrement(&g_slams);
                }
                // A slide starts on the key press (holding the key through a landing does not start one)
                // and may start during a dash, which it replaces.
                if (ctrl_edge && !g_air && !g_sliding) {
                    g_sliding = true;
                    g_slide_mult0 = g_preslide > 3.0f ? 3.0f : g_preslide > 1.0f ? g_preslide : 1.0f;
                    g_dash_left = 0;
                    g_slide_x = dir_x;
                    g_slide_z = dir_z;
                    InterlockedIncrement(&g_slides);
                }
                if (g_sliding && (!ctrl || g_air)) g_sliding = false;
                if (was_sliding && !g_sliding) sound_play("slide_stop", 0.5f, 1.5f);
                if (!g_air) g_slamming = false;

                // FixedUpdate: the multiplier a slide would start with. A slam hands over its force; a fast
                // fall its speed; otherwise the current speed is sampled every 0.2 s (so a slide pressed
                // during or right after a dash is faster).
                if (!g_sliding) {
                    if (g_slamming) {
                        g_preslide_delay = V1_PRESLIDE_DELAY;
                        g_preslide = g_slam_force;
                    } else if (g_dash_left <= 0 && g_air && g_air_time >= 0.2f && speed_before / V1_PRESLIDE_UNIT > g_preslide) {
                        g_preslide = speed_before / V1_PRESLIDE_UNIT;
                        g_preslide_delay = V1_PRESLIDE_DELAY;
                    } else if ((g_preslide_delay -= dt) <= 0) {
                        g_preslide_delay = V1_PRESLIDE_DELAY;
                        g_preslide = speed_before / V1_PRESLIDE_UNIT;
                    }
                }

                bool on_wall = g_air && g_wall_age < 0.05f, clinging = false;
                if (g_slamming) {
                    g_vx = g_vz = 0;
                    g_vy = -V1_SLAM_SPEED;
                    vel[1] = g_vy;
                    g_slam_force += 5.0f * dt;
                    g_slam_time += dt;
                } else if (g_sliding) {
                    // V1 slide: fixed direction at slide speed, A/D nudge it sideways. The multiplier it
                    // started with (up to 3) wears off on the ground.
                    float mult = 1.0f;
                    if (g_preslide > 1.0f) {
                        if (g_preslide > 3.0f) g_preslide = 3.0f;
                        mult = g_preslide;
                        g_preslide -= dt * g_preslide;
                        g_preslide_delay = 0;
                    }
                    g_vx = g_slide_x * V1_SLIDE_SPEED * mult + g_view_right[0] * s * V1_SLIDE_STEER;
                    g_vz = g_slide_z * V1_SLIDE_SPEED * mult + g_view_right[1] * s * V1_SLIDE_STEER;
                } else if (g_dash_left > 0) {
                    // V1 dash: 3x run speed for 0.2 s, flat (gravity off). Afterwards on the ground it drops
                    // back to run speed; in the air the momentum is kept.
                    g_dash_left -= dt;
                    g_vx = g_dash_x * V1_DASH_SPEED;
                    g_vz = g_dash_z * V1_DASH_SPEED;
                    if (g_dash_left <= 0) {
                        g_vx = g_dash_x * V1_RUN_SPEED;
                        g_vz = g_dash_z * V1_RUN_SPEED;
                    }
                    if (g_air) {
                        g_vy = 0;
                        vel[1] = 0;
                    }
                } else if (!g_air) {
                    // V1 on the ground: velocity = Lerp(velocity, input * 16.5, 0.25) every 8 ms physics step
                    float k = 1.0f - powf(0.75f, dt / V1_STEP);
                    g_vx += (wx * V1_RUN_SPEED - g_vx) * k;
                    g_vz += (wz * V1_RUN_SPEED - g_vz) * k;
                    footstep_update(sqrtf(g_vx * g_vx + g_vz * g_vz), dt);
                } else if (on_wall && g_vy < -1.0f * UK_UNIT && wl > 0.01f && -(wx * g_wall_nx + wz * g_wall_nz) > 0.5f * wl) {
                    // Cling: falling against a wall with the keys held towards it. The fall becomes a slow
                    // slide down that speeds up over time (2 u/s per point of clingFade, which grows by 4 per second).
                    g_vx = g_vz = 0;
                    g_vy = -2.0f * g_cling_fade * UK_UNIT;
                    g_cling_fade = g_cling_fade + 4.0f * dt > 50.0f ? 50.0f : g_cling_fade + 4.0f * dt;
                    vel[1] = g_vy;
                    clinging = true;
                    InterlockedIncrement(&g_clings);
                } else {
                    // V1 in the air: each view axis accelerates only until its speed reaches run speed;
                    // momentum above that, and momentum with no key held, is left alone.
                    float axes[2][3] = {{g_view_fwd[0], g_view_fwd[1], f}, {g_view_right[0], g_view_right[1], s}};
                    for (auto &a : axes) {
                        if (a[2] == 0) continue;
                        float dx = a[0] * a[2], dz = a[1] * a[2];
                        float cur = g_vx * dx + g_vz * dz, add = V1_AIR_ACCEL * dt;
                        if (cur + add > V1_RUN_SPEED) add = V1_RUN_SPEED - cur > 0 ? V1_RUN_SPEED - cur : 0;
                        g_vx += dx * add;
                        g_vz += dz * add;
                    }
                    // falling beside a wall is 40% slower to speed up
                    g_vy -= V1_GRAVITY * dt * (on_wall && g_vy < 0 ? 0.6f : 1.0f);
                    if (g_vy < -V1_FALL_SPEED_MAX) g_vy = -V1_FALL_SPEED_MAX;
                    vel[1] = g_vy;
                }
                // Speed into a wall is dropped, as a rigid body's would be. Left in, the character was
                // pressed into walls that lean or have ledges, which then blocked a jump as a ceiling would
                // (v0.35 test: jumping while walking into Firelink's ruin walls went nowhere).
                if (g_wall_age < 0.05f) {
                    for (int i = 0; i < g_wall_count; i++) {
                        float d = g_vx * g_walls[i][0] + g_vz * g_walls[i][1];
                        if (d < 0) {
                            g_vx -= d * g_walls[i][0];
                            g_vz -= d * g_walls[i][1];
                        }
                    }
                }
                vel[0] = g_vx;
                vel[2] = g_vz;
                scrape_loops(g_sliding, clinging);
                rising = g_air && g_vy > 0;
                if (g_air) {
                    ctrl_air = true;
                    ctrl_dt = dt;
                    ctrl_vy = g_vy;
                    have_y0 = proxy_y(proxy, y0);
                }
                // the character faces where the view looks; yaw 0 faces -Z (worked out from the probe log)
                *(float *)((uintptr_t)self + OFF_POS_YAW) = atan2f(-g_view_fwd[0], -g_view_fwd[1]);
            } else {
                g_vx = vel[0];
                g_vz = vel[2];
                g_air = false;
                g_vy = 0;
                scrape_loops(false, false);
            }
            if (!g_ctrl && g_vjump && g_vel_ok) {
                float e = (float)(now_s() - g_vjump_t0);
                rising = e < JUMP_V0 / JUMP_G;
                if (rising) vel[1] = JUMP_V0 - JUMP_G * e;
                float along = vel[0] * g_dir_x + vel[2] * g_dir_z;
                if (!g_ctrl && g_carry > along) {
                    vel[0] += g_dir_x * (g_carry - along);
                    vel[2] += g_dir_z * (g_carry - along);
                }
            }
        }
    }
    g_integrated_seen = false;
    orig(self, step_info, gravity);
    if (!proxy) return;
    // Moves the Havok character the way Step does (lock, set position, unlock).
    auto set_proxy_pos = [&](const float *xyz) {
        uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr);
        uintptr_t phys = *(uintptr_t *)(exe + RVA_PHYS_WORLD);
        void *lock = phys ? *(void **)(phys + 0x28) : nullptr;
        uintptr_t phantom = *(uintptr_t *)(proxy + 0x80);
        uintptr_t body = phantom ? *(uintptr_t *)(phantom + 0x30) : 0;
        if (!lock || !body) return;
        const float *p = (const float *)(body + 0x30);
        alignas(16) float v[4] = {xyz[0], xyz[1], xyz[2], p[3]};
        ((void (*)(void *))(exe + RVA_PHYS_LOCK))(lock);
        ((void (*)(uintptr_t, float *))(exe + RVA_PROXY_SET_POS))(proxy, v);
        ((void (*)(void *))(exe + RVA_PHYS_UNLOCK))(lock);
    };
    auto set_proxy_y = [&](float y) {
        float p[3];
        if (!proxy_pos(proxy, p)) return;
        p[1] = y;
        set_proxy_pos(p);
    };
    // V1's jumps reach places the map was never built for: over Firelink's ruin wall there is no floor at
    // all. Where the map has a kill plane below, the fall still kills (tested: the rescue came too late
    // there); where it has none the fall would never end, so the player is put back where they last
    // stood once they are far below it. Falls onto real ground do no damage (tested up to 11.6 m).
    if (g_ctrl && g_teleport_pending) {
        float p[3];
        g_teleport_pending = false;
        if (proxy_pos(proxy, p)) {
            for (int i = 0; i < 3; i++) p[i] += g_teleport_delta[i];
            set_proxy_pos(p);
            g_vx = g_vy = g_vz = 0;
            g_have_safe = false;
            g_ground_time = 0;
            return;
        }
    }
    if (g_ctrl) {
        float p[3];
        if (proxy_pos(proxy, p)) {
            float dt = *(float *)((uintptr_t)step_info + 8);
            g_ground_time = g_air ? 0 : g_ground_time + dt;
            if (g_ground_time > 0.5f) {
                memcpy(g_safe_pos, p, sizeof(g_safe_pos));
                g_have_safe = true;
            } else if (g_air && g_have_safe && p[1] < g_safe_pos[1] - RESCUE_DROP) {
                set_proxy_pos(g_safe_pos);
                g_vx = g_vy = g_vz = 0;
                g_air = false;
                g_slamming = false;
                g_slam_force = 0;
                g_dash_left = 0;
                g_ground_time = 0;
                InterlockedIncrement(&g_rescues);
                return;
            }
        }
    } else {
        g_have_safe = false;
    }
    float snap = *(float *)((uintptr_t)self + OFF_POS_SNAP_A) + *(float *)((uintptr_t)self + OFF_POS_SNAP_B);
    g_seen_snap = snap;
    g_seen_grounded = *(uint8_t *)((uintptr_t)self + OFF_POS_GROUNDED);
    // After integrating, the game moves the character vertically by the two snap offsets to keep it
    // glued to the ground. On takeoff, put that back so the character leaves the ground.
    // v0.7 showed this offset is applied every frame, airborne or not, so undoing it every frame added
    // 0.31 m per frame. One lift is enough: it breaks ground contact, then the velocity does the rest.
    bool takeoff = rising && snap < 0 && g_seen_grounded && !*(uint8_t *)((uintptr_t)self + OFF_POS_NO_ADJUST);
    // v0.14 waited for the game's grounded flag to land, and it never came back while we drove the
    // fall: the character stayed 'airborne' for 20 s, sliding. Instead, compare how far the physics
    // actually moved us with how far we asked. Blocked going down = landed; going up = ceiling.
    float y1;
    if (ctrl_air && have_y0 && proxy_y(proxy, y1)) {
        float want = ctrl_vy * ctrl_dt, got = y1 - y0;
        g_air_steps++;
        if (ctrl_vy < -0.5f && got > want * 0.3f) {
            // CheckLanding: under 50 u/s the landing is louder the faster the fall; at 50 u/s or more it is the heavy impact
            // (which needs a jump or 0.2 s in the air, so walking down steps makes no sound)
            float fall_u = -ctrl_vy / UK_UNIT;
            if (g_air_jumped || g_air_time >= 0.2f) {
                if (fall_u < 50.0f) sound_play("landing", 0.5f + fall_u * 0.01f, frand(0.9f, 1.1f), false, CH_GROUND);
                else sound_play("landing_heavy", 0.9f, 1.0f);
            }
            g_air = false;
            g_vy = 0;
            g_wall_jumps = 0;
            g_cling_fade = 0;
            if (g_slamming) {
                // GroundCheck: a slam's landing opens the window for the higher jump. A slam that lands
                // on its first step was pressed right above the ground, where V1 starts a slide instead.
                if (g_slam_time <= ctrl_dt * 1.5f && key_down(VK_CONTROL)) {
                    g_sliding = true;
                    g_slide_x = g_view_fwd[0];
                    g_slide_z = g_view_fwd[1];
                    g_preslide = 0;
                    g_slide_mult0 = 1.0f;
                    g_slam_force = 0;
                    InterlockedIncrement(&g_slides);
                } else {
                    g_slam_window = V1_SLAM_JUMP_WINDOW;
                }
                g_slamming = false;
            }
            InterlockedIncrement(&g_landings);
        } else if (ctrl_vy > 0.5f && g_air_steps > 3 && (g_integrated_seen ? g_dbg_integrated : got) < want * 0.3f) {
            g_vy = 0;                           // a ceiling: integrate itself could not rise
            InterlockedIncrement(&g_bonks);
        }
        // In some places the game's ground check then pulls the character down by more than its usual
        // offset (v0.35 test, at the foot of Firelink's ruin wall: integrate rose 0.22 m a step, the
        // character ended 0.09 m lower, and a jump went nowhere). In the air only integrate's own
        // movement counts, so a character left lower than that is put back.
        if (g_air && !takeoff && g_integrated_seen && y1 < y0 + g_dbg_integrated - 0.005f) {
            set_proxy_y(y0 + g_dbg_integrated);
            InterlockedIncrement(&g_pull_fixes);
        }
    }
    if (g_air && g_air_steps > 2000) g_air = false;   // safety net: never stay airborne for ever
    if (g_ctrl) {
        float nx, nz, dist;
        if (wall_contact(proxy, nx, nz, dist)) {
            g_wall_nx = nx;
            g_wall_nz = nz;
            g_wall_dist = dist;
            g_wall_age = 0;
        }
    }
    // V1 takes no fall damage. The game treats a character as falling when this flag is clear
    // (exe+386850 tests it, with the air timer the step wrapper at exe+2BC4D0 only runs while it is
    // clear), and a fall is what leads to landing damage and to dying in mid-air on a long drop. Step
    // has just written the flag from its ground check; while the controller owns the time in the air it
    // is put back to 'on the ground', so the game never sees a fall. The map's kill planes are separate
    // and still kill.
    if (g_ctrl && g_air) *(uint8_t *)((uintptr_t)self + OFF_POS_GROUNDED) = 1;
    if (takeoff) {
        uintptr_t phantom = *(uintptr_t *)(proxy + 0x80);
        uintptr_t body = phantom ? *(uintptr_t *)(phantom + 0x30) : 0;
        if (body) set_proxy_y(*(const float *)(body + 0x34) - snap);
    }
}

// After integrating, Step re-checks the move (exe+2BC870): it sweeps a shape from the old position to the
// new one and, if the sweep reports a hit, cuts the move back to the hit (a guard against passing through
// thin walls). The hook only observes: 'to - from' is exactly how far integrate moved the character,
// which nothing else in Step exposes. (v0.35 briefly skipped the re-check in the air; that was not what
// blocked jumps beside walls, and without it nothing stops a fast character tunnelling.)
typedef void *(*SweepFn)(void *self, float *out, float *from, float *to);
static SweepFn g_orig_sweep = nullptr;
static const uintptr_t RVA_STEP_SWEEP = 0x2BC870;
static const uint8_t SWEEP_PROLOGUE[8] = {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x18, 0x57};
static volatile LONG g_sweep_cuts = 0;
static void *hook_sweep(void *self, float *out, float *from, float *to) {
    bool player = (uintptr_t)self == g_player_pos;
    float to_y = to[1];
    if (player) {
        g_dbg_integrated = to[1] - from[1];
        g_integrated_seen = true;
    }
    void *r = g_orig_sweep(self, out, from, to);
    if (player && g_ctrl && g_air && fabsf(out[1] - to_y) > 0.001f) InterlockedIncrement(&g_sweep_cuts);
    return r;
}

static void hook_step(void *self, void *step_info, void *gravity) { step_common(self, step_info, gravity, g_orig_step); }
static void hook_step_alt(void *self, void *step_info, void *gravity) { step_common(self, step_info, gravity, g_orig_step_alt); }
static const uintptr_t RVA_STEP_ALT = 0x2BC9B0;
static const uint8_t STEP_ALT_PROLOGUE[11] = {0x40, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x04, 0x00, 0x00};

// ---- camera hook. ChrCam::Update(this, float dt, ...) finishes by writing the view matrix at this+0x10:
// three rotation rows, then the position row.
static const uintptr_t RVA_CAM_UPDATE = 0x2348E0;
static const uint8_t CAM_PROLOGUE[7] = {0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x54};
typedef uint64_t (*CamFn)(void *self, float dt, void *a3, void *a4, uint64_t a5, uint64_t a6);
static CamFn g_orig_cam = nullptr;
static volatile bool g_first_person = false;
static volatile float g_eye_height = 1.8f;
static const float EYE_FORWARD = 0.2f;   // keeps the view out of the character's own head
static volatile float g_cam_seen[16];    // the game's own matrix, before we move it
static volatile float g_lens_seen[4];
static volatile float g_fov_scale = 1.8f;         // Davi's setting
static volatile float g_aspect = 16.0f / 9.0f;   // of the picture, kept up to date by the HUD code
static const float BASE_FOV = 0.7505f;           // the follow camera's vertical field of view, radians (read from the log)
static volatile LONG g_cam_calls = 0;
static float g_yaw = 0, g_pitch = 0;            // our view direction, radians; yaw 0 looks along +Z
static bool g_cursor_centred = false;
static float g_eye_drop = 0;                    // how far the view is currently lowered (slide)
static const float SLIDE_EYE_DROP = 0.6f;       // V1's view drops while sliding; 0.6 m is a guess, not from its code
// Mouse sensitivity, matched to ULTRAKILL's at Davi's setting there (mouseSensitivity 10 in its
// LocalPrefs.json). CameraController turns by delta x (mouseSensitivity / 10) degrees, and the look
// binding scales the mouse's delta by 0.05, so that is 0.05 degrees a count. Here each count ends up
// applied twice (measured in v0.56: 300 counts turned the view 0.25 rad at 0.00041, and the game reads
// the mouse twice a frame), so the value is half of 0.05 degrees in radians. Davi's own setting by feel,
// 0.00041, was within 6% of it.
static volatile float g_sens = 0.05f * 3.14159265f / 180.0f / 2.0f;
static volatile float g_volume = 0.2f;           // sound effects volume, 0..1

// ---- camera tilt, as CameraController.Update does it. The view rolls towards the side being strafed
// to: 1 degree, or 5 while dashing or sliding. Every frame the roll is first eased towards level
// (a smooth damp over 0.5 s), then moved towards its target at 25 x (the remaining angle + 0.01)
// degrees per second, or 100 x while dashing or sliding. A dash or slide straight ahead also narrows
// the field of view by a twentieth, one straight back widens it by a tenth, and it returns at
// 300 degrees per second of ULTRAKILL's 105-degree default.
static volatile bool g_camera_tilt = true;       // Page Up
static float g_tilt_deg = 0, g_tilt_vel = 0;     // positive leans the view to the right
static float g_fov_kick = 1.0f;
static float move_towards(float a, float b, float max_step) {
    float d = b - a;
    return fabsf(d) <= max_step ? b : a + (d > 0 ? max_step : -max_step);
}
// the usual critically damped spring (Unity's SmoothDamp) towards `target`
static float smooth_damp(float current, float target, float &velocity, float smooth_time, float dt) {
    float omega = 2.0f / smooth_time, x = omega * dt;
    float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float change = current - target, temp = (velocity + omega * change) * dt;
    velocity = (velocity - omega * temp) * e;
    float out = target + (change + temp) * e;
    if ((target - current > 0.0f) == (out > target)) {
        out = target;
        velocity = 0.0f;
    }
    return out;
}

// ---- the settings the keys change (sensitivity, field of view, eye height, sound volume) are kept in
// ultrasouls.ini next to the exe, so they are still there the next time the game starts.
static void settings_path(wchar_t *path) {
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    wcscpy(slash ? slash + 1 : path, L"ultrasouls.ini");
}
static void save_settings() {
    wchar_t path[MAX_PATH];
    settings_path(path);
    FILE *f = _wfopen(path, L"w");
    if (!f) return;
    fprintf(f, "sensitivity=%.6f\nfov_scale=%.2f\neye_height=%.2f\nvolume=%.2f\ncamera_tilt=%d\nparry_freeze=%d\nhud_motion=%d\n", (float)g_sens,
            (float)g_fov_scale, (float)g_eye_height, (float)g_volume, g_camera_tilt ? 1 : 0, g_parry_freeze ? 1 : 0, g_hud_motion ? 1 : 0);
    fclose(f);
}
static bool load_settings() {
    wchar_t path[MAX_PATH];
    settings_path(path);
    FILE *f = _wfopen(path, L"r");
    if (!f) return false;
    char key[32];
    float v;
    while (fscanf(f, " %31[^=]=%f", key, &v) == 2) {
        if (!strcmp(key, "sensitivity") && v > 0.00002f && v < 0.02f) g_sens = v;
        else if (!strcmp(key, "fov_scale") && v > 0.5f && v < 3.0f) g_fov_scale = v;
        else if (!strcmp(key, "eye_height") && v > 0.5f && v < 3.0f) g_eye_height = v;
        else if (!strcmp(key, "volume") && v >= 0.0f && v <= 1.0f) g_volume = v;
        else if (!strcmp(key, "camera_tilt")) g_camera_tilt = v != 0.0f;
        else if (!strcmp(key, "parry_freeze")) g_parry_freeze = v != 0.0f;
        else if (!strcmp(key, "hud_motion")) g_hud_motion = v != 0.0f;
    }
    fclose(f);
    return true;
}

// Hiding the player's own body in first person. The game already has a way to make a character
// invisible: "camouflage", which the Hidden Body spell uses. Each frame its special-effect code calls
// exe+80C400 on the character's modifier list (chr + 0x388) with a distance range and two opacities
// (the spell passes 1.0 near, 0.0 far). Calling the same function with both opacities 0 hides the body
// at any distance. It has to be called every frame; a frame without the call lets the body return.
// (v0.33 tried the character's "first-person view" flag instead: that only brought up the game's own
// aiming reticle and left the body drawn.)
static const uintptr_t RVA_SET_CAMOUFLAGE = 0x80C400;
static const uint8_t SET_CAMOUFLAGE_BYTES[8] = {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x83, 0xEC, 0x70};
static const uintptr_t OFF_CHR_MODIFIERS = 0x388;
typedef void (*CamouflageFn)(void *modifiers, uint32_t priority, float end_dist, float begin_dist, float far_alpha, float near_alpha, uint8_t flag);
static volatile bool g_hide_body = true;         // PageDown
static volatile LONG g_body_hides = 0;

static uint64_t hook_cam(void *self, float dt, void *a3, void *a4, uint64_t a5, uint64_t a6) {
    if (a3 && (uintptr_t)a3 == g_player_chr && g_ctrl && g_hide_body) {
        uint8_t *fn = (uint8_t *)GetModuleHandleA(nullptr) + RVA_SET_CAMOUFLAGE;
        if (memcmp(fn, SET_CAMOUFLAGE_BYTES, sizeof(SET_CAMOUFLAGE_BYTES)) == 0) {
            // priority 0: a later call in the same frame only replaces these values if its number is lower
            ((CamouflageFn)fn)((uint8_t *)a3 + OFF_CHR_MODIFIERS, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0);
            InterlockedIncrement(&g_body_hides);
        }
    }
    uint64_t r = g_orig_cam(self, dt, a3, a4, a5, a6);
    float *m = (float *)((uintptr_t)self + 0x10);
    for (int i = 0; i < 16; i++) g_cam_seen[i] = m[i];
    float *lens = (float *)((uintptr_t)self + 0x50);   // four floats copied from the active camera; [0] looks like the field of view
    for (int i = 0; i < 4; i++) g_lens_seen[i] = lens[i];
    InterlockedIncrement(&g_cam_calls);
    uintptr_t pos = g_player_pos;
    // mouse movement since the last frame, from whichever source is live
    LONG dix = InterlockedExchange(&g_di_dx, 0), diy = InterlockedExchange(&g_di_dy, 0);
    LONG rawx = InterlockedExchange(&g_raw_dx, 0), rawy = InterlockedExchange(&g_raw_dy, 0);
    // Windows cursor: measure how far it moved from the window centre, then put it back there
    LONG curx = 0, cury = 0;
    HWND wnd = GetForegroundWindow();
    DWORD wpid = 0;
    GetWindowThreadProcessId(wnd, &wpid);
    if (g_first_person && wpid == GetCurrentProcessId()) {
        RECT rc;
        POINT c, now;
        if (GetClientRect(wnd, &rc) && GetCursorPos(&now)) {
            c.x = (rc.left + rc.right) / 2;
            c.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(wnd, &c);
            if (g_cursor_centred) {
                curx = now.x - c.x;
                cury = now.y - c.y;
                if (curx || cury) InterlockedIncrement(&g_cur_events);
            }
            if (curx || cury || !g_cursor_centred) SetCursorPos(c.x, c.y);
            g_cursor_centred = true;
        }
    } else {
        g_cursor_centred = false;
    }
    g_cur_dx = curx;
    g_cur_dy = cury;
    LONG mx = g_di_events ? dix : g_raw_events ? rawx : curx;
    LONG my = g_di_events ? diy : g_raw_events ? rawy : cury;

    bool on = g_first_person && pos;
    if (on && !g_ctrl) {
        // entering first person: start from where the game's camera is looking
        float fl = sqrtf(m[8] * m[8] + m[10] * m[10]);
        g_yaw = atan2f(m[8], m[10]);
        g_pitch = atan2f(m[9], fl);
    }
    g_ctrl = on;
    g_block_keys = on;
    {
        // per-frame work: stamina refill (3 bars of 100, 70/s, not while sliding) and instant fire.
        // The physics step is not called every frame, so in v0.21 both stalled while standing still.
        static double last = 0;
        double now = now_s();
        float frame = last > 0 && now - last < 0.1 ? (float)(now - last) : 0.0f;
        last = now;
        if (on && !g_sliding) g_boost = g_boost + V1_BOOST_REGEN * frame > 300.0f ? 300.0f : g_boost + V1_BOOST_REGEN * frame;
        if (on) try_instant_fire();
        // The game's character update (exe+2BB150) skips the physics step when the character is at
        // rest and the game asked for no movement, except on every 10th frame. Since the game never
        // sees our keys, a jump, dash or slide from standstill waited for that 10th frame, or never
        // started. The same update has a 'step this frame' byte that it clears after stepping.
        if (on) *(uint8_t *)(pos + OFF_POS_FORCE_STEP) = 1;
        static bool prev_space = false;
        bool space = on && key_down(VK_SPACE);
        if (space && !prev_space) g_jump_req = true;
        prev_space = space;
    }
    if (!on) return r;

    // The view direction is ours: the follow camera's own direction drifts when the character moves,
    // because it trails behind and looks at them.
    g_yaw += (float)mx * g_sens;
    g_pitch -= (float)my * g_sens;
    if (g_pitch > 1.5f) g_pitch = 1.5f;
    if (g_pitch < -1.5f) g_pitch = -1.5f;
    float sy = sinf(g_yaw), cy = cosf(g_yaw), sp = sinf(g_pitch), cp = cosf(g_pitch);
    g_view_fwd[0] = sy;
    g_view_fwd[1] = cy;
    g_view_right[0] = cy;
    g_view_right[1] = -sy;
    // rows: right, up, forward (left-handed, Y up), matching what the game writes
    m[0] = cy;       m[1] = 0;   m[2] = -sy;
    m[4] = -sp * sy; m[5] = cp;  m[6] = -sp * cy;
    m[8] = sy * cp;  m[9] = sp;  m[10] = cy * cp;
    {
        float fdt = dt > 0 && dt < 0.1f ? dt : 0.0f;
        float strafe = (float)(key_down('D') - key_down('A'));
        bool boost = g_dash_left > 0 || g_sliding;
        if (fdt > 0) {
            g_tilt_deg = smooth_damp(g_tilt_deg, 0.0f, g_tilt_vel, 0.5f, fdt);
            float target = g_camera_tilt ? strafe * (boost ? 5.0f : 1.0f) : 0.0f;
            float rate = (g_camera_tilt && boost ? 100.0f : 25.0f) * (fabsf(g_tilt_deg - target) + 0.01f);
            g_tilt_deg = move_towards(g_tilt_deg, target, fdt * rate);
        }
        // roll about the view direction: the up row leans towards the right row
        float roll = g_tilt_deg * 3.14159265f / 180.0f, cr = cosf(roll), sr = sinf(roll);
        for (int i = 0; i < 3; i++) {
            float right = m[i], up = m[4 + i];
            m[i] = right * cr - up * sr;
            m[4 + i] = up * cr + right * sr;
        }
        if (boost) {
            float dx = g_sliding ? g_slide_x : g_dash_x, dz = g_sliding ? g_slide_z : g_dash_z;
            float along = dx * sy + dz * cy;             // 1 straight ahead, -1 straight back
            if (along > 0.999f) g_fov_kick = 0.95f;
            else if (along < -0.999f) g_fov_kick = 1.10f;
        } else {
            g_fov_kick = move_towards(g_fov_kick, 1.0f, fdt * 300.0f / 105.0f);
        }
    }
    lens[0] = BASE_FOV * g_fov_scale * g_fov_kick;
    const float *p = (const float *)(pos + OFF_POS_X);
    m[12] = p[0] + sy * EYE_FORWARD;
    float drop_target = g_sliding ? SLIDE_EYE_DROP : 0.0f, k = dt > 0 && dt < 0.1f ? 1.0f - expf(-dt / 0.06f) : 1.0f;
    g_eye_drop += (drop_target - g_eye_drop) * k;
    m[13] = p[1] + g_eye_height - g_eye_drop;
    m[14] = p[2] + cy * EYE_FORWARD;
    return r;
}

// ---- projectile hook. BulletIns::Init(bullet, params, aim, ...) sets up every projectile the game fires.
// Layout worked out from the shots recorded in the v0.19 log:
//   params+0x04  bullet id
//   params+0xB0  point the shot is aimed at    params+0xD4  range (100)
//   params+0xE0  start transform, 3 rows of 4: columns are -right, up, -forward, then position
//   params+0x0C  shooter handle (0x10044000 is the player)
//   aim+0x20     the same aim point
//   a4           launch transform, 3 rows of 4: columns right, up, forward, position
// In first person the player's shots are re-aimed from the eye along the view.
static const uintptr_t RVA_BULLET_INIT = 0x4241D0;
static const uint8_t BULLET_PROLOGUE[8] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55};
typedef uint64_t (*BulletInitFn)(void *bullet, uint8_t *params, float *aim, void *a4, uint64_t a5, uint64_t a6);
static BulletInitFn g_orig_bullet_init = nullptr;
enum { BULLET_LOG = 6, BULLET_PARAM_BYTES = 0x160 };
static volatile LONG g_bullets = 0, g_bullets_aimed = 0, g_player_shots = 0;
static uint8_t g_bullet_params[BULLET_LOG][BULLET_PARAM_BYTES];
static uint8_t g_bullet_a4[BULLET_LOG][0x40];
static uintptr_t g_bullet_emitter[BULLET_LOG];
// the object that fired the player's last shot (params live at +0x1A0 inside it), for the instant-fire test
static volatile uintptr_t g_last_emitter = 0;
static volatile bool g_in_instant_fire = false;
static volatile LONG g_real_shots = 0;
static volatile int g_ammo_id = 0;

// A shot that does not leave from the eye along the view (a coin's ricochet, a shotgun pellet, an
// explosion): where it starts and where it goes. BulletMan::Shoot only queues the projectile; the game
// sets it up (this hook) later in the frame, so v0.36 found its override already cleared and the ricochet
// left from the eye like a plain shot (the log showed every ricochet accepted with no projectile set up
// during the call). Each projectile takes the oldest waiting override made for its bullet row.
struct ShotOverride { int bullet; float start[3], dir[3]; double at; };
enum { MAX_OVERRIDES = 64 };
static ShotOverride g_overrides[MAX_OVERRIDES];
static int g_override_count = 0;
static volatile LONG g_override_inits = 0;       // projectiles set up with their own start and direction
static uint64_t hook_bullet_init(void *bullet, uint8_t *params, float *aim, void *a4, uint64_t a5, uint64_t a6) {
    InterlockedIncrement(&g_bullets);
    uintptr_t pos = g_player_pos;
    bool mine = params && *(int *)(params + 0x0C) == 0x10044000;   // params+0x0C is the shooter's handle
    if (mine) {
        LONG n = InterlockedIncrement(&g_player_shots) - 1;
        if (n < BULLET_LOG) {
            memcpy(g_bullet_params[n], params, BULLET_PARAM_BYTES);
            if (a4) safe_read((uintptr_t)a4, g_bullet_a4[n], sizeof(g_bullet_a4[n]));
            g_bullet_emitter[n] = (uintptr_t)params - 0x1A0;
        }
        g_last_emitter = (uintptr_t)params - 0x1A0;
        if (*(int *)(params + 0x04) < 9000000) {   // not one of the revolver's own rows
            g_ammo_id = *(int *)(params + 0x70);   // item id of the arrow or bolt, as seen in the v0.19 log
            InterlockedIncrement(&g_real_shots);
        }
    }
    if (mine && g_ctrl && pos) {
        const float *p = (const float *)(pos + OFF_POS_X);
        float yaw = g_yaw, pitch = g_pitch;
        int bullet_id = *(int *)(params + 0x04);
        ShotOverride taken;
        const ShotOverride *ov = nullptr;
        for (int i = 0; i < g_override_count; i++) {
            if (g_overrides[i].bullet != bullet_id) continue;
            taken = g_overrides[i];
            memmove(&g_overrides[i], &g_overrides[i + 1], sizeof(ShotOverride) * (g_override_count - i - 1));
            g_override_count--;
            ov = &taken;
            yaw = atan2f(ov->dir[0], ov->dir[2]);
            pitch = asinf(ov->dir[1] > 1 ? 1 : ov->dir[1] < -1 ? -1 : ov->dir[1]);
            break;
        }
        float sy = sinf(yaw), cy = cosf(yaw), sp = sinf(pitch), cp = cosf(pitch);
        float right[3] = {cy, 0, -sy}, up[3] = {-sp * sy, cp, -sp * cy}, fwd[3] = {sy * cp, sp, cy * cp};
        float start[3] = {p[0] + fwd[0] * 0.6f, p[1] + g_eye_height - g_eye_drop + fwd[1] * 0.6f, p[2] + fwd[2] * 0.6f};
        if (ov) {
            memcpy(start, ov->start, sizeof(start));
            InterlockedIncrement(&g_override_inits);
        }
        float range = *(float *)(params + 0xD4);
        if (!(range > 1.0f && range < 1000.0f)) range = 100.0f;
        float *m = (float *)(params + 0xE0), *target = (float *)(params + 0xB0);
        for (int r = 0; r < 3; r++) {
            m[r * 4 + 0] = -right[r];
            m[r * 4 + 1] = up[r];
            m[r * 4 + 2] = -fwd[r];
            m[r * 4 + 3] = start[r];
            target[r] = start[r] + fwd[r] * range;
            if (aim) aim[8 + r] = target[r];
        }
        // The fourth argument is the projectile's actual starting transform: 3 rows of 4 whose columns
        // are right, up, forward, position (decoded from the a4 dumps in the v0.21 log, where the
        // forward column matched the view pitch). The game computes it from `params` before calling
        // Init, so editing only `params` re-aimed each shot with the previous shot's direction.
        float *launch = (float *)a4;
        if (launch) {
            for (int r = 0; r < 3; r++) {
                launch[r * 4 + 0] = right[r];
                launch[r * 4 + 1] = up[r];
                launch[r * 4 + 2] = fwd[r];
                launch[r * 4 + 3] = start[r];
            }
            // Exactly 12 floats and no more: on the instant-fire path this buffer is 0x30 bytes on the
            // game's stack, and v0.23 wrote a 14th value over a saved register, which crashed the game.
        }
        InterlockedIncrement(&g_bullets_aimed);
    }
    return g_orig_bullet_init(bullet, params, aim, a4, a5, a6);
}

// ---- V1's Piercer revolver. Shots go through the game's own entry point for spawning a projectile,
// BulletMan::Shoot(request) at exe+429BA0, so no bow, ammo or attack animation is involved. The request
// layout was read from the function that unpacks it (exe+42A1C0):
//   +00 shooter handle     +04 BehaviorParam id      +08 magic id (-1)     +0C bullet id (-1: from behaviour)
//   +10 goods id (-1)      +14 dummy poly (-1)       +1C target handle (-1)
//   +20 ammo slot code     +24 weapon slot code      +28, +2C multipliers (1.0)
//   +40 start transform, +70 shooter transform: 3 rows of 4 each
// The behaviour rows 9000100 and 9000110 are added to the game's params by tools/patch_v1.py.
struct alignas(16) ShootRequest {
    int shooter, behavior, magic, bullet, goods, dummy_poly;
    uint8_t flag18, pad19[3];
    int target;
    int ammo_slot, weapon_slot;
    float mul_a, mul_b;
    uint8_t flag30, flag31, flag32, pad33[13];
    float start[12];
    float root[12];
};
static_assert(sizeof(ShootRequest) == 0xA0, "request layout");

static const uintptr_t RVA_BULLET_MAN = 0x1C7A488;
static const uintptr_t RVA_BULLET_SHOOT = 0x429BA0;
static const uint8_t BULLET_SHOOT_BYTES[8] = {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0xA0};
static const int PLAYER_HANDLE = 0x10044000;      // seen as the shooter in every recorded player shot
static const int BEHAVIOR_REVOLVER = 9000100, BEHAVIOR_PIERCER = 9000110;
// Revolver.Update: shootCharge refills at 200/s (0.5 s between shots); the alt fire charges at 175/s
// while held and fires on release at full charge, then pierceCharge refills at 40/s (2.5 s).
static const double FIRE_INTERVAL = 0.5;
static const float PIERCE_CHARGE_RATE = 175.0f, PIERCE_RECHARGE_RATE = 40.0f;

static volatile bool g_instant_fire = true;      // F11: off hands both mouse buttons back to the game
static volatile bool g_item_style = true;        // item-style requests are the ones that deal damage; weapon-style is the fallback
static const int GOODS_THROWING_KNIFE = 290;
static volatile LONG g_instant_shots = 0, g_pierce_shots = 0, g_shoot_fails = 0;
static double g_next_shot = 0;
static volatile float g_pierce_charge = 0;        // 0..100 while the alt fire is held
static volatile float g_pierce_ready = 100.0f;    // 0..100, recharging after a charged shot
static bool g_prev_rmb = false;
static volatile double g_last_shot_time = -100.0, g_last_pierce_time = -100.0;
// Which clip each animated model is playing and when it started (the HUD code falls back to the
// revolver's idle loop, and puts the arm away, once a clip has run out). The names are the clips' own:
// the revolver's Animator plays Shoot, Shoot2 or Shoot3 at random on a shot and PickUp when the weapon
// is drawn; the arm's plays Jab or Jab2 on a punch, Hook from 6.5% in when the punch lands, and CoinFlip
// when a coin is tossed.
static const char *volatile g_rev_clip = nullptr;
static volatile double g_rev_clip_start = -100.0;
static const char *volatile g_arm_clip = nullptr;
static volatile double g_arm_clip_start = -100.0;
static void play_revolver(const char *clip) {
    g_rev_clip_start = now_s();
    g_rev_clip = clip;
}
static void play_arm(const char *clip, double already_in = 0.0) {
    g_arm_clip_start = now_s() - already_in;
    g_arm_clip = clip;
}
static const char *volatile g_arm2_clip = nullptr;          // the Knuckleblaster: Punch, Hook, PunchBlast
static volatile double g_arm2_clip_start = -100.0;
static void play_arm2(const char *clip, double already_in = 0.0) {
    g_arm2_clip_start = now_s() - already_in;
    g_arm2_clip = clip;
}
static void play_revolver_shot() {
    static const char *const shots[3] = {"Shoot", "Shoot2", "Shoot3"};
    play_revolver(shots[rand() % 3]);
}

// `from` and `dir` (unit length) send the shot from somewhere other than the eye, e.g. a coin.
static bool send_shot(int behavior, bool item_style, const float *from = nullptr, const float *dir = nullptr) {
    uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr), pos = g_player_pos;
    uintptr_t man = *(uintptr_t *)(exe + RVA_BULLET_MAN);
    if (!man || !pos || memcmp((void *)(exe + RVA_BULLET_SHOOT), BULLET_SHOOT_BYTES, sizeof(BULLET_SHOOT_BYTES)) != 0) return false;
    const float *p = (const float *)(pos + OFF_POS_X);
    float yaw = g_yaw, pitch = g_pitch;
    if (from && dir) {
        yaw = atan2f(dir[0], dir[2]);
        pitch = asinf(dir[1] > 1 ? 1 : dir[1] < -1 ? -1 : dir[1]);
    }
    float sy = sinf(yaw), cy = cosf(yaw), sp = sinf(pitch), cp = cosf(pitch);
    float right[3] = {cy, 0, -sy}, up[3] = {-sp * sy, cp, -sp * cy}, fwd[3] = {sy * cp, sp, cy * cp};
    float eye[3] = {p[0], p[1] + g_eye_height - g_eye_drop, p[2]};
    if (from && dir)
        for (int i = 0; i < 3; i++) eye[i] = from[i] - fwd[i] * 0.6f;
    float flat_fwd[3] = {sy, 0, cy}, flat_up[3] = {0, 1, 0};
    ShootRequest r{};
    r.shooter = PLAYER_HANDLE;
    r.behavior = behavior;
    r.magic = r.bullet = r.goods = r.dummy_poly = r.target = -1;
    r.ammo_slot = r.weapon_slot = -1;
    if (item_style) {
        // Fired the way a thrown item is: the bullet row is named directly and the request carries an
        // item id, which makes the game skip looking up the equipped weapon and ammo. Damage then comes
        // only from the bullet's own attack row, like a throwing knife. (In v0.28 the shot was set up
        // like a crossbow's and did no damage.)
        r.goods = GOODS_THROWING_KNIFE;
        r.bullet = behavior;   // patch_v1.py gives the bullet row the same id as the behaviour row
    } else {
        r.ammo_slot = -3;      // the slot codes a crossbow shot uses
    }
    r.mul_a = r.mul_b = 1.0f;
    for (int i = 0; i < 3; i++) {
        // the game's own transforms here have columns -right, up, -forward, then position
        r.start[i * 4 + 0] = -right[i];
        r.start[i * 4 + 1] = up[i];
        r.start[i * 4 + 2] = -fwd[i];
        r.start[i * 4 + 3] = eye[i] + fwd[i] * 0.6f;
        r.root[i * 4 + 0] = -right[i];
        r.root[i * 4 + 1] = flat_up[i];
        r.root[i * 4 + 2] = -flat_fwd[i];
        r.root[i * 4 + 3] = p[i];
    }
    int id = ((int (*)(uintptr_t, ShootRequest *))(exe + RVA_BULLET_SHOOT))(man, &r);
    if (id != -1 && from && dir) {
        // an override whose projectile never got set up must not be left for a later shot of the same kind
        double now = now_s();
        int keep = 0;
        for (int i = 0; i < g_override_count; i++)
            if (now - g_overrides[i].at < 1.0) g_overrides[keep++] = g_overrides[i];
        g_override_count = keep;
        if (g_override_count < MAX_OVERRIDES) {
            ShotOverride &ov = g_overrides[g_override_count++];
            ov.bullet = behavior;                // patch_v1.py gives each bullet row its behaviour row's id
            ov.at = now;
            memcpy(ov.start, from, sizeof(ov.start));
            memcpy(ov.dir, dir, sizeof(ov.dir));
        }
    }
    return id != -1;
}

static bool shoot(int behavior, const float *from = nullptr, const float *dir = nullptr) {
    bool ok = send_shot(behavior, g_item_style, from, dir);
    if (!ok && g_item_style) ok = send_shot(behavior, false, from, dir);   // refused: fall back to the weapon-style request
    if (!ok) InterlockedIncrement(&g_shoot_fails);
    return ok;
}

// ---- tracers. The shots themselves are the game's projectiles, which are hard to see; each shot also
// gets a line the mod draws itself, from the muzzle (or the coin) to where the game's own ray cast says the
// shot's line ends. Lengths are real; the widths, colours and lifetimes are ours.
struct Tracer { float a[3], b[3]; double born; float life, width, r, g, bl; };
enum { MAX_TRACERS = 24 };
static Tracer g_tracers[MAX_TRACERS];
static int g_tracer_next = 0;
static const float TRACER_WHITE[3] = {1.0f, 0.95f, 0.7f}, TRACER_BLUE[3] = {0.2f, 0.8f, 1.0f}, TRACER_GOLD[3] = {1.0f, 0.8f, 0.2f};
static void add_tracer(const float *a, const float *b, float life, float width, const float *rgb) {
    Tracer &t = g_tracers[g_tracer_next++ % MAX_TRACERS];
    memcpy(t.a, a, sizeof(t.a));
    memcpy(t.b, b, sizeof(t.b));
    t.born = now_s();
    t.life = life;
    t.width = width;
    t.r = rgb[0]; t.g = rgb[1]; t.bl = rgb[2];
}

// ---- the weapon's walking bob (WalkingBob, which sits on ULTRAKILL's "Guns" object): while walking the
// weapon drifts from its rest position to a point 0.08 to the right and 0.025 down, back, then to the same
// point on the left, and so on. Its speed is (2 - 3 x its distance from rest) x min(speed, 15 u/s) / 15,
// a quarter of that per second; when not walking it returns to rest at 1 unit per second.
static volatile float g_bob_x = 0, g_bob_y = 0;
static void update_bob(float dt, bool walking, float speed_u) {
    static int side = 1;
    static bool back = false;
    float x = g_bob_x, y = g_bob_y, tx = 0, ty = 0, step = dt;
    if (walking) {
        float dist = sqrtf(x * x + y * y);
        step = dt * (2.0f - dist * 3.0f) * ((speed_u > 15.0f ? 15.0f : speed_u) / 15.0f) * 0.25f;
        if (!back) { tx = 0.08f * side; ty = -0.025f; }
    }
    float dx = tx - x, dy = ty - y, d = sqrtf(dx * dx + dy * dy);
    if (step < 0) step = 0;
    if (d <= step || d < 1e-6f) {
        x = tx;
        y = ty;
        if (walking) {
            if (back) back = false;                           // reached rest: head out again
            else { back = true; side = -side; }               // reached a side: come back, next time the other side
        }
    } else {
        x += dx / d * step;
        y += dy / d * step;
    }
    g_bob_x = x;
    g_bob_y = y;
}

// ---- the HUD's and the weapons' lean against movement (NewMovement.Update, unless "reduce HUD motion"
// is set). With v the player's velocity in the camera's own axes, in u/s:
//   the HUD object (both panels) eases towards -v / 1000, covering 15 times the remaining distance a second;
//   the HUD camera eases towards +v / 350, no further than 0.2, at 25 times the distance a second. The
//   weapons hang off the main camera, not the HUD's, so on screen they move by the opposite of that.
static float g_hud_sway[3] = {0, 0, 0}, g_hud_cam[3] = {0, 0, 0};
static void view_axes(float *right, float *up, float *fwd);
static void update_sway(float dt) {
    float right[3], up[3], fwd[3], v[3] = {0, 0, 0};
    if (g_ctrl && g_hud_motion) {
        view_axes(right, up, fwd);
        float w[3] = {g_vx / UK_UNIT, (g_air ? g_vy : 0.0f) / UK_UNIT, g_vz / UK_UNIT};
        v[0] = w[0] * right[0] + w[1] * right[1] + w[2] * right[2];
        v[1] = w[0] * up[0] + w[1] * up[1] + w[2] * up[2];
        v[2] = w[0] * fwd[0] + w[1] * fwd[1] + w[2] * fwd[2];
    }
    float cam[3] = {v[0] / 350.0f, v[1] / 350.0f, v[2] / 350.0f}, len = sqrtf(cam[0] * cam[0] + cam[1] * cam[1] + cam[2] * cam[2]);
    if (len > 0.2f)
        for (float &c : cam) c *= 0.2f / len;
    float hud[3] = {-v[0] / 1000.0f, -v[1] / 1000.0f, -v[2] / 1000.0f};
    struct { float *pos; const float *target; float rate; } both[2] = {{g_hud_sway, hud, 15.0f}, {g_hud_cam, cam, 25.0f}};
    for (auto &e : both) {
        float d[3] = {e.target[0] - e.pos[0], e.target[1] - e.pos[1], e.target[2] - e.pos[2]}, dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float step = dt * e.rate * dist;             // Vector3.MoveTowards by this much
        if (dist <= step || dist < 1e-7f) memcpy(e.pos, e.target, 12);
        else
            for (int i = 0; i < 3; i++) e.pos[i] += d[i] / dist * step;
    }
}

// ---- style meter (StyleHUD, StyleCalculator). Each rank has a meter size and a drain speed, read from
// the game's StyleHUD object: D 200/1, C 300/1.25, B 400/1.5, A 500/2, S 700/3, SS 850/4, SSS 1000/6,
// ULTRAKILL 1500/8. Points fill the meter; a full meter moves up a rank and keeps the overflow (never less
// than a quarter); the meter drains at 15 x the rank's speed per second; an empty meter drops a rank, to
// three quarters full, and at D ends the combo. The freshness multiplier is not implemented (1.0).
struct StyleRank { float max_meter, drain; };
static const StyleRank STYLE_RANKS[8] = {{200, 1.0f}, {300, 1.25f}, {400, 1.5f}, {500, 2.0f}, {700, 3.0f}, {850, 4.0f}, {1000, 6.0f}, {1500, 8.0f}};
enum { STYLE_LOG = 6 };
struct StyleEntry { char text[40]; float r, g, b; double at; };
static StyleEntry g_style_log[STYLE_LOG];
static int g_style_log_n = 0;
static volatile int g_style_rank = 0;
static volatile float g_style_meter = 0;
static volatile bool g_style_combo = false;
static volatile LONG g_style_points = 0;
static const float STYLE_WHITE[3] = {1, 1, 1}, STYLE_ORANGE[3] = {1, 0.65f, 0}, STYLE_CYAN[3] = {0, 1, 1}, STYLE_GREEN[3] = {0, 1, 0};
static const double STYLE_LINE_SECONDS = 4.0;    // ours: how long a bonus stays listed

// `name` empty adds points without a line, as ULTRAKILL's unnamed hits do. `count` >= 0 appends " xN".
static void style_add(int points, const char *name, const float *color = STYLE_WHITE, int count = -1) {
    if (points > 0) {
        g_style_meter = g_style_meter + points;
        InterlockedExchangeAdd(&g_style_points, points);
    }
    if (name && name[0]) {
        if (g_style_log_n == STYLE_LOG) {
            memmove(&g_style_log[0], &g_style_log[1], sizeof(StyleEntry) * (STYLE_LOG - 1));
            g_style_log_n--;
        }
        StyleEntry &e = g_style_log[g_style_log_n++];
        if (count >= 0) snprintf(e.text, sizeof(e.text), "+ %s x%d", name, count);
        else snprintf(e.text, sizeof(e.text), "+ %s", name);
        e.r = color[0]; e.g = color[1]; e.b = color[2];
        e.at = now_s();
        logf("style: %s (+%d), rank %d meter %.0f\n", e.text, points, (int)g_style_rank, (float)g_style_meter);
    }
    int rank = g_style_rank;
    float meter = g_style_meter;
    if (meter >= STYLE_RANKS[rank].max_meter && rank < 7) {
        while (meter >= STYLE_RANKS[rank].max_meter && rank < 7) {
            meter -= STYLE_RANKS[rank].max_meter;
            rank++;
        }
        if (meter < STYLE_RANKS[rank].max_meter / 4) meter = STYLE_RANKS[rank].max_meter / 4;
    } else if (meter > STYLE_RANKS[rank].max_meter) {
        meter = STYLE_RANKS[rank].max_meter;
    }
    g_style_rank = rank;
    g_style_meter = meter;
}
static void style_update(float dt) {
    int rank = g_style_rank;
    float meter = g_style_meter;
    if (meter > 0 && !g_style_combo) {
        if (meter < STYLE_RANKS[rank].max_meter / 4) meter = STYLE_RANKS[rank].max_meter / 4;
        g_style_combo = true;
    }
    if (g_style_combo) {
        if (meter < 0) {
            if (rank > 0) {
                rank--;
                meter = STYLE_RANKS[rank].max_meter - STYLE_RANKS[rank].max_meter / 4;
            } else {
                meter = 0;
                g_style_combo = false;
            }
        } else {
            meter -= dt * STYLE_RANKS[rank].drain * 15.0f;
        }
    }
    g_style_rank = rank;
    g_style_meter = meter;
    double now = now_s();
    while (g_style_log_n > 0 && now - g_style_log[0].at > STYLE_LINE_SECONDS) {
        memmove(&g_style_log[0], &g_style_log[1], sizeof(StyleEntry) * (STYLE_LOG - 1));
        g_style_log_n--;
    }
}

// ---- what the player's attacks did, found by watching the health of the hostile characters around.
// Twenty times a second each one's health is compared with the last value seen; a drop within 0.6 s of
// one of the player's attacks is credited to that attack (StyleCalculator.HitCalculator's table):
//   revolver  hit 10, kill 30 KILL (boss 100 BIG KILL)        punch  hit 20 (boss 60 DISRESPECT), kill 30 KILL
// and kills in quick succession add 25 DOUBLE KILL, 50 TRIPLE KILL, then 100 MULTIKILL xN (the window is
// ULTRAKILL's: a timer of 5 that runs down at 10 per second).
static bool eye_pos(float *eye);
enum Hitter { HIT_NONE = 0, HIT_REVOLVER, HIT_PUNCH, HIT_COIN, HIT_SHOTGUN, HIT_SHOTGUN_ZONE, HIT_EXPLOSION };
static volatile int g_last_hitter = HIT_NONE;
static volatile double g_last_attack_time = -100.0;
static void note_attack(int hitter) {
    g_last_hitter = hitter;
    g_last_attack_time = now_s();
}
// ---- blood (Bloodsplatter). In ULTRAKILL a hit leaves a ball of blood at the wound that heals the player
// if they are inside it: a body hit 10 health within 5 u, a kill 30 within 7 u (the "head" blood, which
// deaths use), a shotgun pellet's or an explosion's hit 3 within 5 u. Health is on V1's scale of 100, so
// here it is that many hundredths of the character's full health. The distance is from the eye to the
// enemy's aim point, with 0.75 m allowed for the two bodies.
static volatile LONG g_heals = 0;
static void heal_player(float amount) {
    uintptr_t chr = g_player_chr;
    int hp = 0, max_hp = 0;
    if (!chr || !safe_read(chr + 0x3E8, &hp, 4) || !safe_read(chr + 0x3EC, &max_hp, 4) || max_hp <= 0 || hp <= 0 || hp >= max_hp) return;
    int add = (int)(amount / 100.0f * (float)max_hp + 0.5f);
    hp = hp + add > max_hp ? max_hp : hp + add;
    safe_write(chr + 0x3E8, &hp, 4);
    InterlockedIncrement(&g_heals);
}

// ---- bosses, for the bar across the top. tools/ds_names.py writes the bosses' NpcParam rows and names
// from the game's own text; a character whose NpcParam id (ChrIns + 0xC8) is in that list, alive and in
// the world within 60 m, gets a bar once it has turned on the player, and keeps it until it dies or is
// left behind. The game's own "the fight has begun" is not read; it is inferred from what can be seen of
// the boss: it has lost health, or is in an attack animation, or is doing anything but standing idle
// (an animation other than number 0) with the player in plain sight.
static const uintptr_t OFF_CHR_NPC_PARAM = 0xC8;
struct BossName { int id; char name[HudState::BOSS_NAME_CHARS]; };
enum { MAX_BOSS_NAMES = 96 };
static BossName g_boss_names[MAX_BOSS_NAMES];
static int g_boss_name_count = 0;
static HudState::Boss g_bosses[HudState::MAX_BOSSES];
static volatile int g_boss_count = 0;
static const float BOSS_BAR_RANGE = 60.0f;       // ours
static int load_boss_names(const wchar_t *path) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return -1;
    char line[160];
    while (g_boss_name_count < MAX_BOSS_NAMES && fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        BossName &b = g_boss_names[g_boss_name_count];
        b.id = atoi(line);
        size_t n = 0;
        for (const char *p = tab + 1; *p && *p != '\r' && *p != '\n' && n + 1 < sizeof(b.name); p++)
            b.name[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : ((unsigned char)*p < 128 ? *p : '?');   // the bar's text is in capitals
        b.name[n] = 0;
        if (b.id > 0 && n) g_boss_name_count++;
    }
    fclose(f);
    return g_boss_name_count;
}
static bool enemy_attacking(uintptr_t chr, int &seen);
static int ray_blocked(const float *a, const float *b);
enum { MAX_ENGAGED = 8 };
static uintptr_t g_engaged[MAX_ENGAGED];
static bool boss_engaged(const Target &c, const float *eye, int max_hp) {
    int free_slot = -1;
    for (int i = 0; i < MAX_ENGAGED; i++) {
        if (g_engaged[i] == c.chr) return true;
        if (!g_engaged[i] && free_slot < 0) free_slot = i;
    }
    int anim = -1;
    bool attacking = enemy_attacking(c.chr, anim);
    if (!(c.hp < max_hp || attacking || (anim > 0 && ray_blocked(eye, c.p) != 1))) return false;
    if (free_slot >= 0) {
        g_engaged[free_slot] = c.chr;
        logf("boss bar: chr %p engaged (hp %d of %d, animation %d%s)\n", (void *)c.chr, c.hp, max_hp, anim, attacking ? ", attacking" : "");
    }
    return true;
}
static const char *boss_name(uintptr_t chr) {
    int id = 0;
    if (!g_boss_name_count || !safe_read(chr + OFF_CHR_NPC_PARAM, &id, 4)) return nullptr;
    for (int i = 0; i < g_boss_name_count; i++)
        if (g_boss_names[i].id == id) return g_boss_names[i].name;
    return nullptr;
}

struct Watched { uintptr_t chr; int hp; };
enum { MAX_WATCHED = 96 };
static Watched g_watched[MAX_WATCHED];
static int g_watched_n = 0;
static float g_multikill_timer = 0;
static int g_multikill_count = 0;
static volatile LONG g_style_hits = 0, g_style_kills = 0;
static void style_kill(bool boss) {
    InterlockedIncrement(&g_style_kills);
    // StyleCalculator: an explosion's kill is 45 EXPLODED, the shotgun's 45 KILL, or 100 OVERKILL when
    // the enemy was within the 4 u in front of the muzzle
    if (boss) style_add(100, "BIG KILL");
    else if (g_last_hitter == HIT_EXPLOSION) style_add(45, "EXPLODED");
    else if (g_last_hitter == HIT_SHOTGUN_ZONE) style_add(100, "OVERKILL");
    else style_add(g_last_hitter == HIT_SHOTGUN ? 45 : 30, "KILL");
    g_multikill_count++;
    g_multikill_timer = 5.0f;
    if (g_multikill_count == 2) style_add(25, "DOUBLE KILL", STYLE_ORANGE);
    else if (g_multikill_count == 3) style_add(50, "TRIPLE KILL", STYLE_ORANGE);
    else if (g_multikill_count > 3) style_add(100, "MULTIKILL", STYLE_ORANGE, g_multikill_count);
}
static void style_hit(bool boss) {
    InterlockedIncrement(&g_style_hits);
    switch (g_last_hitter) {
    case HIT_PUNCH: if (boss) style_add(60, "DISRESPECT"); else style_add(20, ""); break;
    case HIT_SHOTGUN: case HIT_SHOTGUN_ZONE: style_add(4, ""); break;
    case HIT_EXPLOSION: style_add(15, ""); break;
    default: style_add(10, ""); break;
    }
}
static void watch_enemies(double t, float dt) {
    if (g_multikill_timer > 0) g_multikill_timer -= dt * 10.0f;
    else g_multikill_count = 0;
    static double next = 0;
    if (t < next) return;
    next = t + 0.05;
    float eye[3];
    if (!eye_pos(eye)) return;
    Target seen[MAX_WATCHED];
    int n = find_targets(eye, seen, MAX_WATCHED, 200.0f, true);
    bool mine = t - g_last_attack_time < 0.6;
    Watched next_list[MAX_WATCHED];
    int next_n = 0, bosses = 0;
    for (int i = 0; i < n; i++) {
        const Target &c = seen[i];
        if (bosses < HudState::MAX_BOSSES && c.present && c.hp > 0 && c.dist <= BOSS_BAR_RANGE) {   // the list is nearest first
            int max_hp = 0;
            const char *name = boss_name(c.chr);
            if (name && safe_read(c.chr + 0x3EC, &max_hp, 4) && max_hp > 0 && boss_engaged(c, eye, max_hp)) {
                HudState::Boss &b = g_bosses[bosses++];
                strncpy(b.name, name, sizeof(b.name) - 1);
                b.name[sizeof(b.name) - 1] = 0;
                b.hp = (float)c.hp / (float)max_hp;
            }
        }
        if (!(c.team == 6 || c.team == 7 || c.team == 9)) continue;
        int before = -1;
        for (int k = 0; k < g_watched_n; k++)
            if (g_watched[k].chr == c.chr) { before = g_watched[k].hp; break; }
        if (mine && before > 0 && c.hp < before) {
            if (c.hp <= 0) style_kill(c.team == 7);
            else style_hit(c.team == 7);
            const float bodies = 0.75f;
            int hitter = g_last_hitter;
            float heal = 0;
            if (hitter == HIT_SHOTGUN || hitter == HIT_SHOTGUN_ZONE) {
                // how many of the twelve pellets landed is not known; taken as all of them at arm's
                // length, falling to four at the edge of the blood's reach
                float k = c.dist / (5.0f * UK_UNIT + bodies);
                if (k <= 1.0f) heal = 3.0f * (12.0f - 8.0f * k);
            } else if (c.dist <= 5.0f * UK_UNIT + bodies) {
                heal = hitter == HIT_EXPLOSION ? 3.0f : 10.0f;
            }
            if (c.hp <= 0 && c.dist <= 7.0f * UK_UNIT + bodies) heal += 30.0f;
            if (heal > 0) {
                heal_player(heal);
                logf("blood: +%.0f health from chr %p at %.1f m%s\n", heal, (void *)c.chr, c.dist, c.hp <= 0 ? " (killed)" : "");
            }
        }
        if (next_n < MAX_WATCHED) next_list[next_n++] = {c.chr, c.hp};
    }
    memcpy(g_watched, next_list, sizeof(Watched) * next_n);
    g_watched_n = next_n;
    g_boss_count = bosses;
    for (uintptr_t &e : g_engaged) {             // a boss that has died, or is 100 m behind, is forgotten
        if (!e) continue;
        bool still = false;
        for (int i = 0; i < n && !still; i++) still = seen[i].chr == e && seen[i].hp > 0 && seen[i].dist <= 100.0f;
        if (!still) e = 0;
    }
}

// ---- the Marksman variation and its coins (Revolver.ThrowCoin, Coin, WeaponCharges in ULTRAKILL's code).
// A coin leaves from 0.5 u under the eye at forward * 20 + up * 15 u/s plus the player's own velocity and
// falls under gravity. From 0.1 s old it can be shot: 0.1 s later the shot goes on to another coin
// (adding 1 to its power) or to the nearest enemy, with the coin's power (2) as damage against a plain
// shot's 1. A coin shot in its flash (0.35 to 0.417 s old) or after 1 s goes to two targets. Coins last
// 5 s; four charges, one refilled every 4 s (25 of 400 per second).
struct Coin { bool alive, shot, flashed, charged; float p[3], v[3], thrown_y; double born, reflect_at; int power, hit_times; };
enum { MAX_COINS = 8 };
static Coin g_coins[MAX_COINS];
static volatile int g_variation = 1;             // the revolver's: 0 Piercer, 1 Marksman, 2 Sharpshooter
enum { WEAPON_REVOLVER = 0, WEAPON_SHOTGUN = 1 };
static volatile int g_weapon = WEAPON_REVOLVER;
static volatile float g_coin_charge = 400.0f;
static volatile LONG g_coins_thrown = 0, g_coins_hit = 0, g_coin_chains = 0, g_coin_shots = 0, g_coin_misses = 0;
static const int BEHAVIOR_COIN = 9000120;        // + (power - 2), up to power 5
static const float COIN_FORWARD = 20.0f * UK_UNIT, COIN_UP = 15.0f * UK_UNIT, COIN_DROP = 0.5f * UK_UNIT, COIN_GRAVITY = 40.0f * UK_UNIT;
static const float COIN_LIFE = 5.0f, COIN_ARMED = 0.1f, COIN_REFLECT_DELAY = 0.1f, COIN_REFILL = 25.0f;
static const float COIN_HIT_RADIUS = 0.5f;       // ours: how close the line of fire must pass (V1's coin has a generous collider)
static const float COIN_RANGE = 150.0f;
static const float COIN_MAX_DROP = 15.0f;        // ours

static void view_axes(float *right, float *up, float *fwd) {
    float sy = sinf(g_yaw), cy = cosf(g_yaw), sp = sinf(g_pitch), cp = cosf(g_pitch);
    right[0] = cy; right[1] = 0; right[2] = -sy;
    up[0] = -sp * sy; up[1] = cp; up[2] = -sp * cy;
    fwd[0] = sy * cp; fwd[1] = sp; fwd[2] = cy * cp;
    float roll = g_tilt_deg * 3.14159265f / 180.0f, cr = cosf(roll), sr = sinf(roll);
    for (int i = 0; i < 3; i++) {
        float r0 = right[i], u0 = up[i];
        right[i] = r0 * cr - u0 * sr;
        up[i] = u0 * cr + r0 * sr;
    }
}
static bool eye_pos(float *eye) {
    uintptr_t pos = g_player_pos;
    if (!pos) return false;
    const float *p = (const float *)(pos + OFF_POS_X);
    eye[0] = p[0]; eye[1] = p[1] + g_eye_height - g_eye_drop; eye[2] = p[2];
    return true;
}
static void throw_coin() {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return;
    view_axes(right, up, fwd);
    for (Coin &c : g_coins) {
        if (c.alive) continue;
        c = Coin{};
        c.alive = true;
        c.born = now_s();
        c.power = 2;
        float pv[3] = {g_vx, g_air ? g_vy : 0.0f, g_vz};
        for (int i = 0; i < 3; i++) {
            c.p[i] = eye[i] - up[i] * COIN_DROP;
            if (i == 1) c.thrown_y = c.p[1];
            c.v[i] = fwd[i] * COIN_FORWARD + (i == 1 ? COIN_UP : 0.0f) + pv[i];
        }
        InterlockedIncrement(&g_coins_thrown);
        sound_play("coin_toss", 1.0f, 1.0f);
        play_arm("CoinFlip");
        return;
    }
}
// The coin the line of fire from the eye passes closest to, if any is within reach.
static Coin *coin_on_line() {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return nullptr;
    view_axes(right, up, fwd);
    double t = now_s();
    Coin *best = nullptr;
    float best_along = COIN_RANGE;
    for (Coin &c : g_coins) {
        if (!c.alive || c.shot || t - c.born < COIN_ARMED) continue;
        float d[3] = {c.p[0] - eye[0], c.p[1] - eye[1], c.p[2] - eye[2]};
        float along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (along <= 0 || along >= best_along) continue;
        float off2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along;
        if (off2 > COIN_HIT_RADIUS * COIN_HIT_RADIUS) continue;
        best = &c;
        best_along = along;
    }
    return best;
}
static void hit_coin(Coin &c, double t) {
    float age = (float)(t - c.born);
    c.shot = true;
    c.reflect_at = t + COIN_REFLECT_DELAY;
    c.hit_times = (age >= 0.35f && age < 0.417f) || age >= 1.0f ? 2 : 1;
    c.v[0] = c.v[1] = c.v[2] = 0;                // it hangs where it was hit until the shot leaves it
    InterlockedIncrement(&g_coins_hit);
    sound_play("coin_hit", 0.35f, 0.65f);
}
// ---- line of sight, through the game's own ray cast: bool hit(world wrapper, filter, from, to - from)
// at exe+2B03E0. The game calls it exactly like this for its own sight checks, e.g. at exe+37B3AA with
// the wrapper [[exe+1C74870]+0x28] and filter 0x26, which is what is passed here.
// (v0.39 called exe+2A8ED0 instead, which turned out to cast inside a phantom, not the world, and
// crashed the game on the first coin shot.)
// Returns 1 if something is in the way, 0 if not, -1 if the cast is unavailable.
static const uintptr_t RVA_RAYCAST = 0x2B03E0;
static const uint8_t RAYCAST_BYTES[8] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48};
static const int RAYCAST_FILTER = 0x26;
static int ray_blocked(const float *a, const float *b) {
    uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr);
    if (memcmp((void *)(exe + RVA_RAYCAST), RAYCAST_BYTES, sizeof(RAYCAST_BYTES)) != 0) return -1;
    uintptr_t phys = *(uintptr_t *)(exe + RVA_PHYS_WORLD);
    uintptr_t wrapper = phys ? *(uintptr_t *)(phys + 0x28) : 0;
    if (!wrapper) return -1;
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1.0f) return 0;
    float keep = (len - 0.5f) / len;             // stop half a metre short, so the target itself is never what is hit
    alignas(16) float from[4] = {a[0], a[1], a[2], 1.0f}, delta[4] = {d[0] * keep, d[1] * keep, d[2] * keep, 0.0f};
    bool hit = ((bool (*)(uintptr_t, int, float *, float *))(exe + RVA_RAYCAST))(wrapper, RAYCAST_FILTER, from, delta);
    return hit ? 1 : 0;
}

// The same world ray cast, but keeping what it hit: how far along the ray, and the surface's normal.
// The game's wrapper above only answers yes or no, so this calls Havok's hkpWorld::castRay (exe+9B5150)
// itself with a closest-hit collector laid out as the game's own use of one at exe+2A8ED0 lays it out:
//   input:     from (16 bytes), to (16), a zero byte at +0x20, the filter at +0x24
//   collector: vtable (exe+130B840), early-out fraction 1.0 at +8, then the result: normal at +0x10,
//              hit fraction at +0x20, -1 at +0x24 and +0x30, the body that was hit at +0x60 (0 if none)
static const uintptr_t RVA_WORLD_CASTRAY = 0x9B5150, RVA_VT_CLOSEST_RAY = 0x130B840;
static const uint8_t WORLD_CASTRAY_BYTES[8] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C};
struct RayHit { bool hit; float fraction, dist, point[3], normal[3]; };
static bool ray_cast(const float *a, const float *b, RayHit &out) {
    out = RayHit{};
    out.fraction = 1.0f;
    uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr);
    if (memcmp((void *)(exe + RVA_WORLD_CASTRAY), WORLD_CASTRAY_BYTES, sizeof(WORLD_CASTRAY_BYTES)) != 0) return false;
    uintptr_t phys = *(uintptr_t *)(exe + RVA_PHYS_WORLD);
    uintptr_t wrapper = phys ? *(uintptr_t *)(phys + 0x28) : 0;
    uintptr_t world = wrapper ? *(uintptr_t *)(wrapper + 8) : 0;
    if (!world) return false;
    alignas(16) uint8_t input[0x30] = {};
    alignas(16) uint8_t collector[0x80] = {};
    float from[4] = {a[0], a[1], a[2], 1.0f}, to[4] = {b[0], b[1], b[2], 1.0f};
    memcpy(input, from, 16);
    memcpy(input + 0x10, to, 16);
    *(int *)(input + 0x24) = RAYCAST_FILTER;
    *(uintptr_t *)collector = exe + RVA_VT_CLOSEST_RAY;
    *(float *)(collector + 0x08) = 1.0f;
    *(float *)(collector + 0x20) = 1.0f;
    *(int *)(collector + 0x24) = -1;
    *(int *)(collector + 0x30) = -1;
    ((void (*)(uintptr_t, uint8_t *, uint8_t *))(exe + RVA_WORLD_CASTRAY))(world, input, collector);
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    out.hit = *(uintptr_t *)(collector + 0x60) != 0;
    out.fraction = out.hit ? *(float *)(collector + 0x20) : 1.0f;
    out.dist = len * out.fraction;
    for (int i = 0; i < 3; i++) {
        out.point[i] = a[i] + d[i] * out.fraction;
        out.normal[i] = out.hit ? *(float *)(collector + 0x10 + i * 4) : 0.0f;
    }
    return true;
}

static uintptr_t g_watch_chr = 0;                // debug: the last ricochet's target, to log its health a second later
static int g_watch_hp = 0;
static double g_watch_at = 0;
static void reflect_coin(Coin &c, double t) {
    c.alive = false;
    sound_play("coin_hit", 0.35f, 0.65f);        // the reflected beam has the same ricochet sound
    Coin *next = nullptr;
    float best = COIN_RANGE * COIN_RANGE;
    for (Coin &o : g_coins) {
        if (!o.alive || o.shot || t - o.born < COIN_ARMED) continue;
        float dx = o.p[0] - c.p[0], dy = o.p[1] - c.p[1], dz = o.p[2] - c.p[2], d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best) { best = d2; next = &o; }
    }
    if (next) {
        hit_coin(*next, t);
        logf("coin shot: power %d passed on to another coin\n", c.power);
        next->power = c.power + 1;
        next->charged = c.charged;
        add_tracer(c.p, next->p, 0.3f, 0.05f, TRACER_GOLD);
        if (c.hit_times > next->hit_times) next->hit_times = c.hit_times;
        InterlockedIncrement(&g_coin_chains);
        return;
    }
    // V1's coin only goes for what it can see: the nearest enemies are tried in turn and those with
    // something between the coin and them are passed over (v0.38 shot at the nearest one through walls).
    Target cand[12], targets[2];
    int in_range = find_targets(c.p, cand, 12, COIN_RANGE), n = 0, want = c.hit_times > 1 ? 2 : 1;
    for (int i = 0; i < in_range && n < want; i++) {
        if (ray_blocked(c.p, cand[i].p) == 1) {
            logf("  not in sight: chr %p at %.1f m\n", (void *)cand[i].chr, cand[i].dist);
            continue;
        }
        targets[n++] = cand[i];
    }
    logf("coin shot: power %d, x%d, %d in range, %d in sight\n", c.power, c.hit_times, in_range, n);
    int power = c.power > 5 ? 5 : c.power;
    if (!n) {
        // Coin.ReflectRevolver with nothing to aim at: the shot leaves in a random direction
        float d[3], len;
        do {
            for (float &v : d) v = (float)rand() / RAND_MAX * 2.0f - 1.0f;
            len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        } while (len < 0.1f || len > 1.0f);
        for (float &v : d) v /= len;
        shoot(BEHAVIOR_COIN + power - 2, c.p, d);
        {
            float far_end[3] = {c.p[0] + d[0] * 60.0f, c.p[1] + d[1] * 60.0f, c.p[2] + d[2] * 60.0f};
            RayHit h;
            ray_cast(c.p, far_end, h);
            add_tracer(c.p, h.point, 0.3f, 0.06f, TRACER_GOLD);
        }
        InterlockedIncrement(&g_coin_misses);
        return;
    }
    for (int i = 0; i < c.hit_times; i++) {
        const Target &tg = targets[i < n ? i : n - 1];   // one enemy and a split: both shots go to it
        if (tg.dist < 0.01f) continue;
        float dir[3] = {(tg.p[0] - c.p[0]) / tg.dist, (tg.p[1] - c.p[1]) / tg.dist, (tg.p[2] - c.p[2]) / tg.dist};
        // a coin hit by the Piercer's charged shot sends that shot on: the piercing row instead of the coin's
        bool ok = shoot(c.charged ? BEHAVIOR_PIERCER : BEHAVIOR_COIN + power - 2, c.p, dir);
        add_tracer(c.p, tg.p, 0.3f, c.charged ? 0.10f : 0.06f, c.charged ? TRACER_BLUE : TRACER_GOLD);
        if (ok) InterlockedIncrement(&g_coin_shots);
        logf("  ricochet from (%.1f %.1f %.1f) to chr %p team %d hp %d at (%.1f %.1f %.1f), %.1f m: %s\n", c.p[0], c.p[1], c.p[2],
             (void *)tg.chr, tg.team, tg.hp, tg.p[0], tg.p[1], tg.p[2], tg.dist, ok ? "accepted" : "REFUSED");
        g_watch_chr = tg.chr;
        g_watch_hp = tg.hp;
        g_watch_at = t + 1.0;
        if (ok && i == 0) {
            // Coin.RicoshotPointsCheck: 50, plus 15 per coin when more than one was chained
            int coins = c.power - 1;
            style_add(50 + (coins > 1 ? coins * 15 : 0), "RICOSHOT", STYLE_CYAN, coins);
            note_attack(HIT_COIN);
        }
    }
}
static void update_coins(double t, float dt) {
    if (g_watch_chr && t >= g_watch_at) {
        int hp = -1;
        safe_read(g_watch_chr + OFF_CHR_HP, &hp, 4);
        logf("  one second after the ricochet: target hp %d -> %d (ricochet projectiles set up so far %ld, of %ld sent)\n", g_watch_hp, hp,
             (long)g_override_inits, (long)g_coin_shots);
        g_watch_chr = 0;
    }
    if (g_coin_charge < 400.0f) g_coin_charge = g_coin_charge + COIN_REFILL * dt > 400.0f ? 400.0f : g_coin_charge + COIN_REFILL * dt;
    for (Coin &c : g_coins) {
        if (!c.alive) continue;
        if (c.shot) {
            if (t >= c.reflect_at) reflect_coin(c, t);
            continue;
        }
        // V1's coins are removed once they come to rest. These pass through the floor, so one that has
        // dropped well below where it was thrown is taken as landed.
        if (t - c.born > COIN_LIFE || c.p[1] < c.thrown_y - COIN_MAX_DROP) { c.alive = false; continue; }
        if (!c.flashed && t - c.born >= 0.35) {
            c.flashed = true;
            sound_play("coin_flash", 0.5f, 1.25f);
        }
        c.v[1] -= COIN_GRAVITY * dt;
        for (int i = 0; i < 3; i++) c.p[i] += c.v[i] * dt;
    }
}

// ---- the arms (Punch, FistControl, WeaponCharges). Punch stamina holds 2 and refills at 1.25 per second;
// both arms share it and the fist cooldown, which runs down at 2 per second.
//   Feedbacker (F):     needs 1 stamina, costs 1, cooldown 0.5; the damage of one revolver shot within 4 u
//                       of the eye, or within a 1 u sphere swept that far.
//   Knuckleblaster (G): costs 1.5, cooldown 0.75; damage 2.5 and four times the force. If the key is still
//                       held partway through the punch it also lets off a blast wave 2 u ahead.
// Each is an unseen short projectile; the rows (9000130 to 9000133) set the damage and how hard Dark
// Souls staggers what is hit.
static const int BEHAVIOR_PUNCH = 9000130, BEHAVIOR_PARRY = 9000131, BEHAVIOR_KNUCKLE = 9000132, BEHAVIOR_BLAST = 9000133;
static const float PUNCH_REACH = 4.0f * UK_UNIT, PUNCH_RADIUS = 1.0f * UK_UNIT;
static const double BLAST_CHECK_AT = 0.42;       // the "BlastCheck" event of the arm's Punch clip
static volatile float g_punch_stamina = 2.0f;
static volatile int g_last_arm = 0;              // 0 Feedbacker, 1 Knuckleblaster: which one the fist icon shows
static float g_fist_cooldown = 0;
static volatile LONG g_punches = 0, g_punch_hits = 0, g_parries = 0, g_blasts = 0;
static bool g_prev_punch_key = false, g_prev_knuckle_key = false;
static double g_blast_check = -1;                // when the Knuckleblaster's punch reaches its blast check
static volatile double g_flash_until = -100.0;

// A parry needs to know that the enemy is attacking. Every character has 31 animation slots at
// [[chr+0x68]+0x20], 0xA8 bytes each, whose first int is the animation playing in the slot (-1 for none;
// found by watching the player's own slots while it walked and swung). Dark Souls numbers a character's
// attack animations 3000 to 3999. `seen` gets that id, or else any id that is playing, for the log.
enum { ANIM_SLOTS = 31, ANIM_SLOT_SIZE = 0xA8 };
static bool enemy_attacking(uintptr_t chr, int &seen) {
    static uint8_t buf[ANIM_SLOTS * ANIM_SLOT_SIZE];
    uintptr_t ctrl = 0, med = 0;
    seen = -1;
    if (!safe_read(chr + 0x68, &ctrl, 8) || ctrl < 0x10000 || !safe_read(ctrl + 0x20, &med, 8) || med < 0x10000) return false;
    if (!safe_read(med, buf, sizeof(buf))) return false;
    for (int i = 0; i < ANIM_SLOTS; i++) {
        int id = *(int *)(buf + i * ANIM_SLOT_SIZE);
        if (id >= 3000 && id < 4000) { seen = id; return true; }
        if (id > 0 && seen < 0) seen = id;
    }
    return false;
}
// The nearest living enemy inside a punch's reach along the view.
static bool punch_target(Target &out) {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return false;
    view_axes(right, up, fwd);
    Target near_by[6];
    int n = find_targets(eye, near_by, 6, PUNCH_REACH + 1.5f);
    for (int i = 0; i < n; i++) {
        float d[3] = {near_by[i].p[0] - eye[0], near_by[i].p[1] - eye[1], near_by[i].p[2] - eye[2]};
        float along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (along < 0 || along > PUNCH_REACH + 0.5f) continue;
        float off2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along;
        float reach = PUNCH_RADIUS + 0.9f;       // the sweep's radius plus about half a body, as the aim point is its middle
        if (off2 > reach * reach) continue;
        out = near_by[i];
        return true;
    }
    return false;
}
// NewMovement.Parry: the flash and quarter-second freeze, full health, full stamina and 100 style points.
// The sound is the ParryLight object's.
static void do_parry(const Target &tg, int anim) {
    InterlockedIncrement(&g_parries);
    style_add(100, "PARRY", STYLE_GREEN);
    uintptr_t chr = g_player_chr;
    int max_hp = 0;
    if (chr && safe_read(chr + 0x3EC, &max_hp, 4) && max_hp > 0) safe_write(chr + 0x3E8, &max_hp, 4);
    g_boost = 300.0f;
    g_punch_stamina = 2.0f;
    g_freeze_request = 0.25f;
    sound_play("coin_hit", 0.6f, 1.0f);
    logf("parry: chr %p hp %d was in attack animation %d\n", (void *)tg.chr, tg.hp, anim);
}
// ---- sounds an animation plays partway through (its events: the shotgun clicking shut, the smacks
// as the cores go back in). Times are the events' own, read from the clips by tools/uk_models.py.
struct TimedSound { double at; const char *name; float volume, pitch; int channel; };
enum { MAX_TIMED_SOUNDS = 8 };
static TimedSound g_timed_sounds[MAX_TIMED_SOUNDS];
static void sound_later(double delay, const char *name, float volume, float pitch, int channel = CH_GUN) {
    for (TimedSound &s : g_timed_sounds)
        if (!s.name) {
            s = {now_s() + delay, name, volume, pitch, channel};
            return;
        }
}
static void timed_sounds_clear() {
    for (TimedSound &s : g_timed_sounds) s.name = nullptr;
}
static void timed_sounds_update(double t) {
    for (TimedSound &s : g_timed_sounds)
        if (s.name && t >= s.at) {
            sound_play(s.name, s.volume, s.pitch, false, s.channel);
            s.name = nullptr;
        }
}

// ---- explosions (Explosion). ULTRAKILL's is a sphere that grows to its full size in a fraction of a
// second and hurts what it reaches: 6 u at 3.5 times a revolver shot for a core or a punched pellet, and
// the "super" one, 12 u at twice that, for a core shot in the air. Here it is one wide, short-lived
// projectile (rows 9000142 and 9000143) sent straight up from the spot, which the game hits everything
// in its radius with. The fireball that is drawn is ours.
static const int BEHAVIOR_PELLET = 9000140, BEHAVIOR_SHOTGUN_PARRY = 9000141, BEHAVIOR_EXPLOSION = 9000142, BEHAVIOR_EXPLOSION_SUPER = 9000143;
static const int BEHAVIOR_SHARP = 9000150;
static const float EXPLOSION_RADIUS = 6.0f * UK_UNIT, EXPLOSION_SUPER_RADIUS = 12.0f * UK_UNIT;
struct BlastFx { float p[3], radius; double born; bool wave; };   // wave: the Knuckleblaster's, a ring with little fire
enum { MAX_BLAST_FX = 6 };
static BlastFx g_blast_fx[MAX_BLAST_FX];
static int g_blast_fx_next = 0;
static volatile LONG g_explosions = 0;
static void explode(const float *p, bool super) {
    const float up[3] = {0, 1, 0};
    bool ok = shoot(super ? BEHAVIOR_EXPLOSION_SUPER : BEHAVIOR_EXPLOSION, p, up);
    note_attack(HIT_EXPLOSION);
    InterlockedIncrement(&g_explosions);
    BlastFx &b = g_blast_fx[g_blast_fx_next++ % MAX_BLAST_FX];
    memcpy(b.p, p, sizeof(b.p));
    b.radius = super ? EXPLOSION_SUPER_RADIUS : EXPLOSION_RADIUS;
    b.born = now_s();
    b.wave = false;
    float eye[3], d = 0;
    if (eye_pos(eye)) d = sqrtf((p[0] - eye[0]) * (p[0] - eye[0]) + (p[1] - eye[1]) * (p[1] - eye[1]) + (p[2] - eye[2]) * (p[2] - eye[2]));
    float vol = 1.0f - d / (75.0f * UK_UNIT);    // the explosion's sound reaches 75 u
    sound_play(super ? "explosion_super" : "explosion", vol < 0.15f ? 0.15f : vol, frand(0.75f, 1.25f));
    logf("explosion%s at (%.1f %.1f %.1f), %.1f m away: %s\n", super ? " (super)" : "", p[0], p[1], p[2], d, ok ? "accepted" : "REFUSED");
}

// Is `p` inside an enemy? Targets are aim points TARGET_HEIGHT above the feet; a body is taken as a
// column half a metre in radius and 2 m tall, plus the radius of what is flying into it.
static bool touches_enemy(const float *p, float radius) {
    Target near_by[4];
    int n = find_targets(p, near_by, 4, 3.0f + radius);
    for (int i = 0; i < n; i++) {
        float dx = near_by[i].p[0] - p[0], dz = near_by[i].p[2] - p[2], dy = p[1] - near_by[i].p[1], reach = 0.5f + radius;
        if (dx * dx + dz * dz < reach * reach && dy > -TARGET_HEIGHT - radius && dy < 2.0f - TARGET_HEIGHT + radius) return true;
    }
    return false;
}

// ---- the shotgun (Shotgun, variation 0: the Core Eject).
//   Primary: twelve pellets from the eye, each turned up to 10 degrees off the view on both axes, at 75 u/s,
//   a quarter of a revolver shot each; ready again 1.33 s later (the ReadyGun event of FireWithReload).
//   An enemy within 4 u along the view that is in the middle of an attack is parried by the shot.
//   Alt fire: held, the core winds up from 0 to 60 in a second; let go, it is thrown from half a unit
//   ahead at (forward + up * force * 0.002) * (force + 10) u/s, falls at 40 u/s^2 and goes off on the
//   first thing it touches. A core shot in the air goes off as the larger explosion. Ready 3.08 s later.
//   Projectile boost: a Feedbacker punch as the shot leaves takes one pellet and sends it on along the
//   view; it goes off where it lands, and landing on an enemy is worth 90 style points. Punch.cs raises a
//   punched pellet's speed to 100 u/s; Davi found that too slow against the original's feel ("so fast that
//   it looks instant"), so it is 250 u/s here, the speed Punch.cs gives a projectile that had none.
// The pellets that do the damage are the game's projectiles (row 9000140); the ones that are drawn, the
// core and the boosted pellet are tracked here.
static const int SHOTGUN_PELLETS = 12;
static const float SHOTGUN_SPREAD = 10.0f * 3.14159265f / 180.0f;
static const float PELLET_SPEED = 75.0f * UK_UNIT, PELLET_RANGE = 60.0f, BOOSTED_SPEED = 250.0f * UK_UNIT, BOOSTED_RANGE = 150.0f;
static const double SHOTGUN_READY = 1.33, SHOTGUN_CLICK = 1.16, CORE_READY = 3.08, CORE_SMACK_1 = 2.26, CORE_SMACK_2 = 2.31, CORE_CLICK = 2.56;
static const double SHOTGUN_EQUIP_READY = 0.44, REVOLVER_PICKUP_READY = 0.36;
static const double BOOST_WINDOW = 0.15;         // ours: how long after the shot a punch still catches a pellet
static const float CORE_GRAVITY = 40.0f * UK_UNIT, CORE_HIT_RADIUS = 0.5f, CORE_LIFE = 10.0f;
struct Pellet { bool alive, boosted; float p[3], v[3], end[3]; double die_at; };
enum { MAX_PELLETS = 48 };
static Pellet g_pellets[MAX_PELLETS];
struct Core { bool alive; float p[3], v[3]; double born; };
enum { MAX_CORES = 4 };
static Core g_cores[MAX_CORES];
static double g_shotgun_ready_at = 0, g_cores_ready_at = 0, g_last_shotgun_shot = -100.0, g_last_core = -100.0, g_last_feedbacker = -100.0;
static bool g_shot_boosted = true;               // one boost per shot
static volatile float g_core_force = 0;          // 0..60 while the alt fire is held
static bool g_core_charging = false;
static volatile LONG g_shotgun_shots = 0, g_cores_thrown = 0, g_boosts = 0, g_shotgun_parries = 0;
static volatile double g_muzzle_flash = -100.0;

static void spawn_pellet(const float *from, const float *dir, float speed, float range, bool boosted) {
    for (Pellet &pl : g_pellets) {
        if (pl.alive) continue;
        pl.alive = true;
        pl.boosted = boosted;
        float far_end[3];
        for (int i = 0; i < 3; i++) {
            pl.p[i] = from[i];
            pl.v[i] = dir[i] * speed;
            far_end[i] = from[i] + dir[i] * range;
        }
        // it flies until the first surface on its line
        RayHit h;
        float reach = ray_cast(from, far_end, h) && h.hit ? h.dist : range;
        for (int i = 0; i < 3; i++) pl.end[i] = from[i] + dir[i] * (reach > 0.2f ? reach - 0.2f : 0.0f);
        pl.die_at = now_s() + reach / speed;
        return;
    }
}
static void update_pellets(double t, float dt) {
    for (Pellet &pl : g_pellets) {
        if (!pl.alive) continue;
        if (pl.boosted && touches_enemy(pl.p, 0.2f)) {
            // Projectile.Collided: a boosted pellet that reaches an enemy is worth 90
            static const float green[3] = {0, 1, 0};
            style_add(90, "PROJECTILE BOOST", green);
            explode(pl.p, false);
            pl.alive = false;
            continue;
        }
        if (t >= pl.die_at) {
            if (pl.boosted) explode(pl.end, false);
            pl.alive = false;
            continue;
        }
        for (int i = 0; i < 3; i++) pl.p[i] += pl.v[i] * dt;
    }
}
static void throw_core(float force) {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return;
    view_axes(right, up, fwd);
    for (Core &c : g_cores) {
        if (c.alive) continue;
        c.alive = true;
        c.born = now_s();
        for (int i = 0; i < 3; i++) {
            c.p[i] = eye[i] + fwd[i] * 0.5f * UK_UNIT;
            c.v[i] = (fwd[i] + (i == 1 ? force * 0.002f : 0.0f)) * (force + 10.0f) * UK_UNIT;
        }
        return;
    }
}
// The core the line of fire from the eye passes closest to, if any is within reach.
static Core *core_on_line() {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return nullptr;
    view_axes(right, up, fwd);
    Core *best = nullptr;
    float best_along = COIN_RANGE;
    for (Core &c : g_cores) {
        if (!c.alive) continue;
        float d[3] = {c.p[0] - eye[0], c.p[1] - eye[1], c.p[2] - eye[2]};
        float along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (along <= 0 || along >= best_along) continue;
        float off2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along;
        if (off2 > CORE_HIT_RADIUS * CORE_HIT_RADIUS) continue;
        best = &c;
        best_along = along;
    }
    return best;
}
static void update_cores(double t, float dt) {
    for (Core &c : g_cores) {
        if (!c.alive) continue;
        if (t - c.born > CORE_LIFE) { c.alive = false; continue; }
        if (touches_enemy(c.p, 0.25f)) {
            explode(c.p, false);
            c.alive = false;
            continue;
        }
        c.v[1] -= CORE_GRAVITY * dt;
        float next[3] = {c.p[0] + c.v[0] * dt, c.p[1] + c.v[1] * dt, c.p[2] + c.v[2] * dt};
        RayHit h;
        if (ray_cast(c.p, next, h) && h.hit) {
            float at[3] = {h.point[0] + h.normal[0] * 0.15f, h.point[1] + h.normal[1] * 0.15f, h.point[2] + h.normal[2] * 0.15f};
            explode(at, false);
            c.alive = false;
            continue;
        }
        memcpy(c.p, next, sizeof(next));
    }
}
// Punch.ParryProjectile on one of the player's own pellets.
static void boost_pellet(double t) {
    float right[3], up[3], fwd[3], eye[3];
    if (g_shot_boosted || !eye_pos(eye)) return;
    g_shot_boosted = true;
    view_axes(right, up, fwd);
    for (Pellet &pl : g_pellets)
        if (pl.alive && !pl.boosted) { pl.alive = false; break; }   // the pellet that was punched
    float start[3] = {eye[0] + fwd[0], eye[1] + fwd[1], eye[2] + fwd[2]};
    spawn_pellet(start, fwd, BOOSTED_SPEED, BOOSTED_RANGE, true);
    InterlockedIncrement(&g_boosts);
    g_freeze_request = 0.25f;                    // TimeController.ParryFlash
    sound_play("punch_projectile", 0.6f, 1.0f);
    play_arm("Hook", 0.065);
    logf("projectile boost: a pellet punched on at %.0f m/s\n", BOOSTED_SPEED);
}
static void fire_shotgun(double t) {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return;
    view_axes(right, up, fwd);
    float start[3] = {eye[0] + fwd[0] * 0.6f, eye[1] + fwd[1] * 0.6f, eye[2] + fwd[2] * 0.6f};
    int sent = 0;
    for (int i = 0; i < SHOTGUN_PELLETS; i++) {
        // transform.Rotate(x, y, z) with each angle random within the spread: a pitch, then a yaw
        float rx = frand(-SHOTGUN_SPREAD, SHOTGUN_SPREAD), ry = frand(-SHOTGUN_SPREAD, SHOTGUN_SPREAD);
        float lx = sinf(ry) * cosf(rx), ly = -sinf(rx), lz = cosf(ry) * cosf(rx), d[3];
        for (int k = 0; k < 3; k++) d[k] = right[k] * lx + up[k] * ly + fwd[k] * lz;
        if (shoot(BEHAVIOR_PELLET, start, d)) sent++;
        spawn_pellet(start, d, PELLET_SPEED, PELLET_RANGE, false);
    }
    InterlockedIncrement(&g_shotgun_shots);
    Target tg;
    int anim = -1;
    bool in_reach = punch_target(tg), parried = in_reach && enemy_attacking(tg.chr, anim);
    note_attack(in_reach ? HIT_SHOTGUN_ZONE : HIT_SHOTGUN);
    if (parried) {
        // the "shotgunzone" hit on an enemy that can be parried
        InterlockedIncrement(&g_shotgun_parries);
        shoot(BEHAVIOR_SHOTGUN_PARRY);
        do_parry(tg, anim);
    }
    sound_play("shotgun_shot", 0.45f, frand(0.95f, 1.05f), false, CH_GUN);
    play_revolver("FireWithReload");
    timed_sounds_clear();
    sound_later(SHOTGUN_CLICK, "shotgun_click", 0.5f, frand(0.95f, 1.05f));
    g_shotgun_ready_at = t + SHOTGUN_READY;
    g_last_shotgun_shot = t;
    g_muzzle_flash = t;
    g_shot_boosted = false;
    if (t - g_last_feedbacker < 0.03) boost_pellet(t);       // punched on the same frame
    logf("shotgun: %d of %d pellets accepted%s\n", sent, SHOTGUN_PELLETS, parried ? ", point-blank parry" : in_reach ? ", enemy in reach" : "");
}

// ---- the Sharpshooter (Revolver, variation 2). Holding the alt fire spins the gun: the charge rises at
// 75 a second to 100 and falls back at the same rate when let go. Letting go (or pressing fire) with 25
// or more spends one of three charges (100 of 300, refilled at 15 a second) on a shot that goes through
// every enemy and bounces off surfaces: once per 25 of charge, three at most. Each bounce leaves 0.1 s
// later along the mirrored direction, unless a living enemy stands within 5 u of that line in plain
// sight, in which case it goes for the enemy (RevolverBeam.RicochetAimAssist).
static const float SHARP_REFILL = 15.0f, TWIRL_RATE = 75.0f;
static const float TRACER_RED[3] = {1.0f, 0.25f, 0.2f};
static volatile float g_sharp_charge = 300.0f, g_twirl_charge = 0, g_twirl_angle = 0;   // the angle in degrees
static float g_twirl_level = 0;
static volatile float g_twirl_blend = 0;         // 0..1: the Animator's TwirlSpeed (level / 3), eased, as the weight of the Twirl clip's pose
static bool g_twirling = false, g_twirl_recovery = false;
struct Ricochet { bool pending; double at; float from[3], dir[3]; int left; };
enum { MAX_RICOCHETS = 6 };
static Ricochet g_ricochets[MAX_RICOCHETS];
static volatile LONG g_sharp_shots = 0, g_sharp_bounces = 0;
static void shot_tracer(const float *end, float life, float width, const float *rgb);

// One leg of the beam, from `from` along `dir` to the first surface; `first` is the leg from the gun.
static void sharp_leg(const float *from, const float *dir, int left, bool first) {
    float far_end[3];
    for (int i = 0; i < 3; i++) far_end[i] = from[i] + dir[i] * 150.0f;
    RayHit h;
    bool cast = ray_cast(from, far_end, h);
    bool ok = first ? shoot(BEHAVIOR_SHARP) : shoot(BEHAVIOR_SHARP, from, dir);
    if (first) shot_tracer(h.point, 0.5f, 0.10f, TRACER_RED);
    else add_tracer(from, h.point, 0.5f, 0.10f, TRACER_RED);
    logf("sharpshooter: %s leg %.1f m, %s, %d bounces left, surface normal (%.2f %.2f %.2f): %s\n", first ? "first" : "bounced", h.dist,
         h.hit ? "hit a surface" : "hit nothing", left, h.normal[0], h.normal[1], h.normal[2], ok ? "accepted" : "REFUSED");
    if (!cast || !h.hit || left <= 0) return;
    for (Ricochet &r : g_ricochets) {
        if (r.pending) continue;
        r.pending = true;
        r.at = now_s() + 0.1;
        r.left = left - 1;
        float dn = dir[0] * h.normal[0] + dir[1] * h.normal[1] + dir[2] * h.normal[2];
        for (int i = 0; i < 3; i++) {
            r.dir[i] = dir[i] - 2.0f * dn * h.normal[i];          // Vector3.Reflect
            r.from[i] = h.point[i] - dir[i] * 0.1f;               // just off the surface, on the side it came from
        }
        return;
    }
}
static void update_ricochets(double t) {
    for (Ricochet &r : g_ricochets) {
        if (!r.pending || t < r.at) continue;
        r.pending = false;
        Target cand[12];
        int n = find_targets(r.from, cand, 12, 150.0f);
        const Target *pick = nullptr;
        float best = 1e9f, tube = 5.0f * UK_UNIT;
        for (int i = 0; i < n; i++) {
            float d[3] = {cand[i].p[0] - r.from[0], cand[i].p[1] - r.from[1], cand[i].p[2] - r.from[2]};
            float along = d[0] * r.dir[0] + d[1] * r.dir[1] + d[2] * r.dir[2];
            if (along < 0.1f || along >= best) continue;
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along > tube * tube) continue;
            if (ray_blocked(r.from, cand[i].p) == 1) continue;
            best = along;
            pick = &cand[i];
        }
        if (pick && pick->dist > 0.01f)
            for (int i = 0; i < 3; i++) r.dir[i] = (pick->p[i] - r.from[i]) / pick->dist;
        InterlockedIncrement(&g_sharp_bounces);
        sound_play("ricochet", 0.35f, 1.0f);
        float from[3], dir[3];
        memcpy(from, r.from, sizeof(from));
        memcpy(dir, r.dir, sizeof(dir));
        if (pick) logf("sharpshooter: bounce aimed at chr %p, %.1f m away\n", (void *)pick->chr, pick->dist);
        sharp_leg(from, dir, r.left, false);
    }
}

// Coin.Punchflection: a Feedbacker punch on a coin sends it at the nearest enemy it can see, for the
// coin's power as damage and 50 style points, and the coin comes off the enemy flying straight up at
// 25 u/s with one more power, ready to be shot or punched again. With no enemy in sight it goes along
// the view to the first surface and comes off that the same way, one unit back from it.
static volatile LONG g_coin_punches = 0;
static bool punch_coin(double t) {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return false;
    view_axes(right, up, fwd);
    Coin *best = nullptr;
    float best_along = PUNCH_REACH + 0.5f;
    for (Coin &c : g_coins) {
        if (!c.alive || c.shot) continue;
        float d[3] = {c.p[0] - eye[0], c.p[1] - eye[1], c.p[2] - eye[2]};
        float along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (along <= 0 || along >= best_along) continue;
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along > 0.8f * 0.8f) continue;   // ours: the punch's sweep plus the coin's generous collider
        best = &c;
        best_along = along;
    }
    if (!best) return false;
    Coin &c = *best;
    Target cand[12];
    int n = find_targets(c.p, cand, 12, COIN_RANGE);
    const Target *tg = nullptr;
    for (int i = 0; i < n && !tg; i++)
        if (ray_blocked(c.p, cand[i].p) != 1 && cand[i].dist > 0.01f) tg = &cand[i];
    int power = c.power > 5 ? 5 : c.power < 2 ? 2 : c.power;
    float land[3];
    if (tg) {
        float dir[3] = {(tg->p[0] - c.p[0]) / tg->dist, (tg->p[1] - c.p[1]) / tg->dist, (tg->p[2] - c.p[2]) / tg->dist};
        bool ok = shoot(BEHAVIOR_COIN + power - 2, c.p, dir);
        add_tracer(c.p, tg->p, 0.3f, 0.06f, TRACER_GOLD);
        style_add(50, "FISTFUL OF DOLLAR", STYLE_CYAN);
        note_attack(HIT_COIN);
        land[0] = tg->p[0];
        land[1] = tg->p[1] + 0.5f;               // the aim point is the chest; the coin comes off the head
        land[2] = tg->p[2];
        logf("coin punch: power %d at chr %p hp %d, %.1f m from the coin: %s\n", power, (void *)tg->chr, tg->hp, tg->dist, ok ? "accepted" : "REFUSED");
    } else {
        float far_end[3] = {eye[0] + fwd[0] * 150.0f, eye[1] + fwd[1] * 150.0f, eye[2] + fwd[2] * 150.0f};
        RayHit h;
        if (!ray_cast(eye, far_end, h) || !h.hit) {
            add_tracer(c.p, far_end, 0.3f, 0.06f, TRACER_GOLD);
            c.alive = false;                     // nothing out there to come back off
            logf("coin punch: no enemy in sight and nothing along the view; the coin is gone\n");
            return true;
        }
        add_tracer(c.p, h.point, 0.3f, 0.06f, TRACER_GOLD);
        for (int i = 0; i < 3; i++) land[i] = h.point[i] - fwd[i] * UK_UNIT;
        logf("coin punch: no enemy in sight; off a surface %.1f m away\n", h.dist);
    }
    InterlockedIncrement(&g_coin_punches);
    memcpy(c.p, land, sizeof(land));
    c.v[0] = c.v[2] = 0;
    c.v[1] = 25.0f * UK_UNIT;
    c.shot = c.flashed = c.charged = false;
    c.born = t;
    c.thrown_y = land[1];
    c.power = c.power + 1;
    c.hit_times = 1;
    sound_play("coin_hit", 0.35f, 1.0f + (power - 2) / 5.0f);
    return true;
}

static void update_punch(bool armed, float dt) {
    g_punch_stamina = g_punch_stamina + 1.25f * dt > 2.0f ? 2.0f : g_punch_stamina + 1.25f * dt;
    g_fist_cooldown = g_fist_cooldown - 2.0f * dt < 0 ? 0 : g_fist_cooldown - 2.0f * dt;
    bool key = armed && key_down('F'), key2 = armed && key_down('G');
    bool edge = key && !g_prev_punch_key, edge2 = key2 && !g_prev_knuckle_key;
    g_prev_punch_key = key;
    g_prev_knuckle_key = key2;
    double t = now_s();

    // the Knuckleblaster's blast, if its key is still down when the punch gets there
    if (g_blast_check > 0 && t >= g_blast_check) {
        g_blast_check = -1;
        if (key2) {
            InterlockedIncrement(&g_blasts);
            play_arm2("PunchBlast");
            sound_play("knuckle_blast", 1.0f, 1.0f);
            sound_later(0.78, "knuckle_eject", 0.5f, 1.0f, CH_FREE);   // the clip's Eject event: the shells go out
            note_attack(HIT_EXPLOSION);
            shoot(BEHAVIOR_BLAST);
            float right[3], up[3], fwd[3], eye[3];
            if (eye_pos(eye)) {
                // the wave starts 2 u ahead of the eye
                view_axes(right, up, fwd);
                BlastFx &b = g_blast_fx[g_blast_fx_next++ % MAX_BLAST_FX];
                for (int i = 0; i < 3; i++) b.p[i] = eye[i] + fwd[i] * 2.0f * UK_UNIT;
                b.radius = 3.0f;                 // the blast row's radius (tools/patch_v1.py)
                b.born = t;
                b.wave = true;
            }
            logf("knuckleblaster: blast wave\n");
        }
    }
    if ((!edge && !edge2) || g_fist_cooldown > 0 || g_punch_stamina < 1.0f) return;
    bool heavy = edge2 && !edge;
    g_last_arm = heavy ? 1 : 0;
    g_fist_cooldown = heavy ? 0.75f : 0.5f;
    g_punch_stamina = g_punch_stamina - (heavy ? 1.5f : 1.0f) < 0 ? 0 : g_punch_stamina - (heavy ? 1.5f : 1.0f);
    InterlockedIncrement(&g_punches);
    note_attack(HIT_PUNCH);
    Target tg;
    bool in_reach = punch_target(tg);
    int anim = -1;
    bool attacking = in_reach && enemy_attacking(tg.chr, anim);
    if (heavy) {
        sound_play("knuckle_swing", 0.5f, 1.0f);
        play_arm2("Punch");
        g_blast_check = t + BLAST_CHECK_AT;
        if (!shoot(BEHAVIOR_KNUCKLE)) return;
        if (in_reach) {
            // (v0.60 played "Hook" here. The Knuckleblaster's controller lists a Hook clip, but it is
            // written for another arm's bones: the arm stood still in its rest pose whenever a punch landed.)
            sound_play("knuckle_hit", 0.8f, 1.0f);
            InterlockedIncrement(&g_punch_hits);
            logf("knuckleblaster: hit chr %p hp %d at %.1f m (its animation %d)\n", (void *)tg.chr, tg.hp, tg.dist, anim);
        }
        return;
    }
    sound_play("punch_swing", 0.5f, 1.0f);
    play_arm(rand() % 2 ? "Jab" : "Jab2");
    g_last_feedbacker = t;
    if (g_weapon == WEAPON_SHOTGUN && t - g_last_shotgun_shot <= BOOST_WINDOW) boost_pellet(t);
    if (punch_coin(t)) play_arm("Hook", 0.065);
    if (!shoot(attacking ? BEHAVIOR_PARRY : BEHAVIOR_PUNCH)) return;
    if (!in_reach) return;                       // a punch or parry on an enemy keeps the jab; the hook is the projectile boost's and the coin's
    InterlockedIncrement(&g_punch_hits);
    if (attacking) {
        do_parry(tg, anim);
    } else {
        sound_play("punch_hit", 0.6f, 1.0f);
        logf("punch: hit chr %p hp %d at %.1f m (its animation %d)\n", (void *)tg.chr, tg.hp, tg.dist, anim);
    }
}

// A tracer for a shot fired from the gun: from about where the barrel is on screen to `end`, or, with no
// end given, to where the view line first meets the world (150 m if it meets nothing).
static void shot_tracer(const float *end, float life, float width, const float *rgb) {
    g_muzzle_flash = now_s();
    float right[3], up[3], fwd[3], eye[3], muzzle[3], far_end[3];
    if (!eye_pos(eye)) return;
    view_axes(right, up, fwd);
    // the muzzle is wherever the HUD last drew it: the point 0.9 m out that lands on that spot of the screen
    float mx = 0.26f, my = -0.32f;
    hud_muzzle(&mx, &my);
    float tan_y = tanf(BASE_FOV * g_fov_scale * 0.5f), tan_x = tan_y * g_aspect;
    for (int i = 0; i < 3; i++) {
        muzzle[i] = eye[i] + (right[i] * mx * tan_x + up[i] * my * tan_y + fwd[i]) * 0.9f;
        far_end[i] = eye[i] + fwd[i] * 150.0f;
    }
    if (end) {
        add_tracer(muzzle, end, life, width, rgb);
        return;
    }
    RayHit h;
    ray_cast(eye, far_end, h);
    add_tracer(muzzle, h.point, life, width, rgb);
}

// once per frame, on the game's thread
static void try_instant_fire() {
    static double last = 0;
    double t = now_s();
    float dt = last > 0 && t - last > 0 && t - last < 0.1 ? (float)(t - last) : 0.0f;   // the clock steps back when a freeze begins
    last = t;
    update_coins(t, dt);
    if (!g_instant_fire || !g_ctrl) {
        if (g_pierce_ready < 100.0f) g_pierce_ready = 100.0f;   // nothing refills, or ticks, outside first person
        g_pierce_charge = 0;
        g_prev_rmb = false;
        g_twirl_charge = 0;
        g_twirling = false;
        g_core_force = 0;
        g_core_charging = false;
    }
    bool armed = g_instant_fire && g_ctrl;
    {
        bool moving = key_down('W') || key_down('A') || key_down('S') || key_down('D');
        update_bob(dt, g_ctrl && !g_air && !g_sliding && moving, sqrtf(g_vx * g_vx + g_vz * g_vz) / UK_UNIT);
        update_sway(dt);
    }
    if (g_ctrl) watch_enemies(t, dt);
    style_update(dt);
    update_punch(armed, dt);
    update_pellets(t, dt);
    update_cores(t, dt);
    update_ricochets(t);
    timed_sounds_update(t);
    bool lmb = armed && key_down(VK_LBUTTON) != 0, rmb = armed && key_down(VK_RBUTTON) != 0;
    // Weapon slots as in ULTRAKILL: 1 is the revolver, 2 the shotgun. The revolver always comes out as
    // the Marksman (the variation last held is not remembered, Davi's choice); 1 again, or E, goes on
    // through Piercer and Sharpshooter and back round. Q goes straight to the last of the three. The
    // shotgun has only the Core Eject so far.
    {
        static bool prev1 = false, prev2 = false, prev_e = false, prev_q = false;
        static const int next_variation[3] = {2, 0, 1};      // Piercer -> Sharpshooter, Marksman -> Piercer, Sharpshooter -> Marksman
        bool k1 = armed && key_down('1'), k2 = armed && key_down('2'), ke = armed && key_down('E'), kq = armed && key_down('Q');
        int weapon = g_weapon, variation = g_variation;
        if (k1 && !prev1) {
            if (weapon == WEAPON_REVOLVER) variation = next_variation[variation];
            else { weapon = WEAPON_REVOLVER; variation = 1; }
        } else if (ke && !prev_e && weapon == WEAPON_REVOLVER) {
            variation = next_variation[variation];
        }
        if (kq && !prev_q) { weapon = WEAPON_REVOLVER; variation = 2; }
        if (k2 && !prev2) weapon = WEAPON_SHOTGUN;
        prev1 = k1;
        prev2 = k2;
        prev_e = ke;
        prev_q = kq;
        if (weapon != g_weapon || variation != g_variation) {
            g_weapon = weapon;
            g_variation = variation;
            g_pierce_charge = 0;
            g_twirl_charge = 0;
            g_twirl_angle = 0;
            g_twirling = g_twirl_recovery = false;
            g_core_force = 0;
            g_core_charging = false;
            timed_sounds_clear();
            sound_play("weapon_draw", 0.35f, 3.0f);
            // Drawing a weapon starts its animation over, and it is ready when the draw's ReadyGun event
            // comes: for the shotgun that cuts a reload short, as swapping weapons does in ULTRAKILL.
            if (weapon == WEAPON_SHOTGUN) {
                play_revolver("Equip");
                g_shotgun_ready_at = g_cores_ready_at = t + SHOTGUN_EQUIP_READY;
            } else {
                play_revolver("PickUp");
                if (g_next_shot < t + REVOLVER_PICKUP_READY) g_next_shot = t + REVOLVER_PICKUP_READY;
            }
            static const char *const names[3] = {"Piercer", "Marksman", "Sharpshooter"};
            Target all[16];
            float eye[3];
            int n = eye_pos(eye) ? find_targets(eye, all, 16, 500.0f, true) : 0;
            logf("weapon: %s; %d other characters active:\n", weapon == WEAPON_SHOTGUN ? "shotgun (Core Eject)" : names[variation], n);
            for (int i = 0; i < n; i++) {
                // how far the view would have to turn to face it (positive = to the right), for driving the game from a script
                float turn = atan2f(all[i].p[0] - eye[0], all[i].p[2] - eye[2]) - g_yaw;
                turn = turn - 6.2831853f * floorf((turn + 3.14159265f) / 6.2831853f);
                int anim = -1;
                bool attacking = enemy_attacking(all[i].chr, anim);
                logf("  chr %p team %d hp %d, %.1f m away, turn %+.2f rad, %.1f m %s, %s, animation %d%s%s\n", (void *)all[i].chr, all[i].team, all[i].hp,
                     all[i].dist, turn, fabsf(all[i].p[1] - eye[1]), all[i].p[1] > eye[1] ? "above" : "below",
                     ray_blocked(eye, all[i].p) == 1 ? "hidden" : "in sight", anim, attacking ? " (attacking)" : "", all[i].present ? "" : " (not in the world)");
            }
        }
    }
    {
        // Test aid, for driving the game from a script (numpad 5): turn the view to the nearest enemy in
        // plain sight; pressed again within 3 s, to the next nearest.
        static bool prev = false;
        static int which = 0;
        static double last_press = -100.0;
        bool k = g_ctrl && key_down(VK_NUMPAD5);
        if (k && !prev) {
            which = t - last_press < 3.0 ? which + 1 : 0;
            last_press = t;
            float eye[3];
            Target cand[16];
            int n = eye_pos(eye) ? find_targets(eye, cand, 16, 150.0f) : 0, seen = 0;
            bool found = false;
            for (int i = 0; i < n && !found; i++) {
                if (ray_blocked(eye, cand[i].p) == 1 || seen++ != which) continue;
                float dx = cand[i].p[0] - eye[0], dy = cand[i].p[1] - eye[1], dz = cand[i].p[2] - eye[2];
                g_yaw = atan2f(dx, dz);
                g_pitch = atan2f(dy, sqrtf(dx * dx + dz * dz));
                int anim = -1;
                enemy_attacking(cand[i].chr, anim);
                logf("test aid: facing chr %p hp %d, %.1f m away, animation %d\n", (void *)cand[i].chr, cand[i].hp, cand[i].dist, anim);
                found = true;
            }
            if (!found) logf("test aid: no %senemy in sight (%d in range)\n", which ? "further " : "", n);
        }
        prev = k;
        // Numpad 4: put the player on the ground 1.6 m from the nearest living enemy, facing it (again
        // within 3 s: the next nearest). The spot is the first of eight around the enemy that it has a
        // clear line to and that has ground under it.
        static bool prev4 = false;
        static int which4 = 0;
        static double last4 = -100.0;
        bool k4 = g_ctrl && key_down(VK_NUMPAD4);
        uintptr_t pos = g_player_pos;
        if (k4 && !prev4 && pos) {
            which4 = t - last4 < 3.0 ? which4 + 1 : 0;
            last4 = t;
            const float *feet = (const float *)(pos + OFF_POS_X);
            float eye[3];
            Target cand[16];
            int n = eye_pos(eye) ? find_targets(eye, cand, 16, 300.0f) : 0;
            bool done = false;
            if (which4 < n) {
                const Target &e = cand[which4];
                float to_me = atan2f(feet[0] - e.p[0], feet[2] - e.p[2]);
                for (int a = 0; a < 8 && !done; a++) {
                    float ang = to_me + a * 0.7853982f;
                    float spot[3] = {e.p[0] + sinf(ang) * 1.6f, e.p[1], e.p[2] + cosf(ang) * 1.6f}, under[3] = {spot[0], spot[1] - 3.0f, spot[2]};
                    RayHit h;
                    if (ray_blocked(e.p, spot) == 1 || !ray_cast(spot, under, h) || !h.hit) continue;
                    g_teleport_delta[0] = spot[0] - feet[0];
                    g_teleport_delta[1] = h.point[1] + 0.1f - feet[1];
                    g_teleport_delta[2] = spot[2] - feet[2];
                    g_teleport_pending = true;
                    g_yaw = atan2f(e.p[0] - spot[0], e.p[2] - spot[2]);
                    g_pitch = 0;
                    logf("test aid: moved next to chr %p hp %d (it was %.1f m away), ground %.1f m under its aim point\n", (void *)e.chr, e.hp, e.dist, h.dist);
                    done = true;
                }
            }
            if (!done) logf("test aid: no place to stand next to enemy %d of %d\n", which4, n);
        }
        prev4 = k4;
    }
    if (g_pierce_ready < 100.0f) g_pierce_ready = g_pierce_ready + PIERCE_RECHARGE_RATE * dt > 100.0f ? 100.0f : g_pierce_ready + PIERCE_RECHARGE_RATE * dt;
    if (g_sharp_charge < 300.0f) g_sharp_charge = g_sharp_charge + SHARP_REFILL * dt > 300.0f ? 300.0f : g_sharp_charge + SHARP_REFILL * dt;
    bool alt_fired = false;
    if (g_weapon != WEAPON_REVOLVER) {
        g_pierce_charge = 0;
        // Shotgun.Update: the alt fire winds the core up while held and throws it when let go
        if (rmb && t - g_last_core > 0.5 && t >= g_cores_ready_at) {
            g_core_charging = true;
            g_core_force = g_core_force + 60.0f * dt > 60.0f ? 60.0f : g_core_force + 60.0f * dt;
        } else if (g_core_charging) {
            float force = g_core_force;
            g_core_charging = false;
            g_core_force = 0;
            throw_core(force);
            InterlockedIncrement(&g_cores_thrown);
            // ShootSinks: the launcher's own sound fades as the charge grows and the shot sound takes over
            sound_play("shotgun_core", 0.45f * sqrtf(1.0f - force * force / 3600.0f), 1.0f);
            sound_play("shotgun_shot", 0.45f * force / 60.0f, frand(0.75f, 0.85f), false, CH_GUN);
            play_revolver("FireWithThrowReload");
            timed_sounds_clear();
            sound_later(CORE_SMACK_1, "shotgun_smack", 0.75f, frand(2.0f, 2.2f));
            sound_later(CORE_SMACK_2, "shotgun_smack", 0.75f, frand(2.0f, 2.2f));
            sound_later(CORE_CLICK, "shotgun_click", 0.5f, frand(0.95f, 1.05f));
            g_last_core = t;
            g_shotgun_ready_at = g_cores_ready_at = t + CORE_READY;
            logf("shotgun: core thrown with force %.0f\n", force);
        }
        if (lmb && t >= g_shotgun_ready_at) fire_shotgun(t);
    } else if (g_variation == 1) {
        if (rmb && !g_prev_rmb && g_coin_charge >= 100.0f) {
            g_coin_charge -= 100.0f;
            throw_coin();
        }
        g_pierce_charge = 0;
    } else if (g_variation == 2) {
        g_pierce_charge = 0;
        bool let_go = (g_prev_rmb && !rmb) || lmb;
        if (let_go && g_twirl_charge >= 25.0f && t >= g_next_shot) {
            int bounces = (int)(g_twirl_charge / 25.0f);
            if (bounces > 3) bounces = 3;
            g_sharp_charge = g_sharp_charge - 100.0f;
            g_twirl_charge = 0;
            if (g_twirling) g_twirl_recovery = true;
            g_twirling = false;
            alt_fired = true;
            InterlockedIncrement(&g_sharp_shots);
            sound_play("shot_sharpshooter", 0.55f, frand(0.9f, 1.1f), false, CH_GUN);
            sound_play("twirl_shot", 0.75f, 1.0f);
            note_attack(HIT_REVOLVER);
            play_revolver("Shoot");
            g_last_pierce_time = t;
            g_next_shot = t + FIRE_INTERVAL;
            if (Coin *c = coin_on_line()) {
                shot_tracer(c->p, 0.5f, 0.10f, TRACER_RED);
                hit_coin(*c, t);
                c->charged = true;
            } else if (Core *k = core_on_line()) {
                shot_tracer(k->p, 0.5f, 0.10f, TRACER_RED);
                explode(k->p, true);
                k->alive = false;
            } else {
                float right[3], up[3], fwd[3], eye[3];
                if (eye_pos(eye)) {
                    view_axes(right, up, fwd);
                    sharp_leg(eye, fwd, bounces, true);
                }
            }
        } else if (rmb && g_sharp_charge >= 100.0f) {
            g_twirling = true;
            g_twirl_recovery = false;
            g_twirl_charge = g_twirl_charge + TWIRL_RATE * dt > 100.0f ? 100.0f : g_twirl_charge + TWIRL_RATE * dt;
        } else {
            if (g_twirling) g_twirl_recovery = true;
            g_twirling = false;
            g_twirl_charge = g_twirl_charge - TWIRL_RATE * dt < 0 ? 0 : g_twirl_charge - TWIRL_RATE * dt;
        }
    } else if (rmb && g_pierce_ready >= 100.0f) {
        g_pierce_charge = g_pierce_charge + PIERCE_CHARGE_RATE * dt > 100.0f ? 100.0f : g_pierce_charge + PIERCE_CHARGE_RATE * dt;
    } else if (g_prev_rmb && g_pierce_charge >= 100.0f) {
        if (Coin *c = coin_on_line()) {
            // the charged shot stops at a coin too, and the coin passes it on
            sound_play("shot_super", 0.5f, 1.0f);
            note_attack(HIT_REVOLVER);
            play_revolver("Shoot");
            shot_tracer(c->p, 0.4f, 0.10f, TRACER_BLUE);
            hit_coin(*c, t);
            c->charged = true;
            g_last_pierce_time = t;
            g_pierce_ready = 0;
            g_next_shot = t + FIRE_INTERVAL;
        } else if (Core *k = core_on_line()) {
            sound_play("shot_super", 0.5f, 1.0f);
            play_revolver("Shoot");
            shot_tracer(k->p, 0.4f, 0.10f, TRACER_BLUE);
            explode(k->p, true);
            k->alive = false;
            g_last_pierce_time = t;
            g_pierce_ready = 0;
            g_next_shot = t + FIRE_INTERVAL;
        } else if (shoot(BEHAVIOR_PIERCER)) {
            shot_tracer(nullptr, 0.4f, 0.10f, TRACER_BLUE);
            InterlockedIncrement(&g_pierce_shots);
            sound_play("shot_super", 0.5f, 1.0f);
            note_attack(HIT_REVOLVER);
            play_revolver("Shoot");
            g_last_pierce_time = t;
            g_pierce_ready = 0;
            g_next_shot = t + FIRE_INTERVAL;
        }
        g_pierce_charge = 0;
    } else {
        g_pierce_charge = 0;   // released early: the charge is lost
    }
    g_prev_rmb = rmb;
    // The Sharpshooter's spin (Revolver.Update): 1200 degrees a second times (level / 3 + 0.5), where the
    // level is 1 to 4 with the charge; let go, the level drops to 0.1 and the gun turns on until it is
    // upright again.
    if (g_twirling || g_twirl_recovery) {
        float before = g_twirl_angle;
        if (g_twirling) g_twirl_level = fminf(3.0f, floorf(g_twirl_charge / 25.0f)) + 1.0f;
        else g_twirl_level = move_towards(g_twirl_level, 0.1f, dt * 100.0f * g_twirl_level);
        float a = before + 1200.0f * (g_twirl_level / 3.0f + 0.5f) * dt;
        if (g_twirl_recovery && before < 0 && a >= 0) {
            a = 0;
            g_twirl_recovery = false;
        } else {
            while (a > 180.0f) a -= 360.0f;
        }
        g_twirl_angle = a;
    } else {
        g_twirl_angle = 0;
    }
    g_twirl_blend = move_towards(g_twirl_blend, g_twirling ? fminf(1.0f, g_twirl_level / 3.0f) : 0.0f, dt * (g_twirling ? 3.0f : 6.0f));
    {
        // The charge's rising whine (pitch and volume follow the charge), the ticking while the
        // Piercer's alt fire refills, and the beep when it is ready again.
        static int charge_voice = 0, refill_voice = 0, twirl_voice = 0, core_voice = 0;
        static bool was_refilling = false;
        float charge = g_pierce_charge;
        if (charge > 0) {
            if (!charge_voice) charge_voice = sound_play("pierce_charge", 0.25f, 0.01f, true);
            sound_set(charge_voice, 0.25f + charge * 0.005f, charge * 0.005f);
        } else if (charge_voice) {
            sound_stop(charge_voice);
            charge_voice = 0;
        }
        bool refilling = g_weapon == WEAPON_REVOLVER && g_variation == 0 && g_pierce_ready < 100.0f;
        if (refilling && !refill_voice) refill_voice = sound_play("pierce_recharging", 0.25f, 1.0f, true, CH_SCREEN);
        if (!refilling && refill_voice) {
            sound_stop(refill_voice);
            refill_voice = 0;
        }
        if (armed && was_refilling && g_pierce_ready >= 100.0f) sound_play("pierce_ready", 0.35f, frand(1.0f, 1.1f), false, CH_SCREEN);
        was_refilling = g_pierce_ready < 100.0f;
        // the Sharpshooter's spin: the same charge-effect source, pitched 0.5 + level / 2
        if (g_twirling) {
            if (!twirl_voice) twirl_voice = sound_play("twirl_loop", 0.25f, 1.0f, true);
            sound_set(twirl_voice, 0.25f + g_twirl_charge * 0.005f, 0.5f + g_twirl_level / 2.0f);
        } else if (twirl_voice) {
            sound_stop(twirl_voice);
            twirl_voice = 0;
        }
        // the core winding up: its pitch is the charge
        if (g_core_charging) {
            if (!core_voice) core_voice = sound_play("shotgun_charge", 0.5f, 0.05f, true);
            sound_set(core_voice, 0.5f, g_core_force / 60.0f < 0.05f ? 0.05f : g_core_force / 60.0f);
        } else if (core_voice) {
            sound_stop(core_voice);
            core_voice = 0;
        }
    }

    if (g_weapon == WEAPON_REVOLVER && !alt_fired && lmb && g_pierce_charge <= 0 && !g_twirling && t >= g_next_shot) {
        g_next_shot = t + FIRE_INTERVAL;
        const char *shot_sound = g_variation == 1 ? "shot_marksman" : g_variation == 2 ? "shot_sharpshooter" : "shot_piercer";
        if (Coin *c = coin_on_line()) {
            sound_play(shot_sound, 0.55f, frand(0.9f, 1.1f), false, CH_GUN);
            play_revolver_shot();
            shot_tracer(c->p, 0.2f, 0.04f, TRACER_WHITE);
            hit_coin(*c, t);                     // the shot stops at the coin
            g_last_shot_time = t;
        } else if (Core *k = core_on_line()) {
            // a core shot in the air goes off as the larger explosion
            sound_play(shot_sound, 0.55f, frand(0.9f, 1.1f), false, CH_GUN);
            play_revolver_shot();
            shot_tracer(k->p, 0.2f, 0.04f, TRACER_WHITE);
            explode(k->p, true);
            k->alive = false;
            g_last_shot_time = t;
        } else if (shoot(BEHAVIOR_REVOLVER)) {
            sound_play(shot_sound, 0.55f, frand(0.9f, 1.1f), false, CH_GUN);
            note_attack(HIT_REVOLVER);
            play_revolver_shot();
            shot_tracer(nullptr, 0.2f, 0.04f, TRACER_WHITE);
            InterlockedIncrement(&g_instant_shots);
            g_last_shot_time = t;
        }
    }
}

// ---- HUD, drawn into the game's back buffer just before it is presented. Only solid rectangles,
// through ID3D11DeviceContext1::ClearView, so no shaders or render state are touched.
typedef HRESULT(WINAPI *PresentFn)(IDXGISwapChain *, UINT, UINT);
static PresentFn g_orig_present = nullptr;
static volatile LONG g_presents = 0, g_hud_draws = 0;

static void fill(ID3D11DeviceContext1 *ctx, ID3D11RenderTargetView *rtv, float r, float g, float b, const D3D11_RECT *rects, UINT n) {
    const float color[4] = {r, g, b, 1.0f};
    ctx->ClearView(rtv, color, rects, n);
}

static void draw_hud(IDXGISwapChain *sc) {
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11DeviceContext1 *ctx1 = nullptr;
    ID3D11Texture2D *back = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev)) && dev) {
        dev->GetImmediateContext(&ctx);
        if (ctx) ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1);
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
        if (ctx1 && back && SUCCEEDED(dev->CreateRenderTargetView(back, nullptr, &rtv)) && rtv) {
            D3D11_TEXTURE2D_DESC d;
            back->GetDesc(&d);
            LONG w = (LONG)d.Width, h = (LONG)d.Height, cx = w / 2, cy = h / 2;
            LONG u = h / 540 > 0 ? h / 540 : 1;   // one HUD unit: 2 px at 1080p
            // crosshair: four ticks and a centre dot, dark outline under a white core
            D3D11_RECT outline[] = {{cx - u, cy - 6 * u, cx + u, cy - 2 * u}, {cx - u, cy + 2 * u, cx + u, cy + 6 * u},
                                    {cx - 6 * u, cy - u, cx - 2 * u, cy + u}, {cx + 2 * u, cy - u, cx + 6 * u, cy + u},
                                    {cx - u, cy - u, cx + u, cy + u}};
            fill(ctx1, rtv, 0.05f, 0.05f, 0.05f, outline, 5);
            LONG v = u > 1 ? u / 2 : 1;
            D3D11_RECT core[] = {{cx - v, cy - 6 * u + v, cx + v, cy - 2 * u - v}, {cx - v, cy + 2 * u + v, cx + v, cy + 6 * u - v},
                                 {cx - 6 * u + v, cy - v, cx - 2 * u - v, cy + v}, {cx + 2 * u + v, cy - v, cx + 6 * u - v, cy + v},
                                 {cx - v, cy - v, cx + v, cy + v}};
            fill(ctx1, rtv, 1.0f, 1.0f, 1.0f, core, 5);
            // Piercer: a bar under the crosshair. It fills while charging; while the shot recharges,
            // a dim bar shows the progress.
            float charge = g_pierce_charge, ready = g_pierce_ready;
            LONG half = 12 * u, by = cy + 10 * u;
            if (charge > 0 || ready < 100.0f) {
                D3D11_RECT track = {cx - half, by, cx + half, by + 2 * u};
                fill(ctx1, rtv, 0.05f, 0.05f, 0.05f, &track, 1);
                float part = (charge > 0 ? charge : ready) / 100.0f;
                D3D11_RECT bar = {cx - half, by, cx - half + (LONG)(2 * half * part), by + 2 * u};
                if (charge >= 100.0f) fill(ctx1, rtv, 1.0f, 1.0f, 1.0f, &bar, 1);
                else if (charge > 0) fill(ctx1, rtv, 0.3f, 0.7f, 1.0f, &bar, 1);
                else fill(ctx1, rtv, 0.25f, 0.28f, 0.33f, &bar, 1);
            }
            // stamina: three bars, bottom left, filled in proportion
            LONG bw = 40 * u, bh = 5 * u, gap = 3 * u, x0 = 30 * u, y0 = h - 40 * u;
            D3D11_RECT bg[3], fg[3];
            UINT nfg = 0;
            float boost = g_boost;
            for (int i = 0; i < 3; i++) {
                LONG x = x0 + i * (bw + gap);
                bg[i] = {x, y0, x + bw, y0 + bh};
                float part = boost - 100.0f * i;
                part = part < 0 ? 0 : part > 100.0f ? 100.0f : part;
                if (part > 0) fg[nfg++] = {x, y0, x + (LONG)(bw * part / 100.0f), y0 + bh};
            }
            fill(ctx1, rtv, 0.08f, 0.08f, 0.10f, bg, 3);
            if (nfg) fill(ctx1, rtv, 0.25f, 0.85f, 1.0f, fg, nfg);
            InterlockedIncrement(&g_hud_draws);
        }
    }
    if (rtv) rtv->Release();
    if (back) back->Release();
    if (ctx1) ctx1->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
}

// The full HUD and viewmodel (hud.cpp), drawn from art in ultrasouls_assets.bin next to the exe.
// Returns false if that cannot be used, and the plain rectangles above are drawn instead.
static volatile float g_hud_health = 100.0f;      // the player's health as a percentage, for the HUD
static volatile int g_hud_status = 0;             // 0 not tried, 1 working, 2 unavailable (see hud_error)
static volatile bool g_viewmodel = true;
static volatile bool g_hide_game_hud = true;      // Delete: hide Dark Souls' own HUD while in first person
static const uintptr_t RVA_GAME_DATA_MAN = 0x1C8A530;   // GameDataMan*; the accessor at exe+755D40 returns [it + 0x58]
static const uintptr_t OFF_GDM_OPTIONS = 0x58;          // -> PcOptionData
static const uintptr_t OFF_OPT_HUD = 0x11;              // the options menu's "HUD" entry: 1 shown, 0 hidden

static bool draw_full_hud(IDXGISwapChain *sc) {
    if (g_hud_status == 2) return false;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11Texture2D *back = nullptr;
    bool ok = false;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev)) && dev) {
        dev->GetImmediateContext(&ctx);
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
        if (ctx && back) {
            static wchar_t pack[MAX_PATH];
            if (!pack[0]) {
                GetModuleFileNameW(nullptr, pack, MAX_PATH);
                wchar_t *slash = wcsrchr(pack, L'\\');
                wcscpy(slash ? slash + 1 : pack, L"ultrasouls_assets.bin");
            }
            ok = hud_init(dev, pack);
            g_hud_status = ok ? 1 : 2;
            if (ok) {
                HudState st;
                st.health = g_hud_health;
                st.stamina = g_boost;
                st.pierce_charge = g_pierce_charge;
                st.pierce_ready = g_pierce_ready;
                st.time = now_s();
                st.last_shot = g_last_shot_time;
                st.last_pierce = g_last_pierce_time;
                st.show_viewmodel = g_viewmodel;
                st.variation = g_variation;
                st.weapon = g_weapon;
                st.twirl = g_twirl_angle * 3.14159265f / 180.0f;
                st.twirl_blend = g_twirl_blend;
                st.arm = g_last_arm;
                st.twirl_charge = g_twirl_charge;
                st.sharp_charge = g_sharp_charge;
                st.core_charge = g_core_force / 60.0f;
                st.muzzle_flash = g_muzzle_flash;
                for (int i = 0; i < 3; i++) {
                    st.hud_sway[i] = g_hud_sway[i];
                    st.weapon_sway[i] = -g_hud_cam[i];
                }
                // Shotgun.UpdateMeter: full while a core can be thrown, the charge (turning red) while
                // it is wound up, empty from the throw until the gun is ready again
                st.core_meter = g_core_charging ? g_core_force / 60.0f : now_s() >= g_cores_ready_at ? 1.0f : 0.0f;
                st.core_meter_red = g_core_charging ? g_core_force / 60.0f : 0.0f;
                st.boss_count = g_ctrl ? (int)g_boss_count : 0;
                for (int i = 0; i < st.boss_count && i < HudState::MAX_BOSSES; i++) st.bosses[i] = g_bosses[i];
                st.coin_charge = g_coin_charge;
                st.punch_stamina = g_punch_stamina;
                st.bob_x = g_bob_x;
                st.bob_y = g_bob_y;
                st.style_rank = g_style_combo ? (int)g_style_rank : -1;
                st.style_meter = g_style_meter / STYLE_RANKS[g_style_rank].max_meter;
                st.style_line_count = 0;
                for (int i = 0; i < g_style_log_n && i < HudState::STYLE_LINES; i++) {
                    HudState::StyleLine &ln = st.style_lines[st.style_line_count++];
                    strncpy(ln.text, g_style_log[i].text, sizeof(ln.text) - 1);
                    ln.text[sizeof(ln.text) - 1] = 0;
                    ln.r = g_style_log[i].r; ln.g = g_style_log[i].g; ln.b = g_style_log[i].b;
                }
                st.revolver_clip = g_rev_clip;
                st.revolver_clip_start = g_rev_clip_start;
                st.arm_clip = g_arm_clip;
                st.arm_clip_start = g_arm_clip_start;
                st.arm2_clip = g_arm2_clip;
                st.arm2_clip_start = g_arm2_clip_start;
                {
                    double left = g_flash_until - now_s();
                    st.flash = left > 0 ? (float)(left / 0.1) : 0.0f;
                }
                D3D11_TEXTURE2D_DESC bd;
                back->GetDesc(&bd);
                float right[3], up[3], fwd[3], eye[3];
                if (eye_pos(eye) && bd.Height) {
                    view_axes(right, up, fwd);
                    float tan_y = tanf(BASE_FOV * g_fov_scale * 0.5f), tan_x = tan_y * (float)bd.Width / (float)bd.Height;
                    g_aspect = (float)bd.Width / (float)bd.Height;
                    // a line between two points of the world, as the HUD wants it: projected, cut at the eye
                    auto line = [&](const float *pa, const float *pb, float width, float r, float g, float b, float alpha) {
                        if (st.tracer_count >= HudState::MAX_TRACERS) return;
                        float v[2][3];
                        for (int e = 0; e < 2; e++) {
                            const float *p = e ? pb : pa;
                            float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
                            v[e][0] = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
                            v[e][1] = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
                            v[e][2] = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
                        }
                        const float NEAR_Z = 0.1f;
                        if (v[0][2] < NEAR_Z && v[1][2] < NEAR_Z) return;
                        for (int e = 0; e < 2; e++) {                 // an end behind the eye is cut back to just in front of it
                            if (v[e][2] >= NEAR_Z) continue;
                            float k = (NEAR_Z - v[e][2]) / (v[1 - e][2] - v[e][2]);
                            for (int i = 0; i < 3; i++) v[e][i] += (v[1 - e][i] - v[e][i]) * k;
                        }
                        HudState::TracerLine &ln = st.tracers[st.tracer_count++];
                        ln.x0 = v[0][0] / (v[0][2] * tan_x); ln.y0 = v[0][1] / (v[0][2] * tan_y);
                        ln.x1 = v[1][0] / (v[1][2] * tan_x); ln.y1 = v[1][1] / (v[1][2] * tan_y);
                        float w0 = width * 0.5f / (v[0][2] * tan_y * 2.0f), w1 = width * 0.5f / (v[1][2] * tan_y * 2.0f);
                        ln.w0 = w0 < 0.0008f ? 0.0008f : w0 > 0.02f ? 0.02f : w0;
                        ln.w1 = w1 < 0.0008f ? 0.0008f : w1 > 0.02f ? 0.02f : w1;
                        ln.r = r; ln.g = g; ln.b = b;
                        ln.a = alpha;
                    };
                    // a glowing ball in the world: `size` metres across, with a shock ring `ring` metres across
                    auto ball = [&](const float *p, float size, float ring, float r, float g, float b, float alpha) {
                        if (st.blast_count >= HudState::MAX_BLASTS) return;
                        float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
                        float vx = d[0] * right[0] + d[1] * right[1] + d[2] * right[2], vy = d[0] * up[0] + d[1] * up[1] + d[2] * up[2],
                              vz = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
                        if (vz < 0.3f) vz = 0.3f;                      // one that is on top of the player still fills the screen
                        HudState::Blast &o = st.blasts[st.blast_count++];
                        o.x = vx / (vz * tan_x);
                        o.y = vy / (vz * tan_y);
                        o.size = size / (vz * tan_y * 2.0f);
                        o.ring = ring / (vz * tan_y * 2.0f);
                        o.r = r; o.g = g; o.b = b; o.a = alpha;
                    };
                    for (const Tracer &tr : g_tracers) {
                        float age = (float)(st.time - tr.born);
                        if (tr.life <= 0 || age < 0 || age > tr.life) continue;
                        line(tr.a, tr.b, tr.width, tr.r, tr.g, tr.bl, 1.0f - age / tr.life);
                    }
                    // pellets in flight: a short streak behind each (the trail lasts a tenth of a second
                    // in ULTRAKILL; here it is as long as the pellet travels in 0.05 s)
                    for (const Pellet &pl : g_pellets) {
                        if (!pl.alive) continue;
                        float tail[3] = {pl.p[0] - pl.v[0] * 0.05f, pl.p[1] - pl.v[1] * 0.05f, pl.p[2] - pl.v[2] * 0.05f};
                        if (pl.boosted) {
                            line(tail, pl.p, 0.16f, 1.0f, 0.35f, 0.0f, 1.0f);
                            ball(pl.p, 0.5f, 0, 1.0f, 0.35f, 0.0f, 1.0f);
                        } else {
                            line(tail, pl.p, 0.05f, 1.0f, 0.85f, 0.3f, 0.9f);
                        }
                    }
                    for (const Core &c : g_cores)
                        if (c.alive) ball(c.p, 0.3f, 0, 1.0f, 0.3f, 0.1f, 1.0f);
                    // explosions: the ball reaches full size in 0.2 s, then fades over 0.3 s as the ring runs on
                    for (const BlastFx &bf : g_blast_fx) {
                        float age = (float)(st.time - bf.born);
                        if (bf.radius <= 0 || age < 0 || age > 0.5f) continue;
                        float grow = age < 0.2f ? age / 0.2f : 1.0f, fade = age < 0.2f ? 1.0f : 1.0f - (age - 0.2f) / 0.3f;
                        if (bf.wave) ball(bf.p, bf.radius * 0.5f * grow, bf.radius * 2.0f * (0.2f + age * 2.0f), 0.85f, 0.92f, 1.0f, fade * 0.7f);
                        else ball(bf.p, bf.radius * 2.0f * (0.3f + 0.7f * grow), bf.radius * 2.0f * (0.4f + age * 2.4f), 1.0f, 0.55f, 0.1f, fade);
                    }
                    for (const Coin &c : g_coins) {
                        if (!c.alive || st.coin_count >= HudState::MAX_COINS) continue;
                        float d[3] = {c.p[0] - eye[0], c.p[1] - eye[1], c.p[2] - eye[2]};
                        float vx = d[0] * right[0] + d[1] * right[1] + d[2] * right[2], vy = d[0] * up[0] + d[1] * up[1] + d[2] * up[2],
                              vz = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
                        if (vz < 0.2f) continue;
                        float age = (float)(st.time - c.born);
                        HudState::CoinDot &dot = st.coins[st.coin_count++];
                        dot.x = vx / (vz * tan_x);
                        dot.y = vy / (vz * tan_y);
                        dot.size = 0.3f / (vz * tan_y * 2.0f);          // a coin drawn 0.3 m across
                        dot.phase = age * 25.0f;
                        dot.flash = c.shot || (age >= 0.35f && age < 0.417f);
                    }
                }
                hud_draw(dev, ctx, back, st);
            }
        }
    }
    if (back) back->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    return ok;
}

// TimeController.ParryFlash: the screen goes white for 0.1 s and time stands still for 0.25 s. Here the
// frame of the parry is shown under a white sheet for 0.1 s, then as it was for the rest, and the thread
// that presents frames sleeps through both; the mod's clock leaves the time out.
static volatile LONG g_freezes = 0;
static HRESULT WINAPI my_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    InterlockedIncrement(&g_presents);
    float freeze = g_freeze_request;
    if (freeze > 0 && !g_parry_freeze) {
        g_freeze_request = freeze = 0;
        g_flash_until = now_s() + 0.1;
    }
    bool full = g_ctrl && draw_full_hud(sc);
    if (g_ctrl && !full) draw_hud(sc);
    if (!(freeze > 0)) return g_orig_present(sc, sync, flags);
    g_freeze_request = 0;
    if (!full) return g_orig_present(sc, sync, flags);
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11Texture2D *back = nullptr;
    bool kept = false;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev)) && dev) {
        dev->GetImmediateContext(&ctx);
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
        if (ctx && back) {
            kept = hud_keep_frame(dev, ctx, back);
            HudState white;
            white.flash_only = true;
            white.flash = 0.85f;
            hud_draw(dev, ctx, back, white);
        }
    }
    if (back) back->Release();
    back = nullptr;
    // The clock is set back by the whole freeze before it starts, so that nothing reading it meanwhile
    // sees time pass, and corrected afterwards by however much the sleeps overran.
    double t0 = real_s();
    g_frozen_s = g_frozen_s + freeze;
    HRESULT hr = g_orig_present(sc, sync, flags);
    DWORD flash_ms = freeze < 0.1f ? (DWORD)(freeze * 1000.0f) : 100, total_ms = (DWORD)(freeze * 1000.0f);
    Sleep(flash_ms);
    if (kept && ctx && total_ms > flash_ms) {
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);   // asked for again: a flip chain has moved on to another buffer
        if (back) {
            hud_put_frame(ctx, back);
            back->Release();
            g_orig_present(sc, sync, flags);
        }
    }
    if (total_ms > flash_ms) Sleep(total_ms - flash_ms);
    g_frozen_s = g_frozen_s + (real_s() - t0) - freeze;
    InterlockedIncrement(&g_freezes);
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    return hr;
}

// Every swap chain shares one vtable, so a throwaway one on a hidden window gives us the slot to patch.
static bool hook_present() {
    HWND wnd = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!wnd) return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 16;
    desc.BufferDesc.Height = 16;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = wnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    IDXGISwapChain *sc = nullptr;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &sc, &dev, nullptr, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        patch_vtable(sc, 8, (void *)my_present, (void **)&g_orig_present);   // IDXGISwapChain::Present
        ok = g_orig_present != nullptr;
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (sc) sc->Release();
    DestroyWindow(wnd);
    return ok;
}

// ---- endless ammo. A real shot tells us the arrow or bolt's item id. Its count in the inventory is found
// by searching memory for that id followed by a small number, and confirmed by seeing that number drop by
// exactly one on the next real shot. Only confirmed addresses are ever written.
struct AmmoCand {
    uintptr_t addr;
    int qty;
};
enum { AMMO_CANDS = 4096, AMMO_SLOTS = 8 };
static AmmoCand g_ammo_cands[AMMO_CANDS];
static int g_ammo_cand_n = 0, g_ammo_cand_id = 0;
static uintptr_t g_ammo_slots[AMMO_SLOTS];
static int g_ammo_slot_n = 0, g_ammo_slot_id = 0;

static void ammo_scan(int id) {
    static uint8_t buf[1 << 20];
    g_ammo_cand_n = 0;
    g_ammo_cand_id = id;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t p = 0x10000; p < 0x7FFFFFFF0000 && VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)); p = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
        uintptr_t rs = (uintptr_t)mbi.BaseAddress, re = rs + mbi.RegionSize;
        for (uintptr_t c = rs; c < re; c += sizeof(buf)) {
            size_t n = re - c < sizeof(buf) ? re - c : sizeof(buf);
            if (!safe_read(c, buf, n)) continue;
            for (size_t i = 0; i + 8 <= n; i += 4) {
                int v[2];
                memcpy(v, buf + i, 8);
                if (v[0] == id && v[1] >= 1 && v[1] <= 999 && g_ammo_cand_n < AMMO_CANDS)
                    g_ammo_cands[g_ammo_cand_n++] = {c + i, v[1]};
            }
        }
    }
}

// called after each real shot
static void ammo_on_shot(int id) {
    if (g_ammo_slot_n && g_ammo_slot_id == id) return;
    if (g_ammo_cand_n && g_ammo_cand_id == id) {
        int found = 0;
        g_ammo_slot_n = 0;
        for (int i = 0; i < g_ammo_cand_n; i++) {
            int v[2];
            if (safe_read(g_ammo_cands[i].addr, v, 8) && v[0] == id && v[1] == g_ammo_cands[i].qty - 1 && g_ammo_slot_n < AMMO_SLOTS) {
                g_ammo_slots[g_ammo_slot_n++] = g_ammo_cands[i].addr;
                found++;
            }
        }
        g_ammo_slot_id = id;
        logf("ammo %d: %d of %d candidates dropped by one -> %s\n", id, found, g_ammo_cand_n, found ? "locked at 99" : "none confirmed, searching again");
        if (found) return;
    }
    ammo_scan(id);
    logf("ammo %d: %d candidate counts found, waiting for the next shot to confirm\n", id, g_ammo_cand_n);
}

static void ammo_keep_full() {
    for (int i = 0; i < g_ammo_slot_n; i++) {
        int v[2];
        if (safe_read(g_ammo_slots[i], v, 8) && v[0] == g_ammo_slot_id && v[1] >= 0 && v[1] < 99) wr(g_ammo_slots[i] + 4, 99);
    }
}

struct Hook {
    uint8_t *fn;
    uint64_t patch;
    int reverts;
};
enum { MAX_HOOKS = 8 };
static Hook g_hooks[MAX_HOOKS];
static int g_hook_n = 0;

// Redirects a game function to `hook` and returns a trampoline that runs the original.
// `n` (5..14) prologue bytes are moved; they must be whole, position-independent instructions.
// `fn` must be 8-byte aligned so the patch is a single store. Only the first 8 bytes are rewritten;
// with a longer prologue the rest is left as it was and is never reached.
static void *install_hook(uintptr_t rva, const uint8_t *prologue, size_t n, void *hook) {
    uint8_t *fn = (uint8_t *)GetModuleHandleA(nullptr) + rva;
    if (n < 5 || n > 14 || ((uintptr_t)fn & 7) || memcmp(fn, prologue, n) != 0) return nullptr;
    uint8_t *t = nullptr;
    for (uintptr_t a = ((uintptr_t)fn & ~(uintptr_t)0xFFFF) - 0x10000; !t && a > (uintptr_t)fn - 0x70000000; a -= 0x10000)
        t = (uint8_t *)VirtualAlloc((void *)a, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    // trampoline: the moved bytes, then an absolute jump back into the function
    static const uint8_t JMP_ABS[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uintptr_t back = (uintptr_t)fn + n, target = (uintptr_t)hook;
    memcpy(t, fn, n);
    memcpy(t + n, JMP_ABS, 6);
    memcpy(t + n + 6, &back, 8);
    memcpy(t + 32, JMP_ABS, 6);
    memcpy(t + 38, &target, 8);
    uint8_t patch[8];
    memcpy(patch, fn, 8);
    patch[0] = 0xE9;
    int32_t rel = (int32_t)((t + 32) - (fn + 5));
    memcpy(patch + 1, &rel, 4);
    for (size_t i = 5; i < n && i < 8; i++) patch[i] = 0x90;
    DWORD old;
    if (!VirtualProtect(fn, 8, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    // one aligned 8-byte store, so a thread entering the function never sees half a patch
    uint64_t patch64;
    memcpy(&patch64, patch, 8);
    *(volatile uint64_t *)fn = patch64;
    VirtualProtect(fn, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 8);
    if (g_hook_n < MAX_HOOKS) g_hooks[g_hook_n++] = {fn, patch64, 0};
    return t;
}

// The game may put its original code back (v0.9: the camera hook stopped being called after ~15 s).
// Returns how many hooks had been reverted and were re-applied.
static int reapply_hooks() {
    int n = 0;
    for (int i = 0; i < g_hook_n; i++) {
        Hook &h = g_hooks[i];
        if (*(volatile uint64_t *)h.fn == h.patch) continue;
        DWORD old;
        if (!VirtualProtect(h.fn, 8, PAGE_EXECUTE_READWRITE, &old)) continue;
        *(volatile uint64_t *)h.fn = h.patch;
        VirtualProtect(h.fn, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), h.fn, 8);
        h.reverts++;
        n++;
    }
    return n;
}

static uintptr_t find_world_chr() {
    // mov rax,[rip+X]; mov rcx,[rax+68]; test rcx,rcx; je ..; cmp [rsi+10],rbx; je ..; 48
    static const int PAT[] = {0x48, 0x8B, 0x05, -1, -1, -1, -1, 0x48, 0x8B, 0x48, 0x68, 0x48, 0x85, 0xC9, 0x0F, 0x84,
                              -1, -1, -1, -1, 0x48, 0x39, 0x5E, 0x10, 0x0F, 0x84, -1, -1, -1, -1, 0x48};
    const size_t plen = sizeof(PAT) / sizeof(PAT[0]);
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &mi, sizeof(mi));
    uintptr_t base = (uintptr_t)mi.lpBaseOfDll, end = base + mi.SizeOfImage;
    for (uintptr_t p = base; p < end;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
        uintptr_t rs = (uintptr_t)mbi.BaseAddress, re = rs + mbi.RegionSize;
        bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                  (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (ok) {
            const uint8_t *b = (const uint8_t *)rs;
            for (size_t i = 0; i + plen <= re - rs; i++) {
                size_t j = 0;
                while (j < plen && (PAT[j] < 0 || b[i + j] == (uint8_t)PAT[j])) j++;
                if (j == plen) {
                    int32_t rel;
                    memcpy(&rel, b + i + 3, 4);
                    return rs + i + 7 + rel;
                }
            }
        }
        p = re;
    }
    return 0;
}

static bool game_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static bool pressed(int vk, bool &was) {
    bool down = game_focused() && (GetAsyncKeyState(vk) & 0x8000);
    bool edge = down && !was;
    was = down;
    return edge;
}

static DWORD WINAPI mod_thread(LPVOID) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "ultrasouls.log");
    g_log = fopen(path, "w");
    logf("ultrasouls v0.61 loaded\n");
    logf("settings: %s (sensitivity %.5f, fov scale %.1f, eye height %.2f, volume %.2f)\n", load_settings() ? "read from ultrasouls.ini" : "defaults, no ultrasouls.ini yet",
         (float)g_sens, (float)g_fov_scale, (float)g_eye_height, (float)g_volume);
    // Patching the code at startup made the game exit before showing a window (v0.7), so the hook
    // goes in later, once a character is loaded.
    bool hooked = false, hook_tried = false;

    uintptr_t world_ptr = 0;
    for (int i = 0; i < 120 && !world_ptr; i++) {
        world_ptr = find_world_chr();
        if (!world_ptr) Sleep(1000);
    }
    if (!world_ptr) { logf("WorldChrBase not found; mod inactive\n"); return 0; }
    logf("WorldChrBase pointer at exe+%llX\n", (unsigned long long)(world_ptr - (uintptr_t)GetModuleHandleA(nullptr)));

    float speed = 1.5f;
    bool speed_on = true, speed_ok = false, speed_checked = false;
    bool k6 = false, k7 = false, k8 = false, kj = false, kk = false, k3 = false, k4 = false, k5 = false, k1 = false, k2 = false, k9 = false, k10 = false, k11 = false, kins = false, kdel = false, kpgdn = false;
    bool game_hud_hidden = false;
    double game_hud_t = 0;
    double mouse_log = 0, fp_t0 = 0;
    int bullets_logged = 0, hud_status_logged = 0;
    LONG shots_seen = 0;
    bool shot_pending = false;
    double shot_seen_t = 0, ammo_t = 0;
    bool fp_air = false;
    float fp_y0 = 0, fp_peak = 0, fp_x0 = 0, fp_z0 = 0;
    double hook_check = 0, t_start = now_s();
    uintptr_t last_player = 0;
    int air = 0;   // 0 grounded, 1 K-jump rising (we write height), 2 falling, 3 J-jump (velocity)
    float jump_y0 = 0, start_x = 0, start_z = 0, last_y = 0, peak_y = 0;
    int still = 0;
    double jump_t0 = 0, frame_t = 0, vel_log = 0, sample_log = 0;
    enum { HIST = 6 };
    double hist_t[HIST] = {};
    float hist_x[HIST] = {}, hist_z[HIST] = {};
    int hist_i = 0;

    for (;;) {
        // The K-jump races the game's per-frame ground snap, so spin while it is rising.
        if (air == 1) YieldProcessor(); else Sleep(1);
        double t = now_s();

        uintptr_t world = 0, player = 0, map = 0, anim = 0, pos = 0;
        if (!rd(world_ptr, world) || !world || !rd(world + OFF_PLAYER, player) || !player ||
            !rd(player + OFF_MAP, map) || !map) {
            last_player = 0;
            air = 0;
            g_vjump = false;
            g_player_pos = 0;
            continue;
        }
        rd(map + OFF_MAP_ANIM, anim);
        rd(map + OFF_MAP_POS, pos);
        g_player_pos = pos;
        g_player_chr = player;
        {
            int hp = 0, hp_max = 0;
            if (rd(player + OFF_HP, hp) && rd(player + OFF_HP + 4, hp_max) && hp_max > 0) g_hud_health = 100.0f * (float)hp / (float)hp_max;
        }
        if (g_hud_status != hud_status_logged) {
            hud_status_logged = g_hud_status;
            logf("full HUD: %s%s\n", hud_status_logged == 1 ? "working" : "unavailable, using plain bars: ", hud_status_logged == 1 ? "" : hud_error());
        }
        g_block_lmb = g_instant_fire && g_ctrl;

        // Dark Souls' own HUD is governed by the "HUD" entry in its options: the player gauge reads
        // that byte every frame (exe+679AF6) and shows or hides itself accordingly. In first person it
        // is held off; leaving first person turns it back on.
        if (t - game_hud_t > 0.1) {
            game_hud_t = t;
            bool want_hidden = g_ctrl && g_hide_game_hud;
            uintptr_t gdm = 0, opts = 0;
            uint8_t cur = 0;
            if ((want_hidden || game_hud_hidden) && rd((uintptr_t)GetModuleHandleA(nullptr) + RVA_GAME_DATA_MAN, gdm) && gdm &&
                rd(gdm + OFF_GDM_OPTIONS, opts) && opts && rd(opts + OFF_OPT_HUD, cur)) {
                if (want_hidden) {
                    if (cur != 0) wr(opts + OFF_OPT_HUD, (uint8_t)0);
                    if (!game_hud_hidden) logf("game HUD hidden (option was %d)\n", cur);
                    game_hud_hidden = true;
                } else {
                    wr(opts + OFF_OPT_HUD, (uint8_t)1);
                    game_hud_hidden = false;
                    logf("game HUD shown again\n");
                }
            }
        }

        if (!hook_tried && speed_checked) {
            hook_tried = true;
            logf("installing physics step hook...\n");
            g_orig_step = (StepFn)install_hook(RVA_STEP, STEP_PROLOGUE, sizeof(STEP_PROLOGUE), (void *)hook_step);
            g_orig_step_alt = (StepFn)install_hook(RVA_STEP_ALT, STEP_ALT_PROLOGUE, sizeof(STEP_ALT_PROLOGUE), (void *)hook_step_alt);
            logf("second physics step hook: %s\n", g_orig_step_alt ? "installed" : "NOT installed (code mismatch)");
            g_orig_sweep = (SweepFn)install_hook(RVA_STEP_SWEEP, SWEEP_PROLOGUE, sizeof(SWEEP_PROLOGUE), (void *)hook_sweep);
            logf("move re-check hook: %s\n", g_orig_sweep ? "installed" : "NOT installed (code mismatch)");
            hooked = g_orig_step != nullptr;
            g_orig_cam = (CamFn)install_hook(RVA_CAM_UPDATE, CAM_PROLOGUE, sizeof(CAM_PROLOGUE), (void *)hook_cam);
            g_orig_bullet_init = (BulletInitFn)install_hook(RVA_BULLET_INIT, BULLET_PROLOGUE, sizeof(BULLET_PROLOGUE), (void *)hook_bullet_init);
            logf("projectile hook: %s\n", g_orig_bullet_init ? "installed" : "NOT installed (code mismatch)");
            logf("HUD (Present) hook: %s\n", hook_present() ? "installed" : "NOT installed");
            logf("menu key filter: %s\n", hook_async_key() ? "installed" : "NOT installed (import slot holds something else)");
            logf("cursor clip filter: %s\n", hook_clip_cursor() ? "installed" : "NOT installed (import slot holds something else)");
            {
                static wchar_t pack[MAX_PATH];
                GetModuleFileNameW(nullptr, pack, MAX_PATH);
                wchar_t *slash = wcsrchr(pack, L'\\');
                wcscpy(slash ? slash + 1 : pack, L"ultrasouls_sounds.bin");
                bool ok = sound_init(pack);
                sound_master(g_volume);
                logf("sounds: %s\n", ok ? "ready" : sound_error());
                wcscpy(slash ? slash + 1 : pack, L"ultrasouls_names.txt");
                int names = load_boss_names(pack);
                if (names < 0) logf("boss names: ultrasouls_names.txt not found, so no boss bars (tools/ds_names.py makes it)\n");
                else logf("boss names: %d\n", names);
            }
            {
                int handle = 0;
                rd(player + 8, handle);
                logf("player handle field reads %08X (expected 10044000)\n", handle);
            }
            logf("camera hook: %s\n", g_orig_cam ? "installed" : "NOT installed (code mismatch); F5 disabled");
            logf("raw input watch: %s; DirectInput devices created: %ld, mouse device %s\n", watch_raw_input() ? "installed" : "no window found",
                 (long)g_dev_n, g_mouse_dev ? "found" : "not seen");
            for (LONG i = 0; i < g_dev_n && i < 8; i++)
                logf("  device guid %08lX-%04X-%04X\n", g_dev_guids[i].Data1, g_dev_guids[i].Data2, g_dev_guids[i].Data3);
            logf("physics step hook: %s\n", hooked ? "installed" : "NOT installed (code mismatch); J disabled");
        }

        if (player != last_player) {
            last_player = player;
            speed_checked = false;
            int hp = 0, hpmax = 0;
            rd(player + OFF_HP, hp);
            rd(player + OFF_HP + 4, hpmax);
            logf("player %p hp %d/%d\n", (void *)player, hp, hpmax);
        }

        // stamina lock
        int stam_max = 0;
        if (rd(player + OFF_STAMINA + 4, stam_max) && stam_max > 0 && stam_max < 100000)
            wr(player + OFF_STAMINA, stam_max);

        // animation speed: only write if the field looked like a speed (1.0) when first seen
        if (anim && !speed_checked) {
            float cur = 0;
            speed_checked = true;
            speed_ok = rd(anim + OFF_ANIM_SPEED, cur) && cur > 0.99f && cur < 1.01f;
            logf("anim speed field reads %.4f -> %s\n", cur, speed_ok ? "ok, enabling" : "unexpected, speed mod disabled");
        }
        if (pressed(VK_F6, k6)) { speed = speed > 0.6f ? speed - 0.1f : speed; logf("speed %.1f\n", speed); }
        if (pressed(VK_F7, k7)) { speed = speed < 3.0f ? speed + 0.1f : speed; logf("speed %.1f\n", speed); }
        if (pressed(VK_F8, k8)) { speed_on = !speed_on; logf("speed %s\n", speed_on ? "on" : "off"); }
        if (g_orig_cam && pressed(VK_F5, k5)) {
            g_first_person = !g_first_person;
            float px = 0, py = 0, pz = 0;
            rd(pos + OFF_POS_X, px);
            rd(pos + OFF_POS_Y, py);
            rd(pos + OFF_POS_Z, pz);
            logf("first person %s; player (%.2f, %.2f, %.2f); game camera rows:\n", g_first_person ? "on" : "off", px, py, pz);
            for (int i = 0; i < 16; i += 4)
                logf("  (%.3f, %.3f, %.3f, %.3f)\n", g_cam_seen[i], g_cam_seen[i + 1], g_cam_seen[i + 2], g_cam_seen[i + 3]);
            logf("  lens (%.4f, %.4f, %.4f, %.4f) camera calls %ld\n", g_lens_seen[0], g_lens_seen[1], g_lens_seen[2], g_lens_seen[3], (long)g_cam_calls);
        }
        while (bullets_logged < BULLET_LOG && bullets_logged < g_player_shots) {
            int b = bullets_logged++;
            logf("player shot %d: bullet id %d, emitter %p, a4:", b, *(int *)(g_bullet_params[b] + 4), (void *)g_bullet_emitter[b]);
            for (int j = 0; j < 0x40; j++) logf(" %02X", g_bullet_a4[b][j]);
            logf("\n");
        }
        if (g_real_shots != shots_seen && t - shot_seen_t > 0.4) {
            // a little after the shot, so the game has taken the arrow out of the inventory
            if (shot_pending) { shots_seen = g_real_shots; shot_pending = false; ammo_on_shot(g_ammo_id); }
            else { shot_pending = true; shot_seen_t = t; }
        }
        if (t - ammo_t > 0.25) { ammo_t = t; ammo_keep_full(); }
        {
            static bool kpgup = false;
            if (pressed(VK_PRIOR, kpgup)) {
                g_camera_tilt = !g_camera_tilt;
                logf("camera tilt: %s\n", g_camera_tilt ? "on" : "off");
                save_settings();
            }
        }
        if (pressed(VK_NEXT, kpgdn)) {
            g_hide_body = !g_hide_body;
            logf("hide the player's body in first person: %s (calls so far %ld)\n", g_hide_body ? "yes" : "no", (long)g_body_hides);
        }
        if (pressed(VK_DELETE, kdel)) {
            g_hide_game_hud = !g_hide_game_hud;
            logf("hide the game's HUD in first person: %s\n", g_hide_game_hud ? "yes" : "no");
        }
        if (pressed(VK_INSERT, kins)) {
            g_viewmodel = !g_viewmodel;
            logf("viewmodel %s\n", g_viewmodel ? "on" : "off");
        }
        if (pressed(VK_F11, k11)) {
            g_instant_fire = !g_instant_fire;
            logf("revolver %s\n", g_instant_fire ? "on" : "off");
        }
        {
            // - and = : sound effects volume, with a beep to hear the new level
            static bool kminus = false, kplus = false;
            bool down = pressed(VK_OEM_MINUS, kminus), up = pressed(VK_OEM_PLUS, kplus);
            if (down || up) {
                float volume = g_volume + (up ? 0.05f : -0.05f);
                g_volume = volume < 0 ? 0 : volume > 1 ? 1 : volume;
                sound_master(g_volume);
                sound_play("pierce_ready", 0.35f, 1.0f);
                logf("sound volume %.2f\n", (float)g_volume);
                save_settings();
            }
            // F12: save the last 30 s of what the mixer sent to the audio device, to study a bad-sounding moment
            static bool kf12 = false;
            if (pressed(VK_F12, kf12)) {
                static wchar_t dump[MAX_PATH];
                GetModuleFileNameW(nullptr, dump, MAX_PATH);
                wchar_t *slash = wcsrchr(dump, L'\\');
                wcscpy(slash ? slash + 1 : dump, L"ultrasouls_sound_dump.wav");
                bool ok = sound_dump(dump);
                logf("sound dump: %s\n", ok ? "written (ultrasouls_sound_dump.wav and .txt next to the exe)" : "failed");
                if (ok) sound_play("pierce_ready", 0.35f, 1.5f);
            }
            static int logged_underruns = 0;
            static double underrun_log_at = 0;
            int u = sound_underruns();
            if (u != logged_underruns && now_s() - underrun_log_at > 5.0) {
                logf("sound: %d dropouts so far\n", u);
                logged_underruns = u;
                underrun_log_at = now_s();
            }
        }
        if (pressed(VK_F9, k9)) { g_sens = g_sens * 0.8f; logf("mouse sensitivity %.5f\n", g_sens); save_settings(); }
        if (pressed(VK_F10, k10)) { g_sens = g_sens * 1.25f; logf("mouse sensitivity %.5f\n", g_sens); save_settings(); }
        if (g_first_person && t - mouse_log > 5.0) {
            mouse_log = t;
            logf("mouse: DirectInput mouse reads %ld (state calls %ld, data calls %ld), raw input %ld, cursor moves %ld; yaw %.2f pitch %.2f; keys hidden from game %ld; projectiles %ld, player shots %ld, re-aimed %ld, revolver shots %ld, charged %ld, refused %ld; frames %ld, HUD draws %ld; steps %ld (second kind %ld)\n",
                 (long)g_di_events, (long)g_di_state_calls, (long)g_di_data_calls, (long)g_raw_events, (long)g_cur_events, g_yaw, g_pitch,
                 (long)g_keys_blocked, (long)g_bullets, (long)g_player_shots, (long)g_bullets_aimed, (long)g_instant_shots, (long)g_pierce_shots, (long)g_shoot_fails, (long)g_presents, (long)g_hud_draws, (long)g_steps, (long)g_steps_alt);
        }
        if (pressed(VK_F1, k1)) { g_fov_scale = g_fov_scale - 0.1f; logf("fov scale %.1f\n", g_fov_scale); save_settings(); }
        if (pressed(VK_F2, k2)) { g_fov_scale = g_fov_scale + 0.1f; logf("fov scale %.1f\n", g_fov_scale); save_settings(); }
        if (t - hook_check > 0.02) {
            hook_check = t;
            if (reapply_hooks())
                logf("t=%.1f game reverted hooked code; re-applied (physics %d, camera %d reverts so far)\n", t - t_start,
                     g_hook_n > 0 ? g_hooks[0].reverts : 0, g_hook_n > 1 ? g_hooks[1].reverts : 0);
        }
        if (pressed(VK_F3, k3)) { g_eye_height = g_eye_height - 0.05f; logf("eye height %.2f\n", g_eye_height); save_settings(); }
        if (pressed(VK_F4, k4)) { g_eye_height = g_eye_height + 0.05f; logf("eye height %.2f\n", g_eye_height); save_settings(); }
        if (anim && speed_ok) wr(anim + OFF_ANIM_SPEED, speed_on ? speed : 1.0f);

        if (!pos) { air = 0; g_vjump = false; continue; }
        float x = 0, y = 0, z = 0;
        if (!rd(pos + OFF_POS_X, x) || !rd(pos + OFF_POS_Y, y) || !rd(pos + OFF_POS_Z, z)) { air = 0; g_vjump = false; continue; }

        // ground velocity over the last ~150 ms, for jump momentum
        if (!air && t - hist_t[hist_i] >= 0.03) {
            hist_i = (hist_i + 1) % HIST;
            hist_t[hist_i] = t;
            hist_x[hist_i] = x;
            hist_z[hist_i] = z;
        }
        int old = (hist_i + 1) % HIST;
        float span = (float)(t - hist_t[old]);
        float gvx = span > 0.05f && span < 0.5f ? (x - hist_x[old]) / span : 0;
        float gvz = span > 0.05f && span < 0.5f ? (z - hist_z[old]) / span : 0;
        float gspeed = sqrtf(gvx * gvx + gvz * gvz);

        // Compare what the hook sees in the velocity field with the measured ground speed, once a second
        // while moving. The velocity jump stays locked until the field has looked like a velocity.
        if (hooked && !air && gspeed > 2.0f && t - vel_log > 1.0) {
            vel_log = t;
            float sx = g_seen_vel[0], sy = g_seen_vel[1], sz = g_seen_vel[2];
            float sh = sqrtf(sx * sx + sz * sz);
            bool sane = sh > gspeed * 0.5f && sh < gspeed * 2.0f && fabsf(sy) < 30.0f;
            if (sane) g_vel_ok = true;
            logf("vel check: measured %.2f m/s (%.2f, %.2f); field (%.2f, %.2f, %.2f) |h|=%.2f -> %s; steps %ld\n",
                 gspeed, gvx, gvz, sx, sy, sz, sh, sane ? "match" : "no match", (long)g_steps);
        }

        bool want_j = !air && pressed('J', kj), want_k = !air && pressed('K', kk);
        if (want_j && g_ctrl) {
            g_jump_req = true;   // first person: the controller in the physics hook owns the whole jump
            want_j = false;
        }
        if (g_ctrl) {
            if (g_air && !fp_air) { fp_y0 = fp_peak = y; fp_t0 = t; fp_x0 = x; fp_z0 = z; }
            if (g_air && y > fp_peak) fp_peak = y;
            if (!g_air && fp_air)
                logf("first-person air: peak +%.2f m, %.2fs, travelled %.2f m (landings %ld, ceiling hits %ld; dashes %ld, slides %ld, slams %ld, wall jumps %ld, slam jumps %ld, cling steps %ld, moves cut by the re-check %ld, pull-downs undone %ld, rescues %ld; "
                     "last wall normal (%.2f, %.2f) dist %.3f age %.2fs, contacts %d; slide multiplier %.2f)\n", fp_peak - fp_y0, t - fp_t0,
                     sqrtf((x - fp_x0) * (x - fp_x0) + (z - fp_z0) * (z - fp_z0)), (long)g_landings, (long)g_bonks,
                     (long)g_dashes, (long)g_slides, (long)g_slams, (long)g_wall_jump_count, (long)g_slam_jump_count, (long)g_clings, (long)g_sweep_cuts, (long)g_pull_fixes, (long)g_rescues,
                     g_wall_nx, g_wall_nz, g_wall_dist, g_wall_age, (int)g_seen_manifold, g_preslide);
            fp_air = g_air;
            static bool fp_slide = false;
            static float sl_x0 = 0, sl_z0 = 0;
            static double sl_t0 = 0;
            if (g_sliding && !fp_slide) { sl_x0 = x; sl_z0 = z; sl_t0 = t; }
            if (!g_sliding && fp_slide)
                logf("first-person slide: %.2f m in %.2fs, started at x%.2f speed\n",
                     sqrtf((x - sl_x0) * (x - sl_x0) + (z - sl_z0) * (z - sl_z0)), t - sl_t0, g_slide_mult0);
            fp_slide = g_sliding;
        }
        if (want_j && (!hooked || !g_vel_ok)) {
            logf("J ignored: %s\n", !hooked ? "hook not installed" : "velocity field not confirmed yet (run for a second first)");
            want_j = false;
        }
        if (want_j || want_k) {
            g_carry = want_j && gspeed > 1.0f ? gspeed * AIR_CARRY : 0;
            g_dir_x = gspeed > 1.0f ? gvx / gspeed : 0;
            g_dir_z = gspeed > 1.0f ? gvz / gspeed : 0;
            air = want_j ? 3 : 1;
            jump_y0 = last_y = peak_y = y;
            jump_t0 = frame_t = g_vjump_t0 = t;
            start_x = x;
            start_z = z;
            still = 0;
            sample_log = 0;
            g_vjump = want_j;
            logf("%s jump start y=%.3f ground speed %.2f m/s, carrying %.2f\n", want_j ? "J" : "K", y, gspeed, g_carry);
        }
        if (air) {
            float e = (float)(t - jump_t0);
            if (y > peak_y) peak_y = y;
            if (air == 1) {
                // absolute trajectory, so a frame where the game snaps us back costs nothing
                wr(pos + OFF_POS_Y, jump_y0 + JUMP_V0 * e - 0.5f * JUMP_G * e * e);
                if (e >= JUMP_V0 / JUMP_G) air = 2;
            }
            if (air == 3 && t - sample_log > 0.05) {
                sample_log = t;
                logf("  t=%.2f y=%.3f game vel (%.2f, %.2f, %.2f) snap %.3f grounded %d\n", e, y, g_seen_vel[0], g_seen_vel[1], g_seen_vel[2], g_seen_snap, g_seen_grounded);
            }
            if (t - frame_t >= 1.0 / 60) {
                bool rising = e < JUMP_V0 / JUMP_G;
                still = !rising && fabsf(y - last_y) < 0.002f ? still + 1 : 0;
                last_y = y;
                frame_t = t;
                if (still >= 3 || e > 3.0f) {
                    float dx = x - start_x, dz = z - start_z;
                    logf("landed y=%.3f peak +%.2f m after %.2fs, travelled %.2f m\n", y, peak_y - jump_y0, e, sqrtf(dx * dx + dz * dz));
                    air = 0;
                    g_vjump = false;
                }
            }
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(nullptr, 0, mod_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}
