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
static GUID g_dev_guids[8];
static volatile LONG g_dev_n = 0;
static const GUID GUID_SYS_MOUSE = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

typedef HRESULT(WINAPI *CreateDeviceFn)(void *self, const GUID &guid, void **dev, void *outer);
typedef HRESULT(WINAPI *GetStateFn)(void *self, DWORD cb, void *data);
typedef HRESULT(WINAPI *GetDataFn)(void *self, DWORD cb_obj, void *rgdod, DWORD *in_out, DWORD flags);
static CreateDeviceFn g_orig_create_device = nullptr;
static GetStateFn g_orig_get_state = nullptr;
static GetDataFn g_orig_get_data = nullptr;

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
    HRESULT hr = g_orig_get_state(self, cb, data);
    if (SUCCEEDED(hr) && self == g_mouse_dev && data && cb >= 8) {
        // DIMOUSESTATE starts with LONG lX, lY
        InterlockedExchangeAdd(&g_di_dx, ((LONG *)data)[0]);
        InterlockedExchangeAdd(&g_di_dy, ((LONG *)data)[1]);
        InterlockedIncrement(&g_di_events);
    }
    return hr;
}

static HRESULT WINAPI my_get_data(void *self, DWORD cb_obj, void *rgdod, DWORD *in_out, DWORD flags) {
    HRESULT hr = g_orig_get_data(self, cb_obj, rgdod, in_out, flags);
    if (SUCCEEDED(hr) && self == g_mouse_dev && rgdod && in_out && !(flags & 1) && cb_obj >= 8) {
        // DIDEVICEOBJECTDATA starts with DWORD dwOfs, dwData; offsets 0 and 4 are the X and Y axes
        for (DWORD i = 0; i < *in_out; i++) {
            const DWORD *e = (const DWORD *)((const uint8_t *)rgdod + i * cb_obj);
            if (e[0] == 0) InterlockedExchangeAdd(&g_di_dx, (LONG)e[1]);
            if (e[0] == 4) InterlockedExchangeAdd(&g_di_dy, (LONG)e[1]);
        }
        if (*in_out) InterlockedIncrement(&g_di_events);
    }
    return hr;
}

