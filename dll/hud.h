// ULTRAKILL-style HUD and revolver viewmodel, drawn with Direct3D 11.
// All art comes from a pack file built by tools/uk_assets.py out of the user's own copy of ULTRAKILL.
#pragma once
#include <d3d11.h>

struct HudState {
    float health = 100.0f;         // 0..100 on V1's scale
    float stamina = 300.0f;        // 0..300, three bars of 100
    float pierce_charge = 0.0f;    // 0..100 while the alt fire is held
    float pierce_ready = 100.0f;   // 0..100, refilling after a charged shot
    double time = 0.0;             // seconds, monotonic
    double last_shot = -100.0;     // time of the last normal shot
    double last_pierce = -100.0;   // time of the last charged shot
    bool show_viewmodel = true;
    float punch_stamina = 2.0f;    // 0..2; a punch needs 1
    int variation = 0;             // revolver variation: 0 Piercer, 1 Marksman
    float coin_charge = 400.0f;    // 0..400, four coins of 100
    // coins in flight, already projected: x, y in -1..1 from the screen centre (y up), size as a fraction
    // of the screen height, phase of the spin in radians
    enum { MAX_COINS = 8 };
    int coin_count = 0;
    struct CoinDot { float x, y, size, phase; bool flash; } coins[MAX_COINS];
};

// Loads the pack and creates GPU resources. Safe to call repeatedly; returns false if it cannot draw.
bool hud_init(ID3D11Device *device, const wchar_t *pack_path);

// Draws onto `target` (the back buffer). Saves and restores every piece of pipeline state it touches.
void hud_draw(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *target, const HudState &state);

// Why the last hud_init failed, for the log.
const char *hud_error();
