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
    // Animated models, from ultrasouls_models.bin beside the asset pack. A named clip plays from its
    // start time; once it has run out the revolver goes back to its idle loop and the arm is put away.
    const char *revolver_clip = nullptr;   // the clip of whichever weapon is held
    double revolver_clip_start = -100.0;
    int weapon = 0;                        // 0 revolver, 1 shotgun, 2 railcannon
    bool alt = false;                      // the revolver is the alternate ("Slab") one
    int weapon_var = 0;                    // the shotgun's variation (0 Core Eject, 1 Pump Charge) or the railcannon's (0 Electric, 1 Malicious)
    float rail_charge = 5.0f;              // 0..5: the railcannon's charge, which lights its pips one by one
    bool meter_rgb_set = false;            // the shotgun's meter in this colour instead of the Core Eject's (the Pump Charge's)
    float meter_rgb[3] = {1, 1, 1};
    float twirl = 0.0f;                    // the Sharpshooter's spin: radians about the revolver's twirl bone
    float twirl_blend = 0.0f;              // 0..1: how far the hand has gone from the idle pose to the Twirl clip's
    float twirl_charge = 0.0f;             // 0..100 while the Sharpshooter's alt fire is held
    float sharp_charge = 300.0f;           // 0..300, three charged shots of 100
    float core_charge = 0.0f;              // 0..1 while the shotgun's core is being wound up
    double muzzle_flash = -100.0;          // time of the last muzzle flash
    // How far ULTRAKILL's movement code pushes the HUD panels and the weapons away from the player's
    // velocity, in HUD camera units (x right, y up, z forward)
    float hud_sway[3] = {0, 0, 0}, weapon_sway[3] = {0, 0, 0};
    float core_meter = 1.0f;               // the shotgun's own meter: 0..1 filled
    float core_meter_red = 0.0f;           // 0..1: how far its colour has gone from the variation's to red
    bool flash_only = false;               // draw nothing but the white flash (the parry's frozen frame)
    // boss health bars across the top: name, health 0..1
    enum { MAX_BOSSES = 2, BOSS_NAME_CHARS = 48 };
    int boss_count = 0;
    struct Boss { char name[BOSS_NAME_CHARS]; float hp; } bosses[MAX_BOSSES];
    const char *arm_clip = nullptr;
    double arm_clip_start = -100.0;
    const char *arm2_clip = nullptr;       // the Knuckleblaster
    double arm2_clip_start = -100.0;
    float flash = 0.0f;                    // 0..1: a white flash over the whole screen (the parry's)
    float punch_stamina = 2.0f;    // 0..2; a punch needs 1
    int arm = 0;                   // the arm last used, for the fist icon: 0 Feedbacker, 1 Knuckleblaster
    // Style meter. Rank -1 hides it; 0..7 are D C B A S SS SSS ULTRAKILL. Lines are the bonus list, oldest first.
    int style_rank = -1;
    float style_meter = 0.0f;      // 0..1 of the current rank's meter
    enum { STYLE_LINES = 6, STYLE_LINE_CHARS = 40 };
    int style_line_count = 0;
    struct StyleLine { char text[STYLE_LINE_CHARS]; float r, g, b; } style_lines[STYLE_LINES];
    // the weapon's walking bob: an offset of the viewmodel in its own space (x right, y up)
    float bob_x = 0.0f, bob_y = 0.0f;
    // shot tracers, already projected: end points in -1..1 from the screen centre (y up), half-widths as
    // fractions of the screen height, colour with alpha
    enum { MAX_TRACERS = 160 };
    int tracer_count = 0;
    struct TracerLine { float x0, y0, x1, y1, w0, w1, r, g, b, a; bool plain; } tracers[MAX_TRACERS];   // plain: the line alone, no soft glow round it
    int variation = 0;             // revolver variation: 0 Piercer, 1 Marksman, 2 Sharpshooter
    float coin_charge = 400.0f;    // 0..400, four coins of 100
    // coins in flight, already projected: x, y in -1..1 from the screen centre (y up), size as a fraction
    // of the screen height, phase of the spin in radians
    enum { MAX_COINS = 8 };
    int coin_count = 0;
    struct CoinDot { float x, y, size, phase; bool flash; } coins[MAX_COINS];
    // explosions and glowing projectiles, already projected: centre in -1..1, the fireball's diameter and
    // the shock ring's (0 for none) as fractions of the screen height, colour with alpha
    enum { MAX_BLASTS = 16 };
    int blast_count = 0;
    struct Blast { float x, y, size, ring, r, g, b, a; } blasts[MAX_BLASTS];
    // ULTRAKILL's own effect meshes, placed in the world: the game camera (eye, its three axes, the
    // tangents of half the field of view across and up), then each mesh by the name of its model in the
    // pack, where it is, its radius, a turn about the camera's right axis (the coin's flip), how far its
    // texture has scrolled, and its colour with alpha.
    bool cam_valid = false;
    float cam_eye[3] = {0, 0, 0}, cam_right[3] = {1, 0, 0}, cam_up[3] = {0, 1, 0}, cam_fwd[3] = {0, 0, 1}, cam_tan_x = 1, cam_tan_y = 1;
    enum { MAX_WORLD_MESHES = 32 };
    int world_mesh_count = 0;
    struct WorldMesh { const char *model; float p[3], radius, flip, uv[2], rgba[4]; } world_meshes[MAX_WORLD_MESHES];
    // effect sprites, already projected like the blasts: centre in -1..1, size as a fraction of the
    // screen height, a turn in degrees, colour with alpha. A null sprite is a plain square.
    enum { MAX_FX_SPRITES = 360 };
    int fx_sprite_count = 0;
    struct FxSprite { const char *sprite; float x, y, size, rot, r, g, b, a; } fx_sprites[MAX_FX_SPRITES];
};

// Whether the model pack has a model of this name (the effect meshes are optional).
bool hud_has_model(const char *name);

// Where the held weapon's muzzle was drawn last frame, in -1..1 from the screen centre (y up).
bool hud_muzzle(float *x, float *y);

// Loads the pack and creates GPU resources. Safe to call repeatedly; returns false if it cannot draw.
bool hud_init(ID3D11Device *device, const wchar_t *pack_path);

// Draws onto `target` (the back buffer). Saves and restores every piece of pipeline state it touches.
void hud_draw(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *target, const HudState &state);

// The parry's freeze: keep a copy of what is on `target` now, and put it back later.
bool hud_keep_frame(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *target);
void hud_put_frame(ID3D11DeviceContext *ctx, ID3D11Texture2D *target);

// Why the last hud_init failed, for the log.
const char *hud_error();
