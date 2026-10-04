// Offline check of the sound mixer: plays a scripted sequence through the same mixing code the DLL
// uses and writes the result as a WAV file, so levels and glitches can be measured without the game.
//   g++ -O2 -static -o build/sound_test.exe dll/sound_test.cpp dll/sound.cpp -lwinmm
//   build/sound_test.exe build/ultrasouls_sounds.bin build/sound_test.wav
#include "sound.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: sound_test <pack> <out.wav>\n"); return 1; }
    wchar_t pack[512];
    mbstowcs(pack, argv[1], 511);
    if (!sound_init_offline(pack)) { printf("pack: %s\n", sound_error()); return 1; }
    const int rate = 48000, frame = 800;                 // rendered in 60 fps steps, as the game would trigger them
    struct Event { int at; const char *name; float vol, pitch; int channel; };
    const Event events[] = {
        {30, "jump", 0.75f, 1.0f},         {75, "dash", 1.0f, 1.0f},          {110, "landing", 0.7f, 1.0f},
        {150, "shot_piercer", 0.55f, 1.0f, 3}, {180, "shot_piercer", 0.55f, 1.1f, 3}, {210, "shot_marksman", 0.55f, 0.9f, 3},
        {240, "coin_toss", 1.0f, 1.0f},    {261, "coin_flash", 0.5f, 1.25f},  {270, "coin_hit", 0.35f, 0.65f},
        {276, "coin_hit", 0.35f, 0.65f},   {300, "shot_super", 0.5f, 1.0f},   {330, "landing_heavy", 0.9f, 1.0f},
        {331, "jump", 0.85f, 2.0f},        {360, "slide_stop", 0.5f, 1.5f},   {390, "stamina_fail", 0.6f, 0.5f},
        {420, "weapon_draw", 0.35f, 3.0f}, {450, "pierce_ready", 0.35f, 1.05f},
    };
    const int total = 660;
    std::vector<int16_t> out((size_t)total * frame * 2);
    int charge = 0, refill = 0;
    for (int f = 0; f < total; f++) {
        for (const Event &e : events)
            if (e.at == f && !sound_play(e.name, e.vol, e.pitch, false, e.channel)) printf("missing: %s\n", e.name);
        // the charge whine from frame 480 to 515, then the refill ticking until 640
        if (f == 480) charge = sound_play("pierce_charge", 0.25f, 0.01f, true);
        if (f > 480 && f < 515) sound_set(charge, 0.25f + (f - 480) * 2.9f * 0.005f, (f - 480) * 2.9f * 0.005f);
        if (f == 515) { sound_stop(charge); sound_play("shot_super", 0.5f, 1.0f); refill = sound_play("pierce_recharging", 0.25f, 1.0f, true); }
        if (f == 640) sound_stop(refill);
        sound_render(out.data() + (size_t)f * frame * 2, frame);
    }
    FILE *w = fopen(argv[2], "wb");
    uint32_t bytes = (uint32_t)(out.size() * 2), u32;
    uint16_t u16;
    fwrite("RIFF", 1, 4, w); u32 = 36 + bytes; fwrite(&u32, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w);
    u32 = 16; fwrite(&u32, 4, 1, w); u16 = 1; fwrite(&u16, 2, 1, w); u16 = 2; fwrite(&u16, 2, 1, w);
    u32 = rate; fwrite(&u32, 4, 1, w); u32 = rate * 4; fwrite(&u32, 4, 1, w); u16 = 4; fwrite(&u16, 2, 1, w); u16 = 16; fwrite(&u16, 2, 1, w);
    fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(out.data(), 1, bytes, w);
    fclose(w);
    printf("%s: %.1f s written\n", argv[2], (double)total * frame / rate);
    return 0;
}
