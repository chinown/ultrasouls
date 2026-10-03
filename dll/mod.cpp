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
static const uint8_t BLOCKED_KEYS[] = {0x39, 0x2A, 0x36, 0x1D, 0x9D};   // DIK_SPACE, L/R SHIFT, L/R CONTROL

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

static double now_s() {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

// ---- physics step hook: runs on the game's thread, once per character per frame
typedef void (*StepFn)(void *self, void *step_info, void *gravity);
static StepFn g_orig_step = nullptr;
static volatile uintptr_t g_player_pos = 0;
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
static void step_common(void *self, void *step_info, void *gravity, StepFn orig) {
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
                if (g_jump_req && !g_air) {
                    // NewMovement.Jump: a slide-jump is lower and keeps the slide speed; a dash-jump is lower
                    // still and keeps the dash speed only if a stamina bar can be paid, else drops to run speed.
                    if (g_sliding) {
                        g_vy = V1_SLIDE_JUMP_SPEED;
                    } else if (g_dash_left > 0) {
                        g_vy = V1_DASH_JUMP_SPEED;
                        if (g_boost >= 100.0f) {
                            g_boost -= 100.0f;
                        } else {
                            g_vx = g_dash_x * V1_RUN_SPEED;
                            g_vz = g_dash_z * V1_RUN_SPEED;
                        }
                    } else {
                        g_vy = V1_JUMP_SPEED;
                    }
                    g_dash_left = 0;
                    g_sliding = false;
                    g_air = true;
                    g_air_steps = 0;
                } else if (!g_air && !flag && air_time >= 0.15f) {
                    g_air = true;                       // walked off a ledge
                    g_air_steps = 0;
                    g_vy = vel[1] < 0 ? vel[1] : 0;
                }
                g_jump_req = false;

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

                if (shift_edge && g_boost >= 100.0f && !g_slamming) {
                    g_boost -= 100.0f;
                    g_dash_left = V1_DASH_TIME;
                    g_dash_x = dir_x;
                    g_dash_z = dir_z;
                    g_sliding = false;
                    InterlockedIncrement(&g_dashes);
                }
                if (ctrl_edge && g_air && !g_slamming) {
                    g_slamming = true;          // ground slam: straight down, no horizontal speed
                    g_dash_left = 0;
                    InterlockedIncrement(&g_slams);
                }
                if (ctrl && !g_air && !g_sliding && g_dash_left <= 0) {
                    g_sliding = true;
                    g_slide_x = dir_x;
                    g_slide_z = dir_z;
                    InterlockedIncrement(&g_slides);
                }
                if (g_sliding && (!ctrl || g_air)) g_sliding = false;
                if (!g_air) g_slamming = false;

                if (g_slamming) {
                    g_vx = g_vz = 0;
                    g_vy = -V1_SLAM_SPEED;
                    vel[1] = g_vy;
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
                } else if (g_sliding) {
                    // V1 slide: fixed direction at slide speed, A/D nudge it sideways
                    g_vx = g_slide_x * V1_SLIDE_SPEED + g_view_right[0] * s * V1_SLIDE_STEER;
                    g_vz = g_slide_z * V1_SLIDE_SPEED + g_view_right[1] * s * V1_SLIDE_STEER;
                } else if (!g_air) {
                    // V1 on the ground: velocity = Lerp(velocity, input * 16.5, 0.25) every 8 ms physics step
                    float k = 1.0f - powf(0.75f, dt / V1_STEP);
                    g_vx += (wx * V1_RUN_SPEED - g_vx) * k;
                    g_vz += (wz * V1_RUN_SPEED - g_vz) * k;
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
                    g_vy -= V1_GRAVITY * dt;
                    vel[1] = g_vy;
                }
                vel[0] = g_vx;
                vel[2] = g_vz;
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
    orig(self, step_info, gravity);
    if (!proxy) return;
    // v0.14 waited for the game's grounded flag to land, and it never came back while we drove the
    // fall: the character stayed 'airborne' for 20 s, sliding. Instead, compare how far the physics
    // actually moved us with how far we asked. Blocked going down = landed; going up = ceiling.
    float y1;
    if (ctrl_air && have_y0 && proxy_y(proxy, y1)) {
        float want = ctrl_vy * ctrl_dt, got = y1 - y0;
        g_air_steps++;
        if (ctrl_vy < -0.5f && got > want * 0.3f) {
            g_air = false;
            g_vy = 0;
            InterlockedIncrement(&g_landings);
        } else if (ctrl_vy > 0.5f && g_air_steps > 3 && got < want * 0.3f) {
            g_vy = 0;
            InterlockedIncrement(&g_bonks);
        }
    }
    if (g_air && g_air_steps > 2000) g_air = false;   // safety net: never stay airborne for ever
    // After integrating, the game moves the character vertically by these two offsets to keep it
    // glued to the ground. On takeoff, put that back so the character leaves the ground.
    float snap = *(float *)((uintptr_t)self + OFF_POS_SNAP_A) + *(float *)((uintptr_t)self + OFF_POS_SNAP_B);
    g_seen_snap = snap;
    g_seen_grounded = *(uint8_t *)((uintptr_t)self + OFF_POS_GROUNDED);
    // v0.7 showed this offset is applied every frame, airborne or not, so undoing it every frame added
    // 0.31 m per frame. One lift is enough: it breaks ground contact, then the velocity does the rest.
    if (rising && snap < 0 && g_seen_grounded && !*(uint8_t *)((uintptr_t)self + OFF_POS_NO_ADJUST)) {
        uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr);
        uintptr_t phys = *(uintptr_t *)(exe + RVA_PHYS_WORLD);
        void *lock = phys ? *(void **)(phys + 0x28) : nullptr;
        uintptr_t phantom = *(uintptr_t *)(proxy + 0x80);
        uintptr_t body = phantom ? *(uintptr_t *)(phantom + 0x30) : 0;
        if (lock && body) {
            const float *p = (const float *)(body + 0x30);
            alignas(16) float v[4] = {p[0], p[1] - snap, p[2], p[3]};
            ((void (*)(void *))(exe + RVA_PHYS_LOCK))(lock);
            ((void (*)(uintptr_t, float *))(exe + RVA_PROXY_SET_POS))(proxy, v);
            ((void (*)(void *))(exe + RVA_PHYS_UNLOCK))(lock);
        }
    }
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
static volatile float g_fov_scale = 1.6f;
static volatile LONG g_cam_calls = 0;
static float g_yaw = 0, g_pitch = 0;            // our view direction, radians; yaw 0 looks along +Z
static bool g_cursor_centred = false;
static float g_eye_drop = 0;                    // how far the view is currently lowered (slide)
static const float SLIDE_EYE_DROP = 0.6f;       // V1's view drops while sliding; 0.6 m is a guess, not from its code
static volatile float g_sens = 0.0008f;          // radians per mouse count

static uint64_t hook_cam(void *self, float dt, void *a3, void *a4, uint64_t a5, uint64_t a6) {
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
    if (lens[0] > 0.2f && lens[0] < 2.5f) lens[0] *= g_fov_scale;
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
static volatile uintptr_t g_player_chr = 0;
// the object that fired the player's last shot (params live at +0x1A0 inside it), for the instant-fire test
static volatile uintptr_t g_last_emitter = 0;
static volatile bool g_in_instant_fire = false;
static volatile LONG g_real_shots = 0;
static volatile int g_ammo_id = 0;

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
        float sy = sinf(g_yaw), cy = cosf(g_yaw), sp = sinf(g_pitch), cp = cosf(g_pitch);
        float right[3] = {cy, 0, -sy}, up[3] = {-sp * sy, cp, -sp * cy}, fwd[3] = {sy * cp, sp, cy * cp};
        float start[3] = {p[0] + fwd[0] * 0.6f, p[1] + g_eye_height - g_eye_drop + fwd[1] * 0.6f, p[2] + fwd[2] * 0.6f};
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
static volatile bool g_item_style = true;        // Insert: which kind of request the shots use
static const int GOODS_THROWING_KNIFE = 290;
static volatile LONG g_instant_shots = 0, g_pierce_shots = 0, g_shoot_fails = 0;
static double g_next_shot = 0;
static volatile float g_pierce_charge = 0;        // 0..100 while the alt fire is held
static volatile float g_pierce_ready = 100.0f;    // 0..100, recharging after a charged shot
static bool g_prev_rmb = false;
static volatile double g_last_shot_time = -100.0, g_last_pierce_time = -100.0;

static bool send_shot(int behavior, bool item_style) {
    uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr), pos = g_player_pos;
    uintptr_t man = *(uintptr_t *)(exe + RVA_BULLET_MAN);
    if (!man || !pos || memcmp((void *)(exe + RVA_BULLET_SHOOT), BULLET_SHOOT_BYTES, sizeof(BULLET_SHOOT_BYTES)) != 0) return false;
    const float *p = (const float *)(pos + OFF_POS_X);
    float sy = sinf(g_yaw), cy = cosf(g_yaw), sp = sinf(g_pitch), cp = cosf(g_pitch);
    float right[3] = {cy, 0, -sy}, up[3] = {-sp * sy, cp, -sp * cy}, fwd[3] = {sy * cp, sp, cy * cp};
    float eye[3] = {p[0], p[1] + g_eye_height - g_eye_drop, p[2]};
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
    return id != -1;
}

static bool shoot(int behavior) {
    bool ok = send_shot(behavior, g_item_style);
    if (!ok && g_item_style) ok = send_shot(behavior, false);   // refused: fall back to the weapon-style request
    if (!ok) InterlockedIncrement(&g_shoot_fails);
    return ok;
}

// once per frame, on the game's thread
static void try_instant_fire() {
    static double last = 0;
    double t = now_s();
    float dt = last > 0 && t - last < 0.1 ? (float)(t - last) : 0.0f;
    last = t;
    if (!g_instant_fire || !g_ctrl) { g_pierce_charge = 0; g_prev_rmb = false; return; }

    bool lmb = key_down(VK_LBUTTON) != 0, rmb = key_down(VK_RBUTTON) != 0;
    if (g_pierce_ready < 100.0f) g_pierce_ready = g_pierce_ready + PIERCE_RECHARGE_RATE * dt > 100.0f ? 100.0f : g_pierce_ready + PIERCE_RECHARGE_RATE * dt;
    if (rmb && g_pierce_ready >= 100.0f) {
        g_pierce_charge = g_pierce_charge + PIERCE_CHARGE_RATE * dt > 100.0f ? 100.0f : g_pierce_charge + PIERCE_CHARGE_RATE * dt;
    } else if (g_prev_rmb && g_pierce_charge >= 100.0f) {
        if (shoot(BEHAVIOR_PIERCER)) {
            InterlockedIncrement(&g_pierce_shots);
            g_last_pierce_time = t;
            g_pierce_ready = 0;
            g_next_shot = t + FIRE_INTERVAL;
        }
        g_pierce_charge = 0;
    } else {
        g_pierce_charge = 0;   // released early: the charge is lost
    }
    g_prev_rmb = rmb;

    if (lmb && g_pierce_charge <= 0 && t >= g_next_shot) {
        g_next_shot = t + FIRE_INTERVAL;
        if (shoot(BEHAVIOR_REVOLVER)) {
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
                hud_draw(dev, ctx, back, st);
            }
        }
    }
    if (back) back->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    return ok;
}

