// Offline check of the HUD renderer: draws it onto an off-screen 1920x1080 target and writes BMP files,
// so the result can be looked at without starting the game.
//   g++ -O2 -o build/hud_test.exe dll/hud_test.cpp dll/hud.cpp -ld3d11 -ldxgi
//   build/hud_test.exe build/ultrasouls_assets.bin build/hud_test
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "hud.h"

static bool save_bmp(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, const std::string &path) {
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) return false;
    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms))) return false;
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t row = d.Width * 4, size = row * d.Height;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + size;
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = (LONG)d.Width;
    ih.biHeight = -(LONG)d.Height;   // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> line(row);
    for (UINT y = 0; y < d.Height; y++) {
        const uint8_t *src = (const uint8_t *)ms.pData + (size_t)y * ms.RowPitch;
        for (UINT x = 0; x < d.Width; x++) {   // RGBA -> BGRA
            line[x * 4 + 0] = src[x * 4 + 2];
            line[x * 4 + 1] = src[x * 4 + 1];
            line[x * 4 + 2] = src[x * 4 + 0];
            line[x * 4 + 3] = 255;
        }
        fwrite(line.data(), row, 1, f);
    }
    fclose(f);
    ctx->Unmap(staging, 0);
    staging->Release();
    return true;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: hud_test <pack file> <output prefix> [width height]\n");
        return 2;
    }
    UINT w = argc >= 5 ? (UINT)atoi(argv[3]) : 1920, h = argc >= 5 ? (UINT)atoi(argv[4]) : 1080;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { printf("no D3D11 device: %08lx\n", (unsigned long)hr); return 1; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *target = nullptr;
    dev->CreateTexture2D(&td, nullptr, &target);
    ID3D11RenderTargetView *rtv = nullptr;
    dev->CreateRenderTargetView(target, nullptr, &rtv);

    std::wstring pack(argv[1], argv[1] + strlen(argv[1]));
    if (!hud_init(dev, pack.c_str())) { printf("hud_init failed: %s\n", hud_error()); return 1; }

    struct Shot {
        const char *name;
        HudState st;
        float bg[4];
    };
    std::vector<Shot> shots;
    HudState s;
    s.time = 10.0;
    shots.push_back({"idle", s, {0.34f, 0.31f, 0.27f, 1}});
    s = HudState();
    s.time = 20.0;
    s.health = 43;
    s.stamina = 130;
    s.pierce_charge = 70;
    shots.push_back({"hurt_charging", s, {0.10f, 0.11f, 0.13f, 1}});
    s = HudState();
    s.time = 30.0;
    s.last_shot = 29.97;
    s.stamina = 250;
    s.pierce_ready = 40;
    shots.push_back({"just_fired", s, {0.55f, 0.52f, 0.47f, 1}});
    s = HudState();
    s.time = 40.0;
    s.variation = 1;
    s.coin_charge = 230;
    s.coin_count = 3;
    s.coins[0] = {0.0f, 0.3f, 0.08f, 0.0f, false};      // facing the camera
    s.coins[1] = {0.35f, 0.5f, 0.04f, 1.2f, false};     // mid-spin, further away
    s.coins[2] = {-0.3f, 0.1f, 0.06f, 0.4f, true};      // in its flash
    shots.push_back({"marksman_coins", s, {0.20f, 0.22f, 0.25f, 1}});
    s = HudState();
    s.time = 45.0;
    s.style_rank = 4;
    s.style_meter = 0.62f;
    s.style_line_count = 4;
    s.style_lines[0] = {"+ KILL", 1, 1, 1};
    s.style_lines[1] = {"+ DOUBLE KILL", 1, 0.65f, 0};
    s.style_lines[2] = {"+ RICOSHOT x2", 0, 1, 1};
    s.style_lines[3] = {"+ DISRESPECT", 1, 1, 1};
    shots.push_back({"style", s, {0.25f, 0.24f, 0.22f, 1}});
    s = HudState();
    s.time = 46.0;
    s.bob_x = 0.08f;
    s.bob_y = -0.025f;
    s.tracer_count = 3;
    s.tracers[0] = {0.18f, -0.22f, 0.0f, 0.0f, 0.006f, 0.0012f, 1.0f, 0.95f, 0.7f, 0.9f};      // a shot towards the crosshair
    s.tracers[1] = {0.18f, -0.22f, -0.4f, 0.5f, 0.012f, 0.003f, 0.2f, 0.8f, 1.0f, 0.8f};      // a charged shot to a coin
    s.tracers[2] = {-0.4f, 0.5f, 0.6f, 0.1f, 0.004f, 0.002f, 1.0f, 0.8f, 0.2f, 0.7f};         // the coin's ricochet
    shots.push_back({"tracers", s, {0.22f, 0.22f, 0.24f, 1}});
    // the animated models: a shot at three moments, the draw, and the punch
    static const struct { const char *name, *rev, *arm; float at; } poses[] = {
        {"anim_shoot_a", "Shoot", nullptr, 0.03f}, {"anim_shoot_b", "Shoot", nullptr, 0.12f}, {"anim_shoot_c", "Shoot", nullptr, 0.35f},
        {"anim_pickup", "PickUp", nullptr, 0.15f}, {"anim_jab_a", nullptr, "Jab", 0.10f},     {"anim_jab_b", nullptr, "Jab", 0.20f},
        {"anim_jab_c", nullptr, "Jab", 0.40f},     {"anim_hook", nullptr, "Hook", 0.25f},     {"anim_coinflip", nullptr, "CoinFlip", 0.25f},
    };
    // the Knuckleblaster's punch at three moments and its blast
    static const struct { const char *name, *clip; float at; } red[] = {
        {"knuckle_a", "Punch", 0.10f}, {"knuckle_b", "Punch", 0.22f}, {"knuckle_c", "Punch", 0.40f}, {"knuckle_blast", "PunchBlast", 0.30f},
    };
    for (const auto &p : red) {
        s = HudState();
        s.time = 60.0;
        s.arm2_clip = p.clip;
        s.arm2_clip_start = 60.0 - p.at;
        shots.push_back({p.name, s, {0.30f, 0.28f, 0.28f, 1}});
    }
    for (const auto &p : poses) {
        s = HudState();
        s.time = 50.0;
        s.revolver_clip = p.rev;
        s.revolver_clip_start = p.rev ? 50.0 - p.at : -100.0;
        s.arm_clip = p.arm;
        s.arm_clip_start = p.arm ? 50.0 - p.at : -100.0;
        shots.push_back({p.name, s, {0.30f, 0.30f, 0.33f, 1}});
    }

    // the shotgun: idle, just fired (muzzle flash, pellets, the reload under way), the core wound up and
    // thrown, and an explosion; then the Sharpshooter mid-spin
    static const struct { const char *name, *clip; float at; } sg[] = {
        {"shotgun_idle", nullptr, 0}, {"shotgun_fire_a", "FireWithReload", 0.03f}, {"shotgun_fire_b", "FireWithReload", 0.5f},
        {"shotgun_fire_c", "FireWithReload", 1.0f}, {"shotgun_core", "FireWithThrowReload", 1.2f}, {"shotgun_equip", "Equip", 0.2f},
    };
    for (const auto &p : sg) {
        s = HudState();
        s.time = 70.0;
        s.weapon = 1;
        s.revolver_clip = p.clip;
        s.revolver_clip_start = p.clip ? 70.0 - p.at : -100.0;
        if (p.clip && p.at < 0.05f) {
            s.muzzle_flash = 70.0 - p.at;
            s.tracer_count = 5;
            for (int i = 0; i < 5; i++) s.tracers[i] = {0.02f * i - 0.04f, -0.03f + 0.02f * i, 0.05f * i - 0.1f, 0.06f * i - 0.1f, 0.004f, 0.002f, 1.0f, 0.85f, 0.3f, 0.9f};
        }
        if (!p.clip) s.core_charge = 0.7f;
        shots.push_back({p.name, s, {0.28f, 0.30f, 0.28f, 1}});
    }
    s = HudState();
    s.time = 80.0;
    s.weapon = 1;
    s.blast_count = 3;
    s.blasts[0] = {0.1f, 0.1f, 0.5f, 0.75f, 1.0f, 0.55f, 0.1f, 1.0f};        // an explosion at full size
    s.blasts[1] = {-0.5f, 0.3f, 0.06f, 0, 1.0f, 0.3f, 0.1f, 1.0f};           // a core in flight
    s.blasts[2] = {0.5f, -0.1f, 0.25f, 0.6f, 1.0f, 0.55f, 0.1f, 0.4f};       // an explosion fading
    shots.push_back({"explosion", s, {0.22f, 0.24f, 0.26f, 1}});
    for (int i = 0; i < 3; i++) {
        static const char *const names[3] = {"sharp_a", "sharp_b", "sharp_c"};
        s = HudState();
        s.time = 90.0;
        s.variation = 2;
        s.twirl = 0.9f + 1.4f * i;
        s.twirl_charge = 30.0f + 30.0f * i;
        s.sharp_charge = 220.0f;
        shots.push_back({names[i], s, {0.30f, 0.26f, 0.26f, 1}});
    }

    // the displays on the weapons, the boss bars, and the HUD pushed about by movement
    static const struct { const char *name; int weapon, variation; float pierce_charge, pierce_ready, coin, sharp, meter, red; } disp[] = {
        {"disp_pierce_full", 0, 0, 0, 100, 400, 300, 1, 0},   {"disp_pierce_charging", 0, 0, 60, 100, 400, 300, 1, 0},
        {"disp_pierce_low", 0, 0, 0, 30, 400, 300, 1, 0},     {"disp_pierce_mid", 0, 0, 0, 70, 400, 300, 1, 0},
        {"disp_marksman_full", 0, 1, 0, 100, 400, 300, 1, 0}, {"disp_marksman_part", 0, 1, 0, 100, 250, 300, 1, 0},
        {"disp_sharp_full", 0, 2, 0, 100, 400, 300, 1, 0},    {"disp_sharp_part", 0, 2, 0, 100, 400, 140, 1, 0},
        {"disp_core_full", 1, 0, 0, 100, 400, 300, 1, 0},     {"disp_core_winding", 1, 0, 0, 100, 400, 300, 0.6f, 0.6f},
        {"disp_core_empty", 1, 0, 0, 100, 400, 300, 0, 0},
    };
    for (const auto &d : disp) {
        s = HudState();
        s.time = 110.0;
        s.weapon = d.weapon;
        s.variation = d.variation;
        s.pierce_charge = d.pierce_charge;
        s.pierce_ready = d.pierce_ready;
        s.coin_charge = d.coin;
        s.sharp_charge = d.sharp;
        s.core_meter = d.meter;
        s.core_meter_red = d.red;
        shots.push_back({d.name, s, {0.30f, 0.30f, 0.32f, 1}});
    }
    s = HudState();
    s.time = 120.0;
    s.boss_count = 2;
    strcpy(s.bosses[0].name, "TAURUS DEMON");
    s.bosses[0].hp = 0.62f;
    strcpy(s.bosses[1].name, "BELL GARGOYLE");
    s.bosses[1].hp = 1.0f;
    s.style_rank = 2;
    s.style_meter = 0.4f;
    shots.push_back({"boss_bars", s, {0.25f, 0.25f, 0.28f, 1}});
    s = HudState();
    s.time = 121.0;
    s.hud_sway[0] = -0.05f;                 // a dash to the right
    s.weapon_sway[0] = -0.14f;
    s.style_rank = 2;
    shots.push_back({"sway_dash_right", s, {0.25f, 0.27f, 0.25f, 1}});
    s = HudState();
    s.time = 122.0;
    s.hud_sway[1] = 0.1f;                   // a slam: falling at 100 u/s
    s.weapon_sway[1] = 0.2f;
    s.style_rank = 2;
    shots.push_back({"sway_falling", s, {0.25f, 0.27f, 0.25f, 1}});
    s = HudState();
    s.time = 123.0;
    s.flash_only = true;
    s.flash = 1.0f;
    shots.push_back({"flash_only", s, {0.25f, 0.27f, 0.25f, 1}});

    // the fist icon: each arm's picture, filled by punch stamina
    static const struct { const char *name; int arm; float stamina; int variation; } fists[] = {
        {"fist_feedbacker_full", 0, 2.0f, 1}, {"fist_feedbacker_low", 0, 0.6f, 0}, {"fist_knuckle_full", 1, 2.0f, 2}, {"fist_knuckle_half", 1, 1.2f, 2},
    };
    for (const auto &f : fists) {
        s = HudState();
        s.time = 130.0;
        s.arm = f.arm;
        s.punch_stamina = f.stamina;
        s.variation = f.variation;
        shots.push_back({f.name, s, {0.30f, 0.30f, 0.32f, 1}});
    }

    // strips for checking motion: the spin through a whole turn, and the shotgun's two reloads
    static char strip_names[40][24];
    int sn = 0;
    for (int i = 0; i < 8; i++) {
        s = HudState();
        s.time = 95.0;
        s.variation = 2;
        s.twirl = 6.2831853f * i / 8.0f;
        s.twirl_blend = 1.0f;
        snprintf(strip_names[sn], sizeof(strip_names[sn]), "twirl_%d", i);
        shots.push_back({strip_names[sn++], s, {0.30f, 0.26f, 0.26f, 1}});
    }
    for (int i = 0; i < 8; i++) {
        s = HudState();
        s.time = 96.0;
        s.weapon = 1;
        s.revolver_clip = "FireWithReload";
        s.revolver_clip_start = 96.0 - 1.6 * i / 8.0;
        snprintf(strip_names[sn], sizeof(strip_names[sn]), "sgfire_%d", i);
        shots.push_back({strip_names[sn++], s, {0.28f, 0.30f, 0.28f, 1}});
    }
    for (int i = 0; i < 8; i++) {
        s = HudState();
        s.time = 97.0;
        s.weapon = 1;
        s.revolver_clip = "FireWithThrowReload";
        s.revolver_clip_start = 97.0 - 3.3 * i / 8.0;
        snprintf(strip_names[sn], sizeof(strip_names[sn]), "sgcore_%d", i);
        shots.push_back({strip_names[sn++], s, {0.28f, 0.30f, 0.28f, 1}});
    }

    for (Shot &shot : shots) {
        ctx->ClearRenderTargetView(rtv, shot.bg);
        // two frames, so values that ease in have settled where a single frame would put them
        HudState warm = shot.st;
        warm.time -= 0.016;
        hud_draw(dev, ctx, target, warm);
        ctx->ClearRenderTargetView(rtv, shot.bg);
        hud_draw(dev, ctx, target, shot.st);
        std::string path = std::string(argv[2]) + "_" + shot.name + ".bmp";
        printf("%s: %s\n", path.c_str(), save_bmp(dev, ctx, target, path) ? "written" : "FAILED");
    }
    // the parry's freeze, as the DLL does it: the frame is kept, shown under the white sheet, then put back
    {
        const float bg[4] = {0.30f, 0.28f, 0.26f, 1};
        HudState a;
        a.time = 200.0;
        a.style_rank = 3;
        ctx->ClearRenderTargetView(rtv, bg);
        hud_draw(dev, ctx, target, a);
        bool kept = hud_keep_frame(dev, ctx, target);
        HudState white;
        white.flash_only = true;
        white.flash = 0.85f;
        hud_draw(dev, ctx, target, white);
        std::string base = std::string(argv[2]);
        printf("%s_freeze_flash.bmp: %s (frame kept: %s)\n", base.c_str(), save_bmp(dev, ctx, target, base + "_freeze_flash.bmp") ? "written" : "FAILED", kept ? "yes" : "NO");
        hud_put_frame(ctx, target);
        printf("%s_freeze_back.bmp: %s\n", base.c_str(), save_bmp(dev, ctx, target, base + "_freeze_back.bmp") ? "written" : "FAILED");
    }
    return 0;
}
