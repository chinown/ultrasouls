// Real-time check of the sound path without the game: opens the audio device exactly as the DLL does,
// plays a short scripted sequence at a volume too low to hear, then saves what the mixer sent (sound_dump)
// and reports dropouts. It exercises the mixer thread, the waveOut queue and the capture.
//   g++ -O2 -static -o build/sound_rt_test.exe dll/sound_rt_test.cpp dll/sound.cpp -lwinmm
//   build/sound_rt_test.exe build/ultrasouls_sounds.bin build/sound_rt_dump.wav [volume]
#include "sound.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: sound_rt_test <pack> <dump.wav> [volume]\n"); return 1; }
    wchar_t pack[512], dump[512];
    mbstowcs(pack, argv[1], 511);
    mbstowcs(dump, argv[2], 511);
    if (!sound_init(pack)) { printf("init: %s\n", sound_error()); return 1; }
    sound_master(argc > 3 ? (float)atof(argv[3]) : 0.002f);
    struct Event { int ms; const char *name; float vol, pitch; int channel; };
    const Event events[] = {
        {300, "jump", 0.75f, 1.0f, 1},          {900, "dash", 1.0f, 1.0f, 1},           {1500, "landing", 0.78f, 1.0f, 2},
        {2000, "shot_piercer", 0.55f, 1.0f, 3}, {2500, "shot_piercer", 0.55f, 1.05f, 3}, {3000, "shot_piercer", 0.55f, 0.95f, 3},
        {3500, "coin_toss", 1.0f, 1.0f, 0},     {3850, "coin_flash", 0.5f, 1.25f, 0},   {4000, "coin_hit", 0.35f, 0.65f, 0},
        {4500, "shot_super", 0.5f, 1.0f, 0},
    };
    DWORD t0 = GetTickCount();
    size_t next = 0;
    while (GetTickCount() - t0 < 6500) {
        DWORD now = GetTickCount() - t0;
        while (next < sizeof(events) / sizeof(events[0]) && (DWORD)events[next].ms <= now) {
            const Event &e = events[next++];
            if (!sound_play(e.name, e.vol, e.pitch, false, e.channel)) printf("missing: %s\n", e.name);
        }
        Sleep(16);
    }
    printf("ran for %.2f s; dropouts: %d\n", (GetTickCount() - t0) / 1000.0, sound_underruns());
    printf("dump: %s\n", sound_dump(dump) ? "written" : "failed");
    return 0;
}