static HRESULT WINAPI my_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    InterlockedIncrement(&g_presents);
    if (g_ctrl && !draw_full_hud(sc)) draw_hud(sc);
    return g_orig_present(sc, sync, flags);
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
    logf("ultrasouls v0.30 loaded\n");
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
    bool k6 = false, k7 = false, k8 = false, kj = false, kk = false, k3 = false, k4 = false, k5 = false, k1 = false, k2 = false, k9 = false, k10 = false, k11 = false, kins = false, khome = false;
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

        if (!hook_tried && speed_checked) {
            hook_tried = true;
            logf("installing physics step hook...\n");
            g_orig_step = (StepFn)install_hook(RVA_STEP, STEP_PROLOGUE, sizeof(STEP_PROLOGUE), (void *)hook_step);
            g_orig_step_alt = (StepFn)install_hook(RVA_STEP_ALT, STEP_ALT_PROLOGUE, sizeof(STEP_ALT_PROLOGUE), (void *)hook_step_alt);
            logf("second physics step hook: %s\n", g_orig_step_alt ? "installed" : "NOT installed (code mismatch)");
            hooked = g_orig_step != nullptr;
            g_orig_cam = (CamFn)install_hook(RVA_CAM_UPDATE, CAM_PROLOGUE, sizeof(CAM_PROLOGUE), (void *)hook_cam);
            g_orig_bullet_init = (BulletInitFn)install_hook(RVA_BULLET_INIT, BULLET_PROLOGUE, sizeof(BULLET_PROLOGUE), (void *)hook_bullet_init);
            logf("projectile hook: %s\n", g_orig_bullet_init ? "installed" : "NOT installed (code mismatch)");
            logf("HUD (Present) hook: %s\n", hook_present() ? "installed" : "NOT installed");
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
        if (pressed(VK_HOME, khome)) {
            g_viewmodel = !g_viewmodel;
            logf("viewmodel %s\n", g_viewmodel ? "on" : "off");
        }
        if (pressed(VK_INSERT, kins)) {
            g_item_style = !g_item_style;
            logf("shot request style: %s\n", g_item_style ? "item" : "weapon");
        }
        if (pressed(VK_F11, k11)) {
            g_instant_fire = !g_instant_fire;
            logf("revolver %s\n", g_instant_fire ? "on" : "off");
        }
        if (pressed(VK_F9, k9)) { g_sens = g_sens * 0.8f; logf("mouse sensitivity %.5f\n", g_sens); }
        if (pressed(VK_F10, k10)) { g_sens = g_sens * 1.25f; logf("mouse sensitivity %.5f\n", g_sens); }
        if (g_first_person && t - mouse_log > 5.0) {
            mouse_log = t;
            logf("mouse: DirectInput mouse reads %ld (state calls %ld, data calls %ld), raw input %ld, cursor moves %ld; yaw %.2f pitch %.2f; keys hidden from game %ld; projectiles %ld, player shots %ld, re-aimed %ld, revolver shots %ld, charged %ld, refused %ld; frames %ld, HUD draws %ld; steps %ld (second kind %ld)\n",
                 (long)g_di_events, (long)g_di_state_calls, (long)g_di_data_calls, (long)g_raw_events, (long)g_cur_events, g_yaw, g_pitch,
                 (long)g_keys_blocked, (long)g_bullets, (long)g_player_shots, (long)g_bullets_aimed, (long)g_instant_shots, (long)g_pierce_shots, (long)g_shoot_fails, (long)g_presents, (long)g_hud_draws, (long)g_steps, (long)g_steps_alt);
        }
        if (pressed(VK_F1, k1)) { g_fov_scale = g_fov_scale - 0.1f; logf("fov scale %.1f\n", g_fov_scale); }
        if (pressed(VK_F2, k2)) { g_fov_scale = g_fov_scale + 0.1f; logf("fov scale %.1f\n", g_fov_scale); }
        if (t - hook_check > 0.02) {
            hook_check = t;
            if (reapply_hooks())
                logf("t=%.1f game reverted hooked code; re-applied (physics %d, camera %d reverts so far)\n", t - t_start,
                     g_hook_n > 0 ? g_hooks[0].reverts : 0, g_hook_n > 1 ? g_hooks[1].reverts : 0);
        }
        if (pressed(VK_F3, k3)) { g_eye_height = g_eye_height - 0.05f; logf("eye height %.2f\n", g_eye_height); }
        if (pressed(VK_F4, k4)) { g_eye_height = g_eye_height + 0.05f; logf("eye height %.2f\n", g_eye_height); }
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
                logf("first-person air: peak +%.2f m, %.2fs, travelled %.2f m (landings %ld, ceiling hits %ld; dashes %ld, slides %ld, slams %ld)\n", fp_peak - fp_y0, t - fp_t0,
                     sqrtf((x - fp_x0) * (x - fp_x0) + (z - fp_z0) * (z - fp_z0)), (long)g_landings, (long)g_bonks,
                     (long)g_dashes, (long)g_slides, (long)g_slams);
            fp_air = g_air;
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
