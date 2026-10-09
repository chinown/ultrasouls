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
#include <vector>
#include <new>

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

#define MOD_VERSION "v0.89"
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
static const uint8_t BLOCKED_KEYS[] = {0x39, 0x2A, 0x36, 0x1D, 0x9D, 0x21, 0x22, 0x2D, 0x2E, 0x13};   // DIK_SPACE, L/R SHIFT, L/R CONTROL, F, G, X, C, R
// The movement keys as well (v0.69). The controller reads them itself and writes the velocity, so the game
// never needed them; but while it saw them its own character went on running under the hidden body, and
// its run animation is what puts down Dark Souls' footstep dust: the white wisps round the feet that show
// when looking down. With the keys hidden the character stands in its idle animation while it is moved.
// g_hide_move is the setting (hide_move_keys in the ini); the controller clears it for the rest of the
// session if the character stops answering to the velocity it is given (see step_common).
static volatile bool g_hide_move = true;
static const uint8_t MOVE_KEYS[] = {0x11, 0x1E, 0x1F, 0x20};   // DIK_W, A, S, D
static const int MOVE_VKS[] = {'W', 'A', 'S', 'D'};

// The game has a second keyboard reader: a table of 236 virtual keys polled with GetAsyncKeyState
// (exe+CA06A0), which is what its menus listen to. With only DirectInput filtered, G still opened the
// gesture menu. Its import slot is pointed at a filter that reports the same keys as up.
static const uintptr_t RVA_IAT_ASYNC_KEY = 0x2017C14;
static const int BLOCKED_VKS[] = {VK_SPACE, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, 'F', 'G', 'X', 'C', 'R'};
typedef SHORT (WINAPI *AsyncKeyFn)(int);
static AsyncKeyFn g_orig_async_key = nullptr;

static SHORT WINAPI my_async_key(int vk) {
    if (g_block_keys) {
        for (int k : BLOCKED_VKS)
            if (vk == k) return 0;
        if (g_hide_move)
            for (int k : MOVE_VKS)
                if (vk == k) return 0;
    }
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
        if (g_hide_move)
            for (uint8_t k : MOVE_KEYS) keys[k] = 0;
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
            if (g_hide_move)
                for (uint8_t k : MOVE_KEYS)
                    if (e[0] == k && (e[1] & 0x80)) e[1] = 0;
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

// Dark Souls' menus go by where the Windows pointer is, and ask for it every frame: exe+1B72D0 (the menus'
// mouse) calls GetCursorPos, ScreenToClient on the game's window and scales the point to the picture; the
// mouse device at exe+CA0E80 asks as well. Window messages play no part. While the title menu is drawn as
// ULTRAKILL's column of buttons (hud.cpp's build_title) its rows are not where the game has them, so the
// game's GetCursorPos import goes through this, and hud_title_cursor moves the point from a button to the
// row it stands for. The pointer itself stays where the player put it, and at every other time the point
// is passed on as it is. (The mod's own GetCursorPos calls do not come this way.)
static const uintptr_t RVA_IAT_CURSOR_POS = 0x2017B84;
typedef BOOL (WINAPI *CursorPosFn)(POINT *);
static CursorPosFn g_orig_cursor_pos = nullptr;
static volatile LONG g_cursor_moves = 0;
static BOOL WINAPI my_cursor_pos(POINT *p) {
    BOOL ok = g_orig_cursor_pos(p);
    if (!ok || !p) return ok;
    static HWND wnd = nullptr;
    if (!wnd) EnumWindows(find_game_window, (LPARAM)&wnd);
    RECT rc;
    POINT c = *p;
    if (!wnd || !ScreenToClient(wnd, &c) || !GetClientRect(wnd, &rc)) return ok;
    float x = (float)c.x, y = (float)c.y;
    if (!hud_title_cursor((float)(rc.right - rc.left), (float)(rc.bottom - rc.top), &x, &y)) return ok;
    c.x = (LONG)lroundf(x);
    c.y = (LONG)lroundf(y);
    if (ClientToScreen(wnd, &c)) {
        *p = c;
        InterlockedIncrement(&g_cursor_moves);
    }
    return ok;
}
static bool hook_cursor_pos() {
    void **slot = (void **)((uintptr_t)GetModuleHandleA(nullptr) + RVA_IAT_CURSOR_POS);
    void *real = (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "GetCursorPos");
    if (*slot == (void *)my_cursor_pos) return true;
    if (*slot != real) return false;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return false;
    g_orig_cursor_pos = (CursorPosFn)real;
    *slot = (void *)my_cursor_pos;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return true;
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
// Which of the revolver's three variations are held in their alternate ("Slab") form: one bit each
// (1 Piercer, 2 Marksman, 4 Sharpshooter). X swaps the one in the hand; kept in ultrasouls.ini as slab=.
static volatile int g_slab_mask = 0;
// The railcannon's zoom: the field of view and the mouse's turn rate are both multiplied by this, which
// goes to a half while its alt fire is held (CameraController.Zoom(defaultFov / 2), 300 degrees a second).
static volatile float g_zoom = 1.0f;
static volatile bool g_in_hud_draw = false;      // set while the mod itself draws, so its own depth buffer is not taken for the game's
static volatile float g_center_dist = -1;        // how far the world is along the view, by the game's ray cast (for checking the depth buffer against)
static void depth_frame(IDXGISwapChain *sc);
static volatile bool g_depth_live = false;       // the game's depth is being handed to the HUD this frame: the world hides effects per pixel
// The style meter, tuned for Dark Souls, where enemies come a few at a time and far apart: points count
// for 1.75 times ULTRAKILL's and the meter drains at half its speed (style_gain= and style_drain= in
// ultrasouls.ini; 1 and 1 are ULTRAKILL's own). Davi found the original numbers too hard to climb with.
static volatile float g_style_gain = 1.75f, g_style_drain = 0.5f;
// Test aid (C): every weapon and arm is held ready, with a tenth of a second between shots.
static volatile bool g_no_cooldown = false;
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
static volatile bool g_fx_dash_pending = false;  // set where the dash starts (the physics step), used once a frame for its streaks
static bool g_sliding = false, g_slamming = false, g_prev_shift = false, g_prev_ctrl = false;
// Slam storage. ULTRAKILL's slam is a state of its own (GroundCheck.heavyFall): while it is on the player is
// held at 100 u/s straight down and slamForce grows by 5 a second. A wall jump made during it does not end
// it: NewMovement.WallJump only sets "slamStorage", which stops the downward hold. The player then moves
// freely with the slam still on and its force still growing, and whenever they next land it is a slam's
// landing, with a jump of 3 + (force - 1) times the jump power, or 12.5 times once the force has passed
// 5.5 (0.9 s in the air), against 2.6 for a plain jump. A dash, a launch or the landing ends it.
static bool g_slam_stored = false;
static volatile LONG g_slam_stores = 0;
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
static float g_teleport_to[3];                   // a move to a place instead of by an amount (the bonfire)
static volatile bool g_teleport_abs = false;
static volatile bool g_teleport_keep_safe = false;   // the move is part of a fall test: the last place stood on stays known
// the same for a launch (an explosion throwing the player): the velocity to leave with, in m/s
static volatile bool g_launch_pending = false;
static float g_launch_v[3];
// On a rocket (v0.85): the player's velocity is the rocket's at every physics step, with no steering in the air
// and no gravity. v0.84 only threw the player along the rocket's way once a frame: between throws the keys
// still pushed the player about and gravity still pulled, so W, A, S and D dragged the player off the rocket
// they were turning and a ride (or standing on a frozen rocket) sank slowly.
static volatile bool g_ride_hold = false;
static float g_ride_v[3];
// The whiplash's pull (HookArm.FixedUpdate, state Pulling): while it lasts the player's velocity is replaced
// each step by 60 u/s straight at the hook, from the middle of the body.
static volatile bool g_whip_pull = false, g_whip_jump = false;
static volatile float g_whip_point[3];
static const float WHIP_PULL_SPEED = 60.0f * 0.5f;      // 60 u/s -> 30 m/s
static const float BODY_MID = 0.95f;                    // the eye is 1.4 u above the capsule's middle (eye height 1.65 m)
// (v0.69) whether the character still goes where its velocity says once the game no longer sees the movement
// keys: how far the controller asked it to go along the ground, and how far it went
static float g_move_want = 0, g_move_got = 0;
static bool g_move_checked = false;
static float g_safe_pos[3], g_ground_time = 0;
static bool g_have_safe = false;
// Knowing sooner that a fall has no floor to end on: the per-frame code looks straight down from a falling
// player, and when there has been nothing within VOID_REACH for a whole second (VOID_TIME) the player
// goes back, once they are VOID_DROP below where they last stood. A fall onto ground, however long, is
// left alone, and so is a jump across a gap. (v0.67 went by a single look and by 6 m, and also by places
// where a fall had killed before; both put Davi back in the middle of ordinary drops, and the second took
// a death by an explosion in the air for a death by falling.)
static const float VOID_DROP = 10.0f, VOID_REACH = 80.0f, VOID_TIME = 1.0f;
static volatile bool g_void_below = false;
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

// From the blow that kills the player until they stand at the bonfire again (see "dying, as ULTRAKILL shows
// it", further down): nothing the mod reads from the keyboard or the mouse counts.
static volatile bool g_dead = false;
// fast_respawn: the player is not let die for the game at all; R puts them back at the bonfire at once.
// respawn_enemies: that also puts every enemy back as it started, through the game's own character reset. chain_slow: how many times ULTRAKILL's 0.05 s a hit of a
// beam sent on by a coin stops the picture, and waits for the next.
// reset_enemies (the ini's key; "respawn_enemies" until v0.83, which is no longer read): at a restart, also put
// every enemy through the game's own reset. Off: an enemy that has been through it comes back unable to be
// hurt, with its body not where it is seen (Davi's play of v0.82, and its log: two rockets into hollows that
// had been reset left them at 255). Without it the living get their health back and the dead stay dead.
static volatile bool g_fast_respawn = true, g_respawn_enemies = false;
// restart_reload: a restart after enemies have died goes through the game's own warp to the bonfire (its
// WarpNextStage_Bonfire, exe+4AAF30), which loads the area afresh: the dead are back as the game makes them. With
// nobody dead the restart stays the mod's own, at once. (Bringing the dead back in place was tried at length on
// 2026-10-09: health and animation make a body stand and swing, but it stays where it died, cannot move and
// cannot be hit, and neither the game's reset, its collision switch, nor putting back what dying changed in
// its controller and physics body changes that.)
static volatile bool g_restart_reload = true;
static volatile bool g_title_menu = true;        // title_menu: Dark Souls' title screen dressed as ULTRAKILL's main menu
static volatile bool g_title_column = false;     // title_column: its buttons in ULTRAKILL's left column with its own letters (v0.83; off until it has been tried in the game)
static volatile float g_chain_slow = 1.6f;
static volatile double g_respawn_grace_until = 0;    // no damage for a moment after standing up again
static float g_proxy_now[3];                         // where the movement code last had the player, and when
static volatile double g_proxy_now_at = -100.0;
struct DeathState {
    volatile bool begin;                         // asked for by whoever saw the killing blow; taken up on the game's thread
    volatile bool seen_dead;                     // the game has had the player dead, or gone, since
    volatile bool fast;                          // held: the game does not have the player dead, and R brings them back
    bool r_up, black;                            // R has been let go since the blow; the black screen is up
    int laughs, buzz;                            // the skull's laughs so far; the black screen's hum
    double t0, gone_since;
    bool cam;                                    // the falling body is set up
    float c[3], v[3], w[3];                      // its middle, its velocity, its turning (radians a second about the world's axes)
    float R[9];                                  // its own right, up and forward
    float pitch;                                 // how far the view is tipped up from the body's forward
    float eye[3], right[3], up[3], fwd[3];       // the view this frame
    int lines, voice, line_voice;
};
static DeathState g_death;
static volatile bool g_bonfire_learn = false;    // the game has just put the player back at the bonfire: note where
static int key_down(int vk) {
    if (g_dead) return 0;
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
struct Tracked { uintptr_t havok; double seen; bool was_alive; };   // was_alive: seen with health (a body that lay there from the start is not brought back)
enum { MAX_TRACKED = 256 };
static Tracked g_tracked[MAX_TRACKED];
static const uintptr_t OFF_HAVOK_OWNER = 0x250, OWNER_DELTA = 0x4F8, OFF_CHR_TEAM = 0xD8, OFF_CHR_HP = 0x3E8;
static const uintptr_t RVA_VT_ENEMY = 0x1322E68;
static bool eye_pos(float *eye);
struct Target;
static int targets_near(const float *from, Target *out, int max_n, float max_dist);
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
struct Target { float p[3], dist; uintptr_t chr; int team, hp; bool present; float yaw; };   // yaw: which way it faces (0 is towards -Z)
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
        safe_read(havok + OFF_POS_YAW, &c.yaw, 4);
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
// (6 and up: the death screen's own, which the slowing of every sound to a stop leaves alone)
enum { CH_FREE = 0, CH_PLAYER = 1, CH_GROUND = 2, CH_GUN = 3, CH_SCREEN = 4, CH_FEET = 5, CH_DEATH = 6, CH_DEATH_LINE = 7, CH_DEATH_LOOP = 8 };
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
                if (g_launch_pending) {
                    // NewMovement.Launch: whatever the player was doing ends (a dash, a slide, a slam) and
                    // the velocity is replaced outright; it counts as a jump for the half second after
                    g_launch_pending = false;
                    g_vx = g_launch_v[0];
                    g_vy = g_launch_v[1];
                    g_vz = g_launch_v[2];
                    g_air = true;
                    g_air_jumped = true;
                    g_air_steps = 0;
                    g_air_time = 0;
                    g_dash_left = 0;
                    g_sliding = false;
                    g_slamming = false;
                    g_slam_force = 0;
                    g_slam_window = 0;
                    g_wall_cd = 0.2f;
                    g_jump_req = false;
                }
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
                    // WallJump during a slam stores it. In ULTRAKILL the slam has to be a frame ahead of the
                    // jump (its Update takes the jump first); here the two keys going down in the same step
                    // count as well, so "both at once" does not hang on which the game saw first (ours).
                    if (!g_slamming && key_down(VK_CONTROL) && !g_prev_ctrl && g_air_time >= V1_SLAM_MIN_AIR) {
                        g_slamming = true;
                        g_slam_force = 1.0f;
                        InterlockedIncrement(&g_slams);
                    }
                    if (g_slamming && !g_slam_stored) {
                        g_slam_stored = true;
                        InterlockedIncrement(&g_slam_stores);
                    }
                    g_slam_time = 1.0f;         // (not a slam "pressed right above the ground", whenever it lands)
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
                    g_fx_dash_pending = true;
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
                if (!g_slamming) g_slam_stored = false;
                if (g_slamming && g_slam_stored) g_slam_force += 5.0f * dt;      // "slamForce += Time.deltaTime * 5f" goes on
                bool pulled = g_whip_pull;
                if (pulled) {
                    // HookArm.ForceGroundCheck: the ground check is switched off for as long as the pull
                    // lasts, and a slide ends (a slam is ended here too)
                    g_sliding = false;
                    g_slamming = false;
                    g_slam_force = 0;
                    if (!g_air) {
                        g_air = true;
                        g_air_jumped = true;
                        g_air_steps = 0;
                        g_air_time = 0;
                    }
                }

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
                if (g_slamming && !g_slam_stored) {
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
                } else if (pulled) {
                    // "if (!boost || sliding) rb.velocity = (hookPoint - position).normalized * 60": a dash goes its own way
                    const float *feet = (const float *)((uintptr_t)self + OFF_POS_X);
                    float d[3] = {g_whip_point[0] - feet[0], g_whip_point[1] - (feet[1] + BODY_MID), g_whip_point[2] - feet[2]};
                    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (len > 0.01f) {
                        g_vx = d[0] / len * WHIP_PULL_SPEED;
                        g_vy = d[1] / len * WHIP_PULL_SPEED;
                        g_vz = d[2] / len * WHIP_PULL_SPEED;
                    }
                    vel[1] = g_vy;
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
                if (g_ride_hold) {
                    g_vx = g_ride_v[0];
                    g_vy = g_ride_v[1];
                    g_vz = g_ride_v[2];
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
            if (g_teleport_abs) memcpy(p, g_teleport_to, sizeof(p));
            else
                for (int i = 0; i < 3; i++) p[i] += g_teleport_delta[i];
            g_teleport_abs = false;
            set_proxy_pos(p);
            g_vx = g_vy = g_vz = 0;
            if (!g_teleport_keep_safe) g_have_safe = false;
            g_teleport_keep_safe = false;
            g_ground_time = 0;
            return;
        }
    }
    if (g_ctrl) {
        float p[3];
        if (proxy_pos(proxy, p)) {
            float dt = *(float *)((uintptr_t)step_info + 8);
            g_ground_time = g_air ? 0 : g_ground_time + dt;
            memcpy(g_proxy_now, p, sizeof(p));
            g_proxy_now_at = now_s();
            {
                // Hiding the movement keys rests on the character going where its velocity says while the
                // game believes it is standing still, which a dash from standstill has always done but a
                // walk has not been seen to. So the first 10 m the controller asks for along the ground are
                // measured, once: if the character covered less than a third of them the keys are given
                // back to the game for the rest of the session.
                static float last[3];
                static bool have_last = false;
                float want = sqrtf(g_vx * g_vx + g_vz * g_vz) * dt;
                if (have_last && g_hide_move && !g_move_checked && !g_air && !g_sliding && g_dash_left <= 0 && g_wall_age > 0.3f && want > 0.02f) {
                    float mx = p[0] - last[0], mz = p[2] - last[2];
                    g_move_want += want;
                    g_move_got += sqrtf(mx * mx + mz * mz);
                    if (g_move_want >= 10.0f) {
                        g_move_checked = true;
                        bool ok = g_move_got >= g_move_want / 3.0f;
                        logf("movement keys hidden from the game: the character went %.1f m of the first %.1f m asked for; %s\n", g_move_got, g_move_want,
                             ok ? "kept hidden" : "GIVEN BACK to the game for this session");
                        if (!ok) g_hide_move = false;
                    }
                }
                memcpy(last, p, sizeof(last));
                have_last = true;
            }
            if (g_ground_time > 0.5f) {
                memcpy(g_safe_pos, p, sizeof(g_safe_pos));
                g_have_safe = true;
            } else if (g_air && g_have_safe && g_vy < 0 && g_void_below && p[1] < g_safe_pos[1] - VOID_DROP) {
                logf("rescue: put back from %.1f m below the last place stood on (nothing under the fall for %.1f s)\n", g_safe_pos[1] - p[1], VOID_TIME);
                g_void_below = false;
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
        if (g_whip_pull) {
            // pulled along or into the ground: nothing lands until the pull is over
        } else if (ctrl_vy < -0.5f && got > want * 0.3f) {
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
static volatile float g_eye_height = 1.65f;       // Davi's setting
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
static bool g_quick_draw = true;                 // the revolver fires the moment it is drawn (ULTRAKILL waits 0.36 s)
static bool g_hide_move_setting = true;          // what the ini says; g_hide_move may be cleared for a session without changing it
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
    fprintf(f, "sensitivity=%.6f\nfov_scale=%.2f\neye_height=%.2f\nvolume=%.2f\ncamera_tilt=%d\nparry_freeze=%d\nhud_motion=%d\nslab=%d\nstyle_gain=%.2f\nstyle_drain=%.2f\nhide_move_keys=%d\nquick_draw=%d\nfast_respawn=%d\nreset_enemies=%d\nchain_slow=%.2f\ntitle_menu=%d\ntitle_column=%d\nrestart_reload=%d\n", (float)g_sens,
            (float)g_fov_scale, (float)g_eye_height, (float)g_volume, g_camera_tilt ? 1 : 0, g_parry_freeze ? 1 : 0, g_hud_motion ? 1 : 0, (int)g_slab_mask & 7,
            (float)g_style_gain, (float)g_style_drain, g_hide_move_setting ? 1 : 0, g_quick_draw ? 1 : 0, g_fast_respawn ? 1 : 0, g_respawn_enemies ? 1 : 0,
            (float)g_chain_slow, g_title_menu ? 1 : 0, g_title_column ? 1 : 0, g_restart_reload ? 1 : 0);
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
        else if (!strcmp(key, "fast_respawn")) g_fast_respawn = v != 0.0f;
        else if (!strcmp(key, "title_menu")) g_title_menu = v != 0.0f;
        else if (!strcmp(key, "title_column")) g_title_column = v != 0.0f;
        else if (!strcmp(key, "restart_reload")) g_restart_reload = v != 0.0f;
        else if (!strcmp(key, "reset_enemies")) g_respawn_enemies = v != 0.0f;
        else if (!strcmp(key, "chain_slow") && v >= 0.5f && v <= 6.0f) g_chain_slow = v;
        else if (!strcmp(key, "hud_motion")) g_hud_motion = v != 0.0f;
        else if (!strcmp(key, "slab")) g_slab_mask = (int)v & 7;
        else if (!strcmp(key, "style_gain") && v >= 0.1f && v <= 10.0f) g_style_gain = v;
        else if (!strcmp(key, "style_drain") && v >= 0.0f && v <= 10.0f) g_style_drain = v;
        else if (!strcmp(key, "hide_move_keys")) g_hide_move = g_hide_move_setting = v != 0.0f;
        else if (!strcmp(key, "quick_draw")) g_quick_draw = v != 0.0f;
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
        if (space && !prev_space) {
            if (g_whip_pull) g_whip_jump = true;         // HookArm.Update: a jump out of the pull (update_whiplash)
            else g_jump_req = true;
        }
        prev_space = space;
    }
    if (!on) return r;

    // The view direction is ours: the follow camera's own direction drifts when the character moves,
    // because it trails behind and looks at them.
    g_yaw += (float)mx * g_sens * g_zoom;
    g_pitch -= (float)my * g_sens * g_zoom;
    // ULTRAKILL lets the view go to straight up and straight down. Until v0.66 this stopped at 86 degrees,
    // which left a coin tossed "straight up" with 1.4 u/s of forward speed: it came down ahead of the player.
    if (g_pitch > 1.5690f) g_pitch = 1.5690f;
    if (g_pitch < -1.5690f) g_pitch = -1.5690f;
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
    lens[0] = BASE_FOV * g_fov_scale * g_fov_kick * g_zoom;
    const float *p = (const float *)(pos + OFF_POS_X);
    m[12] = p[0] + sy * EYE_FORWARD;
    float drop_target = g_sliding ? SLIDE_EYE_DROP : 0.0f, k = dt > 0 && dt < 0.1f ? 1.0f - expf(-dt / 0.06f) : 1.0f;
    g_eye_drop += (drop_target - g_eye_drop) * k;
    m[13] = p[1] + g_eye_height - g_eye_drop;
    m[14] = p[2] + cy * EYE_FORWARD;
    if (g_dead && g_death.cam) {
        // dead: the view is the falling body's (see "dying, as ULTRAKILL shows it")
        for (int i = 0; i < 3; i++) {
            m[i] = g_death.right[i];
            m[4 + i] = g_death.up[i];
            m[8 + i] = g_death.fwd[i];
            m[12 + i] = g_death.eye[i];
        }
    }
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
// ---- other characters' projectiles, for the Feedbacker's parry. BulletIns::Init (decompiled) leaves, in
// the projectile object: +0x10 its position, +0x20 its rotation, +0x30 its velocity (the launch direction
// times the row's initVellocity), +0x40 a second copy of that, +0x88 its state (1 or 2 in flight, 3 its
// explosion), +0x94 its bullet row and +0x9C its shooter's handle (the 0x160-byte block at +0x90 is the
// `params` layout), +0x1F0 a zero and +0x1F4 the row's life. Each one not fired by the player is noted
// here; the parry reads where it is from the object. That the position at +0x10 is kept current in
// flight is taken from the layout, not yet seen in the game.
struct EnemyShot { uintptr_t bullet; int id; double at; float start[3]; };
enum { MAX_ENEMY_SHOTS = 32 };
static EnemyShot g_enemy_shots[MAX_ENEMY_SHOTS];
static int g_enemy_shot_next = 0;
static volatile LONG g_enemy_shots_seen = 0, g_projectile_parries = 0;
static const uintptr_t OFF_BULLET_POS = 0x10, OFF_BULLET_VEL = 0x30, OFF_BULLET_VEL2 = 0x40, OFF_BULLET_STATE = 0x88, OFF_BULLET_ID = 0x94,
                       OFF_BULLET_SHOOTER = 0x9C, OFF_BULLET_LIFE = 0x1F4;

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
    uint64_t result = g_orig_bullet_init(bullet, params, aim, a4, a5, a6);
    if (!mine && params && bullet && g_ctrl) {
        EnemyShot &s = g_enemy_shots[g_enemy_shot_next++ % MAX_ENEMY_SHOTS];
        s.bullet = (uintptr_t)bullet;
        s.id = *(int *)(params + 0x04);
        s.at = now_s();
        const float *launch = (const float *)a4;
        for (int r = 0; r < 3; r++) s.start[r] = launch ? launch[r * 4 + 3] : ((const float *)(params + 0xE0))[r * 4 + 3];
        LONG n = InterlockedIncrement(&g_enemy_shots_seen);
        if (n <= 12) {
            float pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0}, life = 0;
            int state = 0;
            safe_read(s.bullet + OFF_BULLET_POS, pos, 12);
            safe_read(s.bullet + OFF_BULLET_VEL, vel, 12);
            safe_read(s.bullet + OFF_BULLET_LIFE, &life, 4);
            safe_read(s.bullet + OFF_BULLET_STATE, &state, 4);
            logf("enemy projectile %ld: row %d from handle %08X, launched at (%.1f %.1f %.1f); object says position (%.1f %.1f %.1f) velocity (%.1f %.1f %.1f) life %.2f state %d\n",
                 (long)n, s.id, *(unsigned *)(params + 0x0C), s.start[0], s.start[1], s.start[2], pos[0], pos[1], pos[2], vel[0], vel[1], vel[2], life, state);
        }
    }
    return result;
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
// The alternate revolvers' plain, charged and Sharpshooter shots, and the two railcannons' beams (tools/patch_v1.py)
static const int BEHAVIOR_SLAB = 9000160, BEHAVIOR_SLAB_CHARGED = 9000161, BEHAVIOR_SLAB_SHARP = 9000162;
static const int BEHAVIOR_RAIL = 9000170, BEHAVIOR_RAIL_MALICIOUS = 9000171;
enum { EXPLODE_PLAIN = 0, EXPLODE_SUPER = 1, EXPLODE_PUMP = 2, EXPLODE_MALICIOUS = 3, EXPLODE_ULTRA = 4, EXPLODE_BIG = 5, EXPLODE_SUPER_BIG = 6,
       EXPLODE_CANNONBALL = 7, EXPLODE_KINDS };
// What each is in ULTRAKILL (the Explosion component of the prefab's ball, and of the faint shell that runs ahead):
//   plain      'Explosion'                         speed 1     to  6 u, shell 2.5  to  8 u
//   super      'Explosion Super'                   speed 1.75  to 12 u, shell 2.5  to 16 u, its own redder picture
//   pump       'Explosion' with size and speed x 1.5 (Shotgun.Shoot at three pumps)
//   Malicious  'Explosion Malicious Railcannon'    speed 1.5   to 13.5 u, shell 4  to 18 u
//   ultra      'Explosion Super' with size and speed x 2: a core set off by the Malicious Railcannon's beam
//              (RevolverBeam: Explode(..., super, 2f, ultrabooster: true)), the "ultraboost"
//   big        'Explosion' with size and speed x 1.5: a rocket that goes off while frozen, or is shot in the air
//   super big  'Explosion Super' the same way: a levelled-up rocket that goes off while frozen
//   cannonball 'Explosion Big'                     speed 1     to 13.5 u, shell 2.5 to 18 u: a cannonball shot in the air
struct ExplosionKind { float speed, max_size, shell_speed, shell_size; int damage; };
static const ExplosionKind EXPLOSIONS[EXPLODE_KINDS] = {
    {1.0f, 6.0f, 2.5f, 8.0f, 35}, {1.75f, 12.0f, 2.5f, 16.0f, 35}, {1.5f, 9.0f, 3.75f, 12.0f, 50}, {1.5f, 13.5f, 4.0f, 18.0f, 50}, {3.5f, 24.0f, 5.0f, 32.0f, 50},
    {1.5f, 9.0f, 3.75f, 12.0f, 35}, {2.625f, 18.0f, 3.75f, 24.0f, 35}, {1.0f, 13.5f, 2.5f, 18.0f, 50}};
static void explode(const float *p, int kind, bool spare_player = false);
// The alternate revolver (Revolver with altVersion): it is ready again when its Shoot clip's ReadyGun
// event comes, 1.19 s after the shot, and the clip's Click event at 0.89 s is the hammer coming back.
// Put away before that Click and drawn again within 2 s, it comes out with the slow PickUpWithReload
// (ready after 1.15 s) instead of PickUp (0.33 s): WeaponCharges.revaltpickupcharges.
static const double SLAB_READY = 1.19, SLAB_CLICK = 0.89;
static double g_slab_slow_until[3] = {0, 0, 0}, g_slab_click_at = -1;
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
// The Animator plays a clip at its state's speed, and the clip's events come that much sooner. The speeds
// are in the controllers (ShotgunNew, Revolver Heavy, Revolver2, PunchRed), not in the clips:
//   shotgun   FireWithReload 1.1, FireWithThrowReload 1.5, Pump (the clip Pump2) 1.65, FireWithPump 1.1
//   revolver  Shoot, Shoot2, Shoot3 1.2          alternate revolver  PickUp 0.85
//   Knuckleblaster  Jab (the clip Punch) 0.85
// Until v0.66 every clip ran at 1: the shotgun's core was ready after 3.08 s where ULTRAKILL's is after 2.05.
static volatile float g_rev_clip_speed = 1.0f, g_arm2_clip_speed = 1.0f;
static volatile int g_shot_turns = 0;             // shots whose turn of the cylinder is still to be handed to it
static float clip_speed(const char *clip);
static void play_revolver(const char *clip) {
    g_rev_clip_start = now_s();
    g_rev_clip_speed = clip_speed(clip);
    g_rev_clip = clip;
}
static const char *volatile g_arm2_clip = nullptr;          // the Knuckleblaster: Punch, PunchBlast
// V1 has one left arm: whichever arm starts a clip, the other one's is dropped, so only one is ever in view.
static void play_arm(const char *clip, double already_in = 0.0) {
    g_arm_clip_start = now_s() - already_in;
    g_arm_clip = clip;
    g_arm2_clip = nullptr;
}
static volatile double g_arm2_clip_start = -100.0;
static void play_arm2(const char *clip, double already_in = 0.0) {
    g_arm2_clip_start = now_s() - already_in;
    g_arm2_clip_speed = !strcmp(clip, "Punch") ? 0.85f : 1.0f;
    g_arm2_clip = clip;
    g_arm_clip = nullptr;
}
static void play_revolver_shot() {
    static const char *const shots[3] = {"Shoot", "Shoot2", "Shoot3"};
    play_revolver(shots[rand() % 3]);
    g_shot_turns = g_shot_turns + 1;                 // Revolver.Shoot: cylinder.DoTurn(), unless it is the alternate one
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
// shrink: thins to nothing instead of fading. seen: which eighths of the line were in plain sight of
// the eye when it was made, one bit each, so a ricochet that runs behind a wall is not drawn through it.
struct Tracer { float a[3], b[3]; double born; float life, width, r, g, bl; bool shrink; uint8_t seen; int kind; };   // kind: one of the BEAM_ looks below, or 0 for a plain line
static int ray_blocked(const float *a, const float *b);
static bool eye_pos(float *eye);
// Whether the eye has a clear line to a point (the game's own sight ray; within a metre it always has).
static bool in_sight(const float *p) {
    if (g_depth_live) return true;               // the game's depth buffer decides, pixel by pixel
    float eye[3];
    return !eye_pos(eye) || ray_blocked(eye, p) != 1;
}
enum { MAX_TRACERS = 24 };
static Tracer g_tracers[MAX_TRACERS];
static int g_tracer_next = 0;
static const float TRACER_WHITE[3] = {1.0f, 0.95f, 0.7f}, TRACER_BLUE[3] = {0.2f, 0.8f, 1.0f}, TRACER_GOLD[3] = {1.0f, 0.8f, 0.2f};
// `check`: off for a line known to be in sight from end to end (a shot from the gun to where the view ends).
static void add_tracer(const float *a, const float *b, float life, float width, const float *rgb, bool shrink = false, bool check = true) {
    Tracer &t = g_tracers[g_tracer_next++ % MAX_TRACERS];
    memcpy(t.a, a, sizeof(t.a));
    memcpy(t.b, b, sizeof(t.b));
    t.seen = 0xFF;
    if (check) {
        t.seen = 0;
        for (int k = 0; k < 8; k++) {
            float f = (k + 0.5f) / 8.0f, mid[3] = {a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f};
            if (in_sight(mid)) t.seen |= (uint8_t)(1 << k);
        }
    }
    t.born = now_s();
    t.life = life;
    t.width = width;
    t.r = rgb[0]; t.g = rgb[1]; t.bl = rgb[2];
    t.shrink = shrink;
    t.kind = 0;
}
// The revolver's beam (the "Revolver Beam" prefab): a white line 0.25 u wide whose width runs down at 1.5
// a second, so it is gone in a sixth of a second.
static const float BEAM_WHITE[3] = {1.0f, 1.0f, 1.0f};
static const float BEAM_WIDTH = 0.25f * UK_UNIT, BEAM_LIFE = 0.25f / 1.5f;
// ---- the beams, as their prefabs' LineRenderers have them: a flat line of the prefab's width whose colour
// runs from one at the gun to another at the far end, and whose width runs down at 1.5 u a second
// (RevolverBeam.Update), which is all the fading there is.
//   'Revolver Beam'                      0.25 u   white to gold
//   'Revolver Beam Alternative'          0.35 u   orange to white
//   'Revolver Beam Super' (+ Alt.)       0.5 u    white to cyan
//   'Revolver Beam Sharp' (+ Alt.)       0.5 u    white to red
//   'Railcannon Beam'                    1 u      white to cyan, inside two more lines that copy its path and
//                                                 shrink with it: 'Line (1)', the 'lineglow16' picture ten
//                                                 times as wide, and 'Line', the electric arcs (ElectricityLine:
//                                                 one of ten pictures, a width of 15 to 20 times the beam's
//                                                 and two colours off the gradient, all picked afresh every
//                                                 0.05 s; the picture repeats every 40 u along the line)
//   'Railcannon Beam Malicious'          1 u      white to orange, inside the 'charge2' picture three times as wide
// (the looks themselves, BEAM_LOOKS, are in hud.h and hud.cpp, where the beams are drawn)
static void add_beam(const float *a, const float *b, int kind, bool check = true) {
    const BeamLook &k = BEAM_LOOKS[kind];
    add_tracer(a, b, k.width_u / 1.5f, k.width_u * UK_UNIT, k.c0, true, check);
    g_tracers[(g_tracer_next - 1) % MAX_TRACERS].kind = kind;
}

// ---- ULTRAKILL's particle effects, each with the numbers its prefab carries (read with tools/uk_dump.py):
//   LaserHitParticle  where a revolver shot lands: 60 of the "blooddrop" picture thrown out over the half
//                     of space the surface faces, 1 to 50 u/s, 0.5 s, 0.25 u across, twice gravity
//   BulletSpark       where a pellet lands: 5 of the "spark" picture in a 45 degree cone, 1 to 10 u/s,
//                     0.5 s, 0.25 u, gravity
//   the explosion's sparks: 30 to 50 plain white squares, 200 u/s for 0.175 s, 0.5 to 2 u (drawn smaller)
//   DodgeParticle     a dash: 20 faint streaks that start 10 u ahead and fly back past the player at 30 to
//                     50 u/s for 0.5 s (SlideParticle is the same kind, 20 a second at 40 u/s)
// Gravity is ULTRAKILL's 40 u/s^2.
struct Particle { bool alive, streak, hidden; float p[3], v[3]; double born; float life, size, gravity; const char *sprite; float r, g, b, a; float trail; };   // hidden: behind something, as of its last check
enum { MAX_PARTICLES = 420 };
static Particle g_particles[MAX_PARTICLES];
static int g_particle_next = 0;
static const float UK_GRAVITY = 40.0f * UK_UNIT;
static Particle &new_particle(const float *at, const float *vel, float life, float size, float gravity, const char *sprite) {
    Particle &p = g_particles[g_particle_next++ % MAX_PARTICLES];   // when all are in use the oldest goes
    p = Particle{};
    p.alive = true;
    memcpy(p.p, at, sizeof(p.p));
    memcpy(p.v, vel, sizeof(p.v));
    p.born = now_s();
    p.life = life;
    p.size = size;
    p.gravity = gravity;
    p.sprite = sprite;
    p.r = p.g = p.b = p.a = 1.0f;
    return p;
}
static void rand_unit(float *d) {
    float len;
    do {
        for (int i = 0; i < 3; i++) d[i] = frand(-1.0f, 1.0f);
        len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    } while (len < 0.1f || len > 1.0f);
    for (int i = 0; i < 3; i++) d[i] /= len;
}
static void fx_beam_hit(const float *at, const float *normal) {
    if (!in_sight(at)) return;
    for (int k = 0; k < 60; k++) {
        float d[3], v[3], speed = frand(1.0f, 50.0f) * UK_UNIT;
        rand_unit(d);
        if (d[0] * normal[0] + d[1] * normal[1] + d[2] * normal[2] < 0)
            for (float &x : d) x = -x;
        for (int i = 0; i < 3; i++) v[i] = d[i] * speed;
        new_particle(at, v, 0.5f, 0.25f * UK_UNIT, 2.0f * UK_GRAVITY, "blooddrop");
    }
}
static void fx_pellet_hit(const float *at, const float *normal) {
    if (!in_sight(at)) return;
    for (int k = 0; k < 5; k++) {
        float d[3], v[3], speed = frand(1.0f, 10.0f) * UK_UNIT, len = 0;
        rand_unit(d);
        for (int i = 0; i < 3; i++) {                 // within 45 degrees of the surface's own direction
            d[i] = normal[i] + d[i] * frand(0.0f, 1.0f);
            len += d[i] * d[i];
        }
        len = len > 1e-6f ? sqrtf(len) : 1.0f;
        for (int i = 0; i < 3; i++) v[i] = d[i] / len * speed;
        new_particle(at, v, 0.5f, 0.25f * UK_UNIT, UK_GRAVITY, "spark");
    }
}
// The burst of 30 to 50 flying bits each explosion prefab has ('Particle System': speed 200 u/s). The plain
// one's are white squares 0.5 to 2 u that live 0.17 s; the Malicious Railcannon's the same but for 0.5 s,
// so they fly three times as far; the super one's are the blood-drop picture in (1, 0.13, 0), 0.1 to 1 u,
// for 0.25 s. kind: 0 plain, 1 super, 3 Malicious (the EXPLODE_ numbers).
static void fx_explosion_sparks(const float *at, int kind = 0) {
    if (!in_sight(at)) return;
    int count = 30 + rand() % 21;
    bool red = kind == 1 || kind == 4 || kind == 6;
    for (int k = 0; k < count; k++) {
        float d[3], v[3];
        rand_unit(d);
        for (int i = 0; i < 3; i++) v[i] = d[i] * 200.0f * UK_UNIT;
        if (red) {
            Particle &p = new_particle(at, v, 0.25f, frand(0.1f, 1.0f) * UK_UNIT, 0.0f, "blooddrop");
            p.g = 0.13f;
            p.b = 0.0f;
        } else {
            new_particle(at, v, kind == 3 ? 0.5f : 0.175f, frand(0.5f, 2.0f) * UK_UNIT * 0.4f, 0.0f, nullptr);   // ours: 0.4 of the prefab's size
        }
    }
}
// The slam's streaks ('FallParticle', which NewMovement hangs on the player for as long as a slam lasts, a
// stored one included): 20 at once and 20 a second, born in a box 15 u wide, 5 u deep and 5 u tall that sits
// 2.8 u above the player's middle and 2.1 u ahead, each going down at 40 u/s for half a second and leaving
// a trail 0.1 u wide in the trail's (0.4, 0.28, 0), added to the picture, that fades from alpha 0.49. They
// are left in the world as the player drops at 100 u/s, which is what makes them rush up the screen.
static void fx_slam_streaks(const float *mid, const float *right, const float *ahead, int count) {
    for (int k = 0; k < count; k++) {
        float a = frand(-7.5f, 7.5f) - 0.24f, b = frand(-2.5f, 2.5f) + 2.087f, c = frand(-2.5f, 2.5f) + 2.78f, at[3];
        for (int i = 0; i < 3; i++) at[i] = mid[i] + (right[i] * a + ahead[i] * b + (i == 1 ? c : 0.0f)) * UK_UNIT;
        const float v[3] = {0.0f, -40.0f * UK_UNIT, 0.0f};
        Particle &p = new_particle(at, v, 0.5f, 0.1f * UK_UNIT, 0.0f, nullptr);
        p.streak = true;
        p.trail = 0.5f;                               // the whole of its path so far
        p.r = 1.0f; p.g = 0.7f; p.b = 0.0f;           // (the additive dark amber, as it shows over a picture)
        p.a = 0.49f;
    }
}
// `dir` is the way the player is going; the streaks come the other way, out of a box `half` across ahead.
static void fx_wind(const float *middle, const float *dir, int count, float speed_lo, float speed_hi, float half_wide, float half_tall) {
    float side[3] = {dir[2], 0, -dir[0]};
    for (int k = 0; k < count; k++) {
        float at[3], v[3], a = frand(-half_wide, half_wide), b = frand(-half_tall, half_tall), c = frand(-half_wide, half_wide), speed = frand(speed_lo, speed_hi) * UK_UNIT;
        for (int i = 0; i < 3; i++) {
            at[i] = middle[i] + dir[i] * (10.0f * UK_UNIT + c) + side[i] * a + (i == 1 ? b : 0.0f);
            v[i] = -dir[i] * speed;
        }
        Particle &p = new_particle(at, v, 0.5f, 0.2f * UK_UNIT, 0.0f, nullptr);
        p.streak = true;
        p.r = p.g = p.b = 0.85f;
        p.a = 0.3f;                                   // the trail's additive grey, a quarter strength
    }
}
static void update_particles(double t, float dt) {
    for (Particle &p : g_particles) {
        if (!p.alive) continue;
        if (t - p.born > p.life) { p.alive = false; continue; }
        p.v[1] -= p.gravity * dt;
        for (int i = 0; i < 3; i++) p.p[i] += p.v[i] * dt;
    }
    // Sight is checked for 48 particles a frame, in turn, so each one's answer is at most a few frames old.
    static int cursor = 0;
    for (int k = 0; k < 48; k++) {
        Particle &p = g_particles[cursor++ % MAX_PARTICLES];
        if (p.alive) p.hidden = !in_sight(p.p);
    }
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
        g_style_meter = g_style_meter + points * g_style_gain;
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
            meter -= dt * STYLE_RANKS[rank].drain * 15.0f * g_style_drain;
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
struct ScreenBlood { float x, y; int sprite; double born; };
enum { MAX_SCREEN_BLOOD = 12 };
static ScreenBlood g_screen_blood[MAX_SCREEN_BLOOD];
static int g_screen_blood_next = 0;
// NewMovement.GetHealth: a heal of more than 5 puts a splash of blood on the screen ('ScreenBlood': one of
// five pictures, somewhere within 400 units either side of the middle and 250 above or below, in
// (0.68, 0, 0) at alpha 0.49, which runs down by 1 a second), whether or not there was any health to gain.
// (v0.73 only did it when health was gained: with full health, which is most of the time, no blood showed
// for a shotgun kill, a punch or a parry.)
static void splash_screen_blood() {
    ScreenBlood &b = g_screen_blood[g_screen_blood_next++ % MAX_SCREEN_BLOOD];
    b.x = (float)(rand() % 801 - 400);
    b.y = (float)(rand() % 501 - 250);
    b.sprite = rand() % 5;
    b.born = now_s();
}
static void heal_player(float amount) {
    uintptr_t chr = g_player_chr;
    int hp = 0, max_hp = 0;
    if (amount > 5.0f) splash_screen_blood();
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
// hidden: out of the eye's sight this frame. trail: where it was at the last few twenty-fourths of a second
// (the coin's TrailRenderer keeps half a second of its path).
enum { COIN_TRAIL = 12 };
struct Coin {
    bool alive, shot, flashed, charged, hidden;
    float p[3], v[3], thrown_y;
    double born, reflect_at;
    int power, hit_times;
    int carry;                                   // the row of the beam that hit it, when the coin passes that beam on (0: its own)
    bool blasted;                                // thrown by the Knuckleblaster's wave (once), and how much longer it then lasts
    float extra_life;
    int more_hits;                               // hits a Slab beam has gained from coins caught in their flash, so far along the chain
    float trail[COIN_TRAIL][3];
    int trail_n;
    double trail_at;
};
enum { MAX_COINS = 8 };
static Coin g_coins[MAX_COINS];
static volatile int g_variation = 1;             // the revolver's: 0 Piercer, 1 Marksman, 2 Sharpshooter
enum { WEAPON_REVOLVER = 0, WEAPON_SHOTGUN = 1, WEAPON_RAILCANNON = 2, WEAPON_SAWLAUNCHER = 3, WEAPON_ROCKET = 4 };
static volatile int g_weapon = WEAPON_REVOLVER;
static volatile int g_shotgun_var = 0;           // 0 Core Eject, 1 Pump Charge
static volatile int g_rail_var = 0;              // 0 Electric, 1 Malicious
static bool slab_held() { return g_weapon == WEAPON_REVOLVER && ((g_slab_mask >> g_variation) & 1); }
static void slab_fired(double t) {
    g_slab_slow_until[g_variation] = t + 2.0;
    g_slab_click_at = t + SLAB_CLICK;
}
static float clip_speed(const char *clip) {
    if (g_weapon == WEAPON_SHOTGUN) {
        if (!strcmp(clip, "FireWithThrowReload")) return 1.5f;
        if (!strcmp(clip, "Pump2")) return 1.65f;
        if (!strcmp(clip, "FireWithPump") || !strcmp(clip, "FireWithReload")) return 1.1f;
    } else if (g_weapon == WEAPON_REVOLVER) {
        bool slab = slab_held();
        if (!strncmp(clip, "Shoot", 5) && strcmp(clip, "ShootTwirl")) return slab ? 1.0f : 1.2f;
        if (!strcmp(clip, "PickUp")) return slab ? 0.85f : 1.0f;
    }
    return 1.0f;
}
// RevolverCylinder: the cylinder turns a third of a turn after each shot at 240 degrees a second (a
// quarter at 480 on the alternate revolver, when its hammer comes back), and spins freely at ten times the
// charge while the Piercer's shot is wound up. In degrees; the HUD turns the bone.
static float g_cyl_angle = 0, g_cyl_target = 0;
static bool g_cyl_free = false;
static volatile float g_coin_charge = 400.0f;
static volatile LONG g_coins_thrown = 0, g_coins_hit = 0, g_coin_chains = 0, g_coin_shots = 0, g_coin_misses = 0;
static const int BEHAVIOR_COIN = 9000120;        // + (power - 2), up to power 5
static const int BEHAVIOR_COIN_BACK = 9000180;   // the same, not stopped by a shield: for a shot that arrives from behind
static const int BEHAVIOR_COIN_ALT = 9000300;    // + quarters of a revolver shot's damage: a charged or railcannon beam sent on by a coin
enum { COIN_ALT_MAX = 96 };                      // up to 24 revolver shots' worth
static const float COIN_FORWARD = 20.0f * UK_UNIT, COIN_UP = 15.0f * UK_UNIT, COIN_DROP = 0.5f * UK_UNIT, COIN_GRAVITY = 40.0f * UK_UNIT;
static const float COIN_LIFE = 5.0f, COIN_ARMED = 0.1f, COIN_REFLECT_DELAY = 0.1f, COIN_REFILL = 25.0f;
static const float COIN_HIT_RADIUS = 0.5f;       // ours: how close the line of fire must pass (V1's coin has a generous collider)
static const float COIN_RANGE = 150.0f;
static const float COIN_MAX_DROP = 15.0f;        // ours

static float dist3(const float *a, const float *b) {
    float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
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
    // The camera sits EYE_FORWARD ahead of the character's middle, level (the camera hook puts it there).
    // Until v0.64 this left that out, so everything measured or drawn from "the eye" (a tossed coin's
    // start, tracers, effects) was worked out from a point 0.2 m behind the real view.
    eye[0] = p[0] + sinf(g_yaw) * EYE_FORWARD;
    eye[1] = p[1] + g_eye_height - g_eye_drop;
    eye[2] = p[2] + cosf(g_yaw) * EYE_FORWARD;
    return true;
}
static double g_wave_until = -1.0;                // the Knuckleblaster's wave: until when it throws coins, and where it is
static float g_wave_p[3];
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
// Coin.Bounce (the whiplash's hook passing through a coin): the coin is replaced by a fresh one of the same
// power in the same place, going straight up at 25 u/s, with its time in the air starting over.
static void bounce_coin(Coin &c, double t) {
    if (!c.alive || c.shot) return;
    c.v[0] = c.v[2] = 0;
    c.v[1] = 25.0f * UK_UNIT;
    c.born = t;
    c.thrown_y = c.p[1];
    c.flashed = false;
    c.trail_n = 0;
    sound_play("coin_toss", 0.6f, 1.2f);
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

struct Coin;
static bool coin_hits_core(const Coin &c);
static bool coin_hits_flying(const Coin &c, double t);      // (a rocket or a cannonball in the air: with the rocket launcher)
static void explosion_sets_off(const float *p, float radius);
static void stun_enemy(uintptr_t chr, double t);
// ---- A beam that hits more than once (RevolverBeam.PiercingShotCheck). ULTRAKILL's piercing beams strike
// what is on their line one hit at a time. Every hit on a living enemy stops time for 0.05 s
// (TimeController.HitStop: the picture stands still, with no flash) and the next hit comes 0.05 s after
// that (0.025 s for a railcannon's beam); one enemy takes up to maxHitsPerTarget of them and the beam has
// hitAmount in all; what is left when an enemy is dead or has had its share goes to the next enemy
// further along the same line. A row of hits on one enemy is therefore a stutter of the whole game, a
// tenth of a second a hit: the "slowing down" of a Slab shot sent on by coins. This is how a beam that a
// coin sends on is delivered here. (Until v0.77 its hits were added up and landed as one, at once.)
struct BeamChain {
    bool live;
    float from[3], dir[3], at[3];                // the coin, the beam's line, and where the last hit landed
    uintptr_t chr;                               // the enemy being hit
    int row, look, hits_left, per_target, on_target, count, power;
    float gap, reach;                            // seconds between hits; how far along the line the enemy being hit is
    double next_at;
};
enum { MAX_BEAM_CHAINS = 4 };
static BeamChain g_beam_chains[MAX_BEAM_CHAINS];
static volatile float g_hitstop_request = 0;     // seconds; carried out when the frame is presented, like a parry's freeze
static volatile LONG g_chain_hits = 0, g_hitstops = 0;
// A short burst of light at a point of the world: where a coin sends a beam on, where a beam's hit lands.
struct Flash { float p[3], size, life, rgb[3]; double born; };
enum { MAX_FLASHES = 12 };
static Flash g_flashes[MAX_FLASHES];
static int g_flash_next = 0;
static void add_flash(const float *p, float size, float life, const float *rgb) {
    Flash &f = g_flashes[g_flash_next++ % MAX_FLASHES];
    memcpy(f.p, p, sizeof(f.p));
    memcpy(f.rgb, rgb, sizeof(f.rgb));
    f.size = size;
    f.life = life;
    f.born = now_s();
}
static bool chr_alive(uintptr_t chr);
static bool whip_target_point(uintptr_t chr, float *out);
// What a hit of such a beam looks like: the beam's own hit particles (60 drops, as at the end of any revolver
// shot), a ring of sparks in the beam's colour flung out at 30 to 80 u/s, and a burst of light. The sparks
// and the light are ours: ULTRAKILL has its camera shake and the enemy's own blood for the rest.
static void fx_chain_hit(const float *at, const float *back, int look) {
    const BeamLook &k = BEAM_LOOKS[look];
    fx_beam_hit(at, back);
    add_flash(at, 3.0f * UK_UNIT, 0.12f, k.c1);
    if (!in_sight(at)) return;
    for (int i = 0; i < 16; i++) {
        float d[3], v[3], speed = frand(30.0f, 80.0f) * UK_UNIT;
        rand_unit(d);
        for (int j = 0; j < 3; j++) v[j] = d[j] * speed;
        Particle &p = new_particle(at, v, 0.2f, 0.3f * UK_UNIT, 0.0f, "spark");
        p.r = k.c1[0] * 0.5f + 0.5f;
        p.g = k.c1[1] * 0.5f + 0.5f;
        p.b = k.c1[2] * 0.5f + 0.5f;
    }
}
static void beam_chain_hit(BeamChain &b, double t) {
    if (!b.chr || !chr_alive(b.chr) || b.on_target >= b.per_target) {
        // on to the next enemy along the line (within three quarters of a metre of it, and in sight of the coin)
        Target cand[12];
        int n = find_targets(b.from, cand, 12, COIN_RANGE);
        uintptr_t next = 0;
        float best = COIN_RANGE, pt[3] = {0, 0, 0};
        for (int i = 0; i < n; i++) {
            if (cand[i].chr == b.chr) continue;
            float d[3] = {cand[i].p[0] - b.from[0], cand[i].p[1] - b.from[1], cand[i].p[2] - b.from[2]};
            float along = d[0] * b.dir[0] + d[1] * b.dir[1] + d[2] * b.dir[2], off2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along;
            if (along <= b.reach + 0.1f || along >= best || off2 > 0.75f * 0.75f || ray_blocked(b.from, cand[i].p) == 1) continue;
            best = along;
            next = cand[i].chr;
            memcpy(pt, cand[i].p, sizeof(pt));
        }
        if (!next) {
            logf("  beam from a coin: %d hits landed, %d left with no one further along its line\n", b.count, b.hits_left);
            b.live = false;
            return;
        }
        b.chr = next;
        b.on_target = 0;
        b.reach = best;
        memcpy(b.at, pt, sizeof(pt));
    } else {
        float pt[3];
        if (whip_target_point(b.chr, pt)) memcpy(b.at, pt, sizeof(pt));      // (it may have been knocked about)
    }
    float d[3] = {b.at[0] - b.from[0], b.at[1] - b.from[1], b.at[2] - b.from[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 0.01f) { b.live = false; return; }
    float dir[3] = {d[0] / len, d[1] / len, d[2] / len}, back[3] = {-dir[0], -dir[1], -dir[2]};
    bool ok = shoot(b.row, b.from, dir);
    b.on_target++;
    b.hits_left--;
    b.count++;
    add_beam(b.from, b.at, b.look);
    fx_chain_hit(b.at, back, b.look);
    // (each hit a little higher than the last: ours, on the coin's own ring, whose pitch ULTRAKILL raises with the chain's power)
    sound_play("coin_hit", 0.22f, (b.power > 2 ? 1.0f + (b.power - 2) / 5.0f : 1.0f) + 0.06f * (b.count - 1));
    // (ULTRAKILL stops the picture for 0.05 s and waits 0.05 s more. Davi found that too quick here, so both
    // are stretched by chain_slow, 1.6 unless the settings say otherwise.)
    g_hitstop_request = 0.05f * g_chain_slow;
    note_attack(HIT_COIN);
    InterlockedIncrement(&g_chain_hits);
    if (ok) InterlockedIncrement(&g_coin_shots);
    if (b.hits_left <= 0) b.live = false;
    else b.next_at = t + b.gap * g_chain_slow;
}
static void start_beam_chain(const float *from, const Target &tg, int row, int look, int hits, int per_target, float gap, int power, double t) {
    BeamChain *slot = &g_beam_chains[0];
    for (BeamChain &b : g_beam_chains)
        if (!b.live) { slot = &b; break; }
    BeamChain &b = *slot;
    b = BeamChain{};
    b.live = true;
    memcpy(b.from, from, sizeof(b.from));
    memcpy(b.at, tg.p, sizeof(b.at));
    float d[3] = {tg.p[0] - from[0], tg.p[1] - from[1], tg.p[2] - from[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 0.01f) { b.live = false; return; }
    for (int i = 0; i < 3; i++) b.dir[i] = d[i] / len;
    b.reach = len;
    b.chr = tg.chr;
    b.row = row;
    b.look = look;
    b.hits_left = hits;
    b.per_target = per_target;
    b.gap = gap;
    b.power = power;
    beam_chain_hit(b, t);                        // the first hit at once
}
static void update_beam_chains(double t) {
    for (BeamChain &b : g_beam_chains)
        if (b.live && t >= b.next_at) beam_chain_hit(b, t);
}
static uintptr_t g_watch_chr = 0;                // debug: the last ricochet's target, to log its health a second later
static int g_watch_hp = 0;
static double g_watch_at = 0;
// The look of the beam a coin sends on: the one that hit it.
static int coin_beam(const Coin &c) {
    if (c.carry == BEHAVIOR_SLAB) return BEAM_SLAB;
    if (c.carry == BEHAVIOR_RAIL) return BEAM_RAIL;
    if (c.carry == BEHAVIOR_RAIL_MALICIOUS) return BEAM_MALICIOUS;
    if (c.carry == BEHAVIOR_SLAB_SHARP) return BEAM_SHARP;
    return c.charged ? BEAM_SUPER : BEAM_REVOLVER;
}
static void reflect_coin(Coin &c, double t) {
    c.alive = false;
    // the reflected beam has the same ricochet sound; from the second coin of a chain on, higher with every coin
    // ("SetPitch(1 + (power - 2) / 5)")
    sound_play("coin_hit", 0.35f, c.power > 2 ? 1.0f + (c.power - 2) / 5.0f : 0.65f);
    if (c.carry || c.charged) {
        // (ours: a burst of light at a coin that sends another weapon's beam on)
        add_flash(c.p, 2.5f * UK_UNIT, 0.12f, BEAM_LOOKS[coin_beam(c)].c1);
    }
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
        next->carry = c.carry;
        // Coin.ReflectRevolver, strongAlt: each coin of the chain that is caught in its flash gives the beam one more
        // hit (a beam with fewer than 99: the Slab's plain and charged shots, not its Sharpshooter's)
        next->more_hits = c.more_hits + (c.hit_times > 1 && (c.carry == BEHAVIOR_SLAB_CHARGED || c.carry == BEHAVIOR_SLAB) ? 1 : 0);
        add_beam(c.p, next->p, coin_beam(c));
        // (a plain shot's split is the coin's own and goes with it down the chain; another weapon's beam is one beam)
        if (!c.carry && !c.charged && c.hit_times > next->hit_times) next->hit_times = c.hit_times;
        InterlockedIncrement(&g_coin_chains);
        return;
    }
    if (coin_hits_flying(c, t)) return;
    if (coin_hits_core(c)) return;
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
    // A beam that goes through things (hitAmount above 1: every beam but the standard revolver's plain shot
    // and the Malicious Railcannon's) is not replaced by the coin's own shot: RevolverBeam hands the coin a
    // copy of itself and Coin.ReflectRevolver ("altBeam") sends that on, stronger, at the enemy's head. Each
    // of its hits gains a quarter of the coin's power, times the beam's coinDamageBonusMultiplier, and the
    // coin's power is 2 plus one for every coin before it in the chain. The beams, from their prefabs
    // (damage a hit, hitAmount, maxHitsPerTarget, bonus):
    //   Slab plain          1.25   2    2   1      strongAlt
    //   Slab charged        1.25   6    4   1      strongAlt
    //   Slab Sharpshooter   1.25   999  2   1      strongAlt
    //   Piercer charged     1      3    3   0.5    splitcoinable
    //   Sharpshooter        1      999  1   1
    //   Electric Railcannon 2      999  4   1
    // Only the Piercer's charged beam is "splitcoinable": caught in the coin's flash it leaves as two beams.
    // A Slab beam with fewer than 99 hits caught in the flash is not split: it leaves as one beam with one hit
    // more, all of whose hits may land on one enemy (hitAmount++, maxHitsPerTarget = hitAmount). So the Slab's
    // plain shot off a coin is one beam of 2 hits of 1.75 into the nearest enemy's head, 3 off a flashing
    // coin, and its charged shot through one flashing coin 7 hits of 1.75. (Until v0.77 a Slab's plain shot
    // was given the standard revolver's rule, the coin's own shot, split in two off a flashing coin.) The hits
    // are dealt one at a time: see BeamChain. Each is one shot of the row for its damage (9000300 + quarters
    // of a revolver shot; through enemies, not stopped by shields).
    int carried = c.carry ? c.carry : c.charged ? BEHAVIOR_PIERCER : 0, hit_row = 0, hits = 1, per_target = 1, shots = c.hit_times;
    float gap = 0.05f;
    if (carried) {
        bool slab = carried == BEHAVIOR_SLAB || carried == BEHAVIOR_SLAB_CHARGED || carried == BEHAVIOR_SLAB_SHARP;
        bool rail = carried == BEHAVIOR_RAIL || carried == BEHAVIOR_RAIL_MALICIOUS;
        float each = slab ? 1.25f : rail ? 2.0f : 1.0f, bonus = carried == BEHAVIOR_PIERCER ? 0.5f : 1.0f;
        const int NO_LIMIT = 12;                 // (a beam of 999 hits: as many as three enemies in a row can take)
        if (carried == BEHAVIOR_SLAB) { hits = 2; per_target = 2; }
        else if (carried == BEHAVIOR_SLAB_CHARGED) { hits = 6; per_target = 4; }
        else if (carried == BEHAVIOR_SLAB_SHARP) { hits = NO_LIMIT; per_target = 2; }
        else if (carried == BEHAVIOR_PIERCER) { hits = 3; per_target = 3; }
        else if (carried == BEHAVIOR_RAIL) { hits = NO_LIMIT; per_target = 4; }
        if (carried == BEHAVIOR_SLAB || carried == BEHAVIOR_SLAB_CHARGED) {
            int more = c.more_hits + (c.hit_times > 1 ? 1 : 0);
            if (more > 0) {
                hits += more;
                per_target = hits;
            }
        }
        float one = each + power / 4.0f * bonus;
        int quarters = (int)lroundf(one * 4.0f);
        hit_row = BEHAVIOR_COIN_ALT + (quarters < 4 ? 4 : quarters > COIN_ALT_MAX ? COIN_ALT_MAX : quarters);
        if (rail) gap = 0.025f;
        if (carried != BEHAVIOR_PIERCER) shots = 1;
        logf("  the beam goes on from the coin: %.2f a hit, %d hits (%d on one enemy)%s\n", one, hits, per_target, shots > 1 ? ", split in two" : "");
    }
    if (!n) {
        // Coin.ReflectRevolver with nothing to aim at: the shot leaves in a random direction
        float d[3], len;
        do {
            for (float &v : d) v = (float)rand() / RAND_MAX * 2.0f - 1.0f;
            len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        } while (len < 0.1f || len > 1.0f);
        for (float &v : d) v /= len;
        shoot(hit_row ? hit_row : BEHAVIOR_COIN + power - 2, c.p, d);
        {
            float far_end[3] = {c.p[0] + d[0] * 60.0f, c.p[1] + d[1] * 60.0f, c.p[2] + d[2] * 60.0f};
            RayHit h;
            ray_cast(c.p, far_end, h);
            add_beam(c.p, h.point, coin_beam(c));
        }
        InterlockedIncrement(&g_coin_misses);
        return;
    }
    for (int i = 0; i < shots; i++) {
        const Target &tg = targets[i < n ? i : n - 1];   // one enemy and a split: both shots go to it
        if (tg.dist < 0.01f) continue;
        float dir[3] = {(tg.p[0] - c.p[0]) / tg.dist, (tg.p[1] - c.p[1]) / tg.dist, (tg.p[2] - c.p[2]) / tg.dist};
        // a coin hit by the Piercer's charged shot sends that shot on: the piercing row instead of the coin's
        // A shot that reaches the enemy from behind is not stopped by its shield. Dark Souls judges a
        // block by where the attacker stands, so a coin tossed past a shield bearer still had its shot
        // blocked from the back; such a shot is fired with a row that ignores guards (9000180 on).
        bool from_behind = dir[0] * -sinf(tg.yaw) + dir[2] * -cosf(tg.yaw) > 0.3f;
        bool ok = true;
        if (hit_row && hits > 1) {
            start_beam_chain(c.p, tg, hit_row, coin_beam(c), hits, per_target, gap, power, t);
        } else {
            ok = shoot(hit_row ? hit_row : (from_behind ? BEHAVIOR_COIN_BACK : BEHAVIOR_COIN) + power - 2, c.p, dir);
            add_beam(c.p, tg.p, coin_beam(c));
            if (ok) InterlockedIncrement(&g_coin_shots);
        }
        if (from_behind) logf("  (from behind: the target faces %.2f rad)\n", tg.yaw);
        if (c.carry == BEHAVIOR_RAIL_MALICIOUS) explode(tg.p, EXPLODE_MALICIOUS);   // the Malicious beam goes off where the coin sends it
        logf("  ricochet from (%.1f %.1f %.1f) to chr %p team %d hp %d at (%.1f %.1f %.1f), %.1f m: %s\n", c.p[0], c.p[1], c.p[2],
             (void *)tg.chr, tg.team, tg.hp, tg.p[0], tg.p[1], tg.p[2], tg.dist, ok ? "accepted" : "REFUSED");
        g_watch_chr = tg.chr;
        g_watch_hp = tg.hp;
        g_watch_at = t + 1.0;
        if (ok && i == 0) {
            // Coin.RicoshotPointsCheck: 50, plus 15 per coin when more than one was chained, and 50 more with
            // "ULTRA" in orange before it for a beam marked ultraRicocheter (every one a coin sends on but the
            // Slab's plain shot)
            int coins = c.power - 1;
            bool ultra = carried && carried != BEHAVIOR_SLAB;
            style_add(50 + (ultra ? 50 : 0) + (coins > 1 ? coins * 15 : 0), ultra ? "ULTRARICOSHOT" : "RICOSHOT", ultra ? STYLE_ORANGE : STYLE_CYAN, coins);
            // Coin.ReflectRevolver: a ricochet that breaks an enemy's attack is an INTERRUPTION, 100 points
            // and the parry's flash. ULTRAKILL has that for attacks with a breakable weak point; here it
            // is any enemy caught in the middle of an attack animation.
            int anim = -1;
            if (enemy_attacking(tg.chr, anim)) {
                style_add(100, "INTERRUPTION", STYLE_GREEN);
                g_freeze_request = 0.25f;
            }
            note_attack(HIT_COIN);
        }
    }
}
static void update_coins(double t, float dt) {
    update_beam_chains(t);
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
        if (t - c.born > COIN_LIFE + c.extra_life || c.p[1] < c.thrown_y - COIN_MAX_DROP) { c.alive = false; continue; }
        // The Knuckleblaster's wave is there for a while as it grows: a coin that is in it at any time in its
        // first half second is thrown, not only one that was there the frame it started.
        if (!c.blasted && t < g_wave_until && dist3(c.p, g_wave_p) <= 12.0f * UK_UNIT) {
            c.blasted = true;
            c.v[0] = c.v[2] = 0.0f;
            c.v[1] = 100.0f * UK_UNIT;
            c.born = t - COIN_ARMED;
            c.extra_life = 3.0f;                 // (ours: 100 u/s upward is five seconds in the air; it lasts until it is back)
            c.thrown_y = c.p[1];
            c.flashed = false;
            logf("knuckleblaster: the wave threw a coin straight up at 50 m/s\n");
        }
        if (!c.flashed && t - c.born >= 0.35) {
            c.flashed = true;
            sound_play("coin_flash", 0.5f, 1.25f);
        }
        c.v[1] -= COIN_GRAVITY * dt;
        float next[3] = {c.p[0] + c.v[0] * dt, c.p[1] + c.v[1] * dt, c.p[2] + c.v[2] * dt};
        // A coin that meets a floor or wall is spent, as one that comes to rest is in ULTRAKILL. (Until
        // v0.63 coins passed through the world and could be seen falling on under the floor.)
        RayHit h;
        if (t - c.born > 0.05 && ray_cast(c.p, next, h) && h.hit) { c.alive = false; continue; }
        memcpy(c.p, next, sizeof(next));
        c.hidden = !in_sight(c.p);
        if (t >= c.trail_at) {
            c.trail_at = t + 0.5 / COIN_TRAIL;
            memmove(c.trail[1], c.trail[0], sizeof(float) * 3 * (COIN_TRAIL - 1));
            memcpy(c.trail[0], c.p, sizeof(c.p));
            if (c.trail_n < COIN_TRAIL) c.trail_n++;
        }
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
static const double BLAST_CHECK_AT = 0.42 / 0.85;   // the "BlastCheck" event of the arm's Punch clip, which is played at 0.85
static volatile float g_punch_stamina = 2.0f;
static volatile int g_last_arm = 0;              // 0 Feedbacker, 1 Knuckleblaster: which one the fist icon shows
// When the arm in use can punch again: its clip's ReadyToPunch event (Jab 0.33 s, the Knuckleblaster's
// Punch 0.75 s, PunchBlast 1.39 s). Neither arm can start a punch before then, so the two cannot be
// thrown together.
static double g_punch_ready_at = 0;
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
static void parry_rewards() {
    InterlockedIncrement(&g_parries);
    style_add(100, "PARRY", STYLE_GREEN);
    splash_screen_blood();                       // NewMovement.Parry: GetHealth(999, silent: false)
    uintptr_t chr = g_player_chr;
    int max_hp = 0;
    if (chr && safe_read(chr + 0x3EC, &max_hp, 4) && max_hp > 0) safe_write(chr + 0x3E8, &max_hp, 4);
    g_boost = 300.0f;
    g_punch_stamina = 2.0f;
    g_freeze_request = 0.25f;
    sound_play("punch_projectile", 0.6f, 1.0f);  // Punch's "specialHit", which every parry sets off beside the parry light's ring
    sound_play("coin_hit", 0.6f, 1.0f);
}
static void do_parry(const Target &tg, int anim) {
    parry_rewards();
    stun_enemy(tg.chr, now_s());
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
static const int BEHAVIOR_EXPLOSION_PUMP = 9000144, BEHAVIOR_EXPLOSION_MALICIOUS = 9000172, BEHAVIOR_EXPLOSION_ULTRA = 9000174;
static const int BEHAVIOR_EXPLOSION_BIG = 9000145, BEHAVIOR_EXPLOSION_SUPER_BIG = 9000146;
static const int BEHAVIOR_SHARP = 9000150;
static const float EXPLOSION_RADIUS = 6.0f * UK_UNIT, EXPLOSION_SUPER_RADIUS = 12.0f * UK_UNIT;
// wave: the Knuckleblaster's, a ring with little fire. lim_fire, lim_shell: for each vertex of the ball of
// fire and of the shell that runs ahead of it, how far from the middle the world lets it go (the game's
// ray cast along that vertex's direction when the explosion starts), so neither is drawn through a wall or
// a floor. ring: the same, averaged, for the flat ring.
enum { BLAST_VERTS = 64 };
struct BlastFx {
    float p[3], radius;
    double born;
    bool wave, hidden;
    int kind;                                    // an EXPLODE_ number; -1 for the Knuckleblaster's wave
    int n_fire, n_shell;
    float lim_fire[BLAST_VERTS], lim_shell[BLAST_VERTS], ring;
};
enum { MAX_BLAST_FX = 6 };
static BlastFx g_blast_fx[MAX_BLAST_FX];
static int g_blast_fx_next = 0;
static volatile LONG g_explosions = 0;
// kind: the plain one, the "super" one (a core shot in the air), the Pump Charge shotgun going off in the
// hand after three pumps (the plain one half again as large, damage 50 against 35), and the Malicious
// Railcannon's (13.5 u).
static void blast_limits(BlastFx &b) {
    static float dirs_fire[BLAST_VERTS * 3], dirs_shell[BLAST_VERTS * 3];
    static int n_fire = 0, n_shell = 0;
    if (n_fire <= 0) n_fire = hud_model_dirs("fx_sphere", dirs_fire, BLAST_VERTS);       // 0 until the model pack is loaded
    if (n_shell <= 0) n_shell = hud_model_dirs("fx_shock", dirs_shell, BLAST_VERTS);
    float shell_full = b.wave ? b.radius : b.radius * 8.0f / 6.0f;
    auto cast = [&](const float *dirs, int n, float reach, float *out) {
        for (int v = 0; v < n; v++) {
            float far_end[3] = {b.p[0] + dirs[v * 3] * (reach + 0.2f), b.p[1] + dirs[v * 3 + 1] * (reach + 0.2f), b.p[2] + dirs[v * 3 + 2] * (reach + 0.2f)};
            RayHit h;
            out[v] = ray_cast(b.p, far_end, h) && h.hit ? fmaxf(h.dist - 0.03f, 0.02f) : 1e9f;
        }
    };
    cast(dirs_fire, n_fire, b.radius, b.lim_fire);
    cast(dirs_shell, n_shell, shell_full, b.lim_shell);
    b.n_fire = n_fire;
    b.n_shell = n_shell;
    float sum = 0;
    for (int v = 0; v < n_shell; v++) sum += fminf(b.lim_shell[v], shell_full);
    b.ring = n_shell > 0 ? sum / n_shell : 1e9f;
}

// ---- what an explosion does to the player (Explosion.Collide, NewMovement.LaunchFromPoint, Launch and
// GetHurt). An explosion that reaches V1's capsule with nothing of the world in between:
//   throws them: LaunchFromPoint(the explosion's middle, 200, maxSize). The push is 200 times the part of
//   maxSize that is left beyond the player (all of maxSize in the half second after a jump or another
//   launch), away from the middle, or straight up when the middle is within a quarter unit of the player's
//   axis; its upward part is never less than half of that. Launch clamps it to 1000 and applies it times 8
//   as an impulse on V1's mass of 100, replacing the velocity: up to 80 u/s.
//   hurts them by the explosion's damage on V1's scale of 100: 35 for a core's or a boosted pellet's, 50
//   for the Pump Charge's own and the Malicious Railcannon's. Here that is the same share of full health,
//   and never the last point of it: a death by the mod writing 0 into the health has not been seen through.
static volatile LONG g_self_blasts = 0;
// reach_u: how far the explosion's ball has grown by now; nothing happens to a player it has not got to.
// ultra: the "ultrabooster" explosion. Explosion.Collide throws the player a second time if they are within
// 12 u (the second throw finds them "jumping" from the first, so it is at full strength whatever the
// distance; the two are added, each cut to 80 u/s: up to 160 u/s), and its damage is 35 within 3 u and 50 beyond.
static bool blast_player(const float *origin, float max_size_u, int damage, float reach_u, bool ultra = false) {
    uintptr_t pos = g_player_pos, chr = g_player_chr;
    if (!pos || !g_ctrl) return false;
    const float *feet = (const float *)(pos + OFF_POS_X);
    // V1's capsule is 3.5 u tall and 1 u across, its transform is at the capsule's middle, and its camera
    // is on the same upright line 1.4 u above that. So the player's middle is taken from the eye, straight
    // down, and not from the Dark Souls body, which stands 0.2 m behind the eye. (v0.67 measured from the
    // body: an explosion straight under the eye then counted as 0.2 m ahead of the player and at about the
    // height of their middle, and threw them backwards at 22 m/s where ULTRAKILL throws them straight up.)
    float eye[3];
    if (!eye_pos(eye)) return false;
    float mid[3] = {eye[0], eye[1] - 1.4f * UK_UNIT, eye[2]};
    float low = mid[1] - 1.25f * UK_UNIT, high = mid[1] + 1.25f * UK_UNIT;
    if (low < feet[1] + 0.5f * UK_UNIT) low = feet[1] + 0.5f * UK_UNIT;
    float y = origin[1] < low ? low : origin[1] > high ? high : origin[1];
    float gap = sqrtf((origin[0] - mid[0]) * (origin[0] - mid[0]) + (origin[1] - y) * (origin[1] - y) + (origin[2] - mid[2]) * (origin[2] - mid[2])) - 0.5f * UK_UNIT;
    if (gap > reach_u * UK_UNIT || ray_blocked(origin, mid) == 1) return false;
    float d[3] = {mid[0] - origin[0], mid[1] - origin[1], mid[2] - origin[2]};
    bool overhead = d[0] * d[0] + d[2] * d[2] < 0.25f * UK_UNIT * 0.25f * UK_UNIT;
    float dist = overhead ? 0.0f : sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float dir[3] = {0, 1, 0};
    if (dist > 1e-4f)
        for (int i = 0; i < 3; i++) dir[i] = d[i] / dist;
    bool jumping = g_air && g_air_jumped && g_air_time < 0.5f;
    float num = jumping ? max_size_u : fmaxf(0.0f, max_size_u - dist / UK_UNIT);
    float v[3] = {dir[0] * num * 200.0f, 0, dir[2] * num * 200.0f};
    v[1] = fmaxf((g_air ? g_vy : 0.0f) / UK_UNIT, 0.5f * num * 200.0f);
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1000.0f)
        for (float &c : v) c *= 1000.0f / len;
    if (ultra) {
        damage = dist / UK_UNIT < 3.0f ? 35 : 50;
        if (dist / UK_UNIT < 12.0f) {
            float w[3] = {dir[0] * max_size_u * 200.0f, 0.5f * max_size_u * 200.0f, dir[2] * max_size_u * 200.0f};
            float wl = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            for (int i = 0; i < 3; i++) v[i] += w[i] * (wl > 1000.0f ? 1000.0f / wl : 1.0f);
            len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        }
    }
    if (len > 0.0f) {
        for (int i = 0; i < 3; i++) g_launch_v[i] = v[i] * 8.0f / 100.0f * UK_UNIT;
        g_launch_pending = true;
    }
    int hp = 0, max_hp = 0, lost = 0;
    // (Until v0.77 this stopped at 1 health: V1's own explosions could not kill. In ULTRAKILL they do. At 0
    // the thread that holds the game's no-death flag lets it go, and the game kills.)
    if (damage > 0 && !g_no_cooldown && chr && safe_read(chr + 0x3E8, &hp, 4) && safe_read(chr + 0x3EC, &max_hp, 4) && max_hp > 0 && hp > 0) {
        lost = (int)(damage / 100.0f * (float)max_hp + 0.5f);
        if (lost > hp) lost = hp;
        hp -= lost;
        if (hp <= 0) {
            g_death.fast = g_fast_respawn;
            g_death.begin = true;
            if (g_fast_respawn) hp = 1;              // (the game is not told: see the thread that watches the health)
        }
        safe_write(chr + 0x3E8, &hp, 4);
    }
    InterlockedIncrement(&g_self_blasts);
    logf("explosion reached the player: %.1f u from its middle of %.1f%s%s, thrown at (%.1f %.1f %.1f) m/s, %d health lost of %d (full %d)\n", dist / UK_UNIT,
         max_size_u, overhead ? " (straight under or over)" : "", jumping ? " (within a jump)" : "", g_launch_v[0], g_launch_v[1], g_launch_v[2], lost, hp + lost, max_hp);
    return true;
}

// Dashing through one's own explosion. In ULTRAKILL a dash puts the player on the "invincible" layer for
// as long as it lasts (NewMovement.Dodge: gameObject.layer = 15), which an explosion's trigger does not
// meet at all: no damage and no throw. That is the trick of firing an overpumped shotgun and dashing: the
// explosion goes off round a player it cannot touch, and by the time the dash is over they are out of it.
// So an explosion does not reach the player the instant it starts here either. It is followed for as long
// as ULTRAKILL's can hurt: its ball starts 2.6833 u in radius and grows 16.77 u a second (Explosion.FixedUpdate,
// as the drawing has it) until it is its full size, where its collider is switched off. Over that time it
// reaches the player the first moment they are inside the ball and not dashing, and not before BLAST_GRACE
// (ULTRAKILL's explosion first looks for the player a physics step after it appears, so a dash pressed
// together with the shot is in time; the 0.05 s is ours). Past half its size it does two thirds of its
// damage ("halved" in the same function).
struct PendingBlast { bool live, dashed; float p[3], max_size; int damage; double at; float speed; bool ultra; };
enum { MAX_PENDING_BLASTS = 6 };
static PendingBlast g_pending_blasts[MAX_PENDING_BLASTS];
static const double BLAST_GRACE = 0.05;
static const float BLAST_START_U = 2.6833f, BLAST_GROWTH_U = 0.05f / 0.008f * 2.6833f;
static volatile LONG g_blast_dodges = 0;
static void queue_blast(const float *p, float max_size_u, int damage, float speed, bool ultra) {
    for (PendingBlast &b : g_pending_blasts)
        if (!b.live) {
            b = {true, false, {p[0], p[1], p[2]}, max_size_u, damage, now_s(), speed, ultra};
            return;
        }
    blast_player(p, max_size_u, damage, max_size_u, ultra);         // no room to wait in: at once, as before
}
static void update_blasts(double t) {
    for (PendingBlast &b : g_pending_blasts) {
        if (!b.live) continue;
        if (g_dash_left > 0) {
            b.dashed = true;
            continue;
        }
        float age = (float)(t - b.at), size = BLAST_START_U * (b.ultra ? 2.0f : 1.0f) + BLAST_GROWTH_U * b.speed * age;
        if (size > b.max_size) {
            // full size: it can touch nothing any more
            if (b.dashed) {
                InterlockedIncrement(&g_blast_dodges);
                logf("explosion dodged with a dash\n");
            }
            b.live = false;
            continue;
        }
        if (age < BLAST_GRACE) continue;
        int damage = size > b.max_size / 2.0f ? (int)lroundf(b.damage / 1.5f) : b.damage;
        if (blast_player(b.p, b.max_size, damage, size, b.ultra)) b.live = false;
    }
}

// What an explosion does to the enemies in it: one of the game's own projectiles (attack row `row`, 1.5 m of
// reach and half a metre across) is sent at each enemy whose body is within `radius` of `p`, from 0.9 m short
// of its aim point and along the line from the explosion to it, so each is hit once and thrown away from the
// explosion. Returns how many.
// Until v0.82 it was a single projectile as wide as the explosion, set off at its middle and sent upwards.
// That hurt an enemy inside it only now and then. Measured on 2026-10-08 with the "boom" test command: sent
// up from a metre beside a hollow and 0.6 m above its aim point, nothing, time after time (two rockets that
// went off there did nothing either); sent down from the same place, a kill. The game counts a projectile's
// hit on a body it overlaps from the start only when it is moving towards it. (Davi, the day before, of the
// Malicious Railcannon: "it feels like it does not deal damage".)
// A body is the column touches_enemy takes it for; walls are not looked at, as ULTRAKILL's explosions do not.
static int blast_enemies(const float *p, float radius, int row) {
    Target near_by[16];
    int n = find_targets(p, near_by, 16, radius + 2.5f), reached = 0;
    for (int i = 0; i < n; i++) {
        const Target &tg = near_by[i];
        float dx = tg.p[0] - p[0], dz = tg.p[2] - p[2], flat = sqrtf(dx * dx + dz * dz), feet = tg.p[1] - TARGET_HEIGHT, head = feet + 2.0f;
        float dy = p[1] < feet ? feet - p[1] : p[1] > head ? p[1] - head : 0.0f, out = flat > 0.5f ? flat - 0.5f : 0.0f;
        if (out * out + dy * dy > radius * radius) continue;
        float d[3] = {tg.p[0] - p[0], tg.p[1] - p[1], tg.p[2] - p[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), from[3];
        if (len < 0.05f) { d[0] = 0; d[1] = 1; d[2] = 0; len = 1.0f; }
        for (int k = 0; k < 3; k++) {
            d[k] /= len;
            from[k] = tg.p[k] - d[k] * 0.9f;
        }
        if (shoot(row, from, d)) reached++;
    }
    return reached;
}

// spare_player: the explosion neither hurts nor throws the player (a parried projectile going off: Davi's
// report of ULTRAKILL, where standing next to one's own parry's explosion costs nothing).
static void explode(const float *p, int kind, bool spare_player) {
    static const int rows[EXPLODE_KINDS] = {BEHAVIOR_EXPLOSION, BEHAVIOR_EXPLOSION_SUPER, BEHAVIOR_EXPLOSION_PUMP, BEHAVIOR_EXPLOSION_MALICIOUS, BEHAVIOR_EXPLOSION_ULTRA,
                                            BEHAVIOR_EXPLOSION_BIG, BEHAVIOR_EXPLOSION_SUPER_BIG, BEHAVIOR_EXPLOSION_MALICIOUS};
    if (kind < 0 || kind >= EXPLODE_KINDS) kind = 0;
    bool super = kind == EXPLODE_SUPER || kind == EXPLODE_MALICIOUS || kind == EXPLODE_ULTRA || kind == EXPLODE_SUPER_BIG;
    note_attack(HIT_EXPLOSION);
    int reached = blast_enemies(p, EXPLOSIONS[kind].max_size * UK_UNIT, rows[kind]);
    explosion_sets_off(p, EXPLOSIONS[kind].max_size * UK_UNIT);
    InterlockedIncrement(&g_explosions);
    BlastFx &b = g_blast_fx[g_blast_fx_next++ % MAX_BLAST_FX];
    memcpy(b.p, p, sizeof(b.p));
    b.radius = EXPLOSIONS[kind].max_size * UK_UNIT;
    b.born = now_s();
    b.wave = false;
    b.kind = kind;
    b.hidden = false;
    blast_limits(b);
    if (kind == EXPLODE_SUPER || kind == EXPLODE_ULTRA) g_freeze_request = 0.25f;         // RevolverBeam: a beam that reaches a grenade calls ParryFlash
    fx_explosion_sparks(p, kind);
    if (!spare_player) queue_blast(p, EXPLOSIONS[kind].max_size, EXPLOSIONS[kind].damage, EXPLOSIONS[kind].speed, kind == EXPLODE_ULTRA);
    float eye[3], d = 0;
    if (eye_pos(eye)) d = sqrtf((p[0] - eye[0]) * (p[0] - eye[0]) + (p[1] - eye[1]) * (p[1] - eye[1]) + (p[2] - eye[2]) * (p[2] - eye[2]));
    float vol = 1.0f - d / (75.0f * UK_UNIT);    // the explosion's sound reaches 75 u
    // The Malicious one's own sound is the plain explosion's clip an octave down (its RandomPitch: 0.5 +- 0.1).
    // (v0.66 played the "super" explosion's for it.)
    if (kind == EXPLODE_MALICIOUS) sound_play("explosion", vol < 0.15f ? 0.15f : vol, frand(0.4f, 0.6f));
    else if (kind == EXPLODE_CANNONBALL && sound_play("explosion_big", vol < 0.15f ? 0.15f : vol, frand(0.55f, 1.05f))) {}      // 'Explosion 1' at 0.8 +- 0.25
    else sound_play(super ? "explosion_super" : "explosion", vol < 0.15f ? 0.15f : vol, frand(0.75f, 1.25f));
    static const char *const names[EXPLODE_KINDS] = {"", " (super)", " (overpump)", " (Malicious)", " (ULTRABOOST: super, twice the size)",
                                                     " (half again as large)", " (super, half again as large)", " (a cannonball's)"};
    logf("explosion%s at (%.1f %.1f %.1f), %.1f m away: %d enemies in it\n", names[kind], p[0], p[1], p[2], d, reached);
}

// Is `p` inside an enemy? Targets are aim points TARGET_HEIGHT above the feet; a body is taken as a
// column half a metre in radius and 2 m tall, plus the radius of what is flying into it.
static bool touches_enemy(const float *p, float radius) {
    Target near_by[4];
    int n = targets_near(p, near_by, 4, 3.0f + radius);
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
// the clips' own event times over the speed their states are played at (1.1 and 1.5)
static const double SHOTGUN_READY = 1.33 / 1.1, SHOTGUN_CLICK = 1.16 / 1.1, CORE_READY = 3.08 / 1.5, CORE_SMACK_1 = 2.26 / 1.5, CORE_SMACK_2 = 2.31 / 1.5,
                    CORE_CLICK = 2.56 / 1.5;
static const double SHOTGUN_EQUIP_READY = 0.44, REVOLVER_PICKUP_READY = 0.36;
static const double BOOST_WINDOW = 0.15;         // ours: how long after the shot a punch still catches a pellet
static const float CORE_GRAVITY = 40.0f * UK_UNIT, CORE_HIT_RADIUS = 0.5f, CORE_LIFE = 10.0f;
struct Pellet { bool alive, boosted, lands, parried; float p[3], v[3], end[3], normal[3]; double die_at; };   // lands: its line ends on a surface; parried: an enemy's projectile sent on
enum { MAX_PELLETS = 48 };
static Pellet g_pellets[MAX_PELLETS];
// trail: where the core was at the last eighths of a quarter second (the Grenade's TrailRenderer: 0.25 s
// long, 0.52 u wide at the core and tapering to nothing, yellow fading out)
enum { CORE_TRAIL = 8 };
struct Core { bool alive, hidden; float p[3], v[3]; double born; float trail[CORE_TRAIL][3]; int trail_n; double trail_at; };
enum { MAX_CORES = 4 };
static Core g_cores[MAX_CORES];
static double g_shotgun_ready_at = 0, g_cores_ready_at = 0, g_last_shotgun_shot = -100.0, g_last_core = -100.0, g_last_feedbacker = -100.0;
static bool g_shot_boosted = true;               // one boost per shot
static volatile float g_core_force = 0;          // 0..60 while the alt fire is held
static bool g_core_charging = false;
static volatile LONG g_shotgun_shots = 0, g_cores_thrown = 0, g_boosts = 0, g_shotgun_parries = 0;
static volatile int g_pump_charge = 0;           // the Pump Charge shotgun: 0..3 pumps
static double g_pump_beep_at = -100.0;           // when its warning last beeped
static volatile float g_rail_charge = 5.0f;      // the railcannon: 0..5, full at 5
static volatile double g_rail_full_at = -100.0;  // when its charge last completed (the HUD meter flashes white for a second)
static double g_rail_ready_at = 0;
static volatile LONG g_rail_shots = 0, g_pumps = 0;
static bool g_prev_lmb = false;
static volatile double g_muzzle_flash = -100.0;

static void spawn_pellet(const float *from, const float *dir, float speed, float range, bool boosted, bool parried) {
    for (Pellet &pl : g_pellets) {
        if (pl.alive) continue;
        pl.alive = true;
        pl.boosted = boosted;
        pl.parried = parried;
        float far_end[3];
        for (int i = 0; i < 3; i++) {
            pl.p[i] = from[i];
            pl.v[i] = dir[i] * speed;
            far_end[i] = from[i] + dir[i] * range;
        }
        // it flies until the first surface on its line
        RayHit h;
        float reach = ray_cast(from, far_end, h) && h.hit ? h.dist : range;
        pl.lands = h.hit;
        for (int i = 0; i < 3; i++) {
            pl.end[i] = from[i] + dir[i] * (reach > 0.2f ? reach - 0.2f : 0.0f);
            pl.normal[i] = h.hit ? h.normal[i] : -dir[i];
        }
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
            explode(pl.p, EXPLODE_PLAIN, pl.parried);
            pl.alive = false;
            continue;
        }
        if (t >= pl.die_at) {
            if (pl.boosted) explode(pl.end, EXPLODE_PLAIN, pl.parried);
            else if (pl.lands) fx_pellet_hit(pl.end, pl.normal);
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
        c.trail_n = 0;
        c.trail_at = 0;
        for (int i = 0; i < 3; i++) {
            c.p[i] = eye[i] + fwd[i] * 0.5f * UK_UNIT;
            c.v[i] = (fwd[i] + (i == 1 ? force * 0.002f : 0.0f)) * (force + 10.0f) * UK_UNIT;
        }
        return;
    }
}
// (the Attractor's magnets, further down: a shot along the view breaks the one it passes through)
struct MagnetObj;
static MagnetObj *magnet_on_line();
static void break_magnet(MagnetObj &m, double t, const char *why);
static float g_magnet_hit[3];                    // where on its harpoon magnet_on_line found the line of fire
// (the rocket launcher's rockets and cannonballs, further down: a shot along the view sets off the one it meets,
// and a punch sends a cannonball on)
static bool shoot_flying(int beam, bool ultra, double t);
static void explosion_sets_off(const float *p, float radius);
static bool whip_take_on_view(double t);
static void stun_enemy(uintptr_t chr, double t);
static bool visceral_start(double t);
static bool punch_ball(double t);
struct Rocket;
struct Ball;
static bool whip_catch_flying(const float *from, const float *dir, float step, float radius, double t, Rocket *take_rocket = nullptr, Ball *take_ball = nullptr);
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
        c.hidden = !in_sight(c.p);
        if (t >= c.trail_at) {
            c.trail_at = t + 0.25 / CORE_TRAIL;
            memmove(c.trail[1], c.trail[0], sizeof(float) * 3 * (CORE_TRAIL - 1));
            memcpy(c.trail[0], c.p, sizeof(c.p));
            if (c.trail_n < CORE_TRAIL) c.trail_n++;
        }
    }
    // explosions: in sight if the eye can see the middle or the top of the ball
    for (BlastFx &b : g_blast_fx) {
        if (b.radius <= 0 || t - b.born > 1.2) continue;
        float top[3] = {b.p[0], b.p[1] + b.radius, b.p[2]};
        b.hidden = !in_sight(b.p) && !in_sight(top);
    }
}
// A shot coming off a coin goes for a core in the air before it goes for an enemy (the coin's own order
// in ULTRAKILL: other coins, then grenades, then enemies), and sets it off as the larger explosion.
static bool coin_hits_core(const Coin &c) {
    Core *best = nullptr;
    float best_d2 = COIN_RANGE * COIN_RANGE;
    for (Core &k : g_cores) {
        if (!k.alive) continue;
        float dx = k.p[0] - c.p[0], dy = k.p[1] - c.p[1], dz = k.p[2] - c.p[2], d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= best_d2 || ray_blocked(c.p, k.p) == 1) continue;
        best = &k;
        best_d2 = d2;
    }
    if (!best) return false;
    add_beam(c.p, best->p, coin_beam(c));
    logf("coin shot: power %d into a core %.1f m from the coin\n", c.power, sqrtf(best_d2));
    int coins = c.power - 1;
    style_add(50 + (coins > 1 ? coins * 15 : 0), "RICOSHOT", STYLE_CYAN, coins);
    explode(best->p, true);
    best->alive = false;
    return true;
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
    spawn_pellet(start, fwd, BOOSTED_SPEED, BOOSTED_RANGE, true, false);
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
    // Shotgun.Shoot, variation 1: the Pump Charge sends 10 pellets in two thirds of the spread unpumped, 16
    // in the full spread after one pump and 24 in twice the spread after two. After three there are no
    // pellets: the gun goes off as an explosion one unit ahead (or just short of a wall nearer than that).
    bool pump = g_shotgun_var == 1;
    int charge = pump ? (int)g_pump_charge : -1, pellets = SHOTGUN_PELLETS, sent = 0;
    float spread = SHOTGUN_SPREAD, pitch = frand(0.95f, 1.05f);
    if (charge == 0) { pellets = 10; spread = SHOTGUN_SPREAD / 1.5f; pitch = frand(1.15f, 1.25f); }
    else if (charge == 1) pellets = 16;
    else if (charge == 2) { pellets = 24; spread = SHOTGUN_SPREAD * 2.0f; pitch = frand(0.75f, 0.85f); }
    else if (charge >= 3) { pellets = 0; pitch = frand(0.75f, 0.85f); }
    if (charge >= 3) {
        float ahead[3], at[3];
        for (int i = 0; i < 3; i++) ahead[i] = eye[i] + fwd[i] * UK_UNIT;
        RayHit h;
        float reach = ray_cast(eye, ahead, h) && h.hit ? fmaxf(h.dist - 0.1f * UK_UNIT, 0.0f) : UK_UNIT;
        for (int i = 0; i < 3; i++) at[i] = eye[i] + fwd[i] * reach;
        explode(at, EXPLODE_PUMP);
    }
    for (int i = 0; i < pellets; i++) {
        // transform.Rotate(x, y, z) with each angle random within the spread: a pitch, then a yaw
        float rx = frand(-spread, spread), ry = frand(-spread, spread);
        float lx = sinf(ry) * cosf(rx), ly = -sinf(rx), lz = cosf(ry) * cosf(rx), d[3];
        for (int k = 0; k < 3; k++) d[k] = right[k] * lx + up[k] * ly + fwd[k] * lz;
        if (shoot(BEHAVIOR_PELLET, start, d)) sent++;
        spawn_pellet(start, d, PELLET_SPEED, PELLET_RANGE, false, false);
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
    sound_play("shotgun_shot", 0.45f, pitch, false, CH_GUN);
    timed_sounds_clear();
    if (pump) {
        // FireWithPump: the gun pumps itself once after the shot (its sounds at 0.44 and 0.75 s) and is ready at 0.99 s
        play_revolver("FireWithPump");
        sound_later(0.44 / 1.1, "pump1", 1.0f, frand(0.95f, 1.05f), CH_FREE);
        sound_later(0.44 / 1.1, "pump_charge", 0.5f, 1.0f, CH_FREE);
        sound_later(0.75 / 1.1, "pump2", 1.0f, frand(0.95f, 1.05f), CH_FREE);
        g_shotgun_ready_at = t + 0.99 / 1.1;
        g_pump_charge = 0;
    } else {
        play_revolver("FireWithReload");
        sound_later(SHOTGUN_CLICK, "shotgun_click", 0.5f, frand(0.95f, 1.05f));
        g_shotgun_ready_at = t + SHOTGUN_READY;
    }
    g_last_shotgun_shot = t;
    g_muzzle_flash = t;
    g_shot_boosted = pellets == 0;               // nothing to punch on when the gun went off instead
    if (t - g_last_feedbacker < 0.03) boost_pellet(t);       // punched on the same frame
    if (pump) logf("shotgun: Pump Charge fired at %d pumps\n", charge);
    logf("shotgun: %d of %d pellets accepted%s\n", sent, pellets, parried ? ", point-blank parry" : in_reach ? ", enemy in reach" : "");
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
struct Ricochet { bool pending; double at; float from[3], dir[3], wall[3]; int left, bounces; };   // wall: the point it bounces off; bounces: how many it has made
enum { MAX_RICOCHETS = 6 };
static Ricochet g_ricochets[MAX_RICOCHETS];
static volatile LONG g_sharp_shots = 0, g_sharp_bounces = 0;
static int g_sharp_behavior = BEHAVIOR_SHARP;    // the row the beam in flight is fired with: BEHAVIOR_SLAB_SHARP from the alternate revolver
static void shot_beam(const float *end, int kind);

// One leg of the beam, from `from` along `dir` to the first surface; `first` is the leg from the gun.
// bounces: how many surfaces the beam has come off before this leg. RevolverBeam.PiercingShotCheck gives a
// beam that bounces one more hit on each enemy with every bounce ("maxHitsPerTarget++", up to 3, or 4 for the
// Slab's, which starts at 2), so a leg after one bounce lands twice what the leg from the gun does, and
// after two, three times. Those are sent as one shot of the row for that much (9000300 + quarters).
// drawn_from: where the leg is drawn from (the wall itself; the shot leaves from a little off it).
static bool g_sharp_no_shot = false;             // the leg is drawn and bounces on, but its hits are being given one by one elsewhere
static void sharp_leg(const float *from, const float *dir, int left, bool first, int bounces = 0, const float *drawn_from = nullptr) {
    float far_end[3];
    for (int i = 0; i < 3; i++) far_end[i] = from[i] + dir[i] * 150.0f;
    RayHit h;
    bool cast = ray_cast(from, far_end, h);
    int row = g_sharp_behavior;
    if (bounces > 0) {
        bool slab = g_sharp_behavior == BEHAVIOR_SLAB_SHARP;
        int hits = (slab ? 2 : 1) + bounces, most = slab ? 4 : 3;
        if (hits > most) hits = most;
        row = BEHAVIOR_COIN_ALT + (int)lroundf((slab ? 1.25f : 1.0f) * hits * 4.0f);
    }
    bool ok = g_sharp_no_shot ? true : first ? shoot(row) : shoot(row, from, dir);
    if (first) shot_beam(h.point, BEAM_SHARP);
    else add_beam(drawn_from ? drawn_from : from, h.point, BEAM_SHARP);
    if (h.hit) fx_beam_hit(h.point, h.normal);
    logf("sharpshooter: %s leg %.1f m, %s, %d bounces left, surface normal (%.2f %.2f %.2f): %s\n", first ? "first" : "bounced", h.dist,
         h.hit ? "hit a surface" : "hit nothing", left, h.normal[0], h.normal[1], h.normal[2], ok ? "accepted" : "REFUSED");
    if (!cast || !h.hit || left <= 0) return;
    for (Ricochet &r : g_ricochets) {
        if (r.pending) continue;
        r.pending = true;
        r.at = now_s() + 0.1;
        r.left = left - 1;
        r.bounces = bounces + 1;
        float dn = dir[0] * h.normal[0] + dir[1] * h.normal[1] + dir[2] * h.normal[2];
        for (int i = 0; i < 3; i++) {
            r.dir[i] = dir[i] - 2.0f * dn * h.normal[i];          // Vector3.Reflect
            // The next leg leaves from 0.45 m off the surface, along its normal. Until v0.80 it left from
            // 0.1 m back along the way the beam had come: nearer the wall than the 0.3 m the shot itself is
            // across, so a leg could be drawn and its shot die in the wall it had just left, and at a
            // shallow angle hardly off the wall at all. (Davi: the Sharpshooter's shots did not land reliably.)
            r.from[i] = h.point[i] + h.normal[i] * 0.45f;
            r.wall[i] = h.point[i];
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
        // RevolverBeam.RicochetAimAssist, as it is written: a ball of 5 u is swept from where the beam bounced
        // along the mirrored line, and the beam is turned to the first thing the ball touches that the bounce
        // point can see. What the ball already holds as it starts is passed over ("distance < 0.1"), so an
        // enemy within 5 u of the bounce point gets no help: the beam goes where the mirror sends it. A coin
        // in the air comes before any enemy. A body is the ball's 5 u and half a metre of its own.
        // (v0.80 and v0.82 took the enemy whose chest was least far along the line within 7 u of it, also
        // those right at the bounce point: easier than ULTRAKILL, and not its rule.)
        const float tube = 5.0f * UK_UNIT + 0.5f;
        float best = 1e9f;
        for (int i = 0; i < n; i++) {
            float d[3] = {cand[i].p[0] - r.from[0], cand[i].p[1] - r.from[1], cand[i].p[2] - r.from[2]};
            float along = d[0] * r.dir[0] + d[1] * r.dir[1] + d[2] * r.dir[2];
            float off2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along;
            if (off2 > tube * tube) continue;
            float touch = along - sqrtf(tube * tube - off2);          // how far the ball has gone when it first touches
            if (touch < 0.1f * UK_UNIT || touch >= best) continue;
            if (ray_blocked(r.from, cand[i].p) == 1) continue;
            best = touch;
            pick = &cand[i];
        }
        Coin *coin = nullptr;
        {
            const float coin_tube = 5.0f * UK_UNIT + 0.1f;
            float coin_best = 1e9f;
            for (Coin &c : g_coins) {
                if (!c.alive || c.shot || t - c.born < COIN_ARMED) continue;
                float d[3] = {c.p[0] - r.from[0], c.p[1] - r.from[1], c.p[2] - r.from[2]};
                float along = d[0] * r.dir[0] + d[1] * r.dir[1] + d[2] * r.dir[2];
                float off2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along;
                if (along <= 0 || off2 > coin_tube * coin_tube) continue;
                float touch = along - sqrtf(coin_tube * coin_tube - off2);
                if (touch >= coin_best || ray_blocked(r.from, c.p) == 1) continue;
                coin_best = touch;
                coin = &c;
            }
        }
        InterlockedIncrement(&g_sharp_bounces);
        sound_play("ricochet", 0.35f, 1.0f);
        if (coin) {
            // the bounced beam goes into the coin, which sends it on as it does a beam shot at it
            add_beam(r.wall, coin->p, BEAM_SHARP);
            hit_coin(*coin, t);
            coin->charged = true;
            coin->carry = g_sharp_behavior == BEHAVIOR_SLAB_SHARP ? BEHAVIOR_SLAB_SHARP : 0;
            logf("sharpshooter: bounce aimed at a coin\n");
            continue;
        }
        if (pick && pick->dist > 0.01f)
            for (int i = 0; i < 3; i++) r.dir[i] = (pick->p[i] - r.from[i]) / pick->dist;
        float from[3], dir[3];
        memcpy(from, r.from, sizeof(from));
        memcpy(dir, r.dir, sizeof(dir));
        if (pick) logf("sharpshooter: bounce aimed at chr %p, %.1f m away\n", (void *)pick->chr, pick->dist);
        {
            // RevolverBeam: a beam that may hit the same enemy more than once lands its hits one at a time, 0.05 s
            // apart, the picture stopping at each, exactly as a beam sent on by a coin does. A bounced Sharpshooter
            // beam has a hit more for each bounce. (v0.80 gave them all at once, as one larger hit, with no stop.)
            bool slab = g_sharp_behavior == BEHAVIOR_SLAB_SHARP;
            int hits = (slab ? 2 : 1) + r.bounces, most = slab ? 4 : 3;
            if (hits > most) hits = most;
            if (pick && hits >= 2) {
                start_beam_chain(from, *pick, slab ? BEHAVIOR_COIN_ALT + 5 : BEHAVIOR_SHARP, BEAM_SHARP, hits, hits, 0.05f, 2, t);
                g_sharp_no_shot = true;
            }
        }
        sharp_leg(from, dir, r.left, false, r.bounces, r.wall);
        g_sharp_no_shot = false;
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
    c.carry = 0;
    c.trail_n = 0;                               // it starts a new path from where it came off
    c.born = t;
    c.thrown_y = land[1];
    c.power = c.power + 1;
    c.hit_times = 1;
    sound_play("coin_hit", 0.35f, 1.0f + (power - 2) / 5.0f);
    return true;
}

// Punch.ParryProjectile on someone else's projectile: a Feedbacker punch that catches an arrow, a bomb or a
// spell in flight sends it back as the boosted pellet is sent (250 u/s, going off as an explosion where it
// lands), at whoever stands where it was fired from, or straight back along its own path. The punch stays
// able to catch one for 0.15 s (its clip's ActiveEnd is at 0.12 s), within 3.5 m ahead of the eye.
// The game's own projectile is ended: its life is run out, and it is moved far below the map first, so
// that whatever it does as it ends (a firebomb's burst) happens there. Not yet tried in the game.
static const float PROJECTILE_PARRY_REACH = 3.5f;
static double g_projectile_window = 0;
static void spawn_pellet(const float *from, const float *dir, float speed, float range, bool boosted, bool parried);
static bool parry_projectile(double t) {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return false;
    view_axes(right, up, fwd);
    EnemyShot *best = nullptr;
    float best_dist = PROJECTILE_PARRY_REACH, best_pos[3] = {0, 0, 0}, best_vel[3] = {0, 0, 0};
    for (EnemyShot &s : g_enemy_shots) {
        if (!s.bullet) continue;
        float pos[3], vel[3];
        int id = 0, state = 0, shooter = 0;
        if (t - s.at > 15.0 || !safe_read(s.bullet + OFF_BULLET_ID, &id, 4) || id != s.id || !safe_read(s.bullet + OFF_BULLET_SHOOTER, &shooter, 4) ||
            shooter == PLAYER_HANDLE || !safe_read(s.bullet + OFF_BULLET_STATE, &state, 4) || state < 1 || state > 2 ||
            !safe_read(s.bullet + OFF_BULLET_POS, pos, 12) || !safe_read(s.bullet + OFF_BULLET_VEL, vel, 12)) {
            s.bullet = 0;                        // gone, or its place taken by another projectile
            continue;
        }
        float d[3] = {pos[0] - eye[0], pos[1] - eye[1], pos[2] - eye[2]}, dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(dist < best_dist)) continue;
        if (vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2] < 1.0f) continue;                      // lying still
        if (dist > 0.3f && (d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]) / dist < 0.2f) continue;    // behind, or well off to the side
        if (vel[0] * d[0] + vel[1] * d[1] + vel[2] * d[2] > 0) continue;                               // going away
        best = &s;
        best_dist = dist;
        memcpy(best_pos, pos, sizeof(pos));
        memcpy(best_vel, vel, sizeof(vel));
    }
    if (!best) return false;
    // Punch.ParryProjectile turns the projectile to face along the camera: it goes where the player is
    // looking, as a boosted pellet does. (v0.67 sent it back at whoever fired it.)
    float from[3] = {eye[0] + fwd[0], eye[1] + fwd[1], eye[2] + fwd[2]}, dir[3] = {fwd[0], fwd[1], fwd[2]};
    float speed = sqrtf(best_vel[0] * best_vel[0] + best_vel[1] * best_vel[1] + best_vel[2] * best_vel[2]);
    const float away[3] = {best_pos[0], best_pos[1] - 500.0f, best_pos[2]}, still[3] = {0, 0, 0}, spent = 0.001f;
    safe_write(best->bullet + OFF_BULLET_POS, away, 12);
    safe_write(best->bullet + OFF_BULLET_VEL, still, 12);
    safe_write(best->bullet + OFF_BULLET_VEL2, still, 12);
    safe_write(best->bullet + OFF_BULLET_LIFE, &spent, 4);
    logf("projectile parry: row %d caught %.1f m from the eye, flying at %.0f m/s; sent on along the view\n", best->id, best_dist, speed);
    best->bullet = 0;
    spawn_pellet(from, dir, BOOSTED_SPEED, BOOSTED_RANGE, true, true);
    InterlockedIncrement(&g_projectile_parries);
    parry_rewards();
    play_arm("Hook", 0.065);
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
    if (armed && t < g_projectile_window && parry_projectile(t)) g_projectile_window = 0;

    // the Knuckleblaster's blast, if its key is still down when the punch gets there
    if (g_blast_check > 0 && t >= g_blast_check) {
        g_blast_check = -1;
        if (key2) {
            InterlockedIncrement(&g_blasts);
            play_arm2("PunchBlast");
            sound_play("knuckle_blast", 1.0f, 1.0f);
            sound_later(0.78, "knuckle_eject", 0.5f, 1.0f, CH_FREE);   // the clip's Eject event: the shells go out
            g_punch_ready_at = t + 1.39;
            note_attack(HIT_EXPLOSION);
            shoot(BEHAVIOR_BLAST);
            float right[3], up[3], fwd[3], eye[3];
            if (eye_pos(eye)) {
                // the wave starts 2 u ahead of the eye
                view_axes(right, up, fwd);
                BlastFx &b = g_blast_fx[g_blast_fx_next++ % MAX_BLAST_FX];
                for (int i = 0; i < 3; i++) b.p[i] = eye[i] + fwd[i] * 2.0f * UK_UNIT;
                b.radius = 6.0f;                 // 'Explosion Wave Knuckleblaster': maxSize 12 u (the row's radius, tools/patch_v1.py)
                b.born = t;
                b.wave = true;
                b.kind = -1;
                b.hidden = false;
                blast_limits(b);
                // Punch.BlastCheck gives the wave "playerProjectileForceDirection = the camera's forward", and
                // Explosion.Collide then sets the velocity of every loose body the wave touches to that
                // direction at 100 u/s: coins and cores are thrown where the player is looking (straight up,
                // looking up). The wave reaches 12 u (its maxSize; until v0.73 the plain 'Explosion Wave''s 8
                // was used) from where it starts.
                int thrown = 0;
                g_wave_until = t + 0.5;          // (coins that come into it later: update_coins)
                memcpy(g_wave_p, b.p, sizeof(g_wave_p));
                for (Coin &c : g_coins) {
                    if (!c.alive || c.shot || c.blasted || dist3(c.p, b.p) > 12.0f * UK_UNIT) continue;
                    c.blasted = true;
                    c.extra_life = 3.0f;
                    // Coins go straight up. By ULTRAKILL's code (Explosion.Collide) a coin's velocity becomes the
                    // camera's forward at 100 u/s, like a core's, and v0.72 did that; Davi, who knows the game,
                    // reports that there a coin blasted just after its toss goes straight up whatever the
                    // aim, and that the sideways throw here was wrong. Nothing found in the decompiled code
                    // accounts for it, so this is his report, not a reading of the code: 100 u/s, upward.
                    c.v[0] = c.v[2] = 0.0f;
                    c.v[1] = 100.0f * UK_UNIT;
                    c.born = t - COIN_ARMED;         // (its time in the air starts over, or it would be gone before it came down)
                    c.thrown_y = c.p[1];
                    c.flashed = false;
                    thrown++;
                }
                for (Core &k : g_cores) {
                    if (!k.alive || dist3(k.p, b.p) > 12.0f * UK_UNIT || ray_blocked(b.p, k.p) == 1) continue;
                    for (int i = 0; i < 3; i++) k.v[i] = fwd[i] * 100.0f * UK_UNIT;
                    thrown++;
                }
                if (thrown) logf("knuckleblaster: the wave threw %d coins and cores along the view at 50 m/s\n", thrown);
            }
            logf("knuckleblaster: blast wave\n");
        }
    }
    if ((!edge && !edge2) || g_fist_cooldown > 0 || g_punch_stamina < 1.0f || t < g_punch_ready_at) return;
    bool heavy = edge2 && !edge;
    g_last_arm = heavy ? 1 : 0;
    g_punch_ready_at = t + (heavy ? 0.75 / 0.85 : 0.33);
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
        punch_ball(t);                           // (either arm sends a cannonball on)
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
    if (visceral_start(t)) return;
    sound_play("punch_swing", 0.5f, 1.0f);
    play_arm(rand() % 2 ? "Jab" : "Jab2");
    g_last_feedbacker = t;
    g_projectile_window = t + 0.15;
    if (parry_projectile(t)) g_projectile_window = 0;
    if (g_weapon == WEAPON_SHOTGUN && t - g_last_shotgun_shot <= BOOST_WINDOW) boost_pellet(t);
    if (punch_coin(t) || punch_ball(t)) play_arm("Hook", 0.065);
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
// Where a shot along the view ends and which way that spot faces: the first surface on the line, or,
// for a shot that stops at enemies, the first enemy standing on the line before it (a body is taken as
// 0.45 m to either side of its aim point, from 1.2 m under it to 0.7 m over it).
static void shot_end(bool stops_at_enemies, float *end, float *normal) {
    float right[3], up[3], fwd[3], eye[3], far_end[3];
    for (int i = 0; i < 3; i++) end[i] = normal[i] = 0;
    if (!eye_pos(eye)) return;
    view_axes(right, up, fwd);
    for (int i = 0; i < 3; i++) far_end[i] = eye[i] + fwd[i] * 150.0f;
    RayHit h;
    ray_cast(eye, far_end, h);
    float reach = h.hit ? h.dist : 150.0f;
    for (int i = 0; i < 3; i++) normal[i] = h.hit ? h.normal[i] : -fwd[i];
    if (stops_at_enemies) {
        Target cand[12];
        int n = find_targets(eye, cand, 12, reach + 2.0f);
        for (int k = 0; k < n; k++) {
            float d[3] = {cand[k].p[0] - eye[0], cand[k].p[1] - eye[1], cand[k].p[2] - eye[2]};
            float along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
            if (along <= 0.3f || along - 0.3f >= reach) continue;
            float ox = eye[0] + fwd[0] * along - cand[k].p[0], oy = eye[1] + fwd[1] * along - cand[k].p[1], oz = eye[2] + fwd[2] * along - cand[k].p[2];
            if (ox * ox + oz * oz > 0.45f * 0.45f || oy < -1.2f || oy > 0.7f) continue;
            reach = along - 0.3f;
            for (int i = 0; i < 3; i++) normal[i] = -fwd[i];
        }
    }
    for (int i = 0; i < 3; i++) end[i] = eye[i] + fwd[i] * reach;
}

// A plain revolver shot's beam and the particles where it lands.
static void revolver_beam_fx(int kind = BEAM_REVOLVER) {
    float end[3], normal[3];
    shot_end(true, end, normal);
    shot_beam(end, kind);
    fx_beam_hit(end, normal);
}

// A beam for a shot fired from the gun: from about where the barrel is on screen to `end`, or, with no
// end given, to where the view line first meets the world (150 m if it meets nothing).
static void shot_beam(const float *end, int kind) {
    g_muzzle_flash = now_s();
    float right[3], up[3], fwd[3], eye[3], muzzle[3], far_end[3];
    if (!eye_pos(eye)) return;
    view_axes(right, up, fwd);
    // the muzzle is wherever the HUD last drew it: the point 0.9 m out that lands on that spot of the screen
    float mx = 0.26f, my = -0.32f;
    hud_muzzle(&mx, &my);
    float tan_y = tanf(BASE_FOV * g_fov_scale * g_zoom * 0.5f), tan_x = tan_y * g_aspect;
    for (int i = 0; i < 3; i++) {
        muzzle[i] = eye[i] + (right[i] * mx * tan_x + up[i] * my * tan_y + fwd[i]) * 0.9f;
        far_end[i] = eye[i] + fwd[i] * 150.0f;
    }
    if (end) {
        add_beam(muzzle, end, kind, false);
        return;
    }
    RayHit h;
    ray_cast(eye, far_end, h);
    add_beam(muzzle, h.point, kind, false);
    if (h.hit) fx_beam_hit(h.point, h.normal);
}

// Shotgun.Pump (variation 1): each press of the alt fire while the gun is ready pumps it once, three
// times at most. The Pump2 clip's events are the two sounds (0.10 and 0.50 s) and ReadyGun at 0.64 s; the
// first sound comes with a rising tone pitched 1 + pumps / 5.
static void pump_shotgun(double t) {
    if (g_pump_charge < 3) g_pump_charge = g_pump_charge + 1;
    InterlockedIncrement(&g_pumps);
    play_revolver("Pump2");
    timed_sounds_clear();
    sound_later(0.10 / 1.65, "pump1", 1.0f, frand(0.95f, 1.05f), CH_FREE);
    sound_later(0.10 / 1.65, "pump_charge", 0.5f, 1.0f + g_pump_charge / 5.0f, CH_FREE);
    sound_later(0.50 / 1.65, "pump2", 1.0f, frand(0.95f, 1.05f), CH_FREE);
    g_shotgun_ready_at = t + 0.64 / 1.65;
    logf("shotgun: pumped, %d of 3\n", (int)g_pump_charge);
}

// ---- the railcannon (Railcannon, WeaponCharges.raicharge). One charge shared by its variations: 0 to 5 at
// a quarter a second, taken as full from 4 (16 s). A press of fire while it is full empties it.
//   Electric (variation 0)   'Railcannon Beam': a beam 1 u wide through every enemy on the line, damage 2
//                            up to four times on each (row 9000170 deals the 8 at once).
//   Malicious (variation 2)  'Railcannon Beam Malicious': damage 2 to the first thing on the line, where
//                            it sets off an explosion 13.5 u across (rows 9000171 and 9000172).
// Either beam that meets a coin is passed on by the coin, and one that meets a core sets it off. The alt
// fire zooms. The Screwdriver (variation 1), a drill that stays in what it hits, is not in yet.
static void fire_railcannon(double t) {
    bool malicious = g_rail_var == 1;
    int beam = malicious ? BEAM_MALICIOUS : BEAM_RAIL;
    g_rail_charge = 0;
    InterlockedIncrement(&g_rail_shots);
    play_revolver("Fire");
    timed_sounds_clear();
    if (malicious) {
        // 'RailcannonMaliciousFire' is three sources played together: its own clip at three times its
        // pitch, and the explosion's clip at its own pitch and an octave up. (v0.66 played the first alone.)
        sound_play("rail_fire_malicious", 1.0f, 3.0f, false, CH_GUN);
        sound_play("explosion", 1.0f, 1.0f);
        sound_play("explosion", 1.0f, 2.0f);
    } else {
        sound_play("rail_fire", 0.65f, 1.0f, false, CH_GUN);
    }
    note_attack(HIT_REVOLVER);
    if (Coin *c = coin_on_line()) {
        shot_beam(c->p, beam);
        hit_coin(*c, t);
        c->charged = true;
        c->carry = malicious ? BEHAVIOR_RAIL_MALICIOUS : BEHAVIOR_RAIL;
        logf("railcannon: %s, into a coin\n", malicious ? "Malicious" : "Electric");
    } else if (shoot_flying(beam, malicious, t)) {
    } else if (Core *k = core_on_line()) {
        // RevolverBeam: a railcannon beam that hits once (the Malicious) sets a core off as the "ultrabooster"
        shot_beam(k->p, beam);
        explode(k->p, malicious ? EXPLODE_ULTRA : EXPLODE_SUPER);
        k->alive = false;
        logf("railcannon: %s, into a core\n", malicious ? "Malicious" : "Electric");
    } else if (MagnetObj *mg = magnet_on_line()) {
        shot_beam(g_magnet_hit, beam);
        break_magnet(*mg, t, "shot");
    } else if (malicious) {
        float end[3], normal[3], at[3];
        shot_end(true, end, normal);
        bool ok = shoot(BEHAVIOR_RAIL_MALICIOUS);
        shot_beam(end, beam);
        for (int i = 0; i < 3; i++) at[i] = end[i] + normal[i] * 0.15f;
        explode(at, EXPLODE_MALICIOUS);
        logf("railcannon: Malicious, %s\n", ok ? "accepted" : "REFUSED");
    } else {
        bool ok = shoot(BEHAVIOR_RAIL);
        shot_beam(nullptr, beam);                         // through enemies, to the first surface
        logf("railcannon: Electric, %s\n", ok ? "accepted" : "REFUSED");
    }
}

// ---- the whiplash (HookArm), on R as in ULTRAKILL.
//   Throw: a press with the 0.5 s cooldown over sends the hook out from the eye along the view at 250 u/s.
//          It flies for as long as the key is held, and for 0.4 s at the least (until the cooldown is down
//          to 0.1), up to 300 u; then, or on meeting the world, it comes back at 100 u/s plus half a unit
//          for every unit it has to come (25 at the least) and is caught.
//   Catch: the hook takes the first enemy within a sphere round it that grows with the distance (a
//          fifteenth of it, 5 u at most), deals it 0.2 damage and stays in it while the key is held.
//   Pull:  with the key up, the player is pulled to the enemy at 60 u/s (step_common), the ground check off.
//          R again lets go. The pull ends on arriving, with the enemy's death, or when the world comes
//          between; arriving in the air leaves the player rising at 15 u/s (more from below the hook).
//          Jump during it lets go with the speed cut to 30 u/s and 15 u/s more upwards (a plain jump
//          near the ground).
// ULTRAKILL pulls its light enemies (Filth, Strays, Schisms, Soldiers, Drones, Streetcleaners) to the
// player instead: not done here, every enemy pulls the player. Nor are hook points, items or its hard
// damage. The arm, its three clips and the sounds are ULTRAKILL's; the damage is row 9000190.
static const int BEHAVIOR_WHIP = 9000190;
static const float WHIP_SPEED = 250.0f * UK_UNIT, WHIP_MAX = 300.0f * UK_UNIT, WHIP_COOLDOWN = 0.5f;
static const float WHIP_ARRIVE = 1.3f;           // ours: middle to aim point, about two bodies touching (V1's is 0.25 u between the colliders)
enum { WHIP_READY = 0, WHIP_THROWING, WHIP_CAUGHT, WHIP_PULLING };
static volatile int g_whip_state = WHIP_READY;
static volatile bool g_whip_returning = false;
static bool g_prev_whip_key = false;
static float g_whip_hook[3], g_whip_dir[3] = {0, 0, 1}, g_whip_cooldown = 0, g_whip_return_dist = 25.0f, g_whip_warp = 0, g_whip_stuck = 0, g_whip_last_body[3];
static uintptr_t g_whip_chr = 0;
static int g_whip_loop = 0, g_whip_woosh = 0;
static const char *volatile g_whip_clip = nullptr;
static volatile double g_whip_clip_start = -100.0;
static volatile bool g_whip_hold = false;        // the clip's last frame is held (the arm stays out while the hook is)
static volatile LONG g_whip_throws = 0, g_whip_catches = 0, g_whip_pulls = 0;

static void whip_clip(const char *clip, bool hold, double already_in = 0.0) {
    g_whip_clip_start = now_s() - already_in;
    g_whip_hold = hold;
    g_whip_clip = clip;
}
// the arm's own AudioSource: one loop at a time (volume 0.35, pitch 0.9 to 1.1)
static void whip_loop(const char *name) {
    if (g_whip_loop) sound_stop(g_whip_loop);
    g_whip_loop = name ? sound_play(name, 0.35f, frand(0.9f, 1.1f), true) : 0;
}
// Where a caught character's aim point is now; false once it is dead or gone.
static bool whip_target_point(uintptr_t chr, float *out) {
    uintptr_t ctrl = 0, havok = 0, owner = 0;
    int hp = 0;
    if (!chr || !safe_read(chr + 0x68, &ctrl, 8) || ctrl < 0x10000 || !safe_read(ctrl + 0x28, &havok, 8) || havok < 0x10000) return false;
    if (!safe_read(havok + OFF_HAVOK_OWNER, &owner, 8) || owner - OWNER_DELTA != chr) return false;
    if (!safe_read(chr + OFF_CHR_HP, &hp, 4) || hp <= 0) return false;
    if (!safe_read(havok + 0x10, out, 12)) return false;
    out[1] += TARGET_HEIGHT;
    return true;
}
// HookArm.StopThrow: the hook lets go and starts back. anim_time 0 is a throw that is over (the yank and
// the reeling loop), 1 a pull that is (the arm already drawn in, and the pull's last sound).
static void whip_stop(float anim_time) {
    float eye[3];
    if (anim_time == 0) {
        sound_play("hook_pull", 0.75f, frand(0.9f, 1.1f));
        whip_loop("hook_pull_loop");
    } else {
        sound_play("hook_pull_done", 1.0f, frand(1.9f, 2.1f));
    }
    g_whip_pull = false;
    g_whip_jump = false;
    g_whip_chr = 0;
    g_whip_state = WHIP_READY;
    whip_clip("Pull", true, anim_time > 0 ? 10.0 : 0.0);
    g_whip_return_dist = fmaxf(eye_pos(eye) ? dist3(eye, g_whip_hook) / UK_UNIT : 0.0f, 25.0f);
    g_whip_returning = true;
    g_whip_warp = 0;
    if (g_whip_woosh) sound_stop(g_whip_woosh);
    g_whip_woosh = 0;
}
// HookArm.Cancel: put away at once (first person switched off with the hook out)
static void whip_cancel() {
    g_whip_pull = false;
    g_whip_jump = false;
    g_whip_chr = 0;
    g_whip_state = WHIP_READY;
    g_whip_returning = false;
    g_whip_clip = nullptr;
    whip_loop(nullptr);
    if (g_whip_woosh) sound_stop(g_whip_woosh);
    g_whip_woosh = 0;
}
static void update_whiplash(bool armed, double t, float dt) {
    g_whip_cooldown = move_towards(g_whip_cooldown, 0.0f, dt);
    g_whip_warp = move_towards(g_whip_warp, 0.0f, dt * 6.5f);
    bool key = armed && key_down('R'), edge = key && !g_prev_whip_key;
    g_prev_whip_key = key;
    bool out = g_whip_state != WHIP_READY || g_whip_returning;
    float right[3], up[3], fwd[3], eye[3];
    uintptr_t pos = g_player_pos;
    if (!armed || !pos || !eye_pos(eye)) {
        if (out || g_whip_clip) whip_cancel();
        return;
    }
    view_axes(right, up, fwd);
    const float *feet = (const float *)(pos + OFF_POS_X);
    float body[3] = {feet[0], feet[1] + BODY_MID, feet[2]};
    // the Catch clip's CatchOver event, 0.80 s in: the arm is put away
    if (!out && g_whip_clip && !g_whip_hold && t - g_whip_clip_start > 0.80) g_whip_clip = nullptr;

    if (edge) {
        if (g_whip_state == WHIP_PULLING) {
            whip_stop(0);
            logf("whiplash: let go\n");
            return;
        }
        if (g_whip_cooldown <= 0) {
            g_whip_cooldown = WHIP_COOLDOWN;
            if (g_fist_cooldown > 0.1f) g_fist_cooldown = 0.1f;
            memcpy(g_whip_hook, eye, sizeof(g_whip_hook));
            memcpy(g_whip_dir, fwd, sizeof(g_whip_dir));
            g_whip_returning = false;
            g_whip_pull = false;
            g_whip_chr = 0;
            g_whip_warp = 1.0f;
            g_whip_state = WHIP_THROWING;
            whip_clip("Throw", true);
            sound_play("hook_throw", 1.0f, frand(0.9f, 1.1f));
            if (whip_take_on_view(t)) {          // a rocket or a cannonball on its line: taken at once
                sound_play("hook_clink", 0.5f, frand(0.9f, 1.1f));
                whip_stop(0);
                return;
            }
            whip_loop("hook_throw_loop");
            InterlockedIncrement(&g_whip_throws);
        }
    }

    if (g_whip_state == WHIP_READY) {
        if (!g_whip_returning) return;
        // back to a point 1.5 u ahead of the eye, then caught
        float target[3], d[3];
        for (int i = 0; i < 3; i++) {
            target[i] = eye[i] + fwd[i] * 1.5f * UK_UNIT;
            d[i] = target[i] - g_whip_hook[i];
        }
        float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), step = (100.0f + g_whip_return_dist / 2.0f) * UK_UNIT * dt;
        if (len <= step) {
            memcpy(g_whip_hook, target, sizeof(g_whip_hook));
            g_whip_returning = false;
            whip_clip("Catch", false);
            sound_play("hook_catch", 0.65f, frand(0.9f, 1.1f));
            whip_loop(nullptr);
        } else {
            for (int i = 0; i < 3; i++) g_whip_hook[i] += d[i] / len * step;
        }
        return;
    }

    if (g_whip_state == WHIP_THROWING) {
        if (!key && g_whip_cooldown <= 0.1f) {
            whip_stop(0);
            return;
        }
        float step = WHIP_SPEED * dt, next[3], mid[3];
        for (int i = 0; i < 3; i++) next[i] = g_whip_hook[i] + g_whip_dir[i] * step;
        RayHit h;
        bool wall = step > 0 && ray_cast(g_whip_hook, next, h) && h.hit;
        if (wall) step = h.dist;
        // SphereCastAll(hookPoint, Min(distance from the player / 15, 5), throwDirection, step): a body is taken as
        // 0.45 m to either side of its aim point, from 1.2 m under it to 0.7 m over it (as for the shots)
        float gone = dist3(eye, g_whip_hook), radius = fminf(gone / 15.0f, 5.0f * UK_UNIT);
        for (int i = 0; i < 3; i++) mid[i] = g_whip_hook[i] + g_whip_dir[i] * step * 0.5f;
        Target near_by[8];
        int n = find_targets(mid, near_by, 8, step * 0.5f + radius + 2.5f), caught = -1;
        float best = 1e9f;
        for (int k = 0; k < n; k++) {
            const float *c = near_by[k].p;
            float along = (c[0] - g_whip_hook[0]) * g_whip_dir[0] + (c[1] - g_whip_hook[1]) * g_whip_dir[1] + (c[2] - g_whip_hook[2]) * g_whip_dir[2];
            along = along < 0 ? 0 : along > step ? step : along;
            float q[3] = {g_whip_hook[0] + g_whip_dir[0] * along, g_whip_hook[1] + g_whip_dir[1] * along, g_whip_hook[2] + g_whip_dir[2] * along};
            float dh = sqrtf((q[0] - c[0]) * (q[0] - c[0]) + (q[2] - c[2]) * (q[2] - c[2])), dv = q[1] - c[1];
            if (dh > 0.45f + radius || dv < -1.2f - radius || dv > 0.7f + radius) continue;
            if (ray_blocked(q, c) == 1) continue;         // a wall between the hook's path and the body
            if (along < best) {
                best = along;
                caught = k;
            }
        }
        // "case 10 ... CompareTag("Coin") ... component4.Bounce()": a coin inside the hook's sphere on this
        // step of its flight is bounced; the hook goes on
        for (Coin &c : g_coins) {
            if (!c.alive || c.shot || t - c.born < COIN_ARMED) continue;
            float along = (c.p[0] - g_whip_hook[0]) * g_whip_dir[0] + (c.p[1] - g_whip_hook[1]) * g_whip_dir[1] + (c.p[2] - g_whip_hook[2]) * g_whip_dir[2];
            if (along < 0 || along > step) continue;
            float q[3] = {g_whip_hook[0] + g_whip_dir[0] * along, g_whip_hook[1] + g_whip_dir[1] * along, g_whip_hook[2] + g_whip_dir[2] * along};
            float reach = radius + 0.1f * UK_UNIT + 0.25f;       // the sphere, the coin, and a little for the frame's step (ours)
            if (dist3(q, c.p) > reach) continue;
            bounce_coin(c, t);
            logf("whiplash: bounced a coin (power %d)\n", c.power);
        }
        // a rocket or a cannonball in the hook's sphere on this step is taken, and the hook comes back with it
        if (whip_catch_flying(g_whip_hook, g_whip_dir, step, radius, t)) {
            sound_play("hook_clink", 0.5f, frand(0.9f, 1.1f));
            whip_stop(0);
            return;
        }
        if (caught >= 0) {
            const Target &tg = near_by[caught];
            g_whip_chr = tg.chr;
            memcpy(g_whip_hook, tg.p, sizeof(g_whip_hook));
            g_whip_state = WHIP_CAUGHT;
            whip_loop(nullptr);
            sound_play("hook_hit", 0.65f, frand(1.3f, 1.7f));
            // DeliverDamage(..., 0.2f): a short unseen projectile into the body, from the player's side of it
            float d[3] = {tg.p[0] - eye[0], tg.p[1] - eye[1], tg.p[2] - eye[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), from[3];
            if (len > 0.01f) {
                float back = len > 1.0f ? 0.8f : len * 0.8f;
                for (int i = 0; i < 3; i++) {
                    d[i] /= len;
                    from[i] = tg.p[i] - d[i] * back;
                }
                shoot(BEHAVIOR_WHIP, from, d);
            }
            InterlockedIncrement(&g_whip_catches);
            logf("whiplash: caught chr %p hp %d, %.1f m away (the sphere was %.2f m)\n", (void *)tg.chr, tg.hp, len, radius);
            return;
        }
        if (wall) {
            memcpy(g_whip_hook, h.point, sizeof(g_whip_hook));
            sound_play("hook_clink", 0.65f, frand(1.3f, 1.7f));
            fx_pellet_hit(h.point, h.normal);
            whip_stop(0);
            return;
        }
        if (gone + step > WHIP_MAX) {
            whip_stop(0);
            return;
        }
        for (int i = 0; i < 3; i++) g_whip_hook[i] += g_whip_dir[i] * step;
        return;
    }

    float pt[3];
    if (g_whip_state == WHIP_CAUGHT) {
        if (!whip_target_point(g_whip_chr, pt) || ray_blocked(eye, pt) == 1) {
            whip_stop(0);
            return;
        }
        memcpy(g_whip_hook, pt, sizeof(g_whip_hook));
        if (!key) {
            for (int i = 0; i < 3; i++) g_whip_point[i] = pt[i];
            memcpy(g_whip_last_body, body, sizeof(g_whip_last_body));
            g_whip_stuck = 0;
            g_whip_jump = false;
            g_whip_state = WHIP_PULLING;
            g_whip_pull = true;
            whip_clip("Pull", true);
            g_whip_woosh = sound_play("hook_woosh", 0.5f, 1.0f);
            sound_play("hook_pull", 0.75f, frand(0.9f, 1.1f));
            whip_loop("hook_pull_loop");
            InterlockedIncrement(&g_whip_pulls);
            logf("whiplash: pulling, %.1f m to go\n", dist3(body, pt));
        }
        return;
    }

    // Pulling
    if (!whip_target_point(g_whip_chr, pt) || ray_blocked(body, pt) == 1) {
        whip_stop(1);
        return;
    }
    memcpy(g_whip_hook, pt, sizeof(g_whip_hook));
    for (int i = 0; i < 3; i++) g_whip_point[i] = pt[i];
    auto ground_within = [&](float below) {
        float a[3] = {feet[0], feet[1] + 0.3f, feet[2]}, b[3] = {feet[0], feet[1] - below, feet[2]};
        RayHit g;
        return ray_cast(a, b, g) && g.hit;
    };
    if (g_whip_jump) {
        // HookArm.Update: at least 1 u/s upwards, the whole cut to 30 u/s; then 15 u/s more upwards with
        // no ground within 1.5 u below, and NewMovement.Jump otherwise
        float v[3] = {g_vx, fmaxf(g_vy, 1.0f * UK_UNIT), g_vz}, speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), cap = 30.0f * UK_UNIT;
        if (speed > cap)
            for (float &x : v) x *= cap / speed;
        bool near_ground = ground_within(1.5f * UK_UNIT);
        whip_stop(1);
        if (!near_ground) {
            g_launch_v[0] = v[0];
            g_launch_v[1] = v[1] + 15.0f * UK_UNIT;
            g_launch_v[2] = v[2];
            g_launch_pending = true;
        } else {
            g_vx = v[0];
            g_vz = v[2];
            g_vy = 0;
            g_air = false;
            g_jump_req = true;
        }
        logf("whiplash: jumped out of the pull (%s)\n", near_ground ? "from the ground" : "in the air");
        return;
    }
    // Arriving: V1's test is 0.25 u between the two colliders, now or after this step's movement. A character
    // that will not let the player that near (a wide one) would hold the pull for ever, so a pull that has
    // stopped moving for a quarter of a second has arrived as well (ours).
    float left = dist3(body, pt), moved = dist3(body, g_whip_last_body);
    memcpy(g_whip_last_body, body, sizeof(g_whip_last_body));
    g_whip_stuck = dt > 0 && g_dash_left <= 0 && moved < WHIP_PULL_SPEED * dt * 0.2f ? g_whip_stuck + dt : 0.0f;
    if (left - WHIP_PULL_SPEED * dt < WHIP_ARRIVE || g_whip_stuck > 0.25f) {
        bool on_ground = ground_within(0.35f);
        float rise = pt[1] > body[1] ? pt[1] - body[1] : 0.0f;
        logf("whiplash: arrived (%.1f m from it, %s%s)\n", left, on_ground ? "on the ground" : "in the air", g_whip_stuck > 0.25f ? ", held up" : "");
        whip_stop(1);
        if (on_ground) {
            g_air = false;                       // the ground check is back: the run's own handling takes the speed off
            g_vy = 0;
        } else {
            // "rb.velocity = up * 15", or up * (15 + how far below the hook) from under it
            g_launch_v[0] = g_launch_v[2] = 0;
            g_launch_v[1] = 15.0f * UK_UNIT + rise;
            g_launch_pending = true;
        }
    }
}

// ---- the Sawblade Launcher (Nailgun with altVersion set; its saws are Nail with sawblade set), on 3.
// Two variations: the Attractor (blue; in the code "variation 1") and the Overheat (green; "variation 0").
//   Both:      a saw leaves from 1 u ahead of the eye along the view at 200 u/s and keeps that speed: no
//              gravity. The time between shots is "currentFireRate" hundredths of a second (the prefabs'
//              fireRate is 24). A saw that meets the world is turned back off it, for a quarter of a hit; one
//              that touches an enemy cuts it for a whole hit, and the same enemy again only 0.15 s later; with
//              less than one hit left it breaks. Left alone it is gone after 15 s.
//   Attractor: ten saws, coming back at one in two seconds while fire is up. A saw is 4 u across, does 0.75
//              and has 3.9 hits. The alt fire throws a magnet (three; they come back as fast as there is
//              room for them): it flies at 100 u/s, falls, sticks where it lands or in the enemy it meets,
//              and lasts 12 s from then (5 s if it never lands). Any saw within 25 u of a magnet is steered
//              every physics step (8 ms) to 85 degrees off the line to it, on the side that keeps it going
//              the way it was: it circles the magnet. The step is what sets the circle: a saw covers 1.6 u in
//              one, and 1.6 / (2 cos 85) = 9.2 u is the distance at which a step at that angle neither gains
//              nor loses any. (Until v0.76 a frame was cut into equal steps of whatever length fitted, so the
//              circle was anything from 4.6 u to 9.2 u and changed with the frame rate, and the side was picked
//              at random when the saw was fired, so half of them turned right round on meeting a magnet.)
//              A bounce while it circles costs a tenth of a hit. A magnet holds 15 of weight (an Attractor saw
//              weighs 1, an Overheat one 0.5, the heated one 3; as much again for every other magnet within
//              50 u) and breaks under more. However it goes (its time, the weight, a shot: the harpoon is a
//              'Breakable' for the revolvers and the railcannon unless it sits in a living enemy), its saws
//              are all sent at the spot it was in, at the speed they had (Magnet.Launch).
//   Overheat:  every shot heats the gun by an eighth. Up to half heat it fires at the base rate, beyond that
//              slower (24 + (heat - 0.5) x 65) and wider (up to 45 degrees of spread past a quarter heat);
//              a saw is 3 u across, does 0.6 and has between 3 hits (cold) and 1 (hot). The alt fire, with any
//              heat and the heat sink in, throws that heat into one saw 6 u across: 1 a hit, 20.9 hits, off
//              walls along their own normal, and it stops in an enemy to cut it heat x 3 times at 0.15 s
//              apart before going on. The heat sink is then out (the gun fires slow and wild) and comes
//              back in 8 s of not firing.
// The gun (v0.76): the blade in its jaws turns at 250 + heat x 2250 degrees a second while fire is held and
// runs down at 1000 a second; on the Overheat a second blade round it glows orange as strongly as the gun
// is hot; the display on its back shows the saws and the three magnets, or the heat and the heat sink.
// Sounds (v0.77): the heated saw's looping noise, the blade's whirr on the gun, a saw breaking, the harpoon
// landing, the magnet's beep.
// Not in: punching saws, the nailgun itself, the third variation.
enum { SAW_ATTRACTOR = 0, SAW_OVERHEAT = 1, SAW_HEATED = 2 };
static volatile int g_saw_var = SAW_ATTRACTOR;
static float g_saw_ammo = 10.0f, g_magnet_charge = 3.0f, g_saw_heat = 0.0f, g_saw_sinks = 1.0f, g_saw_cooldown = 0.0f;
static double g_saw_ready_at = 0;                // the Equip clip's CanShoot event
static bool g_saw_shot_ok = false, g_prev_saw_rmb = false;
// The blade in the gun's jaws (Nailgun.Update's spinSpeed, in degrees a second, and the angle it adds up to),
// and the weight of the Animator's second layer, which UpdateAnimationWeight sets at every shot.
static float g_saw_spin = 0, g_saw_blade = 0, g_saw_muddle = 0;
static volatile LONG g_saw_shots = 0, g_saw_hits = 0, g_saw_bounces = 0;
static const int BEHAVIOR_SAW = 9000191;         // + SAW_ATTRACTOR, SAW_OVERHEAT, SAW_HEATED
static const float SAW_SPEED = 200.0f * UK_UNIT, SAW_FIRE_RATE = 24.0f, MAGNET_SPEED = 100.0f * UK_UNIT, MAGNET_RANGE = 25.0f * UK_UNIT;
static const double MAGNET_LIFE = 12.0, SAW_LIFE = 15.0;
static const float MAGNET_MAX_WEIGHT = 15.0f;    // the Harpoon prefab's Magnet.maxWeight (the script's own default is 10)
static const float SAW_MASS[3] = {1.0f, 0.5f, 3.0f};     // the three Nail prefabs' rigid bodies
// The harpoon, from its prefab: the magnet ('Sphere') sits 1.35 u up it from its tail; the box that stops
// it at a wall reaches 4 u ahead of the tail; the box a shot breaks ('Cube') runs from 0.2 u behind the
// tail to 4.3 u ahead and is 0.6 u across.
static const float HARPOON_MAGNET = 1.35f * UK_UNIT, HARPOON_LENGTH = 4.0f * UK_UNIT;
enum { MAX_SAWS = 32, SAW_TRAIL = 64, MAX_MAGNETS = 3, MAGNET_TRAIL = 12 };
struct Saw {
    bool alive, stopped, hidden;
    int kind, multi, multi_left, magnet;         // magnet: the one it is going round (-1: none)
    float p[3], v[3], kept_v[3], hits, same_cd, multi_cd;
    uintptr_t last_chr;
    double born, remove_at;
    float trail[SAW_TRAIL][3];                   // where it has been, newest first, and when
    double trail_t[SAW_TRAIL];
    int trail_n, trail_steps;
};
struct MagnetObj {
    bool alive, stuck;
    float o[3], p[3], v[3], fwd[3];              // o: the harpoon's tail; p: the magnet on it, which the saws go round
    uintptr_t chr;
    double born, die_at, beep_at, beeped;        // beeped: when its light last flashed
    float load;                                  // 0..1 of the weight it can hold
    float trail[MAGNET_TRAIL][3];
    double trail_t[MAGNET_TRAIL];
    int trail_n, trail_steps;
};
static Saw g_saws[MAX_SAWS];
static MagnetObj g_magnets[MAX_MAGNETS];
// (further down, with the rocket launcher) the first enemy a way from a to b passes through
static bool enemy_on_path(const float *a, const float *b, float radius, Target &out, float *at, const uintptr_t *skip = nullptr, int skip_n = 0);
static float g_saw_owed = 0;                     // time the saws have not been stepped through yet (less than one step)

static void trail_push(float (*trail)[3], double *when, int &n, int cap, const float *p, double t) {
    int keep = n < cap ? n : cap - 1;
    if (keep > 0) {
        memmove(trail[1], trail[0], sizeof(float) * 3 * keep);
        memmove(when + 1, when, sizeof(double) * keep);
    }
    memcpy(trail[0], p, sizeof(float) * 3);
    when[0] = t;
    if (n < cap) n++;
}
static void saw_break(Saw &s) {
    const float up[3] = {0, 1, 0};
    s.alive = false;
    if (!s.hidden) fx_pellet_hit(s.p, up);
    // 'BreakParticleMetalSaw': the saw's own breaking sound (0.45) over the ricochet's (0.25)
    if (!sound_play("saw_break", 0.45f, 1.0f)) sound_play("saw_bounce", 0.3f, frand(0.6f, 0.7f));
    else sound_play("saw_bounce", 0.25f, 1.0f);
}
static void saw_reflect(Saw &s, const RayHit &h) {
    float speed = sqrtf(s.v[0] * s.v[0] + s.v[1] * s.v[1] + s.v[2] * s.v[2]);
    if (s.kind == SAW_HEATED) {                  // bounceToSurfaceNormal
        for (int i = 0; i < 3; i++) s.v[i] = h.normal[i] * speed;
    } else {
        float d = (s.v[0] * h.normal[0] + s.v[1] * h.normal[1] + s.v[2] * h.normal[2]) * 2.0f;
        for (int i = 0; i < 3; i++) s.v[i] -= h.normal[i] * d;
    }
    for (int i = 0; i < 3; i++) s.p[i] = h.point[i] + h.normal[i] * 0.02f;
}
// Nail.ForceCheckSawbladeRicochet, and the same loop after a bounce: up to three walls within 5 u ahead are
// taken at once (a saw fired into a corner comes straight back out of it)
static void saw_corner_check(Saw &s, float cost, double t) {
    for (int k = 0; k < 3; k++) {
        float speed = sqrtf(s.v[0] * s.v[0] + s.v[1] * s.v[1] + s.v[2] * s.v[2]);
        if (speed < 1e-3f) return;
        float ahead[3] = {s.p[0] + s.v[0] / speed * 5.0f * UK_UNIT, s.p[1] + s.v[1] / speed * 5.0f * UK_UNIT, s.p[2] + s.v[2] / speed * 5.0f * UK_UNIT};
        RayHit h;
        if (!ray_cast(s.p, ahead, h) || !h.hit) return;
        saw_reflect(s, h);
        trail_push(s.trail, s.trail_t, s.trail_n, SAW_TRAIL, s.p, t);
        s.hits -= cost;
        if (!s.hidden) fx_pellet_hit(h.point, h.normal);
    }
}
static void saw_cut(Saw &s, const Target &tg) {
    float d[3] = {tg.p[0] - s.p[0], tg.p[1] - s.p[1], tg.p[2] - s.p[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), from[3];
    if (len < 0.05f) { d[0] = 0; d[1] = -1; d[2] = 0; len = 1.0f; }
    for (int i = 0; i < 3; i++) {
        d[i] /= len;
        from[i] = tg.p[i] - d[i] * 0.6f;
    }
    note_attack(HIT_REVOLVER);
    shoot(BEHAVIOR_SAW + s.kind, from, d);
    sound_play("saw_hit", 0.2f, frand(0.9f, 1.1f));
    InterlockedIncrement(&g_saw_hits);
}
static bool chr_alive(uintptr_t chr) {
    int hp = 0;
    return chr && safe_read(chr + OFF_CHR_HP, &hp, 4) && hp > 0;
}
static void spawn_saw(int kind, const float *dir, float hits, int multi) {
    float eye[3];
    if (!eye_pos(eye)) return;
    // a free place, or the oldest saw's (until v0.76 a shot with 32 saws out was simply lost)
    Saw *slot = nullptr;
    for (Saw &s : g_saws) {
        if (!s.alive) { slot = &s; break; }
        if (!slot || s.born < slot->born) slot = &s;
    }
    Saw &s = *slot;
    s = Saw{};
    s.alive = true;
    s.kind = kind;
    s.hits = hits;
    s.multi = multi;
    s.magnet = -1;
    s.born = now_s();
    s.remove_at = s.born + SAW_LIFE;
    for (int i = 0; i < 3; i++) {
        s.p[i] = eye[i] + dir[i] * 1.0f * UK_UNIT;
        s.v[i] = dir[i] * SAW_SPEED;
    }
    trail_push(s.trail, s.trail_t, s.trail_n, SAW_TRAIL, s.p, s.born);
    saw_corner_check(s, 0.125f, s.born);
    InterlockedIncrement(&g_saw_shots);
}
static void spawn_magnet(const float *dir) {
    float eye[3];
    if (!eye_pos(eye)) return;
    for (MagnetObj &m : g_magnets) {
        if (m.alive) continue;
        m = MagnetObj{};
        m.alive = true;
        m.born = now_s();
        m.die_at = m.born + 5.0;                 // Harpoon.DestroyIfNotHit; its 12 s start when it lands
        for (int i = 0; i < 3; i++) {
            m.o[i] = eye[i] + dir[i] * 1.0f * UK_UNIT;
            m.fwd[i] = dir[i];
            m.p[i] = m.o[i] + dir[i] * HARPOON_MAGNET;
            m.v[i] = dir[i] * MAGNET_SPEED;
        }
        trail_push(m.trail, m.trail_t, m.trail_n, MAGNET_TRAIL, m.o, m.born);
        return;
    }
}
static int magnets_out() {
    int n = 0;
    for (const MagnetObj &m : g_magnets) n += m.alive ? 1 : 0;
    return n;
}
// Magnet.OnDestroy -> Launch: every saw the magnet had goes for the spot it was in, at the speed it has.
static void break_magnet(MagnetObj &m, double t, const char *why) {
    if (!m.alive) return;
    m.alive = false;
    int sent = 0;
    for (Saw &s : g_saws) {
        if (!s.alive || s.stopped) continue;
        float to[3] = {m.p[0] - s.p[0], m.p[1] - s.p[1], m.p[2] - s.p[2]}, len = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        if (len > MAGNET_RANGE || len < 1e-3f) continue;
        float speed = sqrtf(s.v[0] * s.v[0] + s.v[1] * s.v[1] + s.v[2] * s.v[2]);
        for (int i = 0; i < 3; i++) s.v[i] = to[i] / len * speed;
        trail_push(s.trail, s.trail_t, s.trail_n, SAW_TRAIL, s.p, t);
        s.trail_steps = 0;
        s.magnet = -1;
        s.remove_at = t + SAW_LIFE;              // Nail.MagnetRelease (another magnet in reach takes it again at the next step)
        sent++;
    }
    const float up[3] = {0, 1, 0};
    if (in_sight(m.p)) {
        fx_pellet_hit(m.p, up);
        fx_pellet_hit(m.o, up);
    }
    sound_play("saw_bounce", 0.5f, 0.6f);
    logf("sawblade launcher: magnet gone (%s), %d saws sent at it\n", why, sent);
}
// The magnet whose harpoon the line of fire from the eye passes through, nearest first. One that sits in a
// living enemy has no 'Breakable' left (Harpoon.OnTriggerEnter destroys it) and cannot be shot off.
static MagnetObj *magnet_on_line() {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return nullptr;
    view_axes(right, up, fwd);
    MagnetObj *best = nullptr;
    float best_along = COIN_RANGE;
    const float reach = 0.3f * UK_UNIT + 0.1f;   // half the box, and a little for the aim (ours)
    for (MagnetObj &m : g_magnets) {
        if (!m.alive || (m.stuck && m.chr && chr_alive(m.chr))) continue;
        // where the line of fire and the harpoon's axis come closest
        float a[3], e[3], w[3];
        for (int i = 0; i < 3; i++) {
            a[i] = m.o[i] - m.fwd[i] * 0.2f * UK_UNIT;
            e[i] = m.fwd[i] * 4.5f * UK_UNIT;
            w[i] = eye[i] - a[i];
        }
        float B = fwd[0] * e[0] + fwd[1] * e[1] + fwd[2] * e[2], C = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
        float D = fwd[0] * w[0] + fwd[1] * w[1] + fwd[2] * w[2], E = e[0] * w[0] + e[1] * w[1] + e[2] * w[2], den = C - B * B;
        float u = den > 1e-6f ? (E - B * D) / den : 0.0f;
        u = u < 0 ? 0 : u > 1 ? 1 : u;
        float along = u * B - D;
        if (along <= 0 || along >= best_along) continue;
        float off2 = 0, at[3];
        for (int i = 0; i < 3; i++) {
            at[i] = a[i] + e[i] * u;
            float d = eye[i] + fwd[i] * along - at[i];
            off2 += d * d;
        }
        if (off2 > reach * reach) continue;
        if (g_center_dist > 0 && along > g_center_dist + 0.5f) continue;     // behind a wall
        best = &m;
        best_along = along;
        memcpy(g_magnet_hit, at, sizeof(at));
    }
    return best;
}
// One 8 ms physics step of a saw (Nail.FixedUpdate, and what Nail.Update counts down)
static void saw_step(Saw &s, double t, const Target *near_by, int n) {
    const float h_dt = V1_STEP;
    if (s.same_cd > 0 && !s.stopped) {
        s.same_cd -= h_dt;
        if (s.same_cd <= 0) s.last_chr = 0;
    }
    if (s.stopped) {
        // Nail.Update, multiHitAmount > 1: in the enemy, a cut every 0.15 s
        if (s.multi_cd > 0) { s.multi_cd -= h_dt; return; }
        bool live = chr_alive(s.last_chr);
        if (live && s.multi_left > 0) {
            Target tg{};
            if (whip_target_point(s.last_chr, tg.p)) {
                s.multi_left--;
                s.hits -= 1.0f;
                saw_cut(s, tg);
            } else {
                live = false;
            }
        }
        if (!live || s.multi_left <= 0) {
            s.stopped = false;
            memcpy(s.v, s.kept_v, sizeof(s.v));
            if (s.hits <= 0) saw_break(s);
            return;
        }
        s.multi_cd = 0.15f;
        return;
    }
    // The nearest magnet in reach steers it (Nail.GetTargetMagnet, then FixedUpdate): its velocity becomes
    // the direction to the magnet turned 85 degrees about the upright axis, towards the side the saw is
    // already passing the magnet on, at the speed it had.
    int mi = -1;
    float best = MAGNET_RANGE;
    for (int k = 0; k < MAX_MAGNETS; k++) {
        if (!g_magnets[k].alive) continue;
        float d = dist3(g_magnets[k].p, s.p);
        if (d < best) { best = d; mi = k; }
    }
    if (mi >= 0) {
        const MagnetObj &mag = g_magnets[mi];
        s.remove_at = t + SAW_LIFE;              // (Nail.MagnetCaught: its time to live stops; MagnetRelease starts it again)
        float to[3] = {mag.p[0] - s.p[0], mag.p[1] - s.p[1], mag.p[2] - s.p[2]}, len = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        if (len > 1e-3f) {
            // "new Vector3(velocity.z, velocity.y, -velocity.x)": the velocity turned a quarter about the upright axis
            float side = to[0] * s.v[2] + to[1] * s.v[1] - to[2] * s.v[0];
            float a = 85.0f * 3.14159265f / 180.0f * (side > 0 ? -1.0f : 1.0f), ca = cosf(a), sa = sinf(a);
            float speed = sqrtf(s.v[0] * s.v[0] + s.v[1] * s.v[1] + s.v[2] * s.v[2]);
            for (float &x : to) x /= len;
            // Quaternion.Euler(0, 85 x side, 0) * the direction to the magnet
            s.v[0] = (to[0] * ca + to[2] * sa) * speed;
            s.v[1] = to[1] * speed;
            s.v[2] = (-to[0] * sa + to[2] * ca) * speed;
        }
    }
    s.magnet = mi;
    // enemies within half a unit of it (a body: 0.45 m about its aim point, 1.2 m under to 0.7 m over)
    for (int k = 0; k < n && s.alive && !s.stopped; k++) {
        const Target &tg = near_by[k];
        float dx = tg.p[0] - s.p[0], dz = tg.p[2] - s.p[2], dy = s.p[1] - tg.p[1], reach = 0.45f + 0.5f * UK_UNIT;
        if (dx * dx + dz * dz > reach * reach || dy < -1.2f - 0.25f || dy > 0.7f + 0.25f) continue;
        if (s.same_cd > 0 && s.last_chr == tg.chr) continue;
        if (s.multi > 1) {                       // Nail.TouchEnemy: the heated saw stops in it
            s.stopped = true;
            s.multi_left = s.multi;
            s.multi_cd = 0;
            s.last_chr = tg.chr;
            memcpy(s.kept_v, s.v, sizeof(s.v));
            s.same_cd = 0.15f;
        } else {                                 // Nail.HitEnemy
            s.same_cd = 0.15f;
            s.last_chr = tg.chr;
            s.hits -= 1.0f;
            saw_cut(s, tg);
            if (s.hits < 1.0f) saw_break(s);
        }
    }
    if (!s.alive || s.stopped) return;
    float next[3] = {s.p[0] + s.v[0] * h_dt, s.p[1] + s.v[1] * h_dt, s.p[2] + s.v[2] * h_dt};
    RayHit h;
    if (ray_cast(s.p, next, h) && h.hit) {
        if (s.hits <= 0) { saw_break(s); return; }
        saw_reflect(s, h);
        trail_push(s.trail, s.trail_t, s.trail_n, SAW_TRAIL, s.p, t);
        s.trail_steps = 0;
        InterlockedIncrement(&g_saw_bounces);
        if (!s.hidden) fx_pellet_hit(h.point, h.normal);
        // (The heated saw is silent off walls and floors. Its prefab has the same 'SparksSawblade' sound as the
        // others, but it bounces far more often, with its own noise running all the while: Davi found the
        // two together too loud and hears no bounce in ULTRAKILL.)
        if (s.kind != SAW_HEATED) sound_play("saw_bounce", 0.25f, frand(0.9f, 1.1f));
        s.same_cd = 0;
        s.last_chr = 0;
        s.hits -= mi >= 0 ? 0.1f : 0.25f;
        saw_corner_check(s, 0.0f, t);
    } else {
        memcpy(s.p, next, sizeof(next));
        // A point of its trail at every step while a magnet turns it (8 ms: ten degrees of the circle), so the
        // trail bends smoothly; flying straight, one in six will do. (Unity's own trail takes a point every 0.1 u.)
        if (++s.trail_steps >= (mi >= 0 ? 1 : 6)) {
            s.trail_steps = 0;
            trail_push(s.trail, s.trail_t, s.trail_n, SAW_TRAIL, s.p, t);
        }
    }
}
static void update_saws(double t, float dt) {
    // the magnets: in flight until they land in something, then for their 12 s
    for (MagnetObj &m : g_magnets) {
        if (!m.alive) continue;
        if (g_no_cooldown && m.stuck) {          // TimeBomb.freezeOnNoCooldown
            m.die_at += dt;
            m.beep_at += dt;
        }
        if (t >= m.die_at) {
            break_magnet(m, t, m.stuck ? "its time was up" : "it never landed");
            continue;
        }
        if (m.stuck) {
            if (m.chr) {
                float pt[3];
                if (whip_target_point(m.chr, pt)) {
                    for (int i = 0; i < 3; i++) {
                        m.p[i] = pt[i];
                        m.o[i] = pt[i] - m.fwd[i] * HARPOON_MAGNET;
                    }
                } else {
                    m.chr = 0;                   // its enemy is dead: it stays where it was
                }
            }
        } else {
            m.v[1] -= 40.0f * UK_UNIT * dt;
            float speed = sqrtf(m.v[0] * m.v[0] + m.v[1] * m.v[1] + m.v[2] * m.v[2]);
            if (speed > 1.0f * UK_UNIT)          // Harpoon.Update: it points the way it flies
                for (int i = 0; i < 3; i++) m.fwd[i] = m.v[i] / speed;
            float next[3], tip[3];
            for (int i = 0; i < 3; i++) {
                next[i] = m.o[i] + m.v[i] * dt;
                tip[i] = next[i] + m.fwd[i] * HARPOON_LENGTH;
            }
            // An enemy anywhere on the way the harpoon goes this frame, from its tail to where its point will be,
            // takes it. (Until v0.84 only the spot the magnet was in at the start of the frame was looked at, and
            // at 100 u/s it is most of a metre further on each frame: it went through enemies more often than
            // it stuck. Davi: magnets did not stick to enemies.)
            RayHit h;
            bool wall = ray_cast(m.o, tip, h) && h.hit, landed = false;
            Target took;
            float where[3];
            if (enemy_on_path(m.o, wall ? h.point : tip, 0.1f, took, where)) {
                landed = true;
                m.chr = took.chr;
                sound_play("harpoon_pierce", 0.5f, 1.0f);
                for (int i = 0; i < 3; i++) {    // "magnet.transform.position = other.bounds.center"
                    m.p[i] = took.p[i];
                    m.o[i] = m.p[i] - m.fwd[i] * HARPOON_MAGNET;
                }
                logf("sawblade launcher: magnet stuck in chr %p\n", (void *)m.chr);
            }
            if (!landed && wall) {
                // its point is in the wall, the rest of it stands out
                for (int i = 0; i < 3; i++) {
                    m.o[i] = h.point[i] - m.fwd[i] * (HARPOON_LENGTH - 0.4f * UK_UNIT);
                    m.p[i] = m.o[i] + m.fwd[i] * HARPOON_MAGNET;
                }
                landed = true;
                if (!sound_play("harpoon_stop", 0.5f, 1.0f)) sound_play("saw_bounce", 0.4f, 0.8f);
            }
            if (landed) {
                m.stuck = true;
                m.die_at = t + MAGNET_LIFE;      // TimeBomb.StartCountdown
                m.beep_at = t;
                trail_push(m.trail, m.trail_t, m.trail_n, MAGNET_TRAIL, m.o, t);
            } else {
                for (int i = 0; i < 3; i++) {
                    m.o[i] = next[i];
                    m.p[i] = m.o[i] + m.fwd[i] * HARPOON_MAGNET;
                }
                if (++m.trail_steps >= 3) {
                    m.trail_steps = 0;
                    trail_push(m.trail, m.trail_t, m.trail_n, MAGNET_TRAIL, m.o, t);
                }
            }
        }
        // TimeBomb: its light flashes as it lands and then each time a sixth of what is left has gone by
        if (m.stuck && t >= m.beep_at) {
            m.beeped = t;
            m.beep_at = t + (m.die_at - t) / 6.0;
            // the 'Beeper' (0.2), as high as the magnet is loaded: Magnet.Update's "beeperPitch = load / 2 + 0.25"
            float eye[3], away = eye_pos(eye) ? dist3(eye, m.p) / UK_UNIT : 0.0f, gain = away < 1.0f ? 1.0f : away > 80.0f ? 0.0f : 1.0f - (away - 1.0f) / 79.0f;
            if (gain > 0.01f) sound_play("magnet_beep", 0.2f * gain, m.load / 2.0f + 0.25f);
        }
        // Magnet.Update: what it holds against what it can
        float load = 0;
        int linked = 0;
        for (const Saw &s : g_saws)
            if (s.alive && dist3(s.p, m.p) < MAGNET_RANGE) load += SAW_MASS[s.kind];
        for (const MagnetObj &o : g_magnets)
            if (o.alive && &o != &m && dist3(o.p, m.p) < MAGNET_RANGE * 2.0f) linked++;
        float limit = MAGNET_MAX_WEIGHT * (linked + 1);
        if (load > limit) {
            break_magnet(m, t, "more saws than it can hold");
            continue;
        }
        m.load = load / limit;
    }
    // The saws, in ULTRAKILL's own physics steps of 8 ms, as many as have come due: what is left over is
    // kept for the next frame, and the saws are drawn that much further along their way.
    g_saw_owed += dt;
    int steps = (int)(g_saw_owed / V1_STEP);
    if (steps > 12) {                            // a long frame: the saws fall behind sooner than run through walls of time
        steps = 12;
        g_saw_owed = steps * V1_STEP;
    }
    g_saw_owed -= steps * V1_STEP;
    for (Saw &s : g_saws) {
        if (!s.alive) continue;
        if (t >= s.remove_at || t - s.born > 60.0) { s.alive = false; continue; }
        if (steps <= 0) continue;
        // (who is near it is looked up once a frame, not once a step: the list is a walk over every character)
        Target near_by[4];
        int n = targets_near(s.p, near_by, 4, 2.5f + SAW_SPEED * steps * V1_STEP);
        for (int step = 0; step < steps && s.alive; step++) saw_step(s, t - g_saw_owed - (steps - 1 - step) * V1_STEP, near_by, n);
        if (s.alive) s.hidden = !in_sight(s.p);
    }
    // The heated saw's own noise ('chainsawThrown', the looping source on its hub): 0.25 while it flies and, by
    // Nail.Update, louder and an octave up while it is stopped in an enemy, cutting. It is a sound in the
    // world: full within 1 u of the ear, falling off in a straight line to nothing at 50 u.
    static int voices[MAX_SAWS];
    float eye[3];
    bool have_eye = eye_pos(eye);
    for (int i = 0; i < MAX_SAWS; i++) {
        const Saw &s = g_saws[i];
        if (s.alive && s.kind == SAW_HEATED) {
            float away = have_eye ? dist3(eye, s.p) / UK_UNIT : 0.0f, gain = away < 1.0f ? 1.0f : away > 50.0f ? 0.0f : 1.0f - (away - 1.0f) / 49.0f;
            float volume = (s.stopped ? 0.5f : 0.25f) * gain, pitch = s.stopped ? 2.0f : 1.0f;
            if (!voices[i]) voices[i] = sound_play("saw_chainsaw", volume, pitch, true);
            else sound_set(voices[i], volume, pitch);
        } else if (voices[i]) {
            sound_stop(voices[i]);
            voices[i] = 0;
        }
    }
}
// Nailgun.Update and FixedUpdate for the two launchers
static void update_saw_launcher(double t, float dt, bool lmb, bool rmb) {
    bool held = g_weapon == WEAPON_SAWLAUNCHER, overheat = g_saw_var == SAW_OVERHEAT;
    bool can_shoot = held && t >= g_saw_ready_at, firing = can_shoot && lmb, alt = held && rmb && !g_prev_saw_rmb;
    g_prev_saw_rmb = held && rmb;
    // WeaponCharges: what comes back whichever weapon is held
    if (!(held && !overheat && lmb)) g_saw_ammo = move_towards(g_saw_ammo, 10.0f, dt * 0.5f);
    {
        int out = magnets_out();
        if (g_magnet_charge < 3.0f - out) g_magnet_charge = move_towards(g_magnet_charge, 3.0f - out, dt * 3.0f);
    }
    if (!(held && overheat && lmb) && g_saw_sinks < 1.0f) g_saw_sinks = move_towards(g_saw_sinks, 1.0f, dt * 0.125f);
    if (g_no_cooldown) {
        g_saw_ammo = 10.0f;
        g_magnet_charge = 3.0f;
        g_saw_sinks = 1.0f;
    }
    if (g_saw_cooldown > 0) {
        g_saw_cooldown = move_towards(g_saw_cooldown, 0.0f, dt * 100.0f);
        if (g_saw_cooldown < 0.01f) g_saw_cooldown = 0;
    }
    // the heat
    if (overheat && g_saw_sinks < 1.0f) {
        g_saw_heat = move_towards(g_saw_heat, 0.0f, dt);
    } else if (firing && g_saw_heat < 1.0f) {
        if (!overheat) g_saw_heat = 1.0f;        // (the Attractor: only its rate of fire reads it)
    } else if (g_saw_heat > 0 && !firing) {
        g_saw_heat = move_towards(g_saw_heat, 0.0f, dt * (g_saw_cooldown <= 0 ? 0.2f : 0.03f));
    }
    // "spinSpeed = 250 + heatUp * 2250" while fire is held and the gun is up, otherwise it runs down at 1000 a second
    if (firing) g_saw_spin = 250.0f + g_saw_heat * 2250.0f;
    else g_saw_spin = move_towards(g_saw_spin, 0.0f, dt * 1000.0f);
    g_saw_blade = fmodf(g_saw_blade + g_saw_spin * dt, 360.0f);
    {
        // the blade's own whirr ('MachineGunSpin', the looping source on the gun's Blade): 0.2, at a pitch of
        // spinSpeed / 1500 x 2, so it climbs to 3.3 while firing and runs down with the blade
        static int spin_voice = 0;
        if (held && g_saw_spin > 1.0f) {
            float pitch = g_saw_spin / 1500.0f * 2.0f;
            if (!spin_voice) spin_voice = sound_play("saw_spin", 0.2f, pitch, true);
            else sound_set(spin_voice, 0.2f, pitch);
        } else if (spin_voice) {
            sound_stop(spin_voice);
            spin_voice = 0;
        }
    }
    if (!held) {
        g_saw_muddle = 0;                        // (OnEnable: SetLayerWeight(1, 0))
        return;
    }
    float rate;
    if (!overheat) rate = SAW_FIRE_RATE + 3.5f - g_saw_heat * 3.5f;
    else if (g_saw_sinks >= 1.0f) rate = g_saw_heat < 0.5f ? SAW_FIRE_RATE : SAW_FIRE_RATE + (g_saw_heat - 0.5f) * 65.0f;
    else rate = SAW_FIRE_RATE + 50.0f;
    float right[3], up[3], fwd[3];
    view_axes(right, up, fwd);
    if (alt) {
        if (!overheat) {
            if (g_magnet_charge >= 1.0f) {
                g_magnet_charge -= 1.0f;
                spawn_magnet(fwd);
                g_saw_muddle = 0;
                if (can_shoot) play_revolver("Shoot");
                sound_play("saw_magnet", 0.6f, 1.0f);
                logf("sawblade launcher: magnet thrown (%.0f left)\n", g_magnet_charge);
            } else {
                sound_play("saw_no_ammo", 0.6f, 1.0f);
            }
        } else if (can_shoot && g_saw_sinks >= 1.0f && g_saw_heat >= 0.1f) {
            // Nailgun.SuperSaw
            int multi = (int)lroundf(g_saw_heat * 3.0f);
            g_saw_cooldown = rate;
            g_saw_shot_ok = true;
            g_saw_muddle = 0;
            play_revolver("ShootSuper");
            sound_play("saw_shot_super", 0.7f, 1.0f, false, CH_GUN);
            sound_play("saw_magnet", 0.25f, 1.0f);         // 'NailgunMuzzleFlash 4' has the harpoon's shot under the saw's own
            g_muzzle_flash = t;
            spawn_saw(SAW_HEATED, fwd, 20.9f, multi);
            logf("sawblade launcher: heated saw, heat %.2f (%d cuts an enemy)\n", g_saw_heat, multi);
            g_saw_sinks -= 1.0f;
            g_saw_heat = 0;
            return;
        }
    }
    if (!firing) {
        if (!lmb) g_saw_shot_ok = false;
        return;
    }
    if (g_saw_cooldown != 0) return;
    if (!overheat && (int)lroundf(g_saw_ammo) <= 0) {
        g_saw_cooldown = rate * 2.0f;
        sound_play(g_saw_shot_ok ? "saw_last_shot" : "saw_no_ammo", 0.6f, 1.0f);
        g_saw_shot_ok = false;
        return;
    }
    // Nailgun.Shoot
    g_saw_cooldown = rate;
    g_saw_shot_ok = true;
    g_saw_muddle = !overheat ? 0.0f : g_saw_sinks < 1.0f ? 0.9f : g_saw_heat * 0.6f;
    float dir[3] = {fwd[0], fwd[1], fwd[2]}, hits = 3.9f;
    if (!overheat) {
        g_saw_ammo -= 1.0f;
    } else {
        float spread = g_saw_sinks < 1.0f ? 45.0f : 45.0f * fminf(fmaxf(g_saw_heat - 0.25f, 0.0f), 1.0f);
        float yaw = frand(-spread / 3.0f, spread / 3.0f) * 3.14159265f / 180.0f, pitch = frand(-spread / 3.0f, spread / 3.0f) * 3.14159265f / 180.0f;
        float len = 0;
        for (int i = 0; i < 3; i++) {
            dir[i] = fwd[i] + right[i] * tanf(yaw) + up[i] * tanf(pitch);
            len += dir[i] * dir[i];
        }
        len = sqrtf(len);
        for (float &x : dir) x /= len;
        hits = g_saw_sinks >= 1.0f ? 3.0f + (1.0f - 3.0f) * g_saw_heat : 1.0f;
        if (g_saw_sinks >= 1.0f) g_saw_heat = move_towards(g_saw_heat, 1.0f, 0.125f);
    }
    play_revolver("Shoot");
    sound_play("saw_shot", 0.55f, frand(0.95f, 1.05f), false, CH_GUN);
    g_muzzle_flash = t;
    spawn_saw(overheat ? SAW_OVERHEAT : SAW_ATTRACTOR, dir, hits, 1);
}

// Living enemies within `max_dist` of `from`, out of a list of those within 250 m of the eye that is made once
// and kept for a thirtieth of a second. Everything that flies asks who is near it every frame (each saw, rocket,
// cannonball, magnet, core and boosted pellet), and find_targets walks every character the game has with a
// handful of system calls each: with a dozen saws or rockets in the air that alone was several milliseconds a
// frame, more the longer they stayed (v0.88, after Davi's report of the game slowing down as a session went on).
static Target g_frame_targets[32];
static int g_frame_target_n = 0;
static double g_frame_targets_at = -1.0;
static int targets_near(const float *from, Target *out, int max_n, float max_dist) {
    double now = now_s();
    if (now - g_frame_targets_at > 0.033 || now < g_frame_targets_at) {
        float eye[3];
        g_frame_target_n = eye_pos(eye) ? find_targets(eye, g_frame_targets, 32, 250.0f) : 0;
        g_frame_targets_at = now;
    }
    int n = 0;
    for (int i = 0; i < g_frame_target_n && n < max_n; i++) {
        const Target &c = g_frame_targets[i];
        float dx = c.p[0] - from[0], dy = c.p[1] - from[1], dz = c.p[2] - from[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > max_dist) continue;
        out[n] = c;
        out[n].dist = d;
        n++;
    }
    return n;
}

// ---- the rocket launcher (RocketLauncher, variation 0: the Freezeframe, and 1: the S.R.S. Cannon).
//   Primary:   held, a rocket a second (rateOfFire 1; drawn, it waits out what is left of that, a quarter of a
//              second at the least). The rocket leaves the eye along the view at 100 u/s and flies straight, with
//              no gravity, for up to 30 s. Reaching an enemy it goes off as the plain explosion, with 0.05 s of
//              hit stop; reaching anything else it goes off as 'Explosion Rocket Harmless', which is smoke and a
//              noise and hurts nobody (the ball of fire in that prefab is switched off). Shot in the air it goes
//              off half again as large, as a core does.
//   Freezeframe: alt fire stops every rocket in the air where it is, and again lets them go. The clock on the gun
//              has 5 s of it, runs down while they are stopped and comes back at half a second a second;
//              at nothing it lets them go, and it lets go by itself half a second after the last stopped
//              rocket is gone. A rocket that has hung for a second is "levelled up": it goes off as the super
//              explosion. A stopped rocket goes off on an enemy that comes within 1.75 u of a point 1 u ahead
//              of its tail, half again as large.
//   S.R.S. Cannon: alt fire held winds the cannon up over a second (the gun shakes, the clock turns red);
//              let go, a cannonball leaves 1 u ahead of the eye at 150 u/s x the charge (15 at the least),
//              with (1 - charge) of the player's own velocity, and falls at 40 u/s^2. It comes back in 8 s.
//              The ball does 5, or its speed x 0.15 where that is less, to an enemy it reaches at more than
//              10 u/s, and then bounces off it once (straight up at 0.15 of its speed, 5 u/s back the way it
//              came); the second enemy breaks it. Against anything else it breaks, in a wave along the ground
//              that throws what it reaches (12 u, no damage). A punch sends it on along the view at 150 u/s in
//              a straight line (worth 7 if it had bounced: "DUNKED"); punched, it goes through what it kills
//              and breaks on what it does not. A shot sets it off as 'Explosion Big'.
// v0.84 (none of it tried in the game yet):
//   Riding (Grenade.FixedUpdate, LateUpdate): a rocket that has been frozen or hooked can be ridden. Falling
//              onto it in the air (within 2.25 u of a point 1 u ahead of its tail) puts the player on it: it
//              then flies at 65 u/s, W and S tip its nose down and up and A and D turn it (165 degrees a
//              second), and it sinks more and more after the first two seconds (sooner from the fourth ride
//              without touching the ground). Jump leaves it with a jump, slide drops off it; running into the
//              world ends it in the harmless explosion, into an enemy in the super one.
//   Magnets:   a rocket within 25 u of one of the Attractor's magnets turns towards it at 180 degrees a second
//              (a frozen one points straight at it).
//   Whiplash:  the hook takes a rocket or a cannonball it meets. A rocket comes to the player and goes off
//              there (the super explosion if the player is in the air), or is ridden if it came from below
//              or jump is held; a cannonball comes to hang in front of the player, to be punched.
//   Coins:     a coin's shot goes for a rocket or a cannonball in the air before it goes for an enemy.
// Not in: the napalm.
enum { ROCKET_FREEZE = 0, ROCKET_SRS = 1 };
static volatile int g_rocket_var = ROCKET_FREEZE;
static const int BEHAVIOR_CANNONBALL = 9000200, BEHAVIOR_SHOCKWAVE = 9000204;     // the ball: + 0..3 for 2, 3.5, 5 and 7
static const float ROCKET_SPEED = 100.0f * UK_UNIT, ROCKET_LENGTH = 2.1f * UK_UNIT, ROCKET_RADIUS = 0.35f * UK_UNIT;
static const float ROCKET_SENSE = 1.75f * UK_UNIT, ROCKET_SENSE_AHEAD = 1.05f * UK_UNIT;
static const double ROCKET_LIFE = 30.0;
static const float BALL_RADIUS = 1.0f * UK_UNIT, BALL_GRAVITY = 40.0f * UK_UNIT, BALL_SPEED = 150.0f * UK_UNIT;
enum { MAX_ROCKETS = 16, ROCKET_TRAIL = 12, MAX_BALLS = 4, BALL_TRAIL = 16, BALL_HITS = 8 };
struct Rocket {
    bool alive, hidden, levelled;
    bool hooked, hook_below;                     // the whiplash has it and is bringing it in; it was below the player when it was taken
    bool rideable, in_range, ridden;             // has been frozen or hooked; the player is in reach (so a ride does not start twice); has been ridden
    float downpull;                              // Grenade.downpull: from -0.5; above 0 it is how far the ride has turned into a fall
    float p[3], dir[3], frozen_for;              // p: its tail (the model's own origin); the nose is ROCKET_LENGTH ahead
    double born, trail_at;
    float trail[ROCKET_TRAIL][3];
    double trail_t[ROCKET_TRAIL];
    int trail_n;
};
struct Ball {
    bool alive, hidden, launched, launchable, bounced;
    bool hooked;                                 // the whiplash has it and is bringing it in
    bool force_max;                              // Cannonball.forceMaxSpeed: taken by the whiplash, it hits at full strength whatever its speed
    int bounces, durability, hit_n;
    float p[3], v[3], fwd[3], damage, since_bounce;      // since_bounce: Cannonball's instaBreakDefence
    uintptr_t hit[BALL_HITS];
    double born, trail_at;
    float trail[BALL_TRAIL][3];
    double trail_t[BALL_TRAIL];
    int trail_n;
};
static Rocket g_rockets[MAX_ROCKETS];
static Ball g_balls[MAX_BALLS];
static Rocket *g_ride = nullptr;                 // the rocket the player is on
static const float HOOK_PULL = 60.0f;            // m/s: how fast the whiplash brings a rocket or a cannonball in (ours)
static float g_rocket_cooldown = 0.25f;
static volatile float g_freeze_time = 5.0f, g_cb_ready = 1.0f, g_cb_charge = 0.0f;
static volatile bool g_rockets_frozen = false;
static bool g_auto_unfreeze = false, g_prev_rocket_rmb = false, g_rocket_rmb_stale = false;
static double g_frozen_idle_since = 0, g_rocket_drawn_at = -100.0;
static volatile LONG g_rockets_fired = 0, g_balls_fired = 0, g_rocket_hits = 0, g_ball_hits = 0;

// How loud something `away_u` units off is, for a sound that is full within 1 u and gone at `reach_u`.
static float heard(const float *p, float reach_u) {
    float eye[3];
    if (!eye_pos(eye)) return 1.0f;
    float away = dist3(eye, p) / UK_UNIT;
    return away < 1.0f ? 1.0f : away > reach_u ? 0.0f : 1.0f - (away - 1.0f) / (reach_u - 1.0f);
}
// The first enemy whose body the way from `a` to `b` comes within `radius` of, and where on the way that is.
// A body is the column touches_enemy takes it for. `skip`: characters that do not count.
static bool enemy_on_path(const float *a, const float *b, float radius, Target &out, float *at, const uintptr_t *skip, int skip_n) {
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    Target near_by[8];
    int n = targets_near(a, near_by, 8, len + 3.0f + radius);
    if (n <= 0) return false;
    int steps = (int)ceilf(len / 0.2f);
    if (steps > 64) steps = 64;
    for (int s = 0; s <= steps; s++) {
        float k = steps > 0 ? (float)s / steps : 0.0f, p[3] = {a[0] + d[0] * k, a[1] + d[1] * k, a[2] + d[2] * k};
        for (int i = 0; i < n; i++) {
            bool skipped = false;
            for (int j = 0; j < skip_n; j++) skipped = skipped || skip[j] == near_by[i].chr;
            if (skipped) continue;
            float dx = near_by[i].p[0] - p[0], dz = near_by[i].p[2] - p[2], dy = p[1] - near_by[i].p[1], reach = 0.5f + radius;
            if (dx * dx + dz * dz < reach * reach && dy > -TARGET_HEIGHT - radius && dy < 2.0f - TARGET_HEIGHT + radius) {
                out = near_by[i];
                memcpy(at, p, sizeof(float) * 3);
                return true;
            }
        }
    }
    return false;
}
// 'Explosion Rocket Harmless': the smoke and the noise of an explosion with no ball of fire in it, but with its
// shell: the prefab's second ball ('Sphere_8 (1)': an Explosion of damage 0 that grows at 2.5 to 8 u) is on, and
// it throws the player as any explosion does. This is what a rocket fired at one's feet jumps the player with.
// (v0.82 to v0.84 had no wave: Davi, of ULTRAKILL: "it explodes as a wave that can push the player".)
static void harmless_explosion(const float *p) {
    static const float orange[3] = {1.0f, 0.6f, 0.2f};
    if (in_sight(p)) {
        fx_explosion_sparks(p, 0);
        add_flash(p, 2.0f, 0.3f, orange);
    }
    {
        BlastFx &b = g_blast_fx[g_blast_fx_next++ % MAX_BLAST_FX];
        memcpy(b.p, p, sizeof(b.p));
        b.radius = 8.0f * UK_UNIT;
        b.born = now_s();
        b.wave = true;
        b.kind = -1;
        b.hidden = false;
        blast_limits(b);
        queue_blast(p, 8.0f, 0, 2.5f, false);
    }
    float vol = heard(p, 75.0f);
    if (vol > 0.01f) sound_play("explosion_harmless", vol, frand(0.75f, 1.25f));
}
// how: 0 against the world, 1 on an enemy, 2 shot in the air, 3 shot by a beam that makes an "ultrabooster" of it
static void rocket_end(Rocket &r, int how, double t) {
    r.alive = false;
    if (g_ride == &r) g_ride = nullptr;
    g_frozen_idle_since = t;                     // (Grenade.OnDestroy: timeSinceIdleFrozen = 0)
    bool frozen = g_rockets_frozen;
    if (how == 0) {
        harmless_explosion(r.p);
        logf("rocket: against the world at (%.1f %.1f %.1f): the harmless explosion\n", r.p[0], r.p[1], r.p[2]);
        return;
    }
    float freeze_before = g_freeze_request;
    if (how == 1) {
        // Grenade.Collision: the super explosion for one that has levelled up; Explode: half again as large while frozen
        explode(r.p, r.levelled ? (frozen ? EXPLODE_SUPER_BIG : EXPLODE_SUPER) : frozen ? EXPLODE_BIG : EXPLODE_PLAIN);
        g_freeze_request = freeze_before;        // (no ParryFlash for this, only the hit stop)
        g_hitstop_request = 0.05f;
        InterlockedIncrement(&g_rocket_hits);
    } else {
        // RevolverBeam: "Explode(big: rocket, harmless: false, super: !rocket, 1 or 2, ...)" after a ParryFlash
        explode(r.p, how == 3 ? EXPLODE_SUPER_BIG : EXPLODE_BIG);
        g_freeze_request = 0.25f;
    }
}
static void spawn_rocket(double t, const float *aim = nullptr) {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return;
    view_axes(right, up, fwd);
    if (aim) memcpy(fwd, aim, sizeof(fwd));
    Rocket *slot = nullptr;
    for (Rocket &r : g_rockets) {
        if (!r.alive) { slot = &r; break; }
        if (!slot || r.born < slot->born) slot = &r;
    }
    Rocket &r = *slot;
    r = Rocket{};
    r.alive = true;
    r.downpull = -0.5f;
    r.born = t;
    memcpy(r.p, eye, sizeof(eye));
    memcpy(r.dir, fwd, sizeof(fwd));
    InterlockedIncrement(&g_rockets_fired);
}
static int g_rides = 0;                          // rides since the player last stood on the ground (NewMovement.rocketRides)
static bool g_prev_ride_jump = false, g_prev_ride_slide = false;
static const float RIDE_SPEED = 65.0f * UK_UNIT; // rocketSpeed x 0.65
static volatile LONG g_rides_total = 0;
static bool feet_pos(float *feet) {
    if (!eye_pos(feet)) return false;
    feet[1] -= g_eye_height - g_eye_drop;
    return true;
}
static void ride_start(Rocket &r, double t) {
    g_ride = &r;
    r.in_range = true;
    r.rideable = true;
    if (!r.ridden) {
        r.ridden = true;
        if (++g_rides > 3) r.downpull += 0.25f * (g_rides - 3);
    }
    g_prev_ride_jump = key_down(VK_SPACE) != 0;
    g_prev_ride_slide = key_down(VK_CONTROL) != 0;
    InterlockedIncrement(&g_rides_total);
    logf("rocket: the player is riding it (ride %d since the ground)\n", g_rides);
}
// The magnet a rocket is drawn to: the nearest one in reach that it can see.
static const MagnetObj *rocket_magnet(const Rocket &r) {
    const MagnetObj *best = nullptr;
    float best_d = MAGNET_RANGE;
    for (const MagnetObj &m : g_magnets) {
        if (!m.alive) continue;
        float d = dist3(r.p, m.p);
        if (d >= best_d || d < 0.3f || ray_blocked(r.p, m.p) == 1) continue;
        best = &m;
        best_d = d;
    }
    return best;
}
// Turns unit vector `dir` towards the point `to` as seen from `from`, by `most` radians at the most.
static void turn_towards(float *dir, const float *from, const float *to, float most) {
    float w[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]}, len = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (len < 1e-3f) return;
    for (float &x : w) x /= len;
    float c = dir[0] * w[0] + dir[1] * w[1] + dir[2] * w[2];
    c = c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c;
    float angle = acosf(c);
    if (angle <= most || angle < 1e-3f) {
        memcpy(dir, w, sizeof(w));
        return;
    }
    float k = most / angle, sn = sinf(angle), a = sinf((1.0f - k) * angle) / sn, b = sinf(k * angle) / sn, out[3], ol = 0;
    for (int i = 0; i < 3; i++) {
        out[i] = dir[i] * a + w[i] * b;
        ol += out[i] * out[i];
    }
    ol = sqrtf(ol);
    if (ol < 1e-4f) return;                      // (straight behind: left as it is for this step)
    for (int i = 0; i < 3; i++) dir[i] = out[i] / ol;
}
static void rocket_end(Rocket &r, int how, double t);
// Grenade.FixedUpdate and LateUpdate for the rocket the player is on. The player is carried by being thrown
// along the rocket's way every frame (the same call an explosion throws them with), and the rocket is kept
// under their feet.
static void ride_step(Rocket &r, double t, float dt) {
    float feet[3];
    if (g_dead || !feet_pos(feet)) {
        g_ride = nullptr;
        return;
    }
    // "transform.Rotate(forward-and-back x 165 dt, left-and-right x 165 dt, 0, Space.Self)": W tips the nose down
    float turn = (key_down('D') ? 1.0f : 0.0f) - (key_down('A') ? 1.0f : 0.0f), tip = (key_down('W') ? 1.0f : 0.0f) - (key_down('S') ? 1.0f : 0.0f);
    float yaw = atan2f(r.dir[0], r.dir[2]), pitch = asinf(r.dir[1] > 1.0f ? 1.0f : r.dir[1] < -1.0f ? -1.0f : r.dir[1]);
    const float rate = 165.0f * 3.14159265f / 180.0f;
    yaw += turn * rate * dt;
    pitch -= tip * rate * dt;
    pitch = pitch > 1.5f ? 1.5f : pitch < -1.5f ? -1.5f : pitch;
    r.dir[0] = sinf(yaw) * cosf(pitch);
    r.dir[1] = sinf(pitch);
    r.dir[2] = cosf(yaw) * cosf(pitch);
    float v[3] = {0, 0, 0};
    if (!g_rockets_frozen) {
        float pull = r.downpull > 0 ? (r.downpull < 1.0f ? r.downpull : 1.0f) : 0.0f;
        for (int i = 0; i < 3; i++) v[i] = r.dir[i] * RIDE_SPEED * (1.0f - pull) + (i == 1 ? -100.0f * UK_UNIT * pull : 0.0f);
        float sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!g_no_cooldown) r.downpull += dt / 4.5f * fmaxf(1.0f, 1.0f + (sp > 1e-3f ? v[1] / sp : 0.0f));
    }
    bool jump = key_down(VK_SPACE) != 0, slide = key_down(VK_CONTROL) != 0;
    bool jumped = jump && !g_prev_ride_jump, slid = slide && !g_prev_ride_slide;
    g_prev_ride_jump = jump;
    g_prev_ride_slide = slide;
    if (jumped || slid) {
        // "PlayerRideEnd(); Jump()": off it with a jump, keeping its speed; a slide just lets go
        g_ride = nullptr;
        g_launch_v[0] = v[0];
        g_launch_v[1] = v[1] + (jumped ? V1_JUMP_SPEED : 0.0f);
        g_launch_v[2] = v[2];
        g_launch_pending = true;
        logf("rocket: the player %s off it\n", jumped ? "jumped" : "dropped");
        return;
    }
    float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (speed > 1e-3f) {
        // what is in the way of the player's body this frame (CheckPlayerCollision)
        float body[3] = {feet[0], feet[1] + 0.9f, feet[2]}, ahead[3], at[3];
        for (int i = 0; i < 3; i++) ahead[i] = body[i] + v[i] / speed * (speed * dt + 0.7f);
        RayHit h;
        bool wall = ray_cast(body, ahead, h) && h.hit;
        Target tg;
        if (enemy_on_path(body, wall ? h.point : ahead, 0.5f, tg, at)) {
            g_ride = nullptr;
            memcpy(r.p, at, sizeof(at));
            r.levelled = true;                   // (ridden into an enemy: the super explosion)
            logf("rocket: ridden into an enemy (chr %p)\n", (void *)tg.chr);
            rocket_end(r, 1, t);
            return;
        }
        if (wall) {
            g_ride = nullptr;
            for (int i = 0; i < 3; i++) r.p[i] = h.point[i] + h.normal[i] * 0.15f;
            logf("rocket: ridden into the world\n");
            rocket_end(r, 0, t);
            return;
        }
    }
    memcpy(g_ride_v, v, sizeof(v));
    if (!g_ride_hold || !g_air) {                // (getting on, or the feet have found ground: back into the air)
        memcpy(g_launch_v, v, sizeof(v));
        g_launch_pending = true;
    }
    g_ride_hold = true;
    for (int i = 0; i < 3; i++) r.p[i] = feet[i] - r.dir[i] * ROCKET_LENGTH * 0.5f - (i == 1 ? 0.15f : 0.0f);
}
static void update_rockets(double t, float dt) {
    bool frozen = g_rockets_frozen;
    float loudest = 0, feet[3];
    bool have_feet = feet_pos(feet);
    if (!g_air) g_rides = 0;
    if (g_ride && !g_ride->alive) g_ride = nullptr;
    for (Rocket &r : g_rockets) {
        if (!r.alive) continue;
        // (gone after its 30 s, or once it is 400 m off. With the no-cooldown cheat a rocket is only kept past its time
        // while the rockets are frozen: until v0.88 the cheat kept every rocket for good, and sixteen of them
        // flying on into the sky were looked after every frame from then on.)
        if ((t - r.born > ROCKET_LIFE && !(g_no_cooldown && frozen)) || (have_feet && dist3(r.p, feet) > 400.0f)) {
            r.alive = false;
            if (g_ride == &r) g_ride = nullptr;
            g_frozen_idle_since = t;
            continue;
        }
        if (g_ride == &r) {
            ride_step(r, t, dt);
            if (r.alive) {
                r.hidden = false;
                float vol = heard(r.p, 40.0f);
                if (vol > loudest) loudest = vol;
            }
            continue;
        }
        if (r.hooked && have_feet) {
            // HookArm, when the hook is back with a rocket: it is ridden if it was taken from below (within 45 degrees
            // of straight down) or jump is held, and otherwise goes off at the player, the super explosion with
            // the player in the air.
            float to[3] = {feet[0] - r.p[0], feet[1] + 0.6f - r.p[1], feet[2] - r.p[2]}, dist = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]), step = HOOK_PULL * dt;
            if (dist > step + 0.8f) {
                for (int i = 0; i < 3; i++) r.p[i] += to[i] / dist * step;
                r.hidden = false;
                continue;
            }
            r.hooked = false;
            if (!g_ride && (r.hook_below || key_down(VK_SPACE))) {
                for (int i = 0; i < 3; i++) r.p[i] = feet[i] - r.dir[i] * ROCKET_LENGTH * 0.5f - (i == 1 ? 0.15f : 0.0f);
                logf("whiplash: the rocket is at the player: ridden\n");
                ride_start(r, t);
            } else {
                logf("whiplash: the rocket is at the player: it goes off (%s)\n", g_air ? "in the air: the super explosion" : "on the ground");
                r.alive = false;
                g_frozen_idle_since = t;
                float at[3] = {feet[0], feet[1] + 0.5f, feet[2]};
                explode(at, g_air ? EXPLODE_SUPER : EXPLODE_PLAIN);
            }
            continue;
        }
        r.hooked = false;
        // getting on: falling, in the air, within 2.25 u of the point 1 u ahead of its tail (once per approach)
        if (have_feet && !g_dead) {
            float spot[3] = {r.p[0] + r.dir[0] * UK_UNIT, r.p[1] + r.dir[1] * UK_UNIT, r.p[2] + r.dir[2] * UK_UNIT}, d = dist3(feet, spot);
            bool falling = g_air && g_vy < 0;
            if (d < 2.25f * UK_UNIT && falling && r.rideable) {
                if (!g_ride && !r.in_range) {
                    ride_start(r, t);
                    continue;
                }
            } else if (r.in_range && (d > 3.0f * UK_UNIT || !falling)) {
                r.in_range = false;
            }
        }
        const MagnetObj *mag = rocket_magnet(r);
        Target tg;
        float at[3];
        if (frozen) {
            r.rideable = true;
            if (mag) turn_towards(r.dir, r.p, mag->p, 10.0f);      // "transform.LookAt(the magnet)"
            r.frozen_for += dt;
            if (r.frozen_for >= 1.0f && !r.levelled) {
                r.levelled = true;
                float vol = heard(r.p, 40.0f);
                if (vol > 0.01f) sound_play("rocket_levelup", 0.33f * vol, 3.0f);
                logf("rocket: a second frozen, levelled up\n");
            }
            float c[3] = {r.p[0] + r.dir[0] * ROCKET_SENSE_AHEAD, r.p[1] + r.dir[1] * ROCKET_SENSE_AHEAD, r.p[2] + r.dir[2] * ROCKET_SENSE_AHEAD};
            if (enemy_on_path(c, c, ROCKET_SENSE, tg, at)) {
                logf("rocket: frozen, an enemy came to it (chr %p, hp %d)\n", (void *)tg.chr, tg.hp);
                rocket_end(r, 1, t);
                continue;
            }
        } else {
            if (mag) turn_towards(r.dir, r.p, mag->p, 3.14159265f * dt);       // RotateTowards(..., 180 degrees a second)
            float step = ROCKET_SPEED * dt, next[3], tip[3], end[3];
            for (int i = 0; i < 3; i++) {
                next[i] = r.p[i] + r.dir[i] * step;
                tip[i] = next[i] + r.dir[i] * ROCKET_LENGTH;
            }
            RayHit h;
            bool wall = ray_cast(r.p, tip, h) && h.hit;
            memcpy(end, wall ? h.point : tip, sizeof(end));
            if (enemy_on_path(r.p, end, ROCKET_RADIUS, tg, at)) {
                // (it goes off where its nose is, a little short of the body)
                for (int i = 0; i < 3; i++) r.p[i] = at[i] - r.dir[i] * 0.2f;
                logf("rocket: into an enemy (chr %p, hp %d) %.1f s after it was fired%s\n", (void *)tg.chr, tg.hp, t - r.born, r.levelled ? ", levelled up" : "");
                rocket_end(r, 1, t);
                continue;
            }
            if (wall) {
                for (int i = 0; i < 3; i++) r.p[i] = h.point[i] + h.normal[i] * 0.15f;
                rocket_end(r, 0, t);
                continue;
            }
            memcpy(r.p, next, sizeof(next));
            float vol = heard(r.p, 40.0f);
            if (vol > loudest) loudest = vol;
        }
        r.hidden = !in_sight(r.p);
        if (t >= r.trail_at) {
            r.trail_at = t + 0.15 / (ROCKET_TRAIL - 2);
            trail_push(r.trail, r.trail_t, r.trail_n, ROCKET_TRAIL, r.p, t);
        }
    }
    if (!g_ride) g_ride_hold = false;
    // 'RocketLoop' (0.2, an octave down): one voice for all of them, as loud as the nearest one in flight is
    static int voice = 0;
    if (loudest > 0.01f) {
        if (!voice) voice = sound_play("rocket_loop", 0.2f * loudest, 0.5f, true);
        else sound_set(voice, 0.2f * loudest, 0.5f);
    } else if (voice) {
        sound_stop(voice);
        voice = 0;
    }
}
// The rocket the line of fire from the eye passes through, if any (its capsule: 0.35 u about its length).
static Rocket *rocket_on_line(float *where) {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return nullptr;
    view_axes(right, up, fwd);
    Rocket *best = nullptr;
    float best_along = COIN_RANGE;
    for (Rocket &r : g_rockets) {
        if (!r.alive) continue;                  // (also the one being ridden: the Malicious Railcannon down into it)
        for (int k = 0; k <= 4; k++) {
            float c[3] = {r.p[0] + r.dir[0] * ROCKET_LENGTH * k / 4.0f, r.p[1] + r.dir[1] * ROCKET_LENGTH * k / 4.0f, r.p[2] + r.dir[2] * ROCKET_LENGTH * k / 4.0f};
            float d[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]}, along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
            if (along <= 0.5f || along >= best_along) continue;
            // (what a shot has to meet is the rocket's 'InterruptSphere', 0.875 u about its middle, not its thin body;
            // 0.2 m of grace on top is ours)
            float off2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along, reach = 0.875f * UK_UNIT + 0.2f;
            if (off2 > reach * reach || ray_blocked(eye, c) == 1) continue;
            best = &r;
            best_along = along;
            memcpy(where, c, sizeof(c));
        }
    }
    return best;
}

static void ball_break(Ball &b) {
    const float up[3] = {0, 1, 0};
    b.alive = false;
    if (in_sight(b.p))
        for (int k = 0; k < 3; k++) fx_pellet_hit(b.p, up);
    float vol = heard(b.p, 60.0f);
    if (vol > 0.01f) sound_play("cannonball_break", 0.8f * vol, frand(0.9f, 1.1f));
}
// 'PhysicalShockwavePlayer' where the ball meets the world
static void ball_shockwave(const float *p, double t) {
    note_attack(HIT_EXPLOSION);
    int reached = blast_enemies(p, 3.0f, BEHAVIOR_SHOCKWAVE);
    BlastFx &b = g_blast_fx[g_blast_fx_next++ % MAX_BLAST_FX];
    memcpy(b.p, p, sizeof(b.p));
    b.radius = 3.0f;                             // 12 u across
    b.born = t;
    b.wave = true;
    b.kind = -1;
    b.hidden = false;
    blast_limits(b);
    float vol = heard(p, 100.0f);
    if (vol > 0.01f) sound_play("shockwave", 0.8f * vol, 1.0f);
    logf("cannonball: against the world at (%.1f %.1f %.1f), %d enemies in its wave\n", p[0], p[1], p[2], reached);
}
// Cannonball.Bounce
static void ball_bounce(Ball &b) {
    if (b.bounces >= 1) {                        // maxBounces
        ball_break(b);
        return;
    }
    float speed = sqrtf(b.v[0] * b.v[0] + b.v[1] * b.v[1] + b.v[2] * b.v[2]);
    b.since_bounce = 0;
    b.bounces++;
    b.durability = 99;
    b.bounced = true;
    b.launched = false;
    b.launchable = true;
    if (speed > 1e-4f)
        for (int i = 0; i < 3; i++) b.v[i] = (i == 1 ? speed * 0.15f : 0.0f) - b.v[i] / speed * 5.0f * UK_UNIT;
    float vol = heard(b.p, 60.0f);
    if (vol > 0.01f) sound_play("cannonball_bounce", 0.85f * vol, 1.0f);
}
// Cannonball.Collide on an enemy
static void ball_hit(Ball &b, const Target &tg) {
    if (b.hit_n < BALL_HITS) b.hit[b.hit_n++] = tg.chr;
    if (b.since_bounce < 1.0f) return;           // (just bounced: it goes through whoever is there and does nothing)
    float speed_u = sqrtf(b.v[0] * b.v[0] + b.v[1] * b.v[1] + b.v[2] * b.v[2]) / UK_UNIT;
    bool dies = false;
    if (speed_u > 10.0f || b.force_max) {
        static const float row_damage[4] = {2.0f, 3.5f, 5.0f, 7.0f};
        float damage = b.force_max ? b.damage : fminf(b.damage, speed_u * 0.15f);
        int row = damage >= 6.0f ? 3 : damage >= 4.25f ? 2 : damage >= 2.75f ? 1 : 0;
        float d[3] = {tg.p[0] - b.p[0], tg.p[1] - b.p[1], tg.p[2] - b.p[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), from[3];
        if (len < 0.05f) { d[0] = 0; d[1] = -1; d[2] = 0; len = 1.0f; }
        for (int i = 0; i < 3; i++) {
            d[i] /= len;
            from[i] = tg.p[i] - d[i] * 0.6f;
        }
        note_attack(HIT_REVOLVER);
        bool ok = shoot(BEHAVIOR_CANNONBALL + row, from, d);
        // Whether that kills is not known until the game has dealt it: it is taken to if the enemy has no more
        // health than the row's attack value less a fifth for its armour (a revolver shot's value is 150).
        dies = tg.hp <= (int)(row_damage[row] * 150.0f * 0.8f);
        if (b.launched) style_add(b.bounced ? 150 : 50, b.bounced ? "DUNKED" : "CANNONBOOST", STYLE_GREEN);
        InterlockedIncrement(&g_ball_hits);
        logf("cannonball: %s an enemy (chr %p, hp %d) at %.0f u/s, damage %.1f: %s; taken to %s\n", b.launched ? "punched into" : "into", (void *)tg.chr, tg.hp,
             speed_u, row_damage[row], ok ? "accepted" : "REFUSED", dies ? "kill" : "leave it standing");
    }
    if (dies) {
        if (--b.durability <= 0) {
            ball_break(b);
            return;
        }
        if (!b.launched) ball_bounce(b);
    } else if (speed_u < 15.0f) {
        ball_break(b);
    } else {
        ball_bounce(b);
    }
}
static void spawn_ball(float charge, double t, const float *aim = nullptr) {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return;
    view_axes(right, up, fwd);
    if (aim) memcpy(fwd, aim, sizeof(fwd));
    Ball *slot = nullptr;
    for (Ball &b : g_balls) {
        if (!b.alive) { slot = &b; break; }
        if (!slot || b.born < slot->born) slot = &b;
    }
    Ball &b = *slot;
    b = Ball{};
    b.alive = true;
    b.launchable = true;
    b.durability = 2;
    b.damage = 5.0f;
    b.since_bounce = 1.0f;
    b.born = t;
    float speed = fmaxf(15.0f, charge * 150.0f) * UK_UNIT, own[3] = {g_vx, g_vy, g_vz};
    for (int i = 0; i < 3; i++) {
        b.p[i] = eye[i] + fwd[i] * 1.0f * UK_UNIT;
        b.v[i] = (1.0f - charge) * own[i] + fwd[i] * speed;
        b.fwd[i] = fwd[i];
    }
    InterlockedIncrement(&g_balls_fired);
}
static void update_balls(double t, float dt) {
    for (Ball &b : g_balls) {
        if (!b.alive) continue;
        if (t - b.born > 30.0) { b.alive = false; continue; }      // ours: one that has fallen out of the world
        b.since_bounce += dt;
        if (b.hooked) {
            // the hook is bringing it in: to 1.2 m in front of the face, where it is let go with the player's own speed
            float eye2[3], right[3], up[3], fwd[3];
            if (eye_pos(eye2)) {
                view_axes(right, up, fwd);
                float goal[3] = {eye2[0] + fwd[0] * 1.2f, eye2[1] + fwd[1] * 1.2f, eye2[2] + fwd[2] * 1.2f};
                float to[3] = {goal[0] - b.p[0], goal[1] - b.p[1], goal[2] - b.p[2]}, dist = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]), step = HOOK_PULL * dt;
                if (dist > step) {
                    for (int i = 0; i < 3; i++) b.p[i] += to[i] / dist * step;
                    b.hidden = false;
                    if (t >= b.trail_at) {
                        b.trail_at = t + 0.25 / (BALL_TRAIL - 2);
                        trail_push(b.trail, b.trail_t, b.trail_n, BALL_TRAIL, b.p, t);
                    }
                    continue;
                }
                memcpy(b.p, goal, sizeof(goal));
                b.v[0] = g_vx;
                b.v[1] = g_air && g_vy > 0 ? g_vy : 0.0f;
                b.v[2] = g_vz;
                logf("whiplash: the cannonball is in front of the player\n");
            }
            b.hooked = false;
            continue;
        }
        if (b.launched)
            for (int i = 0; i < 3; i++) b.v[i] = b.fwd[i] * BALL_SPEED;
        else
            b.v[1] -= BALL_GRAVITY * dt;
        float speed = sqrtf(b.v[0] * b.v[0] + b.v[1] * b.v[1] + b.v[2] * b.v[2]);
        if (speed < 1e-4f) continue;
        float next[3], tip[3], end[3];
        for (int i = 0; i < 3; i++) {
            next[i] = b.p[i] + b.v[i] * dt;
            tip[i] = next[i] + b.v[i] / speed * BALL_RADIUS;
        }
        RayHit h;
        bool wall = ray_cast(b.p, tip, h) && h.hit;
        for (int i = 0; i < 3; i++) end[i] = wall ? h.point[i] : next[i];
        Target tg;
        float at[3];
        if (enemy_on_path(b.p, end, BALL_RADIUS, tg, at, b.hit, b.hit_n)) {
            memcpy(b.p, at, sizeof(at));
            ball_hit(b, tg);
            if (!b.alive) continue;
        } else if (wall) {
            for (int i = 0; i < 3; i++) b.p[i] = h.point[i] + h.normal[i] * 0.1f;
            ball_shockwave(b.p, t);
            ball_break(b);
            continue;
        } else {
            memcpy(b.p, next, sizeof(next));
        }
        b.hidden = !in_sight(b.p);
        if (t >= b.trail_at) {
            b.trail_at = t + 0.25 / (BALL_TRAIL - 2);
            trail_push(b.trail, b.trail_t, b.trail_n, BALL_TRAIL, b.p, t);
        }
    }
}
// The ball the line of fire from the eye passes through (its 'InterruptHelper': 2 u about its middle).
static Ball *ball_on_line() {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return nullptr;
    view_axes(right, up, fwd);
    Ball *best = nullptr;
    float best_along = COIN_RANGE;
    for (Ball &b : g_balls) {
        if (!b.alive) continue;
        float d[3] = {b.p[0] - eye[0], b.p[1] - eye[1], b.p[2] - eye[2]}, along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (along <= 0.5f || along >= best_along) continue;
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along > 2.0f * UK_UNIT * 2.0f * UK_UNIT || ray_blocked(eye, b.p) == 1) continue;
        best = &b;
        best_along = along;
    }
    return best;
}
// A shot along the view that meets a rocket or a cannonball in the air sets it off. `beam`: how the shot is
// drawn; `ultra`: a beam that makes an "ultrabooster" of a rocket (the Malicious Railcannon's).
static bool shoot_flying(int beam, bool ultra, double t) {
    float where[3];
    if (Rocket *r = rocket_on_line(where)) {
        shot_beam(where, beam);
        memcpy(r->p, where, sizeof(where));
        logf("rocket: shot in the air%s\n", ultra ? " by the Malicious Railcannon" : "");
        rocket_end(*r, ultra ? 3 : 2, t);
        return true;
    }
    if (Ball *b = ball_on_line()) {
        // Cannonball.Explode: its 'Explosion Big', after a ParryFlash
        shot_beam(b->p, beam);
        b->alive = false;
        explode(b->p, EXPLODE_CANNONBALL);
        g_freeze_request = 0.25f;
        logf("cannonball: shot in the air\n");
        return true;
    }
    return false;
}
// Coin.ReflectRevolver: a rocket or a cannonball in the air is what a coin's shot goes for, before any enemy.
// The rocket goes off as one shot in the air does (half again as large), the cannonball as 'Explosion Big'.
static bool coin_hits_flying(const Coin &c, double t) {
    Rocket *rocket = nullptr;
    Ball *ball = nullptr;
    float best = COIN_RANGE, where[3] = {0, 0, 0};
    for (Rocket &r : g_rockets) {
        if (!r.alive || g_ride == &r) continue;
        float mid[3] = {r.p[0] + r.dir[0] * ROCKET_LENGTH * 0.5f, r.p[1] + r.dir[1] * ROCKET_LENGTH * 0.5f, r.p[2] + r.dir[2] * ROCKET_LENGTH * 0.5f};
        float d = dist3(c.p, mid);
        if (d >= best || ray_blocked(c.p, mid) == 1) continue;
        best = d;
        rocket = &r;
        memcpy(where, mid, sizeof(mid));
    }
    for (Ball &b : g_balls) {
        if (!b.alive) continue;
        float d = dist3(c.p, b.p);
        if (d >= best || ray_blocked(c.p, b.p) == 1) continue;
        best = d;
        ball = &b;
        rocket = nullptr;
        memcpy(where, b.p, sizeof(where));
    }
    if (!rocket && !ball) return false;
    add_beam(c.p, where, coin_beam(c));
    int coins = c.power - 1;
    style_add(50 + (coins > 1 ? coins * 15 : 0), "RICOSHOT", STYLE_CYAN, coins);
    logf("coin shot: power %d into a %s %.1f m from the coin\n", c.power, rocket ? "rocket" : "cannonball", best);
    if (rocket) {
        memcpy(rocket->p, where, sizeof(where));
        rocket_end(*rocket, 2, t);
    } else {
        ball->alive = false;
        explode(ball->p, EXPLODE_CANNONBALL);
        g_freeze_request = 0.25f;
    }
    return true;
}
// HookArm: a rocket or a cannonball the hook's sphere meets on this step of its flight is taken. What ULTRAKILL
// does when the hook is back with it is done at once here: a rocket is at the player and is ridden (if it
// came from below, within 45 degrees of straight down, or jump is held) or goes off there (the super
// explosion with the player in the air); a cannonball hangs in front of the player with their own speed.
static bool whip_catch_flying(const float *from, const float *dir, float step, float radius, double t, Rocket *take_rocket, Ball *take_ball) {
    float feet[3];
    if (!feet_pos(feet)) return false;
    auto reached = [&](const float *p, float size) {
        float along = (p[0] - from[0]) * dir[0] + (p[1] - from[1]) * dir[1] + (p[2] - from[2]) * dir[2];
        if (along < 0 || along > step) return false;
        float q[3] = {from[0] + dir[0] * along, from[1] + dir[1] * along, from[2] + dir[2] * along};
        return dist3(q, p) <= radius + size + 0.25f;
    };
    // (v0.84 to v0.87 did at once what ULTRAKILL does when the hook is back: the rocket or the ball was at the
    // player the moment it was taken. Davi: it should come in with the hook. Now it is marked as hooked here and
    // brought in at HOOK_PULL by update_rockets and update_balls, which do the rest when it arrives.)
    if (!take_ball)
        for (Rocket &r : g_rockets) {
            if (!r.alive || g_ride == &r || r.hooked) continue;
            float mid[3] = {r.p[0] + r.dir[0] * ROCKET_LENGTH * 0.5f, r.p[1] + r.dir[1] * ROCKET_LENGTH * 0.5f, r.p[2] + r.dir[2] * ROCKET_LENGTH * 0.5f};
            if (take_rocket ? take_rocket != &r : !reached(mid, ROCKET_RADIUS + 0.3f)) continue;
            float d[3] = {mid[0] - feet[0], mid[1] - feet[1], mid[2] - feet[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            r.hooked = true;
            r.hook_below = len > 0.1f && -d[1] / len > 0.7071f;      // within 45 degrees of straight down
            r.rideable = true;
            logf("whiplash: a rocket, hooked %.1f m away%s\n", len, r.hook_below ? " (from below: it will be ridden)" : "");
            return true;
        }
    if (take_rocket) return false;
    for (Ball &b : g_balls) {
        if (!b.alive || b.hooked) continue;
        if (take_ball ? take_ball != &b : !reached(b.p, BALL_RADIUS)) continue;
        // "cannonball.Unlaunch(); forceMaxSpeed = true; InstaBreakDefenceCancel()"
        b.hooked = true;
        b.launched = false;
        b.launchable = true;
        b.force_max = true;
        b.since_bounce = 1.0f;
        b.hit_n = 0;
        logf("whiplash: a cannonball, hooked\n");
        return true;
    }
    return false;
}
// The whiplash thrown with a rocket or a cannonball on its line: it is taken at once, without waiting for the
// hook to fly out to it and back (v0.84 waited for the hook to reach it. Davi: a ball should be at the face
// immediately). On the line: within a metre of the view's line, and a twentieth of its distance more, in sight.
static bool whip_take_on_view(double t) {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return false;
    view_axes(right, up, fwd);
    Rocket *rocket = nullptr;
    Ball *ball = nullptr;
    float best = 60.0f;
    auto on_line = [&](const float *p) {
        float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]}, along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (along <= 0.5f || along >= best) return false;
        float reach = 1.0f + along * 0.05f;
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along > reach * reach || ray_blocked(eye, p) == 1) return false;
        best = along;
        return true;
    };
    for (Ball &b : g_balls)
        if (b.alive && on_line(b.p)) { ball = &b; rocket = nullptr; }
    for (Rocket &r : g_rockets) {
        if (!r.alive || g_ride == &r) continue;
        float mid[3] = {r.p[0] + r.dir[0] * ROCKET_LENGTH * 0.5f, r.p[1] + r.dir[1] * ROCKET_LENGTH * 0.5f, r.p[2] + r.dir[2] * ROCKET_LENGTH * 0.5f};
        if (on_line(mid)) { rocket = &r; ball = nullptr; }
    }
    if (!rocket && !ball) return false;
    return whip_catch_flying(eye, fwd, 0.0f, 0.0f, t, rocket, ball);
}
// An explosion sets off the rockets and cannonballs inside it (each as one shot in the air goes off).
static void explosion_sets_off(const float *p, float radius) {
    double t = now_s();
    for (Rocket &r : g_rockets) {
        if (!r.alive || g_ride == &r) continue;
        float mid[3] = {r.p[0] + r.dir[0] * ROCKET_LENGTH * 0.5f, r.p[1] + r.dir[1] * ROCKET_LENGTH * 0.5f, r.p[2] + r.dir[2] * ROCKET_LENGTH * 0.5f};
        if (dist3(p, mid) > radius + 0.44f) continue;
        logf("rocket: set off by an explosion\n");
        memcpy(r.p, mid, sizeof(mid));
        rocket_end(r, 2, t);                     // (it is no longer alive when its own explosion looks for more)
    }
    for (Ball &b : g_balls) {
        if (!b.alive || dist3(p, b.p) > radius + BALL_RADIUS) continue;
        b.alive = false;
        logf("cannonball: set off by an explosion\n");
        explode(b.p, EXPLODE_CANNONBALL);
    }
}

// ---- The visceral attack (ours, after Bloodborne's; not ULTRAKILL's). An enemy that has been parried is
// "stunned" for three seconds: a mark turns over it, and every 0.8 s it is hit by a row that does next to
// nothing but makes it reel again, so it stays open. A Feedbacker punch (F) on a stunned enemy within reach is
// the visceral attack, played out over a second (v0.86; v0.85 was a hook and one blow):
//   0.00 s  the view turns to the enemy and the arm draws back; the player stands where they are until the rip
//   0.10 s  the thrust: in 0.08 s the arm goes half a metre and more forward and towards the middle of the view,
//           into the enemy; as it lands, a short stop of the picture, the sound of the hook going in, blood
//   0.18 s  the hand is in: the arm shakes and turns, blood runs, a dark splash covers the hand on the screen,
//           and the enemy is kept reeling
//   0.62 s  the rip: in 0.12 s the arm comes back out past where it started; as it clears, the blow itself (twelve
//           revolver shots' worth, thrown back), a burst of blood towards the player, blood on the screen, half
//           the health bar back, a longer stop of the picture and 250 style
//   1.05 s  the arm is back
// The arm is the Feedbacker's own, held in its Jab clip's reaching pose and moved as a whole (ULTRAKILL has no
// such animation to take).
// The enemy's part (v0.87): as the hand goes in it is made to play animation 7100 and, as the hand comes out,
// 7110, through what the game's ForcePlayAnimation comes down to (exe+32C130(chr, animation, 0)). Those are two
// of the animations a hollow could be forced into when every tenth one from 4000 to 9990 was tried on
// 2026-10-09 (7000, 7100, 7110, 7210 to 7250, 7300, 9100, 9200, 9600 and the 99xx idles): the 7000s are what
// it plays when it is grabbed or thrown. A character that has no such animation is left as it is.
static const int VISC_ANIM_IN = 7100, VISC_ANIM_OUT = 7110;
static void force_animation(uintptr_t chr, int anim) {
    uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr), vt = 0;
    if (!chr || !safe_read(chr, &vt, 8) || vt != exe + RVA_VT_ENEMY) return;
    ((void (*)(uintptr_t, int, int))(exe + 0x32C130))(chr, anim, 0);
}
// v0.88: it takes two and a quarter seconds, after Bloodborne's own pace (Davi: v0.86's second was too fast): the
// arm is drawn back for 0.35 s, goes in over 0.15 s, stays in for nearly a second, turning in the wound and
// working deeper, and comes out in 0.18 s. The times in the list above are v0.86's.
// v0.89: timed on the two clips of Bloodborne's own attack that Davi put in reference/ (25 frames a second; read
// frame by frame on 2026-10-09). In both: the hunter draws the hand back for 0.65 to 0.8 s as they step in, the
// hand goes in within 0.15 s with a burst of blood at the wound, stays in for 1.35 to 1.4 s while small drops keep
// coming off, and comes out in about 0.3 s, pulling a wide fan of blood after it that hangs as a mist for half a
// second more while the enemy is thrown back; the hunter is free again about 0.9 s after that. (The red and grey
// of those clips is their maker's grading, not the game's, so the red wash here is kept faint.)
static const float VISC_WIND = 0.65f, VISC_IN = 0.80f, VISC_RIP = 2.15f, VISC_OUT = 2.42f, VISC_END = 3.30f, VISC_POSE = 0.10f;
static float g_red_tint = 0;                     // the picture washed in red (HudState.red_tint)
static float g_arm_roll = 0;                     // the arm turned about the line of sight (HudState.arm_roll)
static float g_arm_push[3] = {0, 0, 0};          // where the arm is moved to for it (HudState.arm_push)
static int g_visceral_stage = 0;                 // 0 before the hand is in, 1 in, 2 out
static double g_visceral_hold_at = 0;
static volatile bool g_visceral_gore = false;    // the splash over the hand is up
static const int BEHAVIOR_STUN = 9000205, BEHAVIOR_VISCERAL = 9000206;
struct Stun { uintptr_t chr; double until, next_hold; };
enum { MAX_STUNS = 4 };
static Stun g_stuns[MAX_STUNS];
static double g_visceral_at = -1.0;
static uintptr_t g_visceral_chr = 0;
static volatile LONG g_viscerals = 0;
static void stun_enemy(uintptr_t chr, double t) {
    Stun *slot = &g_stuns[0];
    for (Stun &s : g_stuns) {
        if (s.chr == chr) { slot = &s; break; }
        if (s.until < slot->until) slot = &s;
    }
    *slot = {chr, t + 3.0, t + 0.8};
    sound_play("parry_clang", 0.7f, frand(0.95f, 1.05f));      // (made by tools/uk_sounds.py: the ring of a parry that opens an enemy)
    logf("visceral: chr %p is stunned for 3 s\n", (void *)chr);
}
static bool visceral_start(double t) {
    float right[3], up[3], fwd[3], eye[3], pt[3];
    if (g_visceral_at > 0 || !eye_pos(eye)) return false;
    view_axes(right, up, fwd);
    for (Stun &s : g_stuns) {
        if (!s.chr || t > s.until || !chr_alive(s.chr) || !whip_target_point(s.chr, pt)) continue;
        float d[3] = {pt[0] - eye[0], pt[1] - eye[1], pt[2] - eye[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len > 3.0f || (len > 0.3f && (d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]) / len < 0.5f)) continue;
        g_visceral_chr = s.chr;
        g_visceral_at = t;                       // (when it began)
        g_visceral_stage = 0;
        s.until = t + 3.8;                       // (it stays open until the blow has landed)
        sound_play("parry_clang", 0.45f, 0.55f);
        sound_play("punch_swing", 0.6f, 0.7f);
        logf("visceral: begun on chr %p, %.1f m away\n", (void *)s.chr, len);
        return true;
    }
    return false;
}
static void update_viscerals(double t) {
    float eye[3], pt[3];
    if (!eye_pos(eye)) return;
    for (Stun &s : g_stuns) {
        if (!s.chr) continue;
        if (t > s.until || !chr_alive(s.chr)) { s.chr = 0; continue; }
        if (t >= s.next_hold && s.chr != g_visceral_chr && whip_target_point(s.chr, pt)) {
            s.next_hold = t + 0.8;
            float d[3] = {pt[0] - eye[0], pt[1] - eye[1], pt[2] - eye[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 0.05f && len < 40.0f) {
                float from[3];
                for (int i = 0; i < 3; i++) {
                    d[i] /= len;
                    from[i] = pt[i] - d[i] * 0.9f;
                }
                shoot(BEHAVIOR_STUN, from, d);
            }
        }
    }
    g_visceral_gore = false;
    if (g_visceral_at <= 0) {
        g_arm_push[0] = g_arm_push[1] = g_arm_push[2] = 0;
        g_arm_roll = 0;
        g_red_tint = 0;
        return;
    }
    float a = (float)(t - g_visceral_at);
    uintptr_t chr = g_visceral_chr;
    bool there = chr_alive(chr) && whip_target_point(chr, pt);
    float d[3] = {0, 0, 1}, len = 1.0f, from[3] = {0, 0, 0}, back[3] = {0, 0, -1};
    if (there) {
        for (int i = 0; i < 3; i++) d[i] = pt[i] - eye[i];
        len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 0.05f) { d[0] = 0; d[1] = 0; d[2] = 1; len = 1.0f; }
        for (int i = 0; i < 3; i++) {
            d[i] /= len;
            from[i] = pt[i] - d[i] * 0.9f;
            back[i] = -d[i];
        }
    }
    // blood: drops thrown from `at` about the direction `dir` (the 'blooddrop' picture the explosions' sparks use, in red)
    auto blood = [&](const float *at, const float *dir, int count, float speed, float spread) {
        if (!in_sight(at)) return;
        for (int k = 0; k < count; k++) {
            float r[3], v[3];
            rand_unit(r);
            for (int i = 0; i < 3; i++) v[i] = (dir[i] + r[i] * spread) * speed * frand(0.4f, 1.0f);
            Particle &p = new_particle(at, v, frand(0.35f, 0.7f), frand(0.03f, 0.09f), 9.0f, "blooddrop");      // (v0.86's were three times the size: red discs)
            p.r = 0.75f;
            p.g = 0.02f;
            p.b = 0.02f;
        }
    };
    // the arm: its reaching pose, and where the whole of it is
    float pose = 0, right_k = 0, up_k = 0, fwd_k = 0, roll_k = 0;
    if (a < VISC_WIND) {
        // (drawn back low, by the hip, as the hunter's hand is in Davi's two pictures of Bloodborne's attack; pulled
        // back along the view instead, it filled the screen)
        float k = a / VISC_WIND;
        up_k = -0.24f * k * k * (3.0f - 2.0f * k);
    } else if (a < VISC_IN) {
        float k = (a - VISC_WIND) / (VISC_IN - VISC_WIND);
        k = k * k * (3.0f - 2.0f * k);
        pose = VISC_POSE * k;
        fwd_k = 0.60f * k;
        right_k = 0.24f * k;
        up_k = -0.24f + 0.30f * k;                // (up from the hip into the body)
    } else if (a < VISC_RIP) {
        float in = a - VISC_IN, shake = 0.012f + 0.01f * sinf(in * 9.0f);
        float deeper = in / (VISC_RIP - VISC_IN);
        pose = VISC_POSE;
        roll_k = 0.34f * sinf(in * 5.2f) + 0.05f * sinf(in * 33.0f);          // turning in the wound
        fwd_k = 0.60f + 0.07f * deeper + sinf(in * 47.0f) * shake + 0.04f * sinf(in * 6.0f);
        right_k = 0.24f + sinf(in * 61.0f) * shake;
        up_k = 0.06f + sinf(in * 53.0f + 1.0f) * shake;
    } else if (a < VISC_OUT) {
        float k = (a - VISC_RIP) / (VISC_OUT - VISC_RIP);
        pose = VISC_POSE + (0.33f - VISC_POSE) * k;
        fwd_k = 0.60f - 0.72f * k;
        right_k = 0.24f * (1.0f - k);
        up_k = 0.06f * (1.0f - k);
    } else {
        float k = (a - VISC_OUT) / (VISC_END - VISC_OUT);
        k = k > 1.0f ? 1.0f : k;
        pose = 0.33f + (a - VISC_OUT);
        fwd_k = -0.12f * (1.0f - k);
    }
    // the picture goes red while the hand is in, most as it comes out, and clears over half a second (Bloodborne's own
    // screen in the second of Davi's pictures)
    g_red_tint = a < VISC_IN ? 0.0f : a < VISC_RIP ? 0.12f + 0.16f * (a - VISC_IN) / (VISC_RIP - VISC_IN) : a < VISC_OUT ? 0.6f : fmaxf(0.0f, 0.6f * (1.0f - (a - VISC_OUT) / 0.7f));
    g_arm_roll = roll_k;
    g_arm_push[0] = right_k;
    g_arm_push[1] = up_k;
    g_arm_push[2] = fwd_k;
    g_arm_clip = "Jab";
    g_arm2_clip = nullptr;
    g_arm_clip_start = t - pose;
    if (a < VISC_OUT) {
        // the player stands where they are (the rocket ride's hold, with no speed), and looks at the enemy
        g_ride_v[0] = g_ride_v[1] = g_ride_v[2] = 0;
        g_ride_hold = true;
        if (there && a < VISC_IN) {
            float want_yaw = atan2f(d[0], d[2]), want_pitch = asinf(d[1] > 1.0f ? 1.0f : d[1] < -1.0f ? -1.0f : d[1]);
            float turn = want_yaw - g_yaw;
            turn -= 6.2831853f * floorf((turn + 3.14159265f) / 6.2831853f);
            g_yaw = g_yaw + turn * 0.35f;
            g_pitch = g_pitch + (want_pitch - g_pitch) * 0.35f;
        }
    }
    if (g_visceral_stage == 0 && a >= VISC_IN) {
        g_visceral_stage = 1;
        g_visceral_hold_at = t + 0.2;
        if (there) {
            shoot(BEHAVIOR_STUN, from, d);
            force_animation(chr, VISC_ANIM_IN);
            blood(pt, back, 14, 6.0f, 0.6f);
            g_hitstop_request = 0.07f;
            sound_play("hook_hit", 0.8f, 0.6f);
            sound_play("punch_hit_heavy", 0.7f, 0.7f);
        }
    }
    if (g_visceral_stage == 1 && a < VISC_RIP) {
        g_visceral_gore = there;
        if (there && t >= g_visceral_hold_at) {
            g_visceral_hold_at = t + 0.12;
            blood(pt, back, 4, 2.2f, 1.1f);      // (no more blows while it is held: each one would start its animation over)
        }
    }
    if (g_visceral_stage == 1 && a >= VISC_OUT) {
        g_visceral_stage = 2;
        for (Stun &s : g_stuns)
            if (s.chr == chr) s.chr = 0;
        if (there) {
            note_attack(HIT_PUNCH);
            force_animation(chr, VISC_ANIM_OUT);
            bool ok = shoot(BEHAVIOR_VISCERAL, from, d);
            {
                // the fan of blood the hand pulls out after it, back towards the player and off to one side, and the mist it leaves
                float side[3] = {d[2], 0.0f, -d[0]}, sl = sqrtf(side[0] * side[0] + side[2] * side[2]), pull[3];
                if (sl < 1e-3f) { side[0] = 1; side[2] = 0; sl = 1; }
                for (int i = 0; i < 3; i++) pull[i] = back[i] * 0.8f + side[i] / sl * 0.6f + (i == 1 ? 0.15f : 0.0f);
                blood(pt, pull, 150, 13.0f, 0.45f);
                blood(pt, back, 60, 7.0f, 0.9f);
                static const float mist[3] = {0.55f, 0.02f, 0.02f};
                add_flash(pt, 1.5f, 0.7f, mist);
            }
            splash_screen_blood();
            splash_screen_blood();
            heal_player(50.0f);
            g_hitstop_request = 0.16f;
            sound_play("punch_hit_heavy", 0.9f, 0.6f);
            sound_play("punch_projectile", 0.55f, 0.7f);
            sound_play("hook_hit", 0.7f, 0.45f);
            static const float red[3] = {1.0f, 0.15f, 0.1f};
            style_add(250, "VISCERAL", red);
            InterlockedIncrement(&g_viscerals);
            logf("visceral: the rip out of chr %p: %s\n", (void *)chr, ok ? "accepted" : "REFUSED");
        } else {
            logf("visceral: the enemy was gone before the rip\n");
        }
    }
    if (a >= VISC_END) {
        g_visceral_at = -1.0;
        g_visceral_chr = 0;
        g_visceral_stage = 0;
        g_arm_push[0] = g_arm_push[1] = g_arm_push[2] = 0;
    }
}

// Punch on a cannonball: Cannonball.Launch
static bool punch_ball(double t) {
    float right[3], up[3], fwd[3], eye[3];
    if (!eye_pos(eye)) return false;
    view_axes(right, up, fwd);
    Ball *best = nullptr;
    // Its 'ParryHelper' is a ball 2 u in radius and the punch's own sweep is 1 u wide, 4 u ahead: 6 u along the
    // view and 3 u to either side of it. Widened by a third here (ours: a ball coming off an enemy is small and
    // quick on this screen, and Davi could not get a punch on one).
    float best_along = (PUNCH_REACH + 1.0f) * 1.33f;
    for (Ball &b : g_balls) {
        if (!b.alive || !b.launchable) continue;
        float d[3] = {b.p[0] - eye[0], b.p[1] - eye[1], b.p[2] - eye[2]}, along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (along <= -1.0f || along >= best_along) continue;
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along > 4.0f * UK_UNIT * 4.0f * UK_UNIT) continue;
        best = &b;
        best_along = along;
    }
    if (!best) return false;
    Ball &b = *best;
    // "the ball is put 5 u ahead on the view, or on what the view meets before that, and faces the way the view does"
    float ahead[3] = {eye[0] + fwd[0] * 5.0f * UK_UNIT, eye[1] + fwd[1] * 5.0f * UK_UNIT, eye[2] + fwd[2] * 5.0f * UK_UNIT};
    RayHit h;
    if (ray_cast(eye, ahead, h) && h.hit)
        for (int i = 0; i < 3; i++) ahead[i] = h.point[i] - fwd[i] * 0.3f;
    memcpy(b.p, ahead, sizeof(ahead));
    memcpy(b.fwd, fwd, sizeof(fwd));
    b.launched = true;
    b.force_max = false;
    b.hit_n = 0;
    b.since_bounce = 1.0f;
    if (b.bounces == 1 && b.bounced) b.damage += 2.0f;
    b.bounces++;
    b.trail_n = 0;
    g_freeze_request = 0.25f;                    // TimeController.ParryFlash
    sound_play("punch_projectile", 0.6f, 1.0f);
    logf("cannonball: punched on along the view (damage %.0f)\n", b.damage);
    return true;
}
static void freeze_rockets(bool on, double t) {
    g_rockets_frozen = on;
    if (on) {
        g_frozen_idle_since = t;
        sound_play("rocket_freeze", 1.0f, 1.0f);
    } else {
        g_auto_unfreeze = false;
        sound_play("rocket_unfreeze", 1.0f, 1.0f);
    }
    int out = 0;
    for (const Rocket &r : g_rockets) out += r.alive ? 1 : 0;
    logf("rocket launcher: rockets %s (%d in the air, %.1f s on the clock)\n", on ? "frozen" : "let go", out, (float)g_freeze_time);
}
// RocketLauncher.Update, and what WeaponCharges does for it whichever weapon is held
static void update_rocket_launcher(double t, float dt, bool lmb, bool rmb) {
    bool held = g_weapon == WEAPON_ROCKET, srs = g_rocket_var == ROCKET_SRS;
    bool rmb_pressed = held && rmb && !g_prev_rocket_rmb;
    g_prev_rocket_rmb = held && rmb;
    // WeaponCharges.Charge
    float freeze_time = g_freeze_time, cb_ready = g_cb_ready;
    if (cb_ready < 1.0f) cb_ready = move_towards(cb_ready, 1.0f, dt * 0.125f);
    freeze_time = move_towards(freeze_time, g_rockets_frozen ? 0.0f : 5.0f, g_rockets_frozen ? dt : dt / 2.0f);
    if (g_no_cooldown) {
        cb_ready = 1.0f;
        freeze_time = 5.0f;
    }
    g_freeze_time = freeze_time;
    g_cb_ready = cb_ready;
    if (g_rockets_frozen) {
        int out = 0;
        for (const Rocket &r : g_rockets) out += r.alive ? 1 : 0;
        if (out > 0) g_auto_unfreeze = true;
        if ((g_auto_unfreeze && out == 0 && t - g_frozen_idle_since > 0.5) || freeze_time <= 0) freeze_rockets(false, t);
    }
    {
        // 'RocketLauncherTicker' while they are stopped: 0.5, slowing from its own speed to 0.75 of it as the clock runs out
        static int tick = 0;
        if (g_rockets_frozen) {
            float pitch = 0.75f + 0.25f * freeze_time / 5.0f;
            if (!tick) tick = sound_play("rocket_tick", 0.5f, pitch, true);
            else sound_set(tick, 0.5f, pitch);
        } else if (tick) {
            sound_stop(tick);
            tick = 0;
        }
    }
    if (g_rocket_cooldown > 0) g_rocket_cooldown = move_towards(g_rocket_cooldown, 0.0f, dt);
    static int charge_voice = 0;
    if (!held) {
        g_cb_charge = 0;
        if (charge_voice) {
            sound_stop(charge_voice);
            charge_voice = 0;
        }
        return;
    }
    // (alt fire held from before the gun was drawn does not count for its first quarter of a second)
    if (rmb_pressed) g_rocket_rmb_stale = false;
    if (t - g_rocket_drawn_at >= 0.25 || !rmb) g_rocket_rmb_stale = false;
    bool alt_held = rmb && !g_rocket_rmb_stale;
    if (srs) {
        // the clock's wind-up, at each quarter the cannonball comes back by (half as loud until the last)
        static float last_known = 1.0f;
        if (last_known != cb_ready) {
            for (int n = 4; n > 0; n--)
                if (cb_ready >= n / 4.0f && last_known < n / 4.0f) {
                    sound_play("rocket_wind", cb_ready < 1.0f ? 0.25f : 0.5f, 1.6f + n * 0.1f);
                    break;
                }
            last_known = cb_ready;
        }
        float charge = g_cb_charge;
        if (alt_held && !(lmb && !g_prev_lmb) && cb_ready >= 1.0f) {
            // 'MachinePumpLoop' on the display (0.4), from half its speed to one and a half
            if (!charge_voice) charge_voice = sound_play("srs_charge", 0.4f, charge + 0.5f, true);
            else sound_set(charge_voice, 0.4f, charge + 0.5f);
            charge = move_towards(charge, 1.0f, dt);
        } else if (charge > 0) {
            if (charge_voice) {
                sound_stop(charge_voice);
                charge_voice = 0;
            }
            // RocketLauncher.ShootCannonball
            sound_play("rocket_fire", 0.85f, frand(0.6f, 0.8f), false, CH_GUN);
            play_revolver("Fire");
            sound_later(0.04, "rocket_clunk", 0.5f, frand(0.9f, 1.1f), CH_FREE);
            sound_later(0.76, "rocket_clunk", 0.5f, frand(1.05f, 1.25f), CH_FREE);
            g_muzzle_flash = t;
            g_rocket_cooldown = 1.0f;
            spawn_ball(charge, t);
            logf("rocket launcher: cannonball at charge %.2f (%.0f u/s)\n", charge, fmaxf(15.0f, charge * 150.0f));
            charge = 0;
            g_cb_ready = 0;
        }
        g_cb_charge = charge;
    } else {
        g_cb_charge = 0;
        static float last_known = 5.0f;
        if (last_known != freeze_time) {
            for (int n = 4; n > 0; n--)
                if (freeze_time / 5.0f >= n / 4.0f && last_known / 5.0f < n / 4.0f) {
                    sound_play("rocket_wind", 0.5f, 0.6f + n * 0.1f);
                    break;
                }
            last_known = freeze_time;
        }
        if (rmb_pressed && freeze_time > 0) freeze_rockets(!g_rockets_frozen, t);
    }
    if (g_rocket_cooldown <= 0 && lmb) {
        // RocketLauncher.Shoot
        if (charge_voice) {
            sound_stop(charge_voice);
            charge_voice = 0;
        }
        g_cb_charge = 0;
        sound_play("rocket_fire", 0.85f, frand(0.9f, 1.1f), false, CH_GUN);
        play_revolver("Fire");
        // the Fire clip's two Clunk events (Clunk(1) at 0.04 s, Clunk(1.15) at 0.76 s: each +-0.1)
        sound_later(0.04, "rocket_clunk", 0.5f, frand(0.9f, 1.1f), CH_FREE);
        sound_later(0.76, "rocket_clunk", 0.5f, frand(1.05f, 1.25f), CH_FREE);
        g_muzzle_flash = t;
        g_rocket_cooldown = 1.0f;
        spawn_rocket(t);
    }
}

// ---- Dying, as ULTRAKILL shows it (NewMovement.GetHurt at no health, DeathSequence, TextAppearByLines).
//   The body: "rb.constraints = None; rb.AddTorque(Vector3.right * -1, VelocityChange); cc.enabled = false".
//   The player's rigid body is let go: it keeps the velocity it had, is given a turn of one radian a second
//   about the world's x axis, and falls over like any capsule (3.5 u tall, 1 u across), with the camera
//   still fixed in its head (1.4 u above its middle) and no longer answering the mouse. The weapons and
//   the arms are put away and the HUD is switched off.
//   The screen: for two seconds the sequence's lines appear in the corner, one every 0.05 s, each with a
//   sound (one for a warning, one for an error) over the sequence's own; then the black screen.
// Here the body is a rod with a ball at each end, worked out by hand and not by a physics engine: it flies
// on under gravity (40 u/s^2), stops dead on the ground under its middle, slides to rest (ours: losing
// 24 u/s every second), loses most of its speed against a wall, and is turned over by its own weight about
// the end that is down (3 g / 2 L x the sine of its lean, a rod falling about its foot) and, while it
// slides, towards the way it is going (ours: 10 rad/s^2), until it lies flat. So it falls the way it
// was moving, and from standing still the way ULTRAKILL's turn tips it.
// Underneath, the game's own death goes on as it always has (the health is written to 0 at the killing
// blow): its "YOU DIED", its loading screen and the bonfire. The black screen stays up over all of that
// until the player stands at the bonfire, so the wait is Dark Souls' but nothing of it is seen. A respawn
// without the reload is not in this version: the routines for it are found (the game's own "restart the
// player where they started" at exe+4804C0, the character reset at +0x68 of a character's table) but
// have never been called from here with the game running.
// Where each bonfire puts the player: learnt as the game itself does it (the place the player comes back at
// after a death the game handled, and where they are when the game's "last bonfire" changes, which is at
// a bonfire) and kept in ultrasouls_bonfires.txt beside the exe, a line each: id, x, y, z, facing.
// The last bonfire rested at is an id at +0xB34 of the object at exe+1C74E08 (DSR-Gadget's "ChrClassWarp").
struct BonfireSpot { int id; float p[3], yaw; };
enum { MAX_BONFIRES = 96 };
static BonfireSpot g_bonfires[MAX_BONFIRES];
static int g_bonfire_count = -1;                 // -1: the file has not been read yet
static void bonfires_path(wchar_t *path) {
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    wcscpy(slash ? slash + 1 : path, L"ultrasouls_bonfires.txt");
}
static void bonfires_load() {
    if (g_bonfire_count >= 0) return;
    g_bonfire_count = 0;
    wchar_t path[MAX_PATH];
    bonfires_path(path);
    FILE *f = _wfopen(path, L"r");
    if (!f) return;
    BonfireSpot b;
    while (g_bonfire_count < MAX_BONFIRES && fscanf(f, "%d %f %f %f %f", &b.id, &b.p[0], &b.p[1], &b.p[2], &b.yaw) == 5) g_bonfires[g_bonfire_count++] = b;
    fclose(f);
}
static const BonfireSpot *bonfire_find(int id) {
    bonfires_load();
    for (int i = 0; i < g_bonfire_count; i++)
        if (g_bonfires[i].id == id) return &g_bonfires[i];
    return nullptr;
}
static void bonfire_set(int id, const float *p, float yaw) {
    bonfires_load();
    BonfireSpot *b = nullptr;
    for (int i = 0; i < g_bonfire_count; i++)
        if (g_bonfires[i].id == id) b = &g_bonfires[i];
    if (!b && g_bonfire_count < MAX_BONFIRES) b = &g_bonfires[g_bonfire_count++];
    if (!b) return;
    b->id = id;
    memcpy(b->p, p, sizeof(b->p));
    b->yaw = yaw;
    wchar_t path[MAX_PATH];
    bonfires_path(path);
    if (FILE *f = _wfopen(path, L"w")) {
        for (int i = 0; i < g_bonfire_count; i++) fprintf(f, "%d %.3f %.3f %.3f %.4f\n", g_bonfires[i].id, g_bonfires[i].p[0], g_bonfires[i].p[1], g_bonfires[i].p[2], g_bonfires[i].yaw);
        fclose(f);
    }
    logf("bonfire %d is at (%.1f %.1f %.1f)\n", id, p[0], p[1], p[2]);
}
static int last_bonfire_id() {
    uintptr_t warp = 0;
    int id = -1;
    if (!safe_read((uintptr_t)GetModuleHandleA(nullptr) + 0x1C74E08, &warp, 8) || warp < 0x10000 || !safe_read(warp + 0xB34, &id, 4)) return -1;
    return id > 0 && id < 100000000 ? id : -1;
}
static void death_end(const char *why) {
    if (!g_dead) return;
    g_dead = false;
    g_death.cam = false;
    g_death.begin = false;
    g_death.fast = false;
    sound_all_pitch(1.0f, CH_DEATH);
    if (g_death.buzz) sound_stop(g_death.buzz);
    g_death.buzz = 0;
    logf("death: over (%s)\n", why);
}
static void turn_about(float *v, const float *axis, float angle) {
    float c = cosf(angle), sn = sinf(angle), d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
    float x[3] = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]};
    for (int i = 0; i < 3; i++) v[i] = v[i] * c + x[i] * sn + axis[i] * d * (1.0f - c);
}
static void death_begin(double t) {
    DeathState &d = g_death;
    float eye[3];
    d.begin = false;
    if (g_dead || !eye_pos(eye)) return;
    float sy = sinf(g_yaw), cy = cosf(g_yaw);
    const float R[9] = {cy, 0, -sy, 0, 1, 0, sy, 0, cy};
    memcpy(d.R, R, sizeof(R));
    d.pitch = g_pitch;
    d.c[0] = eye[0];
    d.c[1] = eye[1] - 1.4f * UK_UNIT;
    d.c[2] = eye[2];
    d.v[0] = g_vx;
    d.v[1] = g_air ? g_vy : 0.0f;
    d.v[2] = g_vz;
    if (g_launch_pending)                         // the explosion that killed has not thrown the body yet
        for (int i = 0; i < 3; i++) d.v[i] += g_launch_v[i];
    d.w[0] = -1.0f;
    d.w[1] = d.w[2] = 0;
    d.t0 = t;
    d.gone_since = 0;
    d.seen_dead = false;
    d.lines = 0;
    d.cam = true;
    d.r_up = !(GetAsyncKeyState('R') & 0x8000);     // (R is the whiplash: one held at the blow does not count)
    d.black = false;
    d.laughs = 0;
    d.buzz = 0;
    g_dead = true;
    d.voice = sound_play("death_sequence", 0.66f, 1.0f, false, CH_DEATH);
    d.line_voice = 0;
    logf("death: the body is let go at (%.1f %.1f %.1f) with (%.1f %.1f %.1f) m/s; %s\n", d.c[0], d.c[1], d.c[2], d.v[0], d.v[1], d.v[2],
         d.fast ? "held (R brings the player back)" : "the game has the player dead");
}
// Every enemy put back as it started, the way the game's own script command WarpRestart (exe+4804C0) does it
// to one: the character's reset ("call [table + 0x68]", then bit 0x80 of the flags at +0x2A8, which is all
// ChrResetRequest does), its animation reset (exe+31F8C0), its health set to full through the game's own
// setter (exe+322910), and the "be at" call (table + 0x628) with the place it started at (+0x140).
// Tried in the game on 2026-10-08: the reset alone left a dead enemy dead; with the animation and the health
// it stood again (health 255, its death animation gone), and none of it upset the game. Bodies that were
// lying there from the start (Undead Burg has three by the first stairs) are left as they are.
static int g_reset_way = 3;                       // how much of it is done (a test aid can ask for less)
// How many enemies that were seen alive are dead now.
static int dead_enemies() {
    uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr);
    int n = 0;
    for (int i = 0; i < MAX_TRACKED; i++) {
        uintptr_t havok = g_tracked[i].havok, owner = 0, vt = 0;
        int hp = 1;
        if (!havok || havok == g_player_pos || !g_tracked[i].was_alive) continue;
        if (!safe_read(havok + OFF_HAVOK_OWNER, &owner, 8) || owner < 0x10000) continue;
        uintptr_t chr = owner - OWNER_DELTA;
        if (!safe_read(chr, &vt, 8) || vt != exe + RVA_VT_ENEMY) continue;
        if (safe_read(chr + 0x3E8, &hp, 4) && hp <= 0) n++;
    }
    return n;
}
// The game's own warp to a bonfire, which loads the area again (its script command WarpNextStage_Bonfire).
static bool bonfire_reload(int bonfire) {
    uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr), man = 0;
    if (bonfire <= 0 || !safe_read(exe + 0x1C7B1B0, &man, 8) || !man) return false;
    ((void (*)(uintptr_t, int))(exe + 0x4AAF30))(man, bonfire);
    return true;
}
static int g_reset_mask = 31;                     // which of its steps are done: 1 the reset, 2 the flag, 4 the animation, 8 the health, 16 its place
static uintptr_t g_reset_only = 0;                // (a test aid: only this character)
static int reset_enemies() {
    uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr), slot = 0;
    if (!safe_read(exe + RVA_VT_ENEMY + 0x68, &slot, 8) || slot != exe + 0x31F800) return -1;
    int n = 0;
    for (int i = 0; i < MAX_TRACKED; i++) {
        uintptr_t havok = g_tracked[i].havok, owner = 0, vt = 0, ctrl = 0, back = 0;
        if (!havok || havok == g_player_pos) continue;
        if (!safe_read(havok + OFF_HAVOK_OWNER, &owner, 8) || owner < 0x10000) continue;
        uintptr_t chr = owner - OWNER_DELTA;
        if (!safe_read(chr + 0x68, &ctrl, 8) || ctrl < 0x10000 || !safe_read(ctrl + 0x28, &back, 8) || back != havok) continue;
        if (!safe_read(chr, &vt, 8) || vt != exe + RVA_VT_ENEMY) continue;
        int hp_now = 0;
        if (!safe_read(chr + 0x3E8, &hp_now, 4) || (hp_now <= 0 && !g_tracked[i].was_alive)) continue;
        if (g_reset_only && chr != g_reset_only) continue;
        if (g_reset_mask & 1) ((void (*)(uintptr_t))slot)(chr);
        uint8_t flags = 0;
        if ((g_reset_mask & 2) && safe_read(chr + 0x2A8, &flags, 1)) {
            flags |= 0x80;
            safe_write(chr + 0x2A8, &flags, 1);
        }
        if (g_reset_mask & 4) ((void (*)(uintptr_t))(exe + 0x31F8C0))(chr);
        if (g_reset_mask & 8) {
            int max_hp = 0;
            if (safe_read(chr + 0x3EC, &max_hp, 4) && max_hp > 0) ((void (*)(uintptr_t, int))(exe + 0x322910))(chr, max_hp);
        }
        if (g_reset_mask & 128) {
            // What dying changed in the character's controller (chr + 0xDF0) and its physics body, put back as a living
            // one has it (compared on 2026-10-09: the same hollow alive and dead). Without this a body given its
            // health and animation back stands where it died, cannot move and cannot be hit.
            uint32_t v = 0;
            int32_t minus = -1, two = 2;
            float one = 1.0f;
            uint8_t b = 0;
            if (safe_read(havok + 0x1CC, &v, 4)) { v = (v & ~0xFFu) | 2; safe_write(havok + 0x1CC, &v, 4); }
            if (safe_read(havok + 0x1D0, &v, 4)) { v = (v & ~0xFFu) | 2; safe_write(havok + 0x1D0, &v, 4); }
            if (safe_read(havok + 0x31, &b, 1)) { b = 0; safe_write(havok + 0x31, &b, 1); }
            safe_write(ctrl + 0x134, &minus, 4);
            safe_write(ctrl + 0x138, &minus, 4);
            safe_write(ctrl + 0x3A0, &two, 4);
            if (safe_read(ctrl + 0x270, &b, 1)) { b &= ~1; safe_write(ctrl + 0x270, &b, 1); }
            if (safe_read(chr + 0x2A6, &b, 1)) { b &= ~2; safe_write(chr + 0x2A6, &b, 1); }
            b = 0;
            safe_write(chr + 0x2A7, &b, 1);
            safe_write(chr + 0x2CC, &one, 4);
        }
        if (g_reset_mask & 32) ((void (*)(uintptr_t, int))(exe + 0x325D20))(chr, 1);      // what SetColiEnable(chr, true) does
        if (g_reset_mask & 64) ((void (*)(uintptr_t, int))(exe + 0x325B20))(chr, 0);      // what SetDisable(chr, false) does
        if (g_reset_mask & 16) {
            // and back to where it started (+0x140), through the same "be at" call (table + 0x628)
            float pos[4] = {0, 0, 0, 1}, rot[4] = {0, 0, 0, 0};
            uintptr_t be_at = 0;
            if (safe_read(chr + 0x140, pos, 12) && safe_read(vt + 0x628, &be_at, 8) && be_at > exe && be_at < exe + 0x3200000)
                ((void (*)(uintptr_t, float *, float *, int))be_at)(chr, pos, rot, 1);
        }
        n++;
    }
    return n;
}
// R on the death screen ("Press [R] TO RESTART"): back at the bonfire at once, with full health. Not done to
// the letter of a Dark Souls death: nothing is lost (souls, humanity) and no bloodstain is left, flasks are
// not refilled, and enemies stay as they were unless respawn_enemies is set. If the mod does not know yet
// where the last bonfire puts the player, this one death is left to the game, which shows where.
static uintptr_t g_reload_old_chr = 0;            // the player's character as it was when the warp was asked for
static double g_reload_end_at = 0;                // a restart that goes through the game's warp: when the death screen comes down
static void death_respawn(double t) {
    DeathState &d = g_death;
    if (g_reload_end_at > 0) return;
    uintptr_t chr = g_player_chr;
    int id = last_bonfire_id();
    const BonfireSpot *spot = id > 0 ? bonfire_find(id) : nullptr;
    if (!spot || !chr) {
        logf("death: no place known for bonfire %d; this death is left to the game\n", id);
        d.fast = false;
        d.seen_dead = false;
        if (t - d.t0 < 2.0) d.t0 = t - 2.0;      // straight to the black screen, which covers the game's own
        int zero = 0;
        if (chr) safe_write(chr + 0x3E8, &zero, 4);
        return;
    }
    memcpy(g_teleport_to, spot->p, sizeof(g_teleport_to));
    g_teleport_abs = true;
    g_teleport_keep_safe = false;
    g_teleport_pending = true;
    g_yaw = spot->yaw;
    g_pitch = 0;
    g_vx = g_vy = g_vz = 0;
    int max_hp = 0;
    if (safe_read(chr + 0x3EC, &max_hp, 4) && max_hp > 0) safe_write(chr + 0x3E8, &max_hp, 4);
    g_respawn_grace_until = t + 1.5;
    int reset = g_respawn_enemies ? reset_enemies() : -2;
    {
        // What can be done for the enemies without the game's own reset (which waits to be tried with the game
        // open): those still alive get their health back. The dead stay dead and nobody goes back to their place.
        Target all[64];
        int n = find_targets(spot->p, all, 64, 1.0e6f), healed = 0;
        for (int i = 0; i < n; i++) {
            int max_hp_e = 0;
            if (safe_read(all[i].chr + 0x3EC, &max_hp_e, 4) && max_hp_e > all[i].hp && max_hp_e < 10000000 && safe_write(all[i].chr + 0x3E8, &max_hp_e, 4)) healed++;
        }
        logf("death: %d of %d living enemies given their health back\n", healed, n);
    }
    logf("death: back at bonfire %d (%.1f %.1f %.1f), %.2f s after the blow; enemies reset: %d\n", id, spot->p[0], spot->p[1], spot->p[2], t - d.t0, reset);
    {
        int dead = dead_enemies();
        if (g_restart_reload && dead > 0) {
            bool ok = bonfire_reload(id);
            for (int i = 0; i < MAX_TRACKED; i++) g_tracked[i].was_alive = false;
            if (ok) {
                // The death screen stays up as the cover while the game loads, and the "last place stood on" is
                // forgotten: left known, the fall rescue kept putting the player back where they had been while
                // the game was putting them at the bonfire (tried on 2026-10-09).
                g_have_safe = false;
                g_reload_end_at = t + 8.0;
                g_reload_old_chr = chr;
                return;
            }
            logf("death: %d enemies were dead: the game's own warp to the bonfire, which loads the area again: %s\n", dead, ok ? "asked for" : "NOT possible");
        }
    }
    death_end("R");
}
static void death_step(double t, float dt) {
    DeathState &d = g_death;
    if (!g_dead) return;
    // The warp's cover comes down as soon as the game has the player in the world again (a new character, with
    // health) and a third of a second has gone by for the picture; after eight seconds whatever happens. (v0.87
    // waited six seconds every time.)
    uintptr_t reload_chr = 0;
    bool reload_done = false;
    if (g_reload_end_at > 0) {
        static double back_since = 0;
        uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr), man = 0;
        int hp_now = 0;
        bool back = safe_read(exe + 0x1C77E50, &man, 8) && man && safe_read(man + 0x68, &reload_chr, 8) && reload_chr && reload_chr != g_reload_old_chr &&
                    safe_read(reload_chr + 0x3E8, &hp_now, 4) && hp_now > 0;
        if (!back) back_since = 0;
        else if (back_since == 0) back_since = t;
        reload_done = t >= g_reload_end_at || (back && t - back_since > 0.33);
        if (reload_done) back_since = 0;
        if (!back) reload_chr = 0;
    }
    if (g_reload_end_at > 0 && reload_done) {
        g_reload_end_at = 0;
        g_have_safe = false;
        {
            // (the game brings the player back with the health they had, which was 1: full health again)
            uintptr_t chr = reload_chr ? reload_chr : (uintptr_t)g_player_chr;
            int max_hp = 0;
            if (chr && safe_read(chr + 0x3EC, &max_hp, 4) && max_hp > 0) safe_write(chr + 0x3E8, &max_hp, 4);
            g_respawn_grace_until = t + 1.5;
        }
        death_end("the area is loaded again");
        return;
    }
    // the sequence's lines, and the sound each comes with
    float since = (float)(t - d.t0);
    int total = hud_death_lines(), lines = since < 2.0f ? (int)(since / 0.05f) + 1 : total;
    if (lines > total) lines = total;
    if (since < 2.0f && lines > d.lines) d.line_voice = sound_play(hud_death_line_warning(lines - 1) ? "death_warning" : "death_error", 0.75f, 1.0f, false, CH_DEATH_LINE);
    d.lines = lines;
    if (since >= 2.0f && (d.voice || d.line_voice)) {              // DeathSequence.EndSequence: both are stopped
        if (d.voice) sound_stop(d.voice);
        if (d.line_voice) sound_stop(d.line_voice);
        d.voice = d.line_voice = 0;
    }
    // "SetAllPitch(1 - timeSinceDeath / 2)": every other sound runs down to a stop over the two seconds
    sound_all_pitch(since < 2.0f ? 1.0f - since / 2.0f : 0.0f, CH_DEATH);
    if (since >= 2.0f) {
        // 'BlackScreen': the click it comes up with ('TV Off'), its hum for as long as it is up
        // ('TVScreenBuzz'), and the skull's laugh ('8bitAhh', 0.5) half a second into each 1.017 s of its animation
        if (!d.black) {
            d.black = true;
            sound_play("death_tv_off", 1.0f, 1.0f, false, CH_DEATH);
            d.buzz = sound_play("death_buzz", 1.0f, 1.0f, true, CH_DEATH_LOOP);
        }
        int laughs = (int)floorf((since - 2.0f - 0.5f) / (61.0f / 60.0f)) + 1;
        if (laughs > d.laughs) {
            d.laughs = laughs;
            sound_play("death_laugh", 0.5f, 1.0f, false, CH_DEATH_LINE);
        }
    }
    {
        // R: at once, from the blow on (it only has to have been up since)
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        bool r = pid == GetCurrentProcessId() && (GetAsyncKeyState('R') & 0x8000);
        if (!r) d.r_up = true;
        else if (d.r_up && d.fast) {
            d.r_up = false;
            death_respawn(t);
            if (!g_dead) return;
        }
    }
    if (!d.cam || dt <= 0) return;
    const float half = 1.25f * UK_UNIT, ball = 0.5f * UK_UNIT, head = 1.4f * UK_UNIT, length = 3.5f * UK_UNIT;
    int steps = (int)ceilf(dt / V1_STEP);
    if (steps > 12) steps = 12;
    float h = dt / steps, floor_y = -1.0e9f;
    for (int k = 0; k < steps; k++) {
        float *up = d.R + 3;
        d.v[1] -= UK_GRAVITY * h;
        float next[3] = {d.c[0] + d.v[0] * h, d.c[1] + d.v[1] * h, d.c[2] + d.v[2] * h}, hv = sqrtf(d.v[0] * d.v[0] + d.v[2] * d.v[2]);
        RayHit hit;
        if (hv > 0.5f) {
            // a wall in its way: it is turned back off it with a fifth of its speed
            float ahead[3] = {d.c[0] + d.v[0] / hv * (hv * h + ball), d.c[1], d.c[2] + d.v[2] / hv * (hv * h + ball)};
            if (ray_cast(d.c, ahead, hit) && hit.hit && fabsf(hit.normal[1]) < 0.7f) {
                float into = d.v[0] * hit.normal[0] + d.v[2] * hit.normal[2];
                if (into < 0) {
                    d.v[0] = (d.v[0] - 2.0f * into * hit.normal[0]) * 0.2f;
                    d.v[2] = (d.v[2] - 2.0f * into * hit.normal[2]) * 0.2f;
                }
                next[0] = d.c[0];
                next[2] = d.c[2];
            }
        }
        // the ground under its middle: how far down the body reaches depends on how far over it is
        float reach = fabsf(up[1]) * half + ball;
        // (It stays down once it is down: as it goes over, its middle has less far to reach and would be left
        // hanging over the ground for a moment, turning freely; so within 0.3 m of the ground and not rising,
        // it is on it.)
        float from[3] = {next[0], next[1] + 0.5f, next[2]}, to[3] = {next[0], next[1] - reach - 0.35f, next[2]};
        bool on_ground = false;
        if (ray_cast(from, to, hit) && hit.hit) {
            floor_y = hit.point[1];
            if (next[1] - reach <= hit.point[1] + (d.v[1] <= 0.5f ? 0.3f : 0.0f)) {
                on_ground = true;
                next[1] = hit.point[1] + reach;
                if (d.v[1] < 0) d.v[1] = 0;
            }
        }
        memcpy(d.c, next, sizeof(next));
        if (on_ground) {
            float lean = sqrtf(up[0] * up[0] + up[2] * up[2]);          // the sine of how far over it is
            bool flat = fabsf(up[1]) < 0.05f;
            if (hv > 0.01f) {
                float left = fmaxf(hv - 24.0f * UK_UNIT * h, 0.0f);
                if (!flat) {
                    // sliding on its foot, it goes over towards the way it slides
                    const float axis[3] = {d.v[2] / hv, 0.0f, -d.v[0] / hv};
                    for (int i = 0; i < 3; i++) d.w[i] += axis[i] * 10.0f * h;
                }
                d.v[0] *= left / hv;
                d.v[2] *= left / hv;
            }
            if (flat) {
                // lying: all that is left is a roll about its own length, and that dies away
                float along = d.w[0] * up[0] + d.w[1] * up[1] + d.w[2] * up[2], keep = fmaxf(1.0f - 8.0f * h, 0.0f);
                for (int i = 0; i < 3; i++) d.w[i] = up[i] * along * keep;
            } else if (lean > 1.0e-3f) {
                // its own weight, about the end that is down: over towards the way it already leans (head down,
                // that is the other way round)
                const float axis[3] = {up[2] / lean, 0.0f, -up[0] / lean};
                float pull = 1.5f * UK_GRAVITY / length * lean * h * (up[1] >= 0 ? 1.0f : -1.0f);
                for (int i = 0; i < 3; i++) d.w[i] += axis[i] * pull;
            }
        }
        float turn = sqrtf(d.w[0] * d.w[0] + d.w[1] * d.w[1] + d.w[2] * d.w[2]);
        if (turn > 1.0e-4f) {
            if (turn > 12.0f) {                                         // (no faster than two turns a second)
                for (float &x : d.w) x *= 12.0f / turn;
                turn = 12.0f;
            }
            const float axis[3] = {d.w[0] / turn, d.w[1] / turn, d.w[2] / turn};
            float before = up[1];
            for (int r = 0; r < 3; r++) turn_about(d.R + r * 3, axis, turn * h);
            if (on_ground && before != 0 && before * up[1] <= 0) {
                // it has come down flat: it does not go on through the ground. Half of its roll is kept.
                up[1] = 0;
                float ul = sqrtf(up[0] * up[0] + up[2] * up[2]);
                if (ul > 1.0e-4f) { up[0] /= ul; up[2] /= ul; }
                float along = d.w[0] * up[0] + d.w[2] * up[2];
                for (int i = 0; i < 3; i++) d.w[i] = up[i] * along * 0.5f;
            }
            // keep the three axes square to one another
            float *rt = d.R, *fw = d.R + 6;
            float ul = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
            for (int i = 0; i < 3; i++) up[i] /= ul > 1.0e-6f ? ul : 1.0f;
            float dot = fw[0] * up[0] + fw[1] * up[1] + fw[2] * up[2], fl = 0;
            for (int i = 0; i < 3; i++) { fw[i] -= up[i] * dot; fl += fw[i] * fw[i]; }
            fl = fl > 1.0e-12f ? sqrtf(fl) : 1.0f;
            for (int i = 0; i < 3; i++) fw[i] /= fl;
            rt[0] = up[1] * fw[2] - up[2] * fw[1];
            rt[1] = up[2] * fw[0] - up[0] * fw[2];
            rt[2] = up[0] * fw[1] - up[1] * fw[0];
        }
    }
    // the view: fixed in the head, as it was looking
    const float *rt = d.R, *up = d.R + 3, *fw = d.R + 6;
    float cp = cosf(d.pitch), sp = sinf(d.pitch);
    for (int i = 0; i < 3; i++) {
        d.eye[i] = d.c[i] + up[i] * head;
        d.right[i] = rt[i];
        d.up[i] = up[i] * cp - fw[i] * sp;
        d.fwd[i] = fw[i] * cp + up[i] * sp;
    }
    if (d.eye[1] < floor_y + 0.15f) d.eye[1] = floor_y + 0.15f;      // (the eye is not put into the ground)
}
// Whether the death is over: called by the thread that watches the player, with what it sees of them.
static void death_watch(bool player_there, int hp, double t) {
    DeathState &d = g_death;
    {
        // the bonfires' places, learnt: a moment after a death the game handled, and when the last bonfire changes
        static int seen_id = -1;
        static uintptr_t seen_chr = 0;
        static double chr_since = 0, learn_at = 0;
        uintptr_t chr = player_there ? (uintptr_t)g_player_chr : 0;
        if (chr != seen_chr) {
            seen_chr = chr;
            chr_since = t;
            seen_id = -1;
        }
        int id = chr && hp > 0 && !g_dead ? last_bonfire_id() : -1;
        if (id > 0) {
            if (seen_id > 0 && id != seen_id && t - chr_since > 5.0) learn_at = t;       // rested at another one just now
            if (g_bonfire_learn) {
                g_bonfire_learn = false;
                learn_at = t + 1.5;                  // back from a death: when the controller has the player again
            }
            if (learn_at > 0 && t >= learn_at && t - g_proxy_now_at < 0.5) {
                learn_at = 0;
                bonfire_set(id, g_proxy_now, g_yaw);
            }
            seen_id = id;
        }
    }
    if (!g_dead) return;
    if (d.fast) {
        // held: only the game going away ends it (the player quit to the title screen)
        if (player_there) d.gone_since = 0;
        else if (d.gone_since <= 0) d.gone_since = t;
        else if (t - d.gone_since > 10.0) death_end("the player is gone");
        return;
    }
    if (!player_there) {
        d.seen_dead = true;
        if (d.gone_since <= 0) d.gone_since = t;
        else if (t - d.gone_since > 40.0) death_end("no player for 40 s: back at the title screen?");
    } else {
        d.gone_since = 0;
        if (hp <= 0) d.seen_dead = true;
        else if (d.seen_dead && t - d.t0 > 1.0) {
            death_end("the player stands again");
            g_bonfire_learn = true;
        }
        else if (t - d.t0 > 15.0 && !d.seen_dead) death_end("the game never had the player dead");
    }
    if (g_dead && t - d.t0 > 180.0) death_end("three minutes");
}

// once per frame, on the game's thread
static void try_instant_fire() {
    static double last = 0;
    double t = now_s();
    float dt = last > 0 && t - last > 0 && t - last < 0.1 ? (float)(t - last) : 0.0f;   // the clock steps back when a freeze begins
    if (g_death.begin) death_begin(t);
    death_step(t, dt);
    last = t;
    update_coins(t, dt);
    {
        // which of the characters around have been seen alive (twice a second)
        static double next_pass = 0;
        if (t >= next_pass) {
            next_pass = t + 0.5;
            for (int i = 0; i < MAX_TRACKED; i++) {
                uintptr_t havok = g_tracked[i].havok, owner = 0;
                int hp = 0;
                if (!havok || g_tracked[i].was_alive || !safe_read(havok + OFF_HAVOK_OWNER, &owner, 8) || owner < 0x10000) continue;
                if (safe_read(owner - OWNER_DELTA + 0x3E8, &hp, 4) && hp > 0) g_tracked[i].was_alive = true;
            }
        }
    }
    {
        // Test aid: a line in ultrasouls_debug.txt beside the exe is carried out and the file removed.
        //   kill            the nearest living enemy's health to 0
        //   reset N         reset_enemies() the N-th way
        //   rocket          a rocket from the eye at the nearest enemy's aim point
        //   ball N          a cannonball the same way, at a charge of N hundredths
        //   punchball       what a punch does to a cannonball in reach
        //   boom H V D [R]  attack row R (the plain explosion's if left out) set off H cm from the nearest enemy's aim
        //                   point towards the player and V cm above it, sent up (D 0), down (1), at the enemy (2)
        //                   or away from it (3); the enemy's health a second later is logged
        // (the game's ForcePlayAnimation, exe+4AB170, comes down to exe+32C130(chr, animation, flag) once it has the character)
        static uintptr_t scan_chr = 0;
        static int scan_cur = 0, scan_end = 0, scan_step = 1, scan_took = 0;
        static double scan_at = 0;
        if (scan_chr && t >= scan_at) {
            int now_anim = -1;
            enemy_attacking(scan_chr, now_anim);
            if (now_anim == scan_cur) {
                logf("debug: animscan: %d took\n", scan_cur);
                scan_took++;
            }
            scan_cur += scan_step;
            if (scan_cur > scan_end || !chr_alive(scan_chr)) {
                logf("debug: animscan done (%d took)\n", scan_took);
                scan_chr = 0;
            } else {
                ((void (*)(uintptr_t, int, int))((uintptr_t)GetModuleHandleA(nullptr) + 0x32C130))(scan_chr, scan_cur, 0);
                scan_at = t + 0.25;
            }
        }
        static double next_look = 0, report_at = 0;
        static uintptr_t report_chr = 0;
        static int report_hp = 0;
        if (report_chr && t >= report_at) {
            int hp = -1;
            safe_read(report_chr + 0x3E8, &hp, 4);
            logf("debug: boom result: chr %p health %d -> %d\n", (void *)report_chr, report_hp, hp);
            report_chr = 0;
        }
        if (g_ctrl && t >= next_look) {
            next_look = t + 0.25;
            static wchar_t path[MAX_PATH];
            if (!path[0]) {
                GetModuleFileNameW(nullptr, path, MAX_PATH);
                wchar_t *slash = wcsrchr(path, L'\\');
                wcscpy(slash ? slash + 1 : path, L"ultrasouls_debug.txt");
            }
            if (FILE *f = _wfopen(path, L"r")) {
                char cmd[32] = "";
                int arg = 0, arg2 = 0, arg3 = 0, arg4 = 0;
                int got = fscanf(f, "%31s %d %d %d %d", cmd, &arg, &arg2, &arg3, &arg4);
                fclose(f);
                DeleteFileW(path);
                float eye[3];
                if (got >= 1 && !strcmp(cmd, "kill") && eye_pos(eye)) {
                    Target near_by[1];
                    if (find_targets(eye, near_by, 1, 200.0f) == 1) {
                        int zero = 0;
                        safe_write(near_by[0].chr + 0x3E8, &zero, 4);
                        logf("debug: chr %p (hp %d, %.1f m away) set to no health\n", (void *)near_by[0].chr, near_by[0].hp, near_by[0].dist);
                    }
                } else if (got >= 2 && (!strcmp(cmd, "anim") || !strcmp(cmd, "animscan")) && eye_pos(eye)) {
                    // anim N [F]: the nearest living enemy is made to play animation N; animscan A B [S]: every one from A to B
                    // in steps of S, a quarter of a second each, logging the ones it really went into
                    Target near_by[1];
                    if (find_targets(eye, near_by, 1, 200.0f) == 1) {
                        uintptr_t fn = (uintptr_t)GetModuleHandleA(nullptr) + 0x32C130;
                        if (cmd[4]) {
                            scan_chr = near_by[0].chr;
                            scan_cur = arg;
                            scan_end = got >= 3 ? arg2 : arg;
                            scan_step = got >= 4 && arg3 > 0 ? arg3 : 1;
                            scan_took = 0;
                            scan_at = t + 0.25;
                            ((void (*)(uintptr_t, int, int))fn)(scan_chr, scan_cur, 0);
                            logf("debug: animscan %d to %d on chr %p\n", scan_cur, scan_end, (void *)scan_chr);
                        } else {
                            ((void (*)(uintptr_t, int, int))fn)(near_by[0].chr, arg, got >= 3 ? arg2 : 0);
                            logf("debug: anim %d forced on chr %p\n", arg, (void *)near_by[0].chr);
                        }
                    }
                } else if (got >= 2 && !strcmp(cmd, "resetx") && eye_pos(eye)) {
                    // the steps in the mask `arg`, on the nearest character alone (dead or alive) unless a second number is given
                    Target near_by[1];
                    g_reset_only = 0;
                    if (got < 3) {
                        // (the nearest dead one within 8 m if there is one that was seen alive, else the nearest of all)
                        Target all[12];
                        int m = find_targets(eye, all, 12, 200.0f, true);
                        for (int i = 0; i < m && !g_reset_only; i++)
                            if (all[i].hp <= 0 && all[i].dist < 8.0f)
                                for (int k = 0; k < MAX_TRACKED; k++) {
                                    uintptr_t owner = 0;
                                    if (g_tracked[k].havok && g_tracked[k].was_alive && safe_read(g_tracked[k].havok + OFF_HAVOK_OWNER, &owner, 8) && owner - OWNER_DELTA == all[i].chr)
                                        g_reset_only = all[i].chr;
                                }
                        if (!g_reset_only && m > 0) g_reset_only = all[0].chr;
                    }
                    g_reset_mask = arg;
                    int n = reset_enemies();
                    uint32_t f1 = 0, f2 = 0;
                    int hp = -1;
                    if (g_reset_only) {
                        safe_read(g_reset_only + 0x2A8, &f1, 4);
                        safe_read(g_reset_only + 0x524, &f2, 4);
                        safe_read(g_reset_only + 0x3E8, &hp, 4);
                    }
                    logf("debug: resetx mask %d on %d (chr %p): now hp %d, flags +2A8 %08X, +524 %08X\n", arg, n, (void *)g_reset_only, hp, f1, f2);
                    g_reset_only = 0;
                    g_reset_mask = 31;
                } else if (got >= 1 && !strcmp(cmd, "stun") && eye_pos(eye)) {
                    Target near_by[1];
                    if (find_targets(eye, near_by, 1, 200.0f) == 1) stun_enemy(near_by[0].chr, t);
                } else if (got >= 1 && !strcmp(cmd, "warp")) {
                    int id = last_bonfire_id();
                    g_have_safe = false;
                    logf("debug: warp to bonfire %d (%d enemies dead): %s\n", id, dead_enemies(), bonfire_reload(id) ? "asked for" : "not possible");
                } else if (got >= 1 && !strcmp(cmd, "who") && eye_pos(eye)) {
                    Target near_by[4];
                    int n = find_targets(eye, near_by, 4, 200.0f, true);
                    for (int i = 0; i < n; i++) {
                        uint32_t f1 = 0, f2 = 0;
                        int anim = -1;
                        safe_read(near_by[i].chr + 0x2A8, &f1, 4);
                        safe_read(near_by[i].chr + 0x524, &f2, 4);
                        enemy_attacking(near_by[i].chr, anim);
                        logf("debug: who %d: chr %p hp %d, %.1f m, at (%.1f %.1f %.1f), animation %d, flags +2A8 %08X, +524 %08X\n", i, (void *)near_by[i].chr,
                             near_by[i].hp, near_by[i].dist, near_by[i].p[0], near_by[i].p[1], near_by[i].p[2], anim, f1, f2);
                    }
                } else if (got >= 2 && !strcmp(cmd, "reset")) {
                    g_reset_way = arg;
                    int n = reset_enemies();
                    logf("debug: reset way %d on %d enemies\n", arg, n);
                } else if (got >= 1 && (!strcmp(cmd, "rocket") || !strcmp(cmd, "ball")) && eye_pos(eye)) {
                    Target near_by[1];
                    if (find_targets(eye, near_by, 1, 200.0f) == 1) {
                        const Target &tg = near_by[0];
                        float d[3] = {tg.p[0] - eye[0], tg.p[1] - eye[1], tg.p[2] - eye[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        if (len > 1e-3f) {
                            for (float &x : d) x /= len;
                            if (cmd[0] == 'r') spawn_rocket(t, d);
                            else spawn_ball((got >= 2 ? arg : 100) * 0.01f, t, d);
                            logf("debug: %s at chr %p (hp %d, %.1f m away, %s)\n", cmd, (void *)tg.chr, tg.hp, tg.dist, ray_blocked(eye, tg.p) == 1 ? "hidden" : "in sight");
                        }
                    } else {
                        logf("debug: %s: no enemy\n", cmd);
                    }
                } else if (got >= 1 && !strcmp(cmd, "punchball")) {
                    logf("debug: punchball: %s\n", punch_ball(t) ? "a ball was sent on" : "no ball in reach");
                } else if (got >= 4 && !strcmp(cmd, "boom") && eye_pos(eye)) {
                    Target near_by[1];
                    if (find_targets(eye, near_by, 1, 200.0f) == 1) {
                        const Target &tg = near_by[0];
                        float to[3] = {eye[0] - tg.p[0], 0.0f, eye[2] - tg.p[2]}, len = sqrtf(to[0] * to[0] + to[2] * to[2]);
                        if (len < 1e-3f) { to[0] = 1; len = 1; }
                        float src[3] = {tg.p[0] + to[0] / len * arg * 0.01f, tg.p[1] + arg2 * 0.01f, tg.p[2] + to[2] / len * arg * 0.01f};
                        float dir[3] = {0, arg3 == 1 ? -1.0f : 1.0f, 0};
                        if (arg3 >= 2) {
                            float d[3] = {tg.p[0] - src[0], tg.p[1] - src[1], tg.p[2] - src[2]}, dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                            if (dl > 1e-3f)
                                for (int i = 0; i < 3; i++) dir[i] = d[i] / dl * (arg3 == 3 ? -1.0f : 1.0f);
                        }
                        int row = got >= 5 ? arg4 : BEHAVIOR_EXPLOSION;
                        bool ok = shoot(row, src, dir);
                        report_chr = tg.chr;
                        report_hp = tg.hp;
                        report_at = t + 1.0;
                        logf("debug: boom row %d, %d cm towards the player and %d cm above the aim point of chr %p (hp %d, %.1f m away), sent (%.2f %.2f %.2f): %s\n",
                             row, arg, arg2, (void *)tg.chr, tg.hp, tg.dist, dir[0], dir[1], dir[2], ok ? "accepted" : "REFUSED");
                    }
                }
            }
        }
    }
    if (!g_instant_fire || !g_ctrl) {
        if (g_pierce_ready < 100.0f) g_pierce_ready = 100.0f;   // nothing refills, or ticks, outside first person
        g_pierce_charge = 0;
        g_prev_rmb = false;
        g_twirl_charge = 0;
        g_twirling = false;
        g_core_force = 0;
        g_core_charging = false;
    }
    bool armed = g_instant_fire && g_ctrl && !g_dead;
    {
        bool moving = key_down('W') || key_down('A') || key_down('S') || key_down('D');
        update_bob(dt, g_ctrl && !g_air && !g_sliding && moving, sqrtf(g_vx * g_vx + g_vz * g_vz) / UK_UNIT);
        update_sway(dt);
    }
    if (g_ctrl) watch_enemies(t, dt);
    style_update(dt);
    update_punch(armed, dt);
    update_whiplash(armed, t, dt);
    update_particles(t, dt);
    {
        // the wind of a dash, and of a slide while it lasts
        uintptr_t pos = g_player_pos;
        static float slide_owed = 0;
        if (pos && g_ctrl) {
            const float *feet = (const float *)(pos + OFF_POS_X);
            float middle[3] = {feet[0], feet[1] + 1.0f, feet[2]};
            if (g_fx_dash_pending) {
                float dir[3] = {g_dash_x, 0, g_dash_z}, len = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
                if (len > 0.01f) {
                    dir[0] /= len;
                    dir[2] /= len;
                    fx_wind(middle, dir, 20, 30.0f, 50.0f, 2.0f * UK_UNIT, 2.0f * UK_UNIT);
                }
            }
            {
                static bool was_slamming = false;
                static float slam_owed = 0;
                if (g_slamming) {
                    float right[3], up[3], fwd[3], ahead[3] = {sinf(g_yaw), 0.0f, cosf(g_yaw)};
                    view_axes(right, up, fwd);
                    slam_owed += was_slamming ? 20.0f * dt : 20.0f;
                    int count = (int)slam_owed;
                    slam_owed -= count;
                    const float side[3] = {cosf(g_yaw), 0.0f, -sinf(g_yaw)};
                    if (count > 0) fx_slam_streaks(middle, side, ahead, count);
                } else {
                    slam_owed = 0;
                }
                was_slamming = g_slamming;
            }
            float speed = sqrtf(g_vx * g_vx + g_vz * g_vz);
            if (g_sliding && speed > 1.0f) {
                float dir[3] = {g_vx / speed, 0, g_vz / speed};
                slide_owed += 20.0f * dt;
                int count = (int)slide_owed;
                slide_owed -= count;
                if (count > 0) fx_wind(middle, dir, count, 40.0f, 40.0f, 2.5f * UK_UNIT, 0.5f * UK_UNIT);
            }
        }
        g_fx_dash_pending = false;
    }
    update_pellets(t, dt);
    update_cores(t, dt);
    update_blasts(t);
    update_saws(t, dt);
    update_rockets(t, dt);
    update_balls(t, dt);
    update_viscerals(t);
    {
        float right[3], up[3], fwd[3], eye[3];
        if (g_ctrl && eye_pos(eye)) {
            view_axes(right, up, fwd);
            float far_end[3] = {eye[0] + fwd[0] * 300.0f, eye[1] + fwd[1] * 300.0f, eye[2] + fwd[2] * 300.0f};
            RayHit h;
            g_center_dist = ray_cast(eye, far_end, h) && h.hit ? h.dist : -1.0f;
        }
    }
    update_ricochets(t);
    timed_sounds_update(t);
    bool lmb = armed && key_down(VK_LBUTTON) != 0, rmb = armed && key_down(VK_RBUTTON) != 0;
    // Weapon slots as in ULTRAKILL: 1 is the revolver, 2 the shotgun, 4 the railcannon (3, the nailgun, is
    // not in). A weapon always comes out as its first variation (the revolver as the Marksman, Davi's
    // choice; the one last held is not remembered); its key again, or E, goes on to its next variation
    // and back round. Q goes straight to the Sharpshooter. X swaps the revolver variation in the hand
    // between its standard and its alternate ("Slab") form; that choice is kept for each variation.
    {
        static bool prev1 = false, prev2 = false, prev3 = false, prev4 = false, prev5 = false, prev_e = false, prev_q = false, prev_x = false;
        static const int next_variation[3] = {2, 0, 1};      // Piercer -> Sharpshooter, Marksman -> Piercer, Sharpshooter -> Marksman
        bool k1 = armed && key_down('1'), k2 = armed && key_down('2'), k3 = armed && key_down('3'), k4 = armed && key_down('4'), k5 = armed && key_down('5'), ke = armed && key_down('E'),
             kq = armed && key_down('Q'), kx = armed && key_down('X');
        int weapon = g_weapon, variation = g_variation, shotgun_var = g_shotgun_var, rail_var = g_rail_var, slab_mask = g_slab_mask, saw_var = g_saw_var, rocket_var = g_rocket_var;
        bool next = ke && !prev_e;
        if (k1 && !prev1) {
            next = weapon == WEAPON_REVOLVER;
            if (!next) { weapon = WEAPON_REVOLVER; variation = 1; }
        } else if (k2 && !prev2) {
            next = weapon == WEAPON_SHOTGUN;
            if (!next) { weapon = WEAPON_SHOTGUN; shotgun_var = 0; }
        } else if (k3 && !prev3) {
            next = weapon == WEAPON_SAWLAUNCHER;
            if (!next) { weapon = WEAPON_SAWLAUNCHER; saw_var = SAW_ATTRACTOR; }
        } else if (k4 && !prev4) {
            next = weapon == WEAPON_RAILCANNON;
            if (!next) { weapon = WEAPON_RAILCANNON; rail_var = 1; }      // the Malicious first (Davi's choice)
        } else if (k5 && !prev5) {
            next = weapon == WEAPON_ROCKET;
            if (!next) { weapon = WEAPON_ROCKET; rocket_var = ROCKET_FREEZE; }
        }
        if (next) {
            if (weapon == WEAPON_REVOLVER) variation = next_variation[variation];
            else if (weapon == WEAPON_SHOTGUN) shotgun_var ^= 1;
            else if (weapon == WEAPON_SAWLAUNCHER) saw_var ^= 1;
            else if (weapon == WEAPON_ROCKET) rocket_var ^= 1;
            else rail_var ^= 1;
        }
        // Q: the last variation of the weapon in the hand (the Sharpshooter, the Pump Charge, the Malicious)
        if (kq && !prev_q) {
            if (weapon == WEAPON_REVOLVER) variation = 2;
            else if (weapon == WEAPON_SHOTGUN) shotgun_var = 1;
            else if (weapon == WEAPON_SAWLAUNCHER) saw_var = SAW_OVERHEAT;
            else if (weapon == WEAPON_ROCKET) rocket_var = ROCKET_SRS;
            else rail_var = 1;
        }
        if (kx && !prev_x && weapon == WEAPON_REVOLVER) slab_mask ^= 1 << variation;
        prev1 = k1;
        prev2 = k2;
        prev3 = k3;
        prev4 = k4;
        prev5 = k5;
        prev_e = ke;
        prev_q = kq;
        prev_x = kx;
        if (weapon != g_weapon || variation != g_variation || shotgun_var != g_shotgun_var || rail_var != g_rail_var || slab_mask != g_slab_mask ||
            saw_var != g_saw_var || rocket_var != g_rocket_var) {
            bool choice_changed = slab_mask != g_slab_mask;
            g_weapon = weapon;
            g_saw_var = saw_var;
            g_rocket_var = rocket_var;
            g_variation = variation;
            g_shotgun_var = shotgun_var;
            g_rail_var = rail_var;
            g_slab_mask = slab_mask;
            if (choice_changed) save_settings();
            g_pierce_charge = 0;
            g_twirl_charge = 0;
            g_twirl_angle = 0;
            g_twirling = g_twirl_recovery = false;
            g_core_force = 0;
            g_core_charging = false;
            g_pump_charge = 0;                   // Shotgun.OnEnable
            g_slab_click_at = -1;                // a Click that had not come yet does not come
            g_cyl_angle = g_cyl_target = 0;
            g_cyl_free = false;
            timed_sounds_clear();
            sound_play("weapon_draw", 0.35f, 3.0f);
            // Drawing a weapon starts its animation over, and it is ready when the draw's ReadyGun event
            // comes: for the shotgun that cuts a reload short, as swapping weapons does in ULTRAKILL. Each
            // weapon keeps its own time between shots, so the one drawn does not wait out the last one's.
            if (weapon == WEAPON_ROCKET) {
                // RocketLauncher.OnEnable: what is left of the wait since its last shot, a quarter of a second at the least
                play_revolver("Equip");
                if (g_rocket_cooldown < 0.25f) g_rocket_cooldown = 0.25f;
                g_rocket_drawn_at = t;
                g_rocket_rmb_stale = true;
            } else if (weapon == WEAPON_SAWLAUNCHER) {
                // its Equip clip: CanShoot at 0.30 s, the snap of the saw going in at 0.48 s
                play_revolver("Equip");
                g_saw_ready_at = t + 0.30;
                g_saw_cooldown = 0;
                sound_later(0.48, "saw_snap", 0.2f, 1.0f, CH_FREE);
            } else if (weapon == WEAPON_SHOTGUN) {
                play_revolver("Equip");
                g_shotgun_ready_at = g_cores_ready_at = t + SHOTGUN_EQUIP_READY;
            } else if (weapon == WEAPON_RAILCANNON) {
                // Railcannon.Update waits for nothing but its charge and a press of fire: it can be fired
                // the frame it is drawn. (Until v0.71 there was a 0.2 s wait of our own here.)
                play_revolver("Equip");
                g_rail_ready_at = t;
            } else if (slab_held()) {
                bool slow = t < g_slab_slow_until[variation];
                play_revolver(slow ? "PickUpWithReload" : "PickUp");
                g_next_shot = t + (slow ? 1.15 : g_quick_draw ? 0.0 : 0.33 / 0.85);
                if (slow) g_slab_click_at = t + 0.90;
            } else {
                // ULTRAKILL's revolver is ready at its PickUp clip's ReadyGun event, 0.36 s in. Davi asked for
                // it to fire the moment it is drawn (quick_draw in the ini; 0 gives ULTRAKILL's wait back).
                play_revolver("PickUp");
                g_next_shot = t + (g_quick_draw ? 0.0 : REVOLVER_PICKUP_READY);
            }
            static const char *const names[3] = {"Piercer", "Marksman", "Sharpshooter"};
            const char *held = weapon == WEAPON_ROCKET ? (rocket_var ? "rocket launcher (S.R.S. Cannon)" : "rocket launcher (Freezeframe)")
                               : weapon == WEAPON_SAWLAUNCHER ? (saw_var ? "sawblade launcher (Overheat)" : "sawblade launcher (Attractor)")
                               : weapon == WEAPON_SHOTGUN ? (shotgun_var ? "shotgun (Pump Charge)" : "shotgun (Core Eject)")
                               : weapon == WEAPON_RAILCANNON ? (rail_var ? "railcannon (Malicious)" : "railcannon (Electric)") : names[variation];
            Target all[16];
            float eye[3];
            int n = eye_pos(eye) ? find_targets(eye, all, 16, 500.0f, true) : 0;
            logf("weapon: %s%s; %d other characters active:\n", slab_held() ? "Slab " : "", held, n);
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
        static double next_look = 0, void_since = -1;
        static int last_hp = -1;
        uintptr_t pos = g_player_pos, chr = g_player_chr;
        if (pos && g_ctrl) {
            const float *feet = (const float *)(pos + OFF_POS_X);
            if (!g_air || g_vy >= 0) {
                void_since = -1;
                g_void_below = false;
            } else if (t >= next_look) {
                next_look = t + 0.1;
                float from[3] = {feet[0], feet[1] + 0.5f, feet[2]}, below[3] = {feet[0], feet[1] - VOID_REACH, feet[2]};
                RayHit h;
                if (ray_cast(from, below, h) && !h.hit) {
                    if (void_since < 0) void_since = t;
                } else {
                    void_since = -1;
                }
                g_void_below = void_since >= 0 && t - void_since >= VOID_TIME;
            }
            // the player's health, whenever it changes (to see what takes it)
            int hp = 0;
            uint32_t flags2 = 0;
            if (chr && safe_read(chr + 0x3E8, &hp, 4)) {
                safe_read(chr + 0x524, &flags2, 4);
                if (last_hp >= 0 && hp != last_hp)
                    logf("health: %d -> %d (%s, at (%.1f %.1f %.1f), %.1f m under the last place stood on, flags %08X)\n", last_hp, hp, g_air ? "in the air" : "on the ground",
                         feet[0], feet[1], feet[2], g_have_safe ? g_safe_pos[1] - feet[1] : 0.0f, flags2);
                last_hp = hp;
            }
        }
    }
    {
        // Test aid (C): ULTRAKILL's "no weapon cooldown" cheat, which is WeaponCharges.MaxCharges every
        // frame: the alt fires' charges and the punch stamina are held full. How fast a weapon fires and
        // reloads is not touched. (v0.67 also cut every wait to a tenth of a second.)
        static bool prev_c = false;
        bool kc = armed && key_down('C');
        if (kc && !prev_c) {
            g_no_cooldown = !g_no_cooldown;
            sound_play("pierce_ready", 0.35f, g_no_cooldown ? 1.5f : 0.75f, false, CH_SCREEN);
            logf("test aid: no cooldowns and no damage %s\n", g_no_cooldown ? "ON" : "off");
        }
        prev_c = kc;
        if (g_no_cooldown) {
            g_pierce_ready = 100.0f;
            g_sharp_charge = 300.0f;
            g_coin_charge = 400.0f;
            g_rail_charge = 5.0f;
            g_punch_stamina = 2.0f;
            for (double &u : g_slab_slow_until) u = 0;
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
        // Numpad 6: 6 m along the view and 1.5 m up (over a ledge, or out of the map). Numpad 8: 20 m
        // straight up. Both keep the last place stood on, so what follows is a fall like any other.
        static bool prev6 = false, prev8 = false;
        bool k6 = g_ctrl && key_down(VK_NUMPAD6), k8 = g_ctrl && key_down(VK_NUMPAD8);
        if ((k6 && !prev6) || (k8 && !prev8)) {
            bool along = k6 && !prev6;
            g_teleport_delta[0] = along ? sinf(g_yaw) * 6.0f : 0.0f;
            g_teleport_delta[1] = along ? 1.5f : 20.0f;
            g_teleport_delta[2] = along ? cosf(g_yaw) * 6.0f : 0.0f;
            g_teleport_keep_safe = true;
            g_teleport_pending = true;
            logf("test aid: moved %s\n", along ? "6 m along the view and 1.5 m up" : "20 m straight up");
        }
        prev6 = k6;
        prev8 = k8;
    }
    bool slab = slab_held();
    {
        // WeaponCharges.Charge: the alternate Piercer's shot comes back at half the rate (20 a second),
        // and the alternate Sharpshooter's one shot, which costs all 300, at 35 a second against 15.
        float pierce_rate = PIERCE_RECHARGE_RATE * ((g_slab_mask & 1) ? 0.5f : 1.0f), sharp_rate = (g_slab_mask & 4) ? 35.0f : SHARP_REFILL;
        if (g_pierce_ready < 100.0f) g_pierce_ready = fminf(100.0f, g_pierce_ready + pierce_rate * dt);
        if (g_sharp_charge < 300.0f) g_sharp_charge = fminf(300.0f, g_sharp_charge + sharp_rate * dt);
    }
    // the railcannon charges whatever is in the hand: 0 to 5 at a quarter a second, full from 4, with its chime
    if (g_rail_charge < 5.0f) {
        float c = g_rail_charge + 0.25f * dt;
        if (c >= 4.0f) {
            c = 5.0f;
            g_rail_full_at = t;
            if (armed) sound_play("rail_charged", 0.35f, 1.0f, false, CH_FREE);
        }
        g_rail_charge = c;
    }
    g_zoom = move_towards(g_zoom, g_weapon == WEAPON_RAILCANNON && rmb ? 0.5f : 1.0f, dt * 300.0f / 105.0f);
    // the alternate revolver's hammer comes back, if the gun is still in the hand
    if (g_slab_click_at > 0 && t >= g_slab_click_at) {
        g_slab_click_at = -1;
        if (slab) {
            g_cyl_target += 90.0f;
            g_slab_slow_until[g_variation] = 0;
            // (the 'RevolverClick' prefab plays its clip at twice its speed. Until v0.81 it was played at its own: an octave
            // down and twice as long, a slow ratchet after every Slab shot, which Davi took for a crossbow)
            sound_play("slab_click", 0.5f, 2.0f, false, CH_FREE);
        }
    }
    bool alt_fired = false;
    update_saw_launcher(t, dt, lmb, rmb);
    update_rocket_launcher(t, dt, lmb, rmb);
    if (g_weapon == WEAPON_SAWLAUNCHER || g_weapon == WEAPON_ROCKET) {
        g_pierce_charge = 0;
    } else if (g_weapon == WEAPON_RAILCANNON) {
        g_pierce_charge = 0;
        // (a press of fire that came up to 0.15 s before the railcannon was in the hand counts, while the
        // button is still down: fire and the weapon's key pressed together must not depend on which the
        // game happened to see first; ours)
        static double pressed_at = -100.0;
        bool fresh = lmb && !g_prev_lmb;
        if (fresh) pressed_at = t;
        bool early = lmb && t - pressed_at < 0.15 && pressed_at < g_rail_ready_at && g_rail_ready_at - pressed_at < 0.15;
        if ((fresh || early) && g_rail_charge >= 5.0f && t >= g_rail_ready_at) {
            pressed_at = -100.0;
            fire_railcannon(t);
        }
    } else if (g_weapon == WEAPON_SHOTGUN && g_shotgun_var == 1) {
        g_pierce_charge = 0;
        // Shotgun.Update: fire comes first; the alt fire pumps whenever the gun is ready, so holding it pumps again and again
        if (lmb && t >= g_shotgun_ready_at) fire_shotgun(t);
        else if (rmb && t >= g_shotgun_ready_at) pump_shotgun(t);
    } else if (g_weapon == WEAPON_SHOTGUN) {
        g_pierce_charge = 0;
        // Shotgun.Update: the alt fire winds the core up while held and throws it when let go
        bool winding = rmb && t - g_last_core > 0.5 && t >= g_cores_ready_at;
        if (winding && !(g_core_charging && lmb && !g_prev_lmb)) {
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
        // Revolver.Update: the alternate Sharpshooter winds up at 750 a second against 75, and its shot
        // needs, and spends, all 300 of the charge
        float rate = slab ? 750.0f : TWIRL_RATE, cost = slab ? 300.0f : 100.0f;
        bool let_go = (g_prev_rmb && !rmb) || lmb;
        if (let_go && g_twirl_charge >= 25.0f && t >= g_next_shot) {
            int bounces = (int)(g_twirl_charge / 25.0f);
            if (bounces > 3) bounces = 3;
            g_sharp_charge = g_sharp_charge - cost < 0 ? 0 : g_sharp_charge - cost;
            g_twirl_charge = 0;
            if (g_twirling) g_twirl_recovery = true;
            g_twirling = false;
            alt_fired = true;
            InterlockedIncrement(&g_sharp_shots);
            sound_play(slab ? "shot_slab" : "shot_sharpshooter", 0.55f, frand(0.9f, 1.1f), false, CH_GUN);
            sound_play("twirl_shot", 0.75f, 1.0f);
            note_attack(HIT_REVOLVER);
            play_revolver("Shoot");
            g_shot_turns = g_shot_turns + 1;
            g_last_pierce_time = t;
            g_next_shot = t + (slab ? SLAB_READY : FIRE_INTERVAL);
            if (slab) slab_fired(t);
            g_sharp_behavior = slab ? BEHAVIOR_SLAB_SHARP : BEHAVIOR_SHARP;
            if (Coin *c = coin_on_line()) {
                shot_beam(c->p, BEAM_SHARP);
                hit_coin(*c, t);
                c->charged = true;
                c->carry = slab ? BEHAVIOR_SLAB_SHARP : 0;
            } else if (shoot_flying(BEAM_SHARP, false, t)) {
            } else if (Core *k = core_on_line()) {
                shot_beam(k->p, BEAM_SHARP);
                explode(k->p, EXPLODE_SUPER);
                k->alive = false;
            } else if (MagnetObj *mg = magnet_on_line()) {
                shot_beam(g_magnet_hit, BEAM_SHARP);
                break_magnet(*mg, t, "shot");
            } else {
                float right[3], up[3], fwd[3], eye[3];
                if (eye_pos(eye)) {
                    view_axes(right, up, fwd);
                    sharp_leg(eye, fwd, bounces, true);
                }
            }
        } else if (rmb && g_sharp_charge >= cost) {
            g_twirling = true;
            g_twirl_recovery = false;
            g_twirl_charge = g_twirl_charge + rate * dt > 100.0f ? 100.0f : g_twirl_charge + rate * dt;
        } else {
            if (g_twirling) g_twirl_recovery = true;
            g_twirling = false;
            g_twirl_charge = g_twirl_charge - rate * dt < 0 ? 0 : g_twirl_charge - rate * dt;
        }
    } else if (rmb && g_pierce_ready >= 100.0f && (!slab || t >= g_next_shot) && !(lmb && g_pierce_charge >= 100.0f)) {
        g_pierce_charge = g_pierce_charge + PIERCE_CHARGE_RATE * dt > 100.0f ? 100.0f : g_pierce_charge + PIERCE_CHARGE_RATE * dt;
    } else if (g_pierce_charge >= 100.0f && (g_prev_rmb || lmb)) {
        // the charged shot: it stops at a coin, which passes it on, and sets off a core in the air
        bool fired = true;
        if (Coin *c = coin_on_line()) {
            note_attack(HIT_REVOLVER);
            shot_beam(c->p, BEAM_SUPER);
            hit_coin(*c, t);
            c->charged = true;
            c->carry = slab ? BEHAVIOR_SLAB_CHARGED : 0;
        } else if (shoot_flying(BEAM_SUPER, false, t)) {
        } else if (Core *k = core_on_line()) {
            shot_beam(k->p, BEAM_SUPER);
            explode(k->p, EXPLODE_SUPER);
            k->alive = false;
        } else if (MagnetObj *mg = magnet_on_line()) {
            shot_beam(g_magnet_hit, BEAM_SUPER);
            break_magnet(*mg, t, "shot");
        } else if (shoot(slab ? BEHAVIOR_SLAB_CHARGED : BEHAVIOR_PIERCER)) {
            shot_beam(nullptr, BEAM_SUPER);
            InterlockedIncrement(&g_pierce_shots);
            note_attack(HIT_REVOLVER);
        } else {
            fired = false;
        }
        if (fired) {
            sound_play(slab ? "shot_super_alt" : "shot_super", 0.5f, 1.0f);
            play_revolver("Shoot");
            g_shot_turns = g_shot_turns + 1;
            g_last_pierce_time = t;
            g_pierce_ready = 0;
            g_next_shot = t + (slab ? SLAB_READY : FIRE_INTERVAL);
            if (slab) slab_fired(t);
        }
        g_pierce_charge = 0;
    } else {
        g_pierce_charge = 0;   // released early: the charge is lost
    }
    g_prev_rmb = rmb;
    g_prev_lmb = lmb;
    {
        if (g_shot_turns > 0) {
            if (!slab) g_cyl_target += 120.0f * g_shot_turns;      // g_cyl_target += 120.0f per standard shot
            g_shot_turns = 0;
        }
        float step = slab ? 90.0f : 120.0f, speed = slab ? 480.0f : 240.0f;
        float spin = g_weapon == WEAPON_REVOLVER && !slab && g_variation == 0 ? g_pierce_charge * 10.0f : 0.0f;
        if (spin > speed) {
            g_cyl_angle += spin * dt;
            g_cyl_free = true;
        } else {
            if (g_cyl_free) {
                g_cyl_free = false;
                g_cyl_target = roundf(g_cyl_angle / step) * step;
            }
            g_cyl_angle = move_towards(g_cyl_angle, g_cyl_target, speed * dt);
        }
    }
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
        if (refilling && !refill_voice) refill_voice = sound_play("pierce_recharging", 0.25f, slab ? 0.5f : 1.0f, true, CH_SCREEN);   // the alternate one's ticks at half pitch
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
        // the full railcannon hums while it is in the hand
        static int rail_voice = 0;
        bool humming = armed && g_weapon == WEAPON_RAILCANNON && g_rail_charge >= 5.0f;
        if (humming && !rail_voice) rail_voice = sound_play("rail_hum", 0.35f, 1.0f, true);
        if (!humming && rail_voice) {
            sound_stop(rail_voice);
            rail_voice = 0;
        }
        // Shotgun.UpdateMeter: pumped three times, the shotgun beeps five times a second
        if (armed && g_weapon == WEAPON_SHOTGUN && g_shotgun_var == 1 && g_pump_charge >= 3 && t - g_pump_beep_at >= 0.2) {
            g_pump_beep_at = t;
            sound_play("pump_warning", 0.5f, 1.0f, false, CH_SCREEN);
        }
    }

    if (g_weapon == WEAPON_REVOLVER && !alt_fired && lmb && g_pierce_charge <= 0 && !g_twirling && t >= g_next_shot) {
        g_next_shot = t + (slab ? SLAB_READY : FIRE_INTERVAL);
        const char *shot_sound = slab ? "shot_slab" : g_variation == 1 ? "shot_marksman" : g_variation == 2 ? "shot_sharpshooter" : "shot_piercer";
        int beam = slab ? BEAM_SLAB : BEAM_REVOLVER;
        bool fired = true;
        if (Coin *c = coin_on_line()) {
            sound_play(shot_sound, 0.55f, frand(0.9f, 1.1f), false, CH_GUN);
            play_revolver_shot();
            shot_beam(c->p, beam);
            hit_coin(*c, t);                     // the shot stops at the coin
            c->charged = false;
            c->carry = slab ? BEHAVIOR_SLAB : 0; // (the Slab's beam goes through things, so the coin sends that beam on: see reflect_coin)
            g_last_shot_time = t;
            if (slab && c->hit_times > 1) {
                // RevolverBeam: the alternate revolver's shot on a coin in its split window reloads the
                // gun at once (Revolver.InstaClick plays ShootTwirl, ready after 0.47 s)
                play_revolver("ShootTwirl");
                g_next_shot = t + 0.47;
                g_slab_slow_until[g_variation] = 0;
                g_slab_click_at = -1;
                sound_later(0.12, "slab_click", 0.5f, 2.0f, CH_FREE);
                fired = false;                   // (nothing left for slab_fired to arm)
                logf("slab: split-window coin shot, instant reload\n");
            }
        } else if (shoot_flying(beam, false, t)) {
            sound_play(shot_sound, 0.55f, frand(0.9f, 1.1f), false, CH_GUN);
            play_revolver_shot();
            g_last_shot_time = t;
        } else if (Core *k = core_on_line()) {
            // a core shot in the air goes off as the larger explosion
            sound_play(shot_sound, 0.55f, frand(0.9f, 1.1f), false, CH_GUN);
            play_revolver_shot();
            shot_beam(k->p, beam);
            explode(k->p, EXPLODE_SUPER);
            k->alive = false;
            g_last_shot_time = t;
        } else if (MagnetObj *mg = magnet_on_line()) {
            // the Attractor's harpoon breaks under a shot, and lets its saws go
            sound_play(shot_sound, 0.55f, frand(0.9f, 1.1f), false, CH_GUN);
            play_revolver_shot();
            shot_beam(g_magnet_hit, beam);
            break_magnet(*mg, t, "shot");
            g_last_shot_time = t;
        } else if (shoot(slab ? BEHAVIOR_SLAB : BEHAVIOR_REVOLVER)) {
            sound_play(shot_sound, 0.55f, frand(0.9f, 1.1f), false, CH_GUN);
            note_attack(HIT_REVOLVER);
            play_revolver_shot();
            revolver_beam_fx(beam);
            InterlockedIncrement(&g_instant_shots);
            g_last_shot_time = t;
        } else {
            fired = false;
        }
        if (slab && fired) slab_fired(t);
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

// Dark Souls' title screen dressed as ULTRAKILL's main menu (title_menu in the ini; hud.cpp's build_title).
// It is tried whenever there is no player in the world; the renderer looks at the picture and does nothing
// unless it is the title screen.
static volatile LONG g_title_frames = 0;
static void draw_title(IDXGISwapChain *sc) {
    if (g_hud_status == 2) return;
    uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr), man = 0, player = 0;
    if (!safe_read(exe + 0x1C77E50, &man, 8)) return;
    if (man && (!safe_read(man + 0x68, &player, 8) || player)) return;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11Texture2D *back = nullptr;
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
            bool ok = hud_init(dev, pack);
            g_hud_status = ok ? 1 : 2;
            if (ok) {
                static HudState st_store;
                HudState &st = *new (&st_store) HudState();
                st.title = true;
                hud_title_column(g_title_column);
                st.time = now_s();
                st.show_viewmodel = false;
                snprintf(st.title_note, sizeof(st.title_note), "ULTRASOULS %s READY", MOD_VERSION);
                hud_draw(dev, ctx, back, st);
                InterlockedIncrement(&g_title_frames);
                // the menu as it was read off the picture goes to the log whenever it changes (tools/game.py
                // goes by this line to know that the chosen button is CONTINUE before it clicks)
                static char shown[96] = "?";
                const char *menu = hud_title_state();
                if (strcmp(shown, menu)) {
                    snprintf(shown, sizeof(shown), "%s", menu);
                    logf("title menu: %s\n", menu[0] ? menu : "-");
                }
            }
        }
    }
    if (back) back->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
}

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
                // (kept off the stack: with the saws' trails it is about 140 KB; made afresh every frame)
                static HudState st_store;
                HudState &st = *new (&st_store) HudState();
                st.health = g_hud_health;
                if (g_dead) {
                    st.death_time = (float)(now_s() - g_death.t0);
                    st.death_prompt = g_death.fast;      // (a death left to the game has no key to press)
                }
                st.stamina = g_boost;
                st.pierce_charge = g_pierce_charge;
                st.pierce_ready = g_pierce_ready;
                st.time = now_s();
                st.last_shot = g_last_shot_time;
                st.last_pierce = g_last_pierce_time;
                st.show_viewmodel = g_viewmodel;
                st.variation = g_variation;
                st.weapon = g_weapon;
                st.alt = slab_held();
                st.weapon_var = g_weapon == WEAPON_SHOTGUN ? (int)g_shotgun_var : g_weapon == WEAPON_RAILCANNON ? (int)g_rail_var
                                : g_weapon == WEAPON_SAWLAUNCHER ? (int)g_saw_var : g_weapon == WEAPON_ROCKET ? (int)g_rocket_var : 0;
                if (g_weapon == WEAPON_ROCKET) {
                    // RocketLauncher.Update: the clock shows the freeze time left, or the cannonball coming back, or
                    // the cannon winding up (which turns it from the variation's colour to red); a cannonball
                    // that is not back yet leaves it at half strength
                    static const float blue[3] = {0.251f, 0.906f, 1.0f}, green[3] = {0.267f, 1.0f, 0.271f};
                    float charge = g_cb_charge, ready = g_cb_ready;
                    bool srs = g_rocket_var == ROCKET_SRS;
                    st.clock_fill = !srs ? g_freeze_time / 5.0f : charge > 0 ? charge : ready;
                    st.clock_alpha = srs && charge <= 0 && ready < 1.0f ? 0.5f : 1.0f;
                    st.srs_charge = srs ? charge : 0.0f;
                    for (int i = 0; i < 3; i++) st.clock_rgb[i] = srs ? green[i] + ((i == 0 ? 1.0f : 0.0f) - green[i]) * charge : blue[i];
                }
                // (v0.75 wrote the launcher's saws, magnets and heat under the weapon's picture; from v0.76 they are
                // on the gun's own display, as in ULTRAKILL, unless the model pack is an older one without it)
                st.weapon_note[0] = 0;
                static int have_saw_display = -1;
                if (have_saw_display < 0) have_saw_display = hud_has_model("fx_saw") ? 1 : 0;
                if (g_weapon == WEAPON_SAWLAUNCHER && have_saw_display == 0) {
                    if (g_saw_var == SAW_ATTRACTOR) snprintf(st.weapon_note, sizeof(st.weapon_note), "SAWS %d  MAGNETS %d", (int)lroundf(g_saw_ammo), (int)g_magnet_charge);
                    else snprintf(st.weapon_note, sizeof(st.weapon_note), "HEAT %d%%  %s", (int)lroundf(g_saw_heat * 100.0f), g_saw_sinks >= 1.0f ? "SINK IN" : "SINK OUT");
                }
                if (g_weapon == WEAPON_SAWLAUNCHER) {
                    st.clip_muddle = g_saw_muddle;
                    st.blade_spin = g_saw_blade * 3.14159265f / 180.0f;
                    st.blade_heat = g_saw_var == SAW_OVERHEAT ? g_saw_heat : 0.0f;
                    st.saw_count = (int)lroundf(g_saw_ammo);
                    st.magnet_charge = g_magnet_charge;
                    st.heat_sink = g_saw_sinks;
                }
                st.rail_charge = g_rail_charge;
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
                if (g_weapon == WEAPON_SHOTGUN && g_shotgun_var == 1) {
                    // The Pump Charge's meter: a third per pump, going from the variation's green to red
                    // over two pumps; at three it is full and blinks red and black with the warning beep.
                    int pumps = g_pump_charge;
                    // (The slider's fill is 10 units wider than its share of the 50-unit track, on a display 60
                    // wide: with no pumps a sixth of the display is lit, the first of its lights. Until v0.74
                    // the display was dark until the first pump.)
                    st.core_meter = pumps >= 3 ? 1.0f : (10.0f + pumps / 3.0f * 50.0f) / 60.0f;
                    st.meter_rgb_set = true;
                    if (pumps >= 3) {
                        st.meter_rgb[0] = now_s() - g_pump_beep_at < 0.1 ? 1.0f : 0.0f;
                        st.meter_rgb[1] = st.meter_rgb[2] = 0.0f;
                    } else {
                        float k = pumps / 2.0f;
                        st.meter_rgb[0] = 0.2667f + (1.0f - 0.2667f) * k;
                        st.meter_rgb[1] = 1.0f + (0.25f - 1.0f) * k;
                        st.meter_rgb[2] = 0.2706f + (0.25f - 0.2706f) * k;
                    }
                }
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
                st.revolver_clip_speed = g_rev_clip_speed;
                st.arm2_clip_speed = g_arm2_clip_speed;
                st.cylinder = g_cyl_angle * 3.14159265f / 180.0f;
                st.rail_meter = true;
                st.rail_flash = (float)fmax(0.0, 1.0 - (now_s() - g_rail_full_at));
                memcpy(st.arm_push, g_arm_push, sizeof(st.arm_push));
                st.arm_roll = g_arm_roll;
                st.red_tint = g_red_tint;
                if (g_visceral_gore && st.screen_blood_count + 2 <= HudState::MAX_SCREEN_BLOOD) {
                    // Over the hand, where it goes into the enemy: two of the screen's own blood splashes, small, the
                    // second beating. (They are drawn over the arm; the effects out in the world are drawn under it.)
                    float beat = 0.5f + 0.5f * sinf((float)now_s() * 23.0f);
                    st.screen_blood[st.screen_blood_count++] = {-12.0f, 34.0f, 1, 0.62f, 0.50f};
                    st.screen_blood[st.screen_blood_count++] = {-4.0f, 44.0f, 3, 0.45f + 0.15f * beat, 0.30f + 0.05f * beat};
                }
                st.arm_clip = g_arm_clip;
                st.arm_clip_start = g_arm_clip_start;
                st.arm2_clip = g_arm2_clip;
                st.arm2_clip_start = g_arm2_clip_start;
                st.screen_blood_count = 0;
                for (const ScreenBlood &b : g_screen_blood) {
                    float alpha = 0.4902f - (float)(now_s() - b.born);
                    if (b.born <= 0 || alpha <= 0 || st.screen_blood_count >= HudState::MAX_SCREEN_BLOOD) continue;
                    st.screen_blood[st.screen_blood_count++] = {b.x, b.y, b.sprite, alpha};
                }
                st.whip_clip = g_whip_clip;
                st.whip_clip_start = g_whip_clip_start;
                st.whip_hold = g_whip_hold;
                st.whip_out = g_whip_state != WHIP_READY || g_whip_returning;
                {
                    double left = g_flash_until - now_s();
                    st.flash = left > 0 ? (float)(left / 0.1) : 0.0f;
                }
                D3D11_TEXTURE2D_DESC bd;
                back->GetDesc(&bd);
                float right[3], up[3], fwd[3], eye[3];
                if (eye_pos(eye) && bd.Height) {
                    view_axes(right, up, fwd);
                    float tan_y = tanf(BASE_FOV * g_fov_scale * g_zoom * 0.5f), tan_x = tan_y * (float)bd.Width / (float)bd.Height;
                    g_aspect = (float)bd.Width / (float)bd.Height;
                    // a strip between two points of the world (hud_strip; the camera it needs is set just below, before any is drawn)
                    auto seg = [&](const float *pa, const float *pb, float width, const float *c0, const float *c1, float alpha, const char *sprite_name,
                                   float u0, float u1, float wmax) {
                        hud_strip(st, pa, pb, width, c0, c1, alpha, sprite_name, u0, u1, wmax);
                    };
                    // a line of one colour with a soft glow round it (the older tracers)
                    auto line = [&](const float *pa, const float *pb, float width, float r, float g, float b, float alpha) {
                        int before = st.tracer_count;
                        const float c[3] = {r, g, b};
                        seg(pa, pb, width, c, c, alpha, nullptr, 0, 0, 0.02f);
                        if (st.tracer_count > before) {
                            st.tracers[before].plain = false;
                            st.tracers[before].grad = false;
                        }
                    };
                    // the same without the wide soft glow round it, for trails and streaks
                    auto thin_line = [&](const float *pa, const float *pb, float width, float r, float g, float b, float alpha) {
                        int before = st.tracer_count;
                        line(pa, pb, width, r, g, b, alpha);
                        if (st.tracer_count > before) st.tracers[before].plain = true;
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
                        o.z = vz;
                    };
                    // the camera, for the effect meshes
                    st.cam_valid = true;
                    for (int i = 0; i < 3; i++) {
                        st.cam_eye[i] = eye[i];
                        st.cam_right[i] = right[i];
                        st.cam_up[i] = up[i];
                        st.cam_fwd[i] = fwd[i];
                    }
                    st.cam_tan_x = tan_x;
                    st.cam_tan_y = tan_y;
                    static int have_fx = -1;                     // whether the model pack has ULTRAKILL's effect meshes
                    if (have_fx < 0) have_fx = hud_has_model("fx_sphere") && hud_has_model("fx_shock") && hud_has_model("fx_coin") ? 1 : 0;
                    auto mesh = [&](const char *model, const float *p, float radius, float flip, float u, float v, float alpha, const float *limits = nullptr,
                                    int limit_count = 0) {
                        if (st.world_mesh_count >= HudState::MAX_WORLD_MESHES || alpha <= 0) return;
                        HudState::WorldMesh &w = st.world_meshes[st.world_mesh_count++];
                        w.limits = limits;
                        w.limit_count = limit_count;
                        w.model = model;
                        memcpy(w.p, p, sizeof(w.p));
                        w.radius = radius;
                        w.flip = flip;
                        w.uv[0] = u;
                        w.uv[1] = v;
                        w.rgba[0] = w.rgba[1] = w.rgba[2] = 1.0f;
                        w.rgba[3] = alpha > 1.0f ? 1.0f : alpha;
                    };
                    // a picture in the world facing the eye: `size` metres across
                    auto sprite = [&](const char *name, const float *p, float size, float rot, float r, float g, float b, float alpha) {
                        if (st.fx_sprite_count >= HudState::MAX_FX_SPRITES || alpha <= 0) return;
                        float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
                        float vx = d[0] * right[0] + d[1] * right[1] + d[2] * right[2], vy = d[0] * up[0] + d[1] * up[1] + d[2] * up[2],
                              vz = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
                        if (vz < 0.15f) return;
                        HudState::FxSprite &o = st.fx_sprites[st.fx_sprite_count++];
                        o.sprite = name;
                        o.x = vx / (vz * tan_x);
                        o.y = vy / (vz * tan_y);
                        o.size = size / (vz * tan_y * 2.0f);
                        o.rot = rot;
                        o.r = r; o.g = g; o.b = b;
                        o.a = alpha > 1.0f ? 1.0f : alpha;
                        o.z = vz;
                    };
                    for (int ti = 0; ti < MAX_TRACERS; ti++) {
                        const Tracer &tr = g_tracers[ti];
                        float age = (float)(st.time - tr.born);
                        if (tr.life <= 0 || age < 0 || age > tr.life) continue;
                        float left = 1.0f - age / tr.life, width = tr.shrink ? tr.width * left : tr.width, alpha = tr.shrink ? 1.0f : left;
                        if (tr.kind != BEAM_NONE) {
                            hud_beam(st, tr.a, tr.b, tr.kind, width, UK_UNIT, tr.seen, ti);
                            continue;
                        }
                        // each run of eighths that was in sight when the tracer was made
                        for (int k = 0; k < 8; k++) {
                            if (!(tr.seen & (1 << k))) continue;
                            int last = k;
                            while (last + 1 < 8 && (tr.seen & (1 << (last + 1)))) last++;
                            float f0 = k / 8.0f, f1 = (last + 1) / 8.0f, pa[3], pb[3];
                            for (int i = 0; i < 3; i++) {
                                pa[i] = tr.a[i] + (tr.b[i] - tr.a[i]) * f0;
                                pb[i] = tr.a[i] + (tr.b[i] - tr.a[i]) * f1;
                            }
                            line(pa, pb, width, tr.r, tr.g, tr.bl, alpha);
                            k = last;
                        }
                    }
                    // The whiplash's cable (HookArm's LineRenderer): 0.1 u wide, nine points from the hand to
                    // the hook; for a moment after the throw the points between swing up and down off the line
                    // (3 u at the first, less at each one after, dying away at 6.5 a second). Its material is a
                    // dark grey speckle, drawn here as plain dark grey. The hook itself is a short wider
                    // piece at the end, standing in for its model.
                    if (g_whip_state != WHIP_READY || g_whip_returning) {
                        float hx = -0.45f, hy = -0.55f, hand[3], prev[3], pt[3];
                        hud_whip_hand(&hx, &hy);
                        for (int i = 0; i < 3; i++) hand[i] = eye[i] + (right[i] * hx * tan_x + up[i] * hy * tan_y + fwd[i]) * 0.6f;
                        const float wire[3] = {0.16f, 0.16f, 0.155f}, claw[3] = {0.30f, 0.33f, 0.30f};
                        memcpy(prev, hand, sizeof(prev));
                        for (int k = 1; k <= 8; k++) {
                            float wobble = k < 8 ? 1.0f / k * (k % 2 == 0 ? -3.0f : 3.0f) * g_whip_warp * UK_UNIT : 0.0f;
                            for (int i = 0; i < 3; i++)
                                pt[i] = k < 8 ? hand[i] + (g_whip_hook[i] - hand[i]) * (k / 9.0f) + up[i] * wobble : g_whip_hook[i];
                            seg(prev, pt, 0.1f * UK_UNIT, wire, wire, 1.0f, nullptr, 0, 0, 0.01f);
                            memcpy(prev, pt, sizeof(prev));
                        }
                        float d[3] = {g_whip_hook[0] - hand[0], g_whip_hook[1] - hand[1], g_whip_hook[2] - hand[2]},
                              len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        if (len > 0.6f) {
                            float tail[3] = {g_whip_hook[0] - d[0] / len * 0.3f, g_whip_hook[1] - d[1] / len * 0.3f, g_whip_hook[2] - d[2] / len * 0.3f};
                            seg(tail, g_whip_hook, 0.3f * UK_UNIT, claw, claw, 1.0f, nullptr, 0, 0, 0.03f);
                        }
                    }
                    // A model in the world with its z along `dir` (or against it) and its y as near to straight up
                    // as that leaves it: Unity's LookAt. One unit of the model is one of ULTRAKILL's.
                    auto aimed = [&](const char *model, const float *p, const float *dir, bool backwards, float spin) {
                        if (st.world_mesh_count >= HudState::MAX_WORLD_MESHES) return;
                        float z[3] = {dir[0], dir[1], dir[2]}, len = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
                        if (len < 1e-4f) { z[0] = fwd[0]; z[1] = fwd[1]; z[2] = fwd[2]; len = 1.0f; }
                        for (float &x : z) x = x / len * (backwards ? -1.0f : 1.0f);
                        float y[3] = {-z[0] * z[1], 1.0f - z[1] * z[1], -z[2] * z[1]}, yl = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
                        if (yl < 1e-3f) {                              // straight up or down: the eye's own "up" stands in
                            float d = up[0] * z[0] + up[1] * z[1] + up[2] * z[2];
                            for (int i = 0; i < 3; i++) y[i] = up[i] - z[i] * d;
                            yl = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
                            if (yl < 1e-3f) return;
                        }
                        for (float &x : y) x /= yl;
                        HudState::WorldMesh &w = st.world_meshes[st.world_mesh_count++];
                        w.model = model;
                        memcpy(w.p, p, sizeof(w.p));
                        w.radius = UK_UNIT;
                        w.flip = 0;
                        w.uv[0] = w.uv[1] = 0;
                        w.rgba[0] = w.rgba[1] = w.rgba[2] = w.rgba[3] = 1.0f;
                        w.limits = nullptr;
                        w.limit_count = 0;
                        w.oriented = true;
                        w.ax[0] = y[1] * z[2] - y[2] * z[1];
                        w.ax[1] = y[2] * z[0] - y[0] * z[2];
                        w.ax[2] = y[0] * z[1] - y[1] * z[0];
                        memcpy(w.ay, y, sizeof(y));
                        memcpy(w.az, z, sizeof(z));
                        w.spin = spin;
                        w.cutout = true;
                    };
                    // A trail as Unity's TrailRenderer leaves it behind something: from `head` back through the
                    // points it has been at (newest first, with their times), `life` seconds long, `width` wide
                    // at the head and nothing at the tail, from `rgb` at `alpha` to white at none.
                    auto trail = [&](const float *head, const float (*pts)[3], const double *when, int count, float life, float width, const float *rgb, float alpha) {
                        // The trails' material is 'Additive' (the legacy particle shader, tint 0.25): what is
                        // drawn is added to the picture, at twice the tint times the trail's colour, so half
                        // of it. A faint streak of light, then, and not a strip of paint, which is how
                        // v0.76 laid it on.
                        float fa = 1.0f, pa[3] = {head[0], head[1], head[2]}, ca[3] = {rgb[0] * 0.5f, rgb[1] * 0.5f, rgb[2] * 0.5f};
                        hud_trail_begin(st, true);
                        hud_trail_point(st, pa, width, ca, alpha);
                        for (int k = 0; k < count && fa > 0; k++) {
                            float fb = 1.0f - (float)(st.time - when[k]) / life, pb[3] = {pts[k][0], pts[k][1], pts[k][2]}, cb[3];
                            if (fb > fa) fb = fa;
                            if (fb < 0) {                              // the trail ends inside this piece
                                float cut = fa / (fa - fb);
                                for (int i = 0; i < 3; i++) pb[i] = pa[i] + (pb[i] - pa[i]) * cut;
                                fb = 0;
                            }
                            for (int i = 0; i < 3; i++) cb[i] = (rgb[i] + (1.0f - rgb[i]) * (1.0f - fb)) * 0.5f;
                            hud_trail_point(st, pb, width * fb, cb, alpha * fb);
                            memcpy(pa, pb, sizeof(pa));
                            fa = fb;
                        }
                    };
                    // Sawblades, as their prefabs have them: a hub with the flat picture of the teeth on it (4 u
                    // across for the Attractor's, 3 u and grey for the Overheat's, 6 u and orange for the heated
                    // one), the teeth turning at 1440 degrees a second. Nail.Update keeps the saw's back to the
                    // way it flies ("LookAt(position - velocity)"), so the disc lies flat along its path, like a
                    // thrown plate, and banks as the path climbs or dives. (Until v0.76 it was a picture that
                    // always faced the eye: a wheel rolling away, in Davi's words.) Behind it its trail: half a
                    // second long, 2 u wide at the saw (3 u for the heated one) down to nothing, cyan, grey
                    // or orange from alpha 0.49.
                    static int have_saw = -1;
                    if (have_saw < 0) have_saw = hud_has_model("fx_saw") && hud_has_model("fx_saw_overheat") && hud_has_model("fx_saw_heated") ? 1 : 0;
                    for (const Saw &sw : g_saws) {
                        if (!sw.alive) continue;
                        static const float sizes[3] = {4.0f, 3.0f, 6.0f}, widths[3] = {2.0f, 2.0f, 3.0f};
                        static const float trail_rgb[3][3] = {{0.0f, 0.876f, 1.0f}, {0.5f, 0.5f, 0.5f}, {1.0f, 0.592f, 0.0f}};
                        static const float tint[3][3] = {{1, 1, 1}, {0.5f, 0.5f, 0.5f}, {1.0f, 0.6f, 0.0f}};
                        static const char *const models[3] = {"fx_saw", "fx_saw_overheat", "fx_saw_heated"};
                        // (drawn as far past its last physics step as the clock is)
                        float at[3];
                        for (int i = 0; i < 3; i++) at[i] = sw.p[i] + (sw.stopped ? 0.0f : sw.v[i] * g_saw_owed);
                        trail(at, sw.trail, sw.trail_t, sw.trail_n, 0.5f, widths[sw.kind] * UK_UNIT, trail_rgb[sw.kind], 0.49f);
                        float dx = at[0] - eye[0], dy = at[1] - eye[1], dz = at[2] - eye[2];
                        if (dx * dx + dy * dy + dz * dz < 1.0f) continue;        // (not in the player's face as it leaves)
                        float turn = (float)fmod((st.time - sw.born) * 1440.0, 360.0);
                        if (have_saw == 1) aimed(models[sw.kind], at, sw.stopped ? sw.kept_v : sw.v, true, turn * 3.14159265f / 180.0f + 0.001f);
                        else sprite(sw.kind == SAW_HEATED ? "sawblade 2" : "sawblade", at, sizes[sw.kind] * UK_UNIT, turn, tint[sw.kind][0], tint[sw.kind][1], tint[sw.kind][2], 1.0f);
                    }
                    // the bursts of light where coins send beams on and where those beams land
                    for (const Flash &f : g_flashes) {
                        float age = (float)(st.time - f.born);
                        if (f.life <= 0 || age < 0 || age >= f.life) continue;
                        float k = age / f.life;
                        ball(f.p, f.size * (0.6f + 0.8f * k), 0, f.rgb[0] * 0.5f + 0.5f, f.rgb[1] * 0.5f + 0.5f, f.rgb[2] * 0.5f + 0.5f, 1.0f - k);
                    }
                    // Magnets: the harpoon's own mesh, pointing the way it flew, with its trail (a second
                    // long, 0.5 u wide, cyan to white). Its TimeBomb's light flashes where the harpoon's tail
                    // is each time it beeps: 6.4 u across, half as much again for a full load, shrinking away
                    // at five times its size a second; green with nothing on the magnet, red with all it can hold.
                    static int have_harpoon = -1;
                    if (have_harpoon < 0) have_harpoon = hud_has_model("fx_harpoon") ? 1 : 0;
                    for (const MagnetObj &mg : g_magnets) {
                        if (!mg.alive) continue;
                        const float cyan[3] = {0.0f, 0.876f, 1.0f};
                        trail(mg.o, mg.trail, mg.trail_t, mg.trail_n, 1.0f, 0.5f * UK_UNIT, cyan, 1.0f);
                        float dx = mg.o[0] - eye[0], dy = mg.o[1] - eye[1], dz = mg.o[2] - eye[2];
                        if (dx * dx + dy * dy + dz * dz < 1.0f) continue;
                        if (have_harpoon == 1) aimed("fx_harpoon", mg.o, mg.fwd, false, 0);
                        else ball(mg.p, 0.3f, 0, 0.3f, 0.8f, 1.0f, 1.0f);
                        float since = (float)(st.time - mg.beeped);
                        if (mg.stuck && since >= 0 && since < 1.0f)
{
                                // (v0.76 to v0.84 drew the prefab's own flash picture here, a hard-edged star, which read
                                // as a flat picture stuck on the harpoon. Davi: "feels like a png". Now a soft light: a
                                // wide glow in the load's colour with a small white heart, shrinking as before.)
                                float size = 6.4f * UK_UNIT * (1.0f + mg.load) * expf(-5.0f * since), fade = 1.0f - since;
                                sprite("softglow", mg.o, size * 1.5f, 0, mg.load, 1.0f - mg.load, 0.05f, 0.75f * fade);
                                sprite("softglow", mg.o, size * 0.45f, 0, 1.0f, 1.0f, 0.9f, 0.9f * fade);
                            }
                    }
                    // the mark over a stunned enemy (ours): a ring turning at its chest, beating between orange and white
                    for (const Stun &sn : g_stuns) {
                        float pt[3];
                        if (!sn.chr || st.time > sn.until || !whip_target_point(sn.chr, pt)) continue;
                        float beat = 0.5f + 0.5f * sinf((float)st.time * 14.0f);
                        sprite("RageEffectWhite", pt, 0.75f, (float)fmod(st.time * 180.0, 360.0), 1.0f, 0.45f + 0.5f * beat, 0.1f + 0.7f * beat, 0.8f);
                        sprite("softglow", pt, 0.45f, 0, 1.0f, 0.5f, 0.1f, 0.25f + 0.2f * beat);
                    }
                    // Rockets: the 'Rocket' prefab's own model along its way (2.1 u long), behind it its trail
                    // (0.15 s long, 1 u wide down to nothing, orange at alpha 0.39) and at its tail the blinking
                    // flash it carries ('muzzleflashshotgun 1', 1.9 u across, orange, added to the picture). One
                    // that is frozen has the Freezeframe's ring round it instead of a trail (the 'FreezeEffect':
                    // 'RageEffectWhite', 3.4 u across, turning at 60 degrees a second, in the variation's blue
                    // at alpha 0.75, 0.7 u ahead of the tail); one that has levelled up glows.
                    static int have_rocket = -1;
                    if (have_rocket < 0) have_rocket = hud_has_model("fx_rocket") ? 1 : 0;
                    for (const Rocket &rk : g_rockets) {
                        if (!rk.alive) continue;
                        const float orange[3] = {1.0f, 0.5f, 0.0f};
                        bool frozen = g_rockets_frozen;
                        if (!frozen) trail(rk.p, rk.trail, rk.trail_t, rk.trail_n, 0.15f, 1.0f * UK_UNIT, orange, 0.39f);
                        float dx = rk.p[0] - eye[0], dy = rk.p[1] - eye[1], dz = rk.p[2] - eye[2];
                        if (dx * dx + dy * dy + dz * dz < 1.0f || rk.hidden) continue;       // (not in the player's face as it leaves)
                        if (have_rocket == 1) aimed("fx_rocket", rk.p, rk.dir, false, 0);
                        else ball(rk.p, 0.3f, 0, 1.0f, 0.4f, 0.1f, 1.0f);
                        float phase = (float)fmod((st.time - rk.born) * 25.0, 1.0), blink = phase < 0.5f ? 1.0f - phase * 2.0f : phase * 2.0f - 1.0f;
                        if (!frozen) sprite("muzzleflashshotgun 1", rk.p, 1.92f * UK_UNIT, 0, 1.0f, 0.5f, 0.0f, blink);
                        float mid[3] = {rk.p[0] + rk.dir[0] * 0.7f * UK_UNIT, rk.p[1] + rk.dir[1] * 0.7f * UK_UNIT, rk.p[2] + rk.dir[2] * 0.7f * UK_UNIT};
                        if (frozen) sprite("RageEffectWhite", mid, 3.36f * UK_UNIT, (float)fmod(st.time * 60.0, 360.0), 0.251f, 0.906f, 1.0f, 0.749f);
                        if (rk.levelled) sprite("softglow", mid, 3.0f * UK_UNIT, 0, 1.0f, 0.75f, 0.3f, 0.45f + 0.25f * sinf((float)st.time * 20.0f));
                    }
                    // Cannonballs: the 'Cannonball' mesh (2 u across, dark grey) with the picture it carries
                    // facing the eye ('splash 2', 3.5 u across, turning) and its trail (0.25 s long, 1.5 u wide
                    // down to nothing, white at alpha 0.49).
                    static int have_ball = -1;
                    if (have_ball < 0) have_ball = hud_has_model("fx_cannonball") ? 1 : 0;
                    for (const Ball &cb : g_balls) {
                        if (!cb.alive) continue;
                        const float white[3] = {1.0f, 1.0f, 1.0f};
                        trail(cb.p, cb.trail, cb.trail_t, cb.trail_n, 0.25f, 1.5f * UK_UNIT, white, 0.49f);
                        float dx = cb.p[0] - eye[0], dy = cb.p[1] - eye[1], dz = cb.p[2] - eye[2];
                        if (dx * dx + dy * dy + dz * dz < 1.0f || cb.hidden) continue;
                        sprite("splash 2", cb.p, 3.52f * UK_UNIT, (float)fmod((st.time - cb.born) * 360.0, 360.0), 1.0f, 1.0f, 1.0f, 0.6f);
                        if (have_ball == 1) mesh("fx_cannonball", cb.p, BALL_RADIUS, (float)(st.time - cb.born) * 6.0f, 0, 0, 1.0f);
                        else ball(cb.p, 1.0f, 0, 0.25f, 0.25f, 0.25f, 1.0f);
                    }
                    // Pellets in flight ("Shotgun Projectile"): a small yellow ball 0.2 u across, with no
                    // trail (v0.66 and before drew one). A punched one is orange and four times the size.
                    for (const Pellet &pl : g_pellets) {
                        if (!pl.alive) continue;
                        if (pl.boosted) {
                            float short_tail[3] = {pl.p[0] - pl.v[0] * 0.03f, pl.p[1] - pl.v[1] * 0.03f, pl.p[2] - pl.v[2] * 0.03f};
                            line(short_tail, pl.p, 0.16f, 1.0f, 0.35f, 0.0f, 1.0f);
                            ball(pl.p, 0.4f, 0, 1.0f, 0.35f, 0.0f, 1.0f);
                        } else {
                            sprite("softglow", pl.p, 0.2f, 0, 1.0f, 0.8f, 0.1f, 1.0f);
                        }
                    }
                    // The core in flight: the "Grenade" object's own mesh (a small canister with a skull's
                    // face), tumbling, at its real size of 0.31 u.
                    static int have_core = -1;
                    if (have_core < 0) have_core = hud_has_model("fx_core") ? 1 : 0;
                    for (const Core &c : g_cores) {
                        if (!c.alive || c.hidden) continue;
                        {
                            // It leaves from a quarter of a metre in front of the eye: for its first frame it
                            // filled the whole view with orange (seen in a recording). Not drawn within a metre.
                            float dx = c.p[0] - eye[0], dy = c.p[1] - eye[1], dz = c.p[2] - eye[2];
                            if (dx * dx + dy * dy + dz * dz < 1.0f) continue;
                        }
                        const float *from = c.p;
                        for (int k = 0; k < c.trail_n; k++) {
                            float fade = 1.0f - (k + 0.5f) / CORE_TRAIL;
                            thin_line(from, c.trail[k], 0.52f * UK_UNIT * fade, 1.0f, 0.85f, 0.0f, fade);
                            from = c.trail[k];
                        }
                        if (have_core == 1) mesh("fx_core", c.p, 0.3092f * UK_UNIT, (float)(st.time - c.born) * 12.0f, 0, 0, 1.0f);
                        else ball(c.p, 0.3f, 0, 1.0f, 0.3f, 0.1f, 1.0f);
                        // The flash round it, which is most of what a core looks like in flight: the Grenade
                        // object carries the shotgun's muzzle-flash picture facing the eye, 3.84 u across (32
                        // pixels at 100 to the unit, on an object scaled by 24 under one scaled by a half),
                        // in (1, 0.74, 0.49), added to the picture and blinking (SpriteController: its alpha
                        // runs between 1 and 0 at 50 a second, a full blink every 0.04 s). (Not drawn before
                        // v0.71: the core was a small canister with a thin trail.)
                        {
                            float phase = (float)fmod((st.time - c.born) * 25.0, 1.0);
                            float alpha = phase < 0.5f ? 1.0f - phase * 2.0f : phase * 2.0f - 1.0f;
                            sprite("muzzleflashshotgun", c.p, 3.84f * UK_UNIT, 0, 1.0f, 0.7414f, 0.4858f, alpha);
                        }
                    }
                    // particles: pictures facing the eye, or thin streaks along their own motion
                    for (const Particle &pt : g_particles) {
                        if (!pt.alive || pt.hidden) continue;
                        if (pt.streak) {
                            float age = (float)(st.time - pt.born), back = pt.trail > 0 ? fminf(age, pt.trail) : 0.06f;
                            float tail[3] = {pt.p[0] - pt.v[0] * back, pt.p[1] - pt.v[1] * back, pt.p[2] - pt.v[2] * back};
                            thin_line(tail, pt.p, pt.trail > 0 ? pt.size : 0.03f, pt.r, pt.g, pt.b, pt.a * (1.0f - age / pt.life));
                        } else {
                            sprite(pt.sprite, pt.p, pt.size, 0, pt.r, pt.g, pt.b, pt.a);
                        }
                    }
                    // Explosions, as the "Explosion" prefab is put together. The ball of fire grows by 15 u
                    // of radius a second (1.75 times that for the larger one) and, past its full size, fades
                    // over half a second while its texture scrolls. The faint shell runs ahead at 2.5 times
                    // the speed to a third again the size. Two rings spread from the middle, the first by
                    // 192 u of width a second and gone in 0.4 s, the second by 58 u a second, gone in 0.67 s.
                    // The Knuckleblaster's wave is the shell and the slower ring alone.
                    for (const BlastFx &bf : g_blast_fx) {
                        float age = (float)(st.time - bf.born);
                        if (bf.radius <= 0 || age < 0 || age > 1.2f || bf.hidden) continue;
                        const ExplosionKind *ek = bf.wave || bf.kind < 0 ? nullptr : &EXPLOSIONS[bf.kind];
                        float speed = ek ? ek->speed : 1.0f, twice = bf.kind == EXPLODE_ULTRA ? 2.0f : 1.0f;
                        // Explosion.FixedUpdate: the ball starts at its prefab's size (2.68 u of radius; the shell at
                        // a tenth of that) and its scale gains 0.05 x speed every physics step of 8 ms, which is
                        // 16.8 u of radius a second at speed 1. Past its full size it starts to fade (2 a second)
                        // and grows at a quarter of that. (Until v0.68 it started from nothing and kept its full
                        // speed while it faded: seen in a recording, the ball ended more than twice the size it
                        // should have, swallowing a player 5 m away.)
                        const float grow = 0.05f / V1_STEP * 2.6833f * UK_UNIT;
                        float fire_rate = grow * speed, shell_rate = grow * (ek ? ek->shell_speed : 2.5f), shell_full = ek ? ek->shell_size * UK_UNIT : bf.radius;
                        float fire0 = fminf(2.6833f * UK_UNIT * twice, bf.radius), shell0 = 0.26833f * UK_UNIT * twice;
                        float t_fire = (bf.radius - fire0) / fire_rate, t_shell = (shell_full - shell0) / shell_rate;
                        float fire = age <= t_fire ? fire0 + fire_rate * age : bf.radius + fire_rate * 0.25f * (age - t_fire);
                        float shell = age <= t_shell ? shell0 + shell_rate * age : shell_full + shell_rate * 0.25f * (age - t_shell);
                        float fire_alpha = age <= t_fire ? 1.0f : 1.0f - 2.0f * (age - t_fire);
                        float shell_alpha = age <= t_shell ? 1.0f : 1.0f - 2.0f * (age - t_shell);
                        if (have_fx == 1) {
                            // With the game's depth at hand the world itself cuts the ball and the shell where
                            // they meet it, pixel by pixel, and they are drawn round. Pulling their few vertices
                            // in to the walls (the stand-in for when there is no depth) made flat-sided blocks
                            // of them in a corridor. The shell is faint, as its near-black material makes it.
                            bool cut = g_depth_live;
                            // (the super explosion's ball has a picture of its own, 'explosion 1')
                            static int have_super = -1;
                            if (have_super < 0) have_super = hud_has_model("fx_sphere_super") ? 1 : 0;
                            bool red = have_super == 1 && (bf.kind == EXPLODE_SUPER || bf.kind == EXPLODE_ULTRA || bf.kind == EXPLODE_SUPER_BIG);
                            if (!bf.wave) mesh(red ? "fx_sphere_super" : "fx_sphere", bf.p, fire, 0, 0, age, fire_alpha, cut ? nullptr : bf.lim_fire, cut ? 0 : bf.n_fire);
                            mesh("fx_shock", bf.p, shell, 0, age, 0, 0.12f * shell_alpha, cut ? nullptr : bf.lim_shell, cut ? 0 : bf.n_shell);
                        } else if (!bf.wave) {
                            ball(bf.p, fire * 2.0f, 0, 1.0f, 0.55f, 0.1f, fire_alpha);
                        }
                        // One ring: the prefab's second ring is never switched on. By the prefab's numbers
                        // it would spread by 192 u of width a second; it is a flat picture standing in the
                        // world there, seldom seen face on, and drawn facing the eye at that rate it was far
                        // larger than ULTRAKILL's looks (Davi's comparison), so it spreads at a third of it.
                        // (and no wider than the room: 2.3 times the mean of how far the shell can go)
                        // The Malicious Railcannon's ring starts fainter (alpha 0.69), spreads half again as fast
                        // (scaleSpeed 75 against 50) and takes 0.69 s to go where the plain one takes 0.4; the
                        // super explosion has three rings, at 50, 70 and 30.
                        if (bf.kind == EXPLODE_MALICIOUS || bf.kind == EXPLODE_CANNONBALL) {
                            sprite("Shockwave", bf.p, fminf((3.84f + 96.0f * age) * UK_UNIT, bf.ring * 2.3f), 0, 1, 1, 1, (0.686f - age) * 0.5f);
                        } else if (bf.kind == EXPLODE_SUPER || bf.kind == EXPLODE_ULTRA || bf.kind == EXPLODE_SUPER_BIG) {
                            static const float rates[3] = {64.0f, 90.0f, 38.0f};
                            for (float rate : rates)
                                sprite("Shockwave", bf.p, fminf((3.84f + rate * age) * UK_UNIT * twice, bf.ring * 2.3f), 0, 1, 1, 1, (1.0f - 2.5f * age) * 0.5f);
                        } else {
                            sprite("Shockwave", bf.p, fminf((3.84f + 64.0f * age) * UK_UNIT, bf.ring * 2.3f), 0, 1, 1, 1, (1.0f - 2.5f * age) * 0.5f);
                        }
                    }
                    // Coins: ULTRAKILL's own coin at its own size (0.2 u across), turning end over end, with
                    // the flash of its "CoinFlash" (the muzzle flash picture, 1.28 u across) while it can be
                    // split and when it is hit, and a short pale trail. Without the effect meshes it is the
                    // gold ring of before.
                    for (const Coin &c : g_coins) {
                        if (!c.alive || c.hidden) continue;
                        float age = (float)(st.time - c.born);
                        bool flash = c.shot || (age >= 0.35f && age < 0.417f);
                        if (have_fx == 1) {
                            // The trail: the last half second of the coin's path, 0.2 u wide, white, from a
                            // quarter strength at the coin to nothing at its end (the gradient's 49% through
                            // an additive material tinted a quarter grey).
                            const float *from = c.p;
                            for (int k = 0; k < c.trail_n; k++) {
                                float fade = 1.0f - (k + 0.5f) / COIN_TRAIL;
                                thin_line(from, c.trail[k], 0.2f * UK_UNIT * fade, 1.0f, 1.0f, 1.0f, 0.25f * fade);
                                from = c.trail[k];
                            }
                            mesh("fx_coin", c.p, 0.1005f * UK_UNIT, age * 25.0f, 0, 0, 1.0f);
                            if (flash) sprite("muzzleflash", c.p, 1.28f * UK_UNIT, age * 720.0f, 1, 1, 1, 1);
                            continue;
                        }
                        if (st.coin_count >= HudState::MAX_COINS) continue;
                        float d[3] = {c.p[0] - eye[0], c.p[1] - eye[1], c.p[2] - eye[2]};
                        float vx = d[0] * right[0] + d[1] * right[1] + d[2] * right[2], vy = d[0] * up[0] + d[1] * up[1] + d[2] * up[2],
                              vz = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
                        if (vz < 0.2f) continue;
                        HudState::CoinDot &dot = st.coins[st.coin_count++];
                        dot.x = vx / (vz * tan_x);
                        dot.y = vy / (vz * tan_y);
                        dot.size = 0.3f / (vz * tan_y * 2.0f);          // a coin drawn 0.3 m across
                        dot.phase = age * 25.0f;
                        dot.flash = flash;
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

// TimeController.ParryFlash: a white sheet at 49% (the ParryFlash image's own colour) goes over the
// screen and time stands still for 0.25 s of real time. The sheet is taken away by a timer of 0.1 s that
// only runs once time does again, so it covers the whole stop and a tenth of a second after it. Here the
// frame of the parry is shown under the sheet while the thread that presents frames waits out the stop
// (to one deadline, so the two cannot add up to more), and the sheet then fades over the next 0.1 s.
// The mod's clock leaves the stop out. (Until v0.63 the sheet was 85% white for 0.1 s and the picture was
// then shown again without it for the rest of the stop.)
static volatile LONG g_freezes = 0;
static HRESULT WINAPI my_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    InterlockedIncrement(&g_presents);
    float freeze = g_freeze_request, stop = g_hitstop_request;
    g_hitstop_request = 0;
    if (freeze > 0 && !g_parry_freeze) {
        g_freeze_request = freeze = 0;
        g_flash_until = now_s() + 0.1;
    }
    if (!g_ctrl && !g_dead && g_title_menu) {
        g_in_hud_draw = true;
        draw_title(sc);
        g_in_hud_draw = false;
    }
    depth_frame(sc);
    g_in_hud_draw = true;
    bool full = (g_ctrl || g_dead) && draw_full_hud(sc);       // (dead: the black screen stays up through the game's own loading)
    if (g_ctrl && !full) draw_hud(sc);
    g_in_hud_draw = false;
    if (!(freeze > 0)) {
        if (!(stop > 0) || !g_parry_freeze || !full) return g_orig_present(sc, sync, flags);
        // TimeController.HitStop: the frame of the hit stays up for the length of the stop, with no flash,
        // and the mod's clock leaves the stop out, as with the parry's freeze below
        double t0 = real_s();
        g_frozen_s = g_frozen_s + stop;
        HRESULT hr = g_orig_present(sc, sync, flags);
        while (real_s() - t0 < (double)stop - 0.002) Sleep(1);
        g_frozen_s = g_frozen_s + (real_s() - t0) - stop;
        InterlockedIncrement(&g_hitstops);
        return hr;
    }
    g_freeze_request = 0;
    if (!full) return g_orig_present(sc, sync, flags);
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11Texture2D *back = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev)) && dev) {
        dev->GetImmediateContext(&ctx);
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
        if (ctx && back) {
            static HudState white_store;                 // (kept off the stack, like the frame's own)
            HudState &white = *new (&white_store) HudState();
            white.flash_only = true;
            white.flash = 0.4902f;
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
    while (real_s() - t0 < (double)freeze - 0.002) Sleep(1);
    g_frozen_s = g_frozen_s + (real_s() - t0) - freeze;
    g_flash_until = now_s() + 0.1;
    InterlockedIncrement(&g_freezes);
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    return hr;
}

// Every swap chain shares one vtable, so a throwaway one on a hidden window gives us the slot to patch.
// ---- the game's depth buffer. The mod's effects are drawn over the finished picture; to have the world
// stand in front of them the picture's depth is needed. The game's immediate context is watched for the
// depth-stencil views it clears and binds: the one the size of the back buffer that is bound most in a
// frame is taken as the scene's.
struct DepthSeen {
    ID3D11DepthStencilView *dsv;
    UINT w, h, fmt, bind, samples;
    LONG clears, binds, frame_clears, frame_binds;
    LONG mark, lately;                           // binds when the last two-second count ended, and how many came in it
    float clear_value;
};
enum { MAX_DEPTH_SEEN = 24 };
static DepthSeen g_depth_seen[MAX_DEPTH_SEEN];
static volatile LONG g_depth_seen_n = 0;
static SRWLOCK g_depth_lock = SRWLOCK_INIT;      // several of the game's threads record draw calls at once
static DepthSeen *depth_seen(ID3D11DepthStencilView *dsv) {
    if (!dsv) return nullptr;
    LONG n = g_depth_seen_n;
    for (LONG i = 0; i < n; i++)
        if (g_depth_seen[i].dsv == dsv) return &g_depth_seen[i];
    AcquireSRWLockExclusive(&g_depth_lock);
    n = g_depth_seen_n;
    for (LONG i = 0; i < n; i++)
        if (g_depth_seen[i].dsv == dsv) {
            ReleaseSRWLockExclusive(&g_depth_lock);
            return &g_depth_seen[i];
        }
    if (n >= MAX_DEPTH_SEEN) {
        ReleaseSRWLockExclusive(&g_depth_lock);
        return nullptr;
    }
    DepthSeen &d = g_depth_seen[n];
    d = DepthSeen{};
    d.dsv = dsv;
    dsv->AddRef();                               // kept alive: it is looked at again at every Present
    ID3D11Resource *res = nullptr;
    dsv->GetResource(&res);
    if (res) {
        ID3D11Texture2D *tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) && tex) {
            D3D11_TEXTURE2D_DESC td;
            tex->GetDesc(&td);
            d.w = td.Width;
            d.h = td.Height;
            d.fmt = td.Format;
            d.bind = td.BindFlags;
            d.samples = td.SampleDesc.Count;
            tex->Release();
        }
        res->Release();
    }
    g_depth_seen_n = n + 1;
    ReleaseSRWLockExclusive(&g_depth_lock);
    return &d;
}
typedef void (STDMETHODCALLTYPE *ClearDsvFn)(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT, FLOAT, UINT8);
typedef void (STDMETHODCALLTYPE *SetTargetsFn)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
static ClearDsvFn g_orig_clear_dsv = nullptr;
static SetTargetsFn g_orig_set_targets = nullptr;
// (diagnostic) how many calls of each kind have come through, whoever made them
static volatile LONG g_dw_imm_set = 0, g_dw_imm_clear = 0, g_dw_imm_uav = 0, g_dw_imm_exec = 0, g_dw_def_set = 0, g_dw_def_clear = 0, g_dw_def_uav = 0, g_dw_def_finish = 0;
typedef void (STDMETHODCALLTYPE *SetTargetsUavFn)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *, UINT, UINT,
                                                  ID3D11UnorderedAccessView *const *, const UINT *);
typedef void (STDMETHODCALLTYPE *ExecListFn)(ID3D11DeviceContext *, ID3D11CommandList *, BOOL);
typedef HRESULT (STDMETHODCALLTYPE *FinishListFn)(ID3D11DeviceContext *, BOOL, ID3D11CommandList **);
static SetTargetsUavFn g_orig_set_uav = nullptr, g_orig_set_uav_def = nullptr;
static ExecListFn g_orig_exec = nullptr;
static FinishListFn g_orig_finish_def = nullptr;
static void STDMETHODCALLTYPE my_clear_dsv(ID3D11DeviceContext *ctx, ID3D11DepthStencilView *dsv, UINT flags, FLOAT depth, UINT8 stencil) {
    InterlockedIncrement(&g_dw_imm_clear);
    if (!g_in_hud_draw && (flags & D3D11_CLEAR_DEPTH))
        if (DepthSeen *d = depth_seen(dsv)) {
            d->clears++;
            d->frame_clears++;
            d->clear_value = depth;
        }
    g_orig_clear_dsv(ctx, dsv, flags, depth, stencil);
}
static DepthSeen *depth_seen(ID3D11DepthStencilView *dsv);
static void STDMETHODCALLTYPE my_set_uav(ID3D11DeviceContext *ctx, UINT n, ID3D11RenderTargetView *const *rtvs, ID3D11DepthStencilView *dsv, UINT slot, UINT uavs,
                                         ID3D11UnorderedAccessView *const *views, const UINT *counts) {
    InterlockedIncrement(&g_dw_imm_uav);
    if (!g_in_hud_draw)
        if (DepthSeen *d = depth_seen(dsv)) {
            d->binds++;
            d->frame_binds++;
        }
    g_orig_set_uav(ctx, n, rtvs, dsv, slot, uavs, views, counts);
}
static void STDMETHODCALLTYPE my_set_uav_def(ID3D11DeviceContext *ctx, UINT n, ID3D11RenderTargetView *const *rtvs, ID3D11DepthStencilView *dsv, UINT slot, UINT uavs,
                                             ID3D11UnorderedAccessView *const *views, const UINT *counts) {
    InterlockedIncrement(&g_dw_def_uav);
    if (DepthSeen *d = depth_seen(dsv)) {
        d->binds++;
        d->frame_binds++;
    }
    g_orig_set_uav_def(ctx, n, rtvs, dsv, slot, uavs, views, counts);
}
static void STDMETHODCALLTYPE my_exec(ID3D11DeviceContext *ctx, ID3D11CommandList *list, BOOL restore) {
    InterlockedIncrement(&g_dw_imm_exec);
    g_orig_exec(ctx, list, restore);
}
static HRESULT STDMETHODCALLTYPE my_finish_def(ID3D11DeviceContext *ctx, BOOL restore, ID3D11CommandList **list) {
    InterlockedIncrement(&g_dw_def_finish);
    return g_orig_finish_def(ctx, restore, list);
}
static void STDMETHODCALLTYPE my_set_targets(ID3D11DeviceContext *ctx, UINT n, ID3D11RenderTargetView *const *rtvs, ID3D11DepthStencilView *dsv) {
    InterlockedIncrement(&g_dw_imm_set);
    if (!g_in_hud_draw)
        if (DepthSeen *d = depth_seen(dsv)) {
            d->binds++;
            d->frame_binds++;
        }
    g_orig_set_targets(ctx, n, rtvs, dsv);
}
// The game draws through deferred contexts (v0.68's first try watched the immediate one alone and saw
// nothing at all), which are another class with a vtable of its own: the same two, for that one.
static ClearDsvFn g_orig_clear_dsv_def = nullptr;
static SetTargetsFn g_orig_set_targets_def = nullptr;
static void STDMETHODCALLTYPE my_clear_dsv_def(ID3D11DeviceContext *ctx, ID3D11DepthStencilView *dsv, UINT flags, FLOAT depth, UINT8 stencil) {
    InterlockedIncrement(&g_dw_def_clear);
    if (flags & D3D11_CLEAR_DEPTH)
        if (DepthSeen *d = depth_seen(dsv)) {
            d->clears++;
            d->frame_clears++;
            d->clear_value = depth;
        }
    g_orig_clear_dsv_def(ctx, dsv, flags, depth, stencil);
}
static void STDMETHODCALLTYPE my_set_targets_def(ID3D11DeviceContext *ctx, UINT n, ID3D11RenderTargetView *const *rtvs, ID3D11DepthStencilView *dsv) {
    InterlockedIncrement(&g_dw_def_set);
    if (DepthSeen *d = depth_seen(dsv)) {
        d->binds++;
        d->frame_binds++;
    }
    g_orig_set_targets_def(ctx, n, rtvs, dsv);
}
// Once a frame, at Present. The scene's view is the one the size of the back buffer that was bound most
// since the last Present. Its texture is copied into one the HUD's shaders can read, and how the game
// stores depth is worked out rather than assumed: now and then the middle pixel is read back and turned
// into a distance both ways a projection with the camera's near and far planes (the lens the camera hook
// sees: 0.05 and 3100) can store it, plain or reversed, and each is compared with what the game's ray
// cast along the view says. The way that agrees is used; until one does, nothing is handed over and the
// rough sight checks stay in charge.
static void depth_frame(IDXGISwapChain *sc) {
    static LONG frames = 0, logged = 0, samples = 0, votes[2] = {0, 0}, said = 0;
    static ID3D11Texture2D *staging = nullptr, *copy = nullptr;
    static ID3D11ShaderResourceView *copy_srv = nullptr;
    static UINT copy_w = 0, copy_h = 0, copy_fmt = 0;
    static int way = -1;                         // 0: plain (far is 1), 1: reversed (far is 0)
    static bool watching = false;
    frames++;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11Texture2D *back = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev)) || !dev) return;
    dev->GetImmediateContext(&ctx);
    if (!watching && ctx) {
        // The watch goes on the game's own contexts. Direct3D keeps a context's vtable inside the context
        // object itself (the pointer at its start points 8 bytes on, into the same block), one copy for
        // each context: patching one context's table changes nothing for another, and a stand-in device's
        // even less (the first two tries: the game's calls never came through). The game draws through
        // deferred contexts it made long before this runs (thousands of command lists were counted going
        // through the immediate context, and not one depth view bound on it), and Direct3D has no way of
        // listing them. So they are looked for: one deferred context is made from the game's device to
        // learn the functions its table points at, and the process's own writable memory is searched for
        // every block that starts with a pointer to 8 bytes past itself and has those functions at the
        // same places in the table that follows. Each one found is a deferred context of this device.
        // 33 is OMSetRenderTargets, 34 OMSetRenderTargetsAndUnorderedAccessViews, 53 ClearDepthStencilView.
        watching = true;
        patch_vtable(ctx, 53, (void *)my_clear_dsv, (void **)&g_orig_clear_dsv);
        patch_vtable(ctx, 33, (void *)my_set_targets, (void **)&g_orig_set_targets);
        patch_vtable(ctx, 34, (void *)my_set_uav, (void **)&g_orig_set_uav);
        patch_vtable(ctx, 58, (void *)my_exec, (void **)&g_orig_exec);                  // ExecuteCommandList (counted only)
        static ID3D11DeviceContext *sample = nullptr;                                  // never released: its table is patched too
        HRESULT hr = dev->CreateDeferredContext(0, &sample);
        int found = 0;
        double took = 0;
        if (SUCCEEDED(hr) && sample && (uintptr_t)*(void ***)sample == (uintptr_t)sample + 8) {
            void **svt = *(void ***)sample;
            void *f33 = svt[33], *f34 = svt[34], *f53 = svt[53];
            enum { MAX_FOUND = 64 };
            uintptr_t found_at[MAX_FOUND];
            const size_t CHUNK = 1 << 20, TAIL = 0x400;       // entry 53 is 0x1B0 bytes into the table
            std::vector<uint8_t> buf(CHUNK + TAIL);
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress, top = (uintptr_t)si.lpMaximumApplicationAddress;
            double t0 = real_s();
            unsigned long long scanned = 0;
            while (addr < top) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) break;
                uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & 0xFF) == PAGE_READWRITE && base != (uintptr_t)buf.data()) {
                    for (uintptr_t at = base; at < end; at += CHUNK) {
                        size_t want = end - at < CHUNK + TAIL ? end - at : CHUNK + TAIL;
                        SIZE_T got = 0;
                        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)at, buf.data(), want, &got) || got < 0x200) continue;
                        size_t limit = got - 0x1C0 < CHUNK ? got - 0x1C0 : CHUNK;
                        scanned += limit;
                        for (size_t off = 0; off < limit; off += 8) {
                            uintptr_t v;
                            memcpy(&v, &buf[off], 8);
                            if (v != at + off + 8) continue;
                            void *e33, *e53;
                            memcpy(&e33, &buf[off + 8 + 33 * 8], 8);
                            memcpy(&e53, &buf[off + 8 + 53 * 8], 8);
                            if (e33 == f33 && e53 == f53 && found < MAX_FOUND) found_at[found++] = at + off;
                        }
                    }
                }
                addr = end;
            }
            took = real_s() - t0;
            for (int i = 0; i < found; i++) {
                void *obj = (void *)found_at[i];
                patch_vtable(obj, 53, (void *)my_clear_dsv_def, (void **)&g_orig_clear_dsv_def);
                patch_vtable(obj, 33, (void *)my_set_targets_def, (void **)&g_orig_set_targets_def);
                patch_vtable(obj, 34, (void *)my_set_uav_def, (void **)&g_orig_set_uav_def);
            }
            logf("depth watch: %d deferred contexts found in %.0f MB of memory in %.2f s (one of them the mod's own), functions %p %p %p\n", found, scanned / 1048576.0, took,
                 f33, f34, f53);
        } else {
            logf("depth watch: no deferred context to learn from (%08lX), or its table is not inside it\n", (unsigned long)hr);
        }
        logf("depth watch: %s on the game's immediate context, %s on its deferred ones\n", g_orig_clear_dsv && g_orig_set_targets ? "installed" : "NOT installed",
             found > 1 ? "installed" : "not installed");
    }
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
    D3D11_TEXTURE2D_DESC bd{};
    if (back) back->GetDesc(&bd);
    LONG n = g_depth_seen_n;
    DepthSeen *scene = nullptr;
    for (LONG i = 0; i < n; i++) {
        DepthSeen &d = g_depth_seen[i];
        // (by its binds over the last couple of seconds, not this frame's: the game records its command lists ahead
        // of the frame being shown, and a count taken from one Present to the next kept missing the full-size views;
        // and not by all its binds ever, which a view the game has stopped using would go on winning)
        if (frames % 120 == 0) {
            d.lately = d.binds - d.mark;
            d.mark = d.binds;
        }
        if (d.w == bd.Width && d.h == bd.Height && d.samples == 1 && d.lately > 30 && (!scene || d.lately > scene->lately)) scene = &d;
    }
    if (g_ctrl && frames % 600 == 100 && logged < 1) {
        logged++;
        logf("depth watch calls so far: immediate set %ld, set+uav %ld, clear %ld, command lists run %ld; deferred set %ld, set+uav %ld, clear %ld, lists finished %ld\n",
             (long)g_dw_imm_set, (long)g_dw_imm_uav, (long)g_dw_imm_clear, (long)g_dw_imm_exec, (long)g_dw_def_set, (long)g_dw_def_uav, (long)g_dw_def_clear,
             (long)g_dw_def_finish);
        logf("depth views seen so far (%ld), back buffer %ux%u format %u:\n", (long)n, bd.Width, bd.Height, (unsigned)bd.Format);
        for (LONG i = 0; i < n; i++) {
            const DepthSeen &d = g_depth_seen[i];
            logf("  %p %ux%u format %u bind %X samples %u: cleared %ld times (to %.1f), bound %ld times; this frame %ld clears, %ld binds%s\n", (void *)d.dsv, d.w, d.h, d.fmt,
                 d.bind, d.samples, (long)d.clears, d.clear_value, (long)d.binds, (long)d.frame_clears, (long)d.frame_binds, &d == scene ? "  <- taken as the scene's" : "");
        }
    }
    bool live = false;
    float near_z = g_lens_seen[2], far_z = g_lens_seen[3];
    if (g_ctrl && scene && ctx && near_z > 0.0001f && far_z > near_z * 2.0f) {
        ID3D11Resource *res = nullptr;
        scene->dsv->GetResource(&res);
        ID3D11Texture2D *tex = nullptr;
        if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex);
        D3D11_TEXTURE2D_DESC td{};
        if (tex) tex->GetDesc(&td);
        // the typeless form of the format (what a copy is made as) and the form its depth is read through
        DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN;
        switch (td.Format) {
        case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS: typeless = DXGI_FORMAT_R24G8_TYPELESS; view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: typeless = DXGI_FORMAT_R32_TYPELESS; view = DXGI_FORMAT_R32_FLOAT; break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: typeless = DXGI_FORMAT_R32G8X24_TYPELESS; view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: typeless = DXGI_FORMAT_R16_TYPELESS; view = DXGI_FORMAT_R16_UNORM; break;
        default: break;
        }
        if (tex && typeless != DXGI_FORMAT_UNKNOWN) {
            if (!copy || copy_w != td.Width || copy_h != td.Height || copy_fmt != (UINT)typeless) {
                if (copy_srv) copy_srv->Release();
                if (copy) copy->Release();
                if (staging) staging->Release();
                copy = staging = nullptr;
                copy_srv = nullptr;
                D3D11_TEXTURE2D_DESC cd = td;
                cd.Format = typeless;
                cd.Usage = D3D11_USAGE_DEFAULT;
                cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                cd.CPUAccessFlags = 0;
                cd.MiscFlags = 0;
                HRESULT hr = dev->CreateTexture2D(&cd, nullptr, &copy);
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = view;
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                HRESULT hr2 = copy ? dev->CreateShaderResourceView(copy, &vd, &copy_srv) : E_FAIL;
                cd.Usage = D3D11_USAGE_STAGING;
                cd.BindFlags = 0;
                cd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                HRESULT hr3 = dev->CreateTexture2D(&cd, nullptr, &staging);
                copy_w = td.Width;
                copy_h = td.Height;
                copy_fmt = typeless;
                logf("depth: the scene's buffer is %ux%u format %u (bind %X); copy %s (%08lX), its view %s (%08lX), read-back copy %s (%08lX)\n", td.Width, td.Height,
                     (unsigned)td.Format, td.BindFlags, copy ? "made" : "FAILED", (unsigned long)hr, copy_srv ? "made" : "FAILED", (unsigned long)hr2,
                     staging ? "made" : "FAILED", (unsigned long)hr3);
            }
            if (copy && copy_srv) {
                ctx->CopyResource(copy, tex);
                // the two ways the depth may be stored: stored = a + b / distance
                float span = far_z - near_z;
                const float a[2] = {far_z / span, -near_z / span}, b[2] = {-far_z * near_z / span, far_z * near_z / span};
                float ray = g_center_dist;
                if (staging && (way < 0 ? frames % 20 == 0 : frames % 900 == 0) && ray > 0.5f && ray < 150.0f) {
                    ctx->CopyResource(staging, copy);
                    D3D11_MAPPED_SUBRESOURCE ms;
                    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms))) {
                        UINT bpp = ms.RowPitch / td.Width;
                        const uint8_t *px = (const uint8_t *)ms.pData + (size_t)ms.RowPitch * (td.Height / 2) + (size_t)(td.Width / 2) * bpp;
                        uint32_t raw = 0;
                        memcpy(&raw, px, bpp < 4 ? bpp : 4);
                        float stored;
                        if (typeless == DXGI_FORMAT_R24G8_TYPELESS) stored = (raw & 0xFFFFFF) / 16777215.0f;
                        else if (typeless == DXGI_FORMAT_R16_TYPELESS) stored = (raw & 0xFFFF) / 65535.0f;
                        else memcpy(&stored, &raw, 4);
                        ctx->Unmap(staging, 0);
                        float dist[2];
                        for (int k = 0; k < 2; k++) {
                            float den = stored - a[k];
                            dist[k] = fabsf(den) > 1e-9f ? b[k] / den : -1.0f;
                            if (way < 0 && dist[k] > 0 && fabsf(dist[k] - ray) < 0.25f * ray + 0.3f) votes[k]++;
                        }
                        samples++;
                        if (samples <= 8 || (way >= 0 && samples % 20 == 0))
                            logf("depth check %ld: the middle pixel holds %.7f, which is %.2f m if plain and %.2f m if reversed; the ray cast says %.2f m (agreed so far: plain %ld, reversed %ld)\n",
                                 (long)samples, stored, dist[0], dist[1], ray, (long)votes[0], (long)votes[1]);
                        if (way < 0 && samples >= 6)
                            for (int k = 0; k < 2; k++)
                                if (votes[k] >= 5 && votes[k] * 10 >= samples * 6 && votes[k] > votes[1 - k] * 3) {
                                    way = k;
                                    logf("depth: stored %s (near %.3f, far %.0f); the world now hides the mod's effects\n", k ? "reversed" : "plain", near_z, far_z);
                                }
                        if (way < 0 && samples == 60 && said++ == 0) logf("depth: neither way of storing it agrees with the ray cast after 60 looks; left off\n");
                    }
                }
                if (way >= 0) {
                    hud_set_depth(copy_srv, a[way], b[way]);
                    live = true;
                }
            }
        }
        if (tex) tex->Release();
        if (res) res->Release();
    }
    if (!live) hud_set_depth(nullptr, 0, 0);
    g_depth_live = live;
    for (LONG i = 0; i < n; i++) g_depth_seen[i].frame_clears = g_depth_seen[i].frame_binds = 0;
    if (back) back->Release();
    if (ctx) ctx->Release();
    dev->Release();
}

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
    logf("ultrasouls %s loaded\n", MOD_VERSION);
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
    // The hook on the picture goes in now, before any character is loaded: it is what draws over the title
    // screen. (It changes a table of the graphics system's own, not the game's code, which is what could not be
    // touched this early.) It waits for the game's window to be there.
    bool present_hooked = false;
    if (g_title_menu) {
        for (int i = 0; i < 120; i++) {
            HWND wnd = nullptr;
            EnumWindows(find_game_window, (LPARAM)&wnd);
            if (wnd) break;
            Sleep(500);
        }
        Sleep(1500);
        present_hooked = hook_present();
        logf("HUD (Present) hook, before the title screen: %s\n", present_hooked ? "installed" : "NOT installed");
        if (g_title_column) logf("title menu pointer: %s\n", hook_cursor_pos() ? "installed" : "NOT installed (import slot holds something else)");
    }

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
            death_watch(false, 0, t);
            continue;
        }
        rd(map + OFF_MAP_ANIM, anim);
        rd(map + OFF_MAP_POS, pos);
        g_player_pos = pos;
        g_player_chr = player;
        {
            int hp = 0, hp_max = 0;
            if (rd(player + OFF_HP, hp) && rd(player + OFF_HP + 4, hp_max) && hp_max > 0) {
                g_hud_health = 100.0f * (float)hp / (float)hp_max;
                death_watch(true, hp, t);
            }
            // With the no-death flag (below) set, the game stops a killing blow at 1 health instead of 0: the
            // log of Davi's play on 2026-10-07 has 365 -> 1, 271 -> 1 and 304 -> 1. "Has no health left"
            // then never came true, the flag was never let go, and the player could not die in first person
            // (the earlier trial had written the 0 itself). So a drop to exactly 1 is taken for the blow that
            // would have killed: the health is written to 0, which lets the flag go below, and the game
            // kills. Unless it comes while falling well under where the player last stood, or with nothing
            // below at all: that is a kill box, which the flag is there to stop, and what it took is given back.
            {
                static int seen_hp = -1;
                static uintptr_t seen_player = 0;
                if (player != seen_player) {
                    seen_player = player;
                    seen_hp = -1;
                }
                if (g_ctrl && hp == 1 && seen_hp > 1) {
                    float feet[3] = {0, 0, 0};
                    bool have_feet = pos && safe_read(pos + OFF_POS_X, feet, 12);
                    bool kill_box = g_void_below || (g_air && g_have_safe && have_feet && feet[1] < g_safe_pos[1] - 3.0f);
                    // (with fast_respawn the game is not told: the health stays at 1 and the death is the mod's own)
                    int put = kill_box ? seen_hp : g_fast_respawn ? 1 : 0;
                    if (put != hp) wr(player + OFF_HP, put);
                    if (!kill_box) {
                        g_death.fast = g_fast_respawn;
                        g_death.begin = true;
                    }
                    logf("health %d -> 1 with the no-death flag set: %s\n", seen_hp,
                         kill_box ? "a kill box (falling under the last place stood on); the health is given back"
                         : g_fast_respawn ? "a killing blow; the death is the mod's own (R restarts)" : "a killing blow; health written to 0 so that the game kills");
                    hp = put;
                }
                seen_hp = hp;
            }
            // Kill boxes. Parts of the maps kill whoever touches them: the floors under places a player
            // was never meant to reach, which V1's jumps and an explosion's throw reach all the time.
            // The game's own "no death" flag (bit 0x20 of the second flag word, +0x524) switches them
            // off, along with dying itself. So in first person the flag is held set while the player
            // has health left, and cleared the moment they have none, when the game then kills them as
            // it always does. (Tried in the game: with the flag set, health written to 0 left the
            // character standing; clearing the flag killed it at once.) A fall that the kill box would
            // have ended goes on until the rescue puts the player back.
            uint32_t f2 = 0;
            if (rd(player + 0x524, f2)) {
                bool hold = g_ctrl && hp > 0;
                uint32_t want = hold ? (f2 | 0x20u) : (f2 & ~0x20u);
                // The C test aid also takes no damage (Davi's wish): the game's own "no damage" flag, bit
                // 0x40 of the same word, is held set while it is on and cleared when it goes off. The mod's
                // own damage, from explosions, is skipped in blast_player.
                static bool no_damage_set = false;
                bool shield = g_ctrl && (g_no_cooldown || (g_dead && g_death.fast) || t < g_respawn_grace_until);
                if (shield) {
                    want |= 0x40u;
                    no_damage_set = true;
                } else if (no_damage_set) {
                    want &= ~0x40u;
                    no_damage_set = false;
                }
                if (want != f2) {
                    safe_write(player + 0x524, &want, 4);
                    static int said = 0;
                    if (said++ < 12) logf("no-death flag %s (health %d)\n", hold ? "set: kill boxes are off" : "cleared", hp);
                }
            }
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
            logf("HUD (Present) hook: %s\n", present_hooked ? "installed already" : hook_present() ? "installed" : "NOT installed");
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