static HRESULT WINAPI my_create_device(void *self, const GUID &guid, void **dev, void *outer) {
    HRESULT hr = g_orig_create_device(self, guid, dev, outer);
    LONG n = InterlockedIncrement(&g_dev_n) - 1;
    if (n < 8) g_dev_guids[n] = guid;
    if (SUCCEEDED(hr) && dev && *dev && IsEqualGUID(guid, GUID_SYS_MOUSE)) {
        g_mouse_dev = *dev;
        patch_vtable(*dev, 9, (void *)my_get_state, (void **)&g_orig_get_state);
        patch_vtable(*dev, 10, (void *)my_get_data, (void **)&g_orig_get_data);
    }
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

static bool watch_raw_input() {
    HWND wnd = nullptr;
    EnumWindows(find_game_window, (LPARAM)&wnd);
    if (!wnd) return false;
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
static const float RUN_SPEED = 9.0f;        // m/s, provisional until V1's real numbers are read from its code
static const float AIR_CONTROL = 6.0f;      // 1/s, how fast held keys steer airborne momentum; provisional

static int key_down(int vk) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
}
static double g_vjump_t0 = 0;
static float g_carry = 0, g_dir_x = 0, g_dir_z = 0;
static volatile float g_seen_vel[3];        // what the game asked for this frame, before we touch it
static volatile LONG g_steps = 0;
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

static void hook_step(void *self, void *step_info, void *gravity) {
    uintptr_t proxy = 0;
    bool rising = false;
    if ((uintptr_t)self == g_player_pos && g_player_pos) {
        proxy = *(uintptr_t *)((uintptr_t)self + OFF_POS_PROXY);
        if (proxy) {
            float *vel = (float *)(proxy + OFF_PROXY_VEL);
            g_seen_vel[0] = vel[0];
            g_seen_vel[1] = vel[1];
            g_seen_vel[2] = vel[2];
            InterlockedIncrement(&g_steps);
            if (g_ctrl) {
                // First-person movement: WASD sets the horizontal velocity directly, relative to the view.
                // On the ground it is instant; in the air the current momentum is steered, and kept if no key is held.
                float dt = *(float *)((uintptr_t)step_info + 8);
                bool grounded = *(uint8_t *)((uintptr_t)self + OFF_POS_GROUNDED) != 0;
                float f = (float)(key_down('W') - key_down('S')), s = (float)(key_down('D') - key_down('A'));
                float wx = g_view_fwd[0] * f + g_view_right[0] * s, wz = g_view_fwd[1] * f + g_view_right[1] * s;
                float wl = sqrtf(wx * wx + wz * wz);
                if (wl > 1.0f) { wx /= wl; wz /= wl; }
                if (grounded) {
                    g_vx = wx * RUN_SPEED;
                    g_vz = wz * RUN_SPEED;
                } else if (wl > 0.01f) {
                    float k = AIR_CONTROL * dt < 1.0f ? AIR_CONTROL * dt : 1.0f;
                    g_vx += (wx * RUN_SPEED - g_vx) * k;
                    g_vz += (wz * RUN_SPEED - g_vz) * k;
                }
                vel[0] = g_vx;
                vel[2] = g_vz;
                // the character faces where the view looks; yaw 0 faces -Z (worked out from the probe log)
                *(float *)((uintptr_t)self + OFF_POS_YAW) = atan2f(-g_view_fwd[0], -g_view_fwd[1]);
            } else {
                g_vx = vel[0];
                g_vz = vel[2];
            }
            if (g_vjump && g_vel_ok) {
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
    g_orig_step(self, step_info, gravity);
    if (!proxy) return;
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
static volatile float g_sens = 0.0025f;         // radians per mouse count

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
    LONG mx = g_di_events ? dix : rawx, my = g_di_events ? diy : rawy;

    bool on = g_first_person && pos;
    if (on && !g_ctrl) {
        // entering first person: start from where the game's camera is looking
        float fl = sqrtf(m[8] * m[8] + m[10] * m[10]);
        g_yaw = atan2f(m[8], m[10]);
        g_pitch = atan2f(m[9], fl);
    }
    g_ctrl = on;
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
    m[13] = p[1] + g_eye_height;
    m[14] = p[2] + cy * EYE_FORWARD;
    return r;
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
// `n` (5..8) prologue bytes are moved; they must be whole, position-independent instructions.
// `fn` must be 8-byte aligned so the patch is a single store.
static void *install_hook(uintptr_t rva, const uint8_t *prologue, size_t n, void *hook) {
    uint8_t *fn = (uint8_t *)GetModuleHandleA(nullptr) + rva;
    if (n < 5 || n > 8 || ((uintptr_t)fn & 7) || memcmp(fn, prologue, n) != 0) return nullptr;
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
    for (size_t i = 5; i < n; i++) patch[i] = 0x90;
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
    logf("ultrasouls v0.12 loaded\n");
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
    bool k6 = false, k7 = false, k8 = false, kj = false, kk = false, k3 = false, k4 = false, k5 = false, k1 = false, k2 = false, k9 = false, k10 = false;
    double mouse_log = 0;
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

        if (!hook_tried && speed_checked) {
            hook_tried = true;
            logf("installing physics step hook...\n");
            g_orig_step = (StepFn)install_hook(RVA_STEP, STEP_PROLOGUE, sizeof(STEP_PROLOGUE), (void *)hook_step);
            hooked = g_orig_step != nullptr;
            g_orig_cam = (CamFn)install_hook(RVA_CAM_UPDATE, CAM_PROLOGUE, sizeof(CAM_PROLOGUE), (void *)hook_cam);
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
        if (pressed(VK_F9, k9)) { g_sens = g_sens * 0.8f; logf("mouse sensitivity %.5f\n", g_sens); }
        if (pressed(VK_F10, k10)) { g_sens = g_sens * 1.25f; logf("mouse sensitivity %.5f\n", g_sens); }
        if (g_first_person && t - mouse_log > 5.0) {
            mouse_log = t;
            logf("mouse: DirectInput reads %ld, raw input messages %ld; yaw %.2f pitch %.2f\n", (long)g_di_events, (long)g_raw_events, g_yaw, g_pitch);
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
