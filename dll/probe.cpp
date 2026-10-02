// ULTRASOULS probe: dinput8.dll proxy that locates the player in memory and logs what it finds.
// Changes nothing in the game; it only reads. Output goes to ultrasouls.log next to the exe.
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <initializer_list>

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

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID *out, LPUNKNOWN outer) {
    typedef HRESULT(WINAPI *Fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    if (!g_real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\dinput8.dll");
        g_real = LoadLibraryA(path);
    }
    Fn fn = g_real ? (Fn)GetProcAddress(g_real, "DirectInput8Create") : nullptr;
    return fn ? fn(inst, ver, iid, out, outer) : E_FAIL;
}

static bool safe_read(uintptr_t addr, void *out, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n, &got) && got == n;
}

template <typename T> static bool rd(uintptr_t addr, T &v) { return safe_read(addr, &v, sizeof(T)); }

// pattern bytes with -1 as wildcard
static int scan(const int *pat, size_t plen, uintptr_t *hits, int max_hits) {
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &mi, sizeof(mi));
    uintptr_t base = (uintptr_t)mi.lpBaseOfDll, end = base + mi.SizeOfImage;
    int n = 0;
    for (uintptr_t p = base; p < end;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
        uintptr_t rs = (uintptr_t)mbi.BaseAddress, re = rs + mbi.RegionSize;
        bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                        (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (readable) {
            const uint8_t *b = (const uint8_t *)rs;
            size_t len = re - rs;
            for (size_t i = 0; i + plen <= len; i++) {
                size_t j = 0;
                while (j < plen && (pat[j] < 0 || b[i + j] == (uint8_t)pat[j])) j++;
                if (j == plen) {
                    if (n < max_hits) hits[n] = rs + i;
                    n++;
                }
            }
        }
        p = re;
    }
    return n;
}

static void dump(const char *label, uintptr_t addr, int words) {
    logf("  %s @ %p:", label, (void *)addr);
    for (int i = 0; i < words; i++) {
        uint32_t u;
        if (!rd(addr + i * 4, u)) { logf(" ??"); break; }
        float f;
        memcpy(&f, &u, 4);
        logf(" [%02X]=%08X/%.3f", i * 4, u, f);
    }
    logf("\n");
}

static DWORD WINAPI probe_thread(LPVOID) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "ultrasouls.log");
    g_log = fopen(path, "w");
    logf("probe loaded, exe base %p\n", (void *)GetModuleHandleA(nullptr));

    // WorldChrBase: mov rax,[rip+X]; mov rcx,[rax+68]; test rcx,rcx; je ...; cmp [rsi+10],rbx; je ...; 48
    static const int PAT[] = {0x48, 0x8B, 0x05, -1, -1, -1, -1, 0x48, 0x8B, 0x48, 0x68, 0x48, 0x85, 0xC9, 0x0F, 0x84,
                              -1, -1, -1, -1, 0x48, 0x39, 0x5E, 0x10, 0x0F, 0x84, -1, -1, -1, -1, 0x48};
    uintptr_t world_chr_ptr = 0;
    for (int attempt = 0; attempt < 120 && !world_chr_ptr; attempt++) {
        uintptr_t hits[8];
        int n = scan(PAT, sizeof(PAT) / sizeof(PAT[0]), hits, 8);
        if (attempt % 10 == 0 || n) logf("scan attempt %d: %d match(es)\n", attempt, n);
        if (n >= 1) {
            int32_t rel;
            memcpy(&rel, (void *)(hits[0] + 3), 4);
            world_chr_ptr = hits[0] + 7 + rel;
            logf("match at %p -> WorldChrBase pointer at %p (exe+%llX)\n", (void *)hits[0], (void *)world_chr_ptr,
                 (unsigned long long)(world_chr_ptr - (uintptr_t)GetModuleHandleA(nullptr)));
        } else {
            Sleep(1000);
        }
    }
    if (!world_chr_ptr) { logf("pattern never matched; giving up\n"); return 0; }

    for (int tick = 0; tick < 600; tick++) {
        Sleep(2000);
        uintptr_t world = 0, chr = 0;
        if (!rd(world_chr_ptr, world) || !world) { logf("t%d: WorldChr null (menu/loading)\n", tick); continue; }
        if (!rd(world + 0x68, chr) || !chr) { logf("t%d: player null\n", tick); continue; }
        logf("t%d: WorldChr %p player %p\n", tick, (void *)world, (void *)chr);
        dump("player+3E0 (hp/stamina?)", chr + 0x3E0, 10);
        for (uintptr_t off : {(uintptr_t)0x48, (uintptr_t)0x68}) {
            uintptr_t map = 0, pos = 0, anim = 0;
            if (!rd(chr + off, map) || !map) { logf("  player+%llX: null\n", (unsigned long long)off); continue; }
            rd(map + 0x28, pos);
            rd(map + 0x18, anim);
            logf("  via player+%llX: map %p pos %p anim %p\n", (unsigned long long)off, (void *)map, (void *)pos, (void *)anim);
            if (pos) dump("pos+00", pos, 10);
            if (anim) dump("anim+40", anim + 0x40, 6);
        }
    }
    logf("probe finished\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(nullptr, 0, probe_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}
