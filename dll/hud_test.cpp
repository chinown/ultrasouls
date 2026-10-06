// Offline check of the HUD renderer: draws it onto an off-screen 1920x1080 target and writes BMP files,
// so the result can be looked at without starting the game.
//   g++ -O2 -o build/hud_test.exe dll/hud_test.cpp dll/hud.cpp -ld3d11 -ldxgi
//   build/hud_test.exe build/ultrasouls_assets.bin build/hud_test
#include <windows.h>
#include <d3d11.h>
#include <cmath>
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

    // ULTRAKILL's effect meshes and sprites in the world, seen by a camera at the origin looking along +Z
    for (int k = 0; k < 3; k++) {
        static const char *const fx_names[3] = {"fx_explosion_a", "fx_explosion_b", "fx_coins"};
        s = HudState();
        s.time = 140.0;
        s.cam_valid = true;
        s.cam_tan_y = 0.8f;
        s.cam_tan_x = 0.8f * 16.0f / 9.0f;
        if (k < 2) {
            float grow = k == 0 ? 0.6f : 1.0f, fade = k == 0 ? 1.0f : 0.5f;
            s.world_meshes[s.world_mesh_count++] = {"fx_sphere", {0, 0.5f, 9}, 3.0f * grow, 0, {0, 0.3f * (k + 1)}, {1, 1, 1, fade}};
            s.world_meshes[s.world_mesh_count++] = {"fx_shock", {0, 0.5f, 9}, 4.0f * grow, 0, {0.4f * (k + 1), 0}, {1, 1, 1, 0.35f * fade}};
            s.fx_sprites[s.fx_sprite_count++] = {"Shockwave", 0.0f, 0.07f, 0.9f + 0.6f * k, 0, 1, 1, 1, 0.5f * fade};
            for (int i = 0; i < 24; i++)
                s.fx_sprites[s.fx_sprite_count++] = {nullptr, cosf(i * 0.9f) * (0.25f + 0.02f * i), 0.07f + sinf(i * 0.9f) * (0.3f + 0.02f * i), 0.012f + 0.001f * i, 0, 1, 1, 1, 1};
        } else {
            for (int i = 0; i < 5; i++)
                s.world_meshes[s.world_mesh_count++] = {"fx_coin", {-2.0f + i, 0.6f, 5}, 0.3f, 0.7f * i, {0, 0}, {1, 1, 1, 1}};
            for (int i = 0; i < 20; i++)
                s.fx_sprites[s.fx_sprite_count++] = {i % 2 ? "blooddrop" : "spark", -0.5f + 0.05f * i, -0.3f + 0.02f * (i % 5), 0.02f, 0, 1, 1, 1, 1};
            s.fx_sprites[s.fx_sprite_count++] = {"muzzleflash", 0.3f, 0.4f, 0.2f, 30, 1, 1, 1, 1};
            for (int i = 0; i < 4; i++)
                s.world_meshes[s.world_mesh_count++] = {"fx_core", {-1.2f + 0.8f * i, -0.4f, 3}, 0.31f, 0.9f * i, {0, 0}, {1, 1, 1, 1}};
        }
        shots.push_back({fx_names[k], s, {0.22f, 0.24f, 0.28f, 1}});
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

    // v0.66: the alternate revolvers, the Pump Charge shotgun and the railcannons
    static char new_names[80][24];
    int nn = 0;
    auto add = [&](const char *fmt, int i, const HudState &st, float r, float g, float b) {
        snprintf(new_names[nn], sizeof(new_names[nn]), fmt, i);
        shots.push_back({new_names[nn++], st, {r, g, b, 1}});
    };
    for (int v = 0; v < 3; v++) {
        s = HudState();
        s.time = 150.0;
        s.alt = true;
        s.variation = v;
        s.sharp_charge = 200.0f;
        s.coin_charge = 250.0f;
        add("slab_idle_%d", v, s, 0.30f, 0.30f, 0.33f);
    }
    for (int i = 0; i < 8; i++) {
        s = HudState();
        s.time = 151.0;
        s.alt = true;
        s.variation = 1;
        s.revolver_clip = "Shoot";
        s.revolver_clip_start = 151.0 - 1.4 * i / 8.0;
        add("slab_shoot_%d", i, s, 0.30f, 0.30f, 0.33f);
    }
    for (int i = 0; i < 8; i++) {
        s = HudState();
        s.time = 152.0;
        s.alt = true;
        s.variation = 2;
        s.twirl = 6.2831853f * i / 8.0f;
        s.twirl_blend = 1.0f;
        s.sharp_charge = 300.0f;
        add("slab_twirl_%d", i, s, 0.30f, 0.26f, 0.26f);
    }
    for (int i = 0; i < 4; i++) {
        s = HudState();
        s.time = 153.0;
        s.alt = true;
        s.variation = 0;
        s.revolver_clip = i < 2 ? "PickUpWithReload" : "ShootTwirl";
        s.revolver_clip_start = 153.0 - (i < 2 ? 0.3 + 0.6 * i : 0.1 + 0.25 * (i - 2));
        add("slab_reload_%d", i, s, 0.30f, 0.30f, 0.33f);
    }
    for (int i = 0; i < 6; i++) {
        static const float charges[6] = {0.0f, 0.6f, 1.5f, 2.5f, 3.7f, 5.0f};
        s = HudState();
        s.time = 160.0;
        s.weapon = 2;
        s.rail_charge = charges[i];
        add("rail_charge_%d", i, s, 0.28f, 0.28f, 0.32f);
    }
    s = HudState();
    s.time = 161.0;
    s.weapon = 2;
    s.weapon_var = 1;
    add("rail_malicious_%d", 0, s, 0.30f, 0.26f, 0.26f);
    for (int i = 0; i < 6; i++) {
        s = HudState();
        s.time = 162.0;
        s.weapon = 2;
        s.rail_charge = 0.0f;
        s.revolver_clip = "Fire";
        s.revolver_clip_start = 162.0 - 1.3 * i / 6.0;
        if (i == 0) s.muzzle_flash = 162.0;
        add("rail_fire_%d", i, s, 0.28f, 0.28f, 0.32f);
    }
    for (int i = 0; i < 3; i++) {
        s = HudState();
        s.time = 163.0;
        s.weapon = 2;
        s.revolver_clip = "Equip";
        s.revolver_clip_start = 163.0 - 0.1 - 0.35 * i;
        add("rail_equip_%d", i, s, 0.28f, 0.28f, 0.32f);
    }
    for (int i = 0; i < 4; i++) {
        static const float rgb[4][3] = {{0.27f, 1, 0.27f}, {0.27f, 1, 0.27f}, {0.63f, 0.63f, 0.26f}, {1, 0, 0}};
        s = HudState();
        s.time = 170.0;
        s.weapon = 1;
        s.weapon_var = 1;
        s.core_meter = i / 3.0f;
        s.meter_rgb_set = true;
        memcpy(s.meter_rgb, rgb[i], sizeof(s.meter_rgb));
        add("pump_meter_%d", i, s, 0.28f, 0.30f, 0.28f);
    }
    for (int i = 0; i < 6; i++) {
        s = HudState();
        s.time = 171.0;
        s.weapon = 1;
        s.weapon_var = 1;
        s.revolver_clip = i < 3 ? "Pump2" : "FireWithPump";
        s.revolver_clip_start = 171.0 - (i < 3 ? 0.1 + 0.2 * i : 0.2 + 0.3 * (i - 3));
        add("pump_clip_%d", i, s, 0.28f, 0.30f, 0.28f);
    }

    // v0.67: the lights on the guns, the railcannon's meter and moving parts, beams drawn with pictures,
    // and an explosion's ball pressed against a floor and a wall
    for (int v = 0; v < 6; v++) {
        s = HudState();
        s.time = 180.0;
        s.alt = v >= 3;
        s.variation = v % 3;
        s.cylinder = 0.5f * v;
        s.pierce_ready = 70.0f;
        add("lights_%d", v, s, 0.10f, 0.10f, 0.12f);
    }
    for (int i = 0; i < 4; i++) {
        static const float charges[4] = {0.5f, 2.4f, 4.5f, 5.0f};
        s = HudState();
        s.time = 181.0 + 0.013 * i;
        s.weapon = 2;
        s.weapon_var = i == 3 ? 1 : 0;
        s.rail_charge = charges[i] > 4.0f ? 5.0f : charges[i];
        s.rail_meter = true;
        s.rail_flash = i == 2 ? 0.7f : 0.0f;
        add("railhud_%d", i, s, 0.10f, 0.10f, 0.12f);
    }
    for (int k = 0; k < 8; k++) {
        // each beam from the muzzle (down and to the right of an eye at the origin looking along +Z) to a wall
        // 40 m off, fresh and then half spent; the railcannons' also seen from the side, 6 m away
        static const int kinds[8] = {BEAM_REVOLVER, BEAM_SLAB, BEAM_SUPER, BEAM_SHARP, BEAM_RAIL, BEAM_RAIL, BEAM_MALICIOUS, BEAM_MALICIOUS};
        int kind = kinds[k];
        bool side = k == 5 || k == 7;
        s = HudState();
        s.time = 182.0 + 0.07 * k;
        s.weapon = kind >= BEAM_RAIL ? 2 : 0;
        s.weapon_var = kind == BEAM_MALICIOUS ? 1 : 0;
        s.alt = kind == BEAM_SLAB;
        s.variation = kind == BEAM_SHARP ? 2 : kind == BEAM_SUPER ? 0 : 1;
        s.rail_charge = 0.0f;
        s.rail_meter = true;
        s.show_viewmodel = !side;
        s.cam_valid = true;
        s.cam_tan_y = 1.3f;
        s.cam_tan_x = 1.3f * 16.0f / 9.0f;
        float left = side ? 0.8f : k % 2 ? 0.5f : 1.0f;
        if (side) {
            const float a[3] = {-12, -0.5f, 6}, b[3] = {14, 0.5f, 6};
            hud_beam(s, a, b, kind, BEAM_LOOKS[kind].width_u * 0.5f * left, 0.5f, 0xFF, k);
        } else {
            const float a[3] = {0.35f, -0.3f, 0.9f}, b[3] = {0.5f, 1.0f, 40.0f};
            hud_beam(s, a, b, kind, BEAM_LOOKS[kind].width_u * 0.5f * left, 0.5f, 0xFF, k);
        }
        add("beam_%d", k, s, 0.10f, 0.10f, 0.12f);
    }
    static float limit_a[64], limit_b[64];
    for (int k = 0; k < 2; k++) {
        // an explosion 1 m over a floor and 1.5 m from a wall on its right, at two sizes
        s = HudState();
        s.time = 183.0;
        s.show_viewmodel = true;
        s.cam_valid = true;
        s.cam_tan_y = 0.8f;
        s.cam_tan_x = 0.8f * 16.0f / 9.0f;
        float dirs[64 * 3];
        const char *names[2] = {"fx_sphere", "fx_shock"};
        float *limits[2] = {limit_a, limit_b};
        int counts[2];
        for (int m = 0; m < 2; m++) {
            counts[m] = hud_model_dirs(names[m], dirs, 64);
            for (int v = 0; v < counts[m]; v++) {
                float lim = 1e9f;
                if (dirs[v * 3 + 1] < -0.01f) lim = fminf(lim, 1.0f / -dirs[v * 3 + 1]);
                if (dirs[v * 3] > 0.01f) lim = fminf(lim, 1.5f / dirs[v * 3]);
                limits[m][v] = lim;
            }
        }
        float grow = k == 0 ? 0.6f : 1.0f;
        s.world_meshes[s.world_mesh_count++] = {"fx_sphere", {0, 0.2f, 9}, 3.0f * grow, 0, {0, 0.3f}, {1, 1, 1, 1}, limit_a, counts[0]};
        s.world_meshes[s.world_mesh_count++] = {"fx_shock", {0, 0.2f, 9}, 4.0f * grow, 0, {0.4f, 0}, {1, 1, 1, 0.35f}, limit_b, counts[1]};
        // where the floor and the wall are, as lines
        s.tracers[s.tracer_count++] = {-0.9f, -0.2f * 1.0f / 0.8f * 0.11f - 0.0f, 0.9f, -0.2f * 1.0f / 0.8f * 0.11f, 0.002f, 0.002f, 1, 0, 0, 1, true};
        add("fx_pressed_%d", k, s, 0.22f, 0.24f, 0.28f);
        printf("fx_sphere has %d vertices, fx_shock %d\n", counts[0], counts[1]);
    }

    // v0.69: the whiplash arm through its three clips, with its cable to a point 20 m ahead
    {
        static const struct { const char *clip; float at; bool out; float warp; } whip[] = {
            {"Throw", 0.05f, true, 0.8f}, {"Throw", 0.20f, true, 0.3f}, {"Throw", 0.60f, true, 0.0f}, {"Pull", 0.20f, true, 0.0f},
            {"Pull", 1.60f, true, 0.0f},  {"Catch", 0.10f, false, 0.0f}, {"Catch", 0.40f, false, 0.0f}, {"Catch", 0.70f, false, 0.0f},
        };
        int k = 0;
        for (const auto &w : whip) {
            s = HudState();
            s.time = 190.0;
            s.whip_clip = w.clip;
            s.whip_clip_start = 190.0 - w.at;
            s.whip_hold = w.out;
            s.whip_out = w.out;
            s.cam_valid = true;
            s.cam_tan_y = 0.8f;
            s.cam_tan_x = 0.8f * 16.0f / 9.0f;
            if (w.out) {
                // as the DLL does it: the cable starts where the hand was drawn the frame before
                hud_draw(dev, ctx, target, s);
                float hx = -0.45f, hy = -0.55f;
                bool known = hud_whip_hand(&hx, &hy);
                printf("whip %s %.2f: hand at %.2f %.2f (%s)\n", w.clip, w.at, hx, hy, known ? "drawn" : "default");
                const float hook[3] = {0.5f, 0.6f, 20.0f}, wire[3] = {0.16f, 0.16f, 0.155f}, claw[3] = {0.30f, 0.33f, 0.30f};
                float hand[3] = {hx * s.cam_tan_x * 0.6f, hy * s.cam_tan_y * 0.6f, 0.6f}, prev[3], pt[3];
                memcpy(prev, hand, sizeof(prev));
                for (int j = 1; j <= 8; j++) {
                    float wobble = j < 8 ? 1.0f / j * (j % 2 == 0 ? -3.0f : 3.0f) * w.warp * 0.5f : 0.0f;
                    for (int i = 0; i < 3; i++) pt[i] = j < 8 ? hand[i] + (hook[i] - hand[i]) * (j / 9.0f) + (i == 1 ? wobble : 0.0f) : hook[i];
                    hud_strip(s, prev, pt, 0.05f, wire, wire, 1.0f, nullptr, 0, 0, 0.01f);
                    memcpy(prev, pt, sizeof(prev));
                }
                float tail[3] = {hook[0], hook[1], hook[2] - 0.3f};
                hud_strip(s, tail, hook, 0.15f, claw, claw, 1.0f, nullptr, 0, 0, 0.03f);
            }
            add("whip_%d", k++, s, 0.45f, 0.43f, 0.40f);
        }
    }

    // v0.70: an explosion's pictures behind a punching arm and the gun (0), and the same given no distance,
    // which is how the HUD's own pictures are drawn: over everything (1)
    for (int k = 0; k < 2; k++) {
        s = HudState();
        s.time = 195.0;
        s.arm_clip = "Jab";
        s.arm_clip_start = 195.0 - 0.2;
        float z = k == 0 ? 6.0f : 0.0f;
        s.fx_sprites[s.fx_sprite_count++] = {"Shockwave", 0.0f, -0.2f, 1.2f, 0, 1, 1, 1, 0.8f, z};
        for (int i = 0; i < 40; i++)
            s.fx_sprites[s.fx_sprite_count++] = {nullptr, -0.8f + 0.04f * i, -0.6f + 0.2f * (i % 5), 0.03f, 0, 1, 1, 1, 1, z};
        s.blasts[s.blast_count++] = {0.1f, -0.3f, 0.3f, 0, 1.0f, 0.55f, 0.1f, 1.0f, z};
        add("cover_%d", k, s, 0.30f, 0.30f, 0.33f);
    }

    // v0.75: the Sawblade Launcher: at rest (both variations), drawing, firing, the heated shot; saws and a magnet in the world
    {
        static const struct { const char *clip; float at; int var; const char *note; } saw[] = {
            {nullptr, 0, 0, "SAWS 10  MAGNETS 3"}, {nullptr, 0, 1, "HEAT 62%  SINK IN"}, {"Equip", 0.15f, 0, ""}, {"Equip", 0.50f, 0, ""},
            {"Shoot", 0.04f, 0, "SAWS 7  MAGNETS 1"}, {"Shoot", 0.15f, 0, ""}, {"ShootSuper", 0.10f, 1, "HEAT 0%  SINK OUT"}, {"ShootSuper", 0.40f, 1, ""},
        };
        int k = 0;
        for (const auto &w : saw) {
            s = HudState();
            s.time = 198.0;
            s.weapon = 3;
            s.weapon_var = w.var;
            snprintf(s.weapon_note, sizeof(s.weapon_note), "%s", w.note);
            s.revolver_clip = w.clip;
            s.revolver_clip_start = w.clip ? 198.0 - w.at : -100.0;
            s.cam_valid = true;
            s.cam_tan_y = 0.8f;
            s.cam_tan_x = 0.8f * 16.0f / 9.0f;
            if (k < 2) {
                // a saw 8 m off (2 m across for the Attractor's, 1.5 m for the Overheat's), a heated one further, and a magnet
                s.fx_sprites[s.fx_sprite_count++] = {"sawblade", -0.25f, 0.2f, (k == 0 ? 2.0f : 1.5f) / (8.0f * 0.8f * 2.0f), 30, k == 0 ? 1.0f : 0.5f, k == 0 ? 1.0f : 0.5f, k == 0 ? 1.0f : 0.5f, 1, 8.0f};
                s.fx_sprites[s.fx_sprite_count++] = {"sawblade 2", 0.35f, 0.35f, 3.0f / (14.0f * 0.8f * 2.0f), 70, 1.0f, 0.6f, 0.0f, 1, 14.0f};
                s.world_meshes[s.world_mesh_count++] = {"fx_magnet", {-1.5f, 0.5f, 6.0f}, 0.4857f, 0, {0, 0}, {1, 1, 1, 1}, nullptr, 0};
            }
            add("saw_%d", k++, s, 0.33f, 0.34f, 0.37f);
        }
    }

    // v0.73: blood on the screen after a heal, fresh and half gone
    s = HudState();
    s.time = 196.0;
    s.health = 60;
    s.screen_blood_count = 4;
    s.screen_blood[0] = {-300, 120, 0, 0.49f};
    s.screen_blood[1] = {250, -180, 2, 0.49f};
    s.screen_blood[2] = {60, 40, 4, 0.25f};
    s.screen_blood[3] = {380, 230, 1, 0.12f};
    add("screen_blood_%d", 0, s, 0.55f, 0.52f, 0.47f);

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
