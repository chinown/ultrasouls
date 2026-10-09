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
    float revolver_clip_speed = 1.0f;      // how fast its Animator state plays the clip (1.5 for the shotgun's core reload, ...)
    float cylinder = 0.0f;                 // how far the revolver's cylinder has turned, in radians about its own axis
    // The sawblade launcher: how far its clips are held back towards the rest pose (0..1, the weight of its
    // Animator's second layer), how far the blade in its jaws has turned (radians), and how hot the
    // Overheat's blade glows (0..1).
    float clip_muddle = 0.0f, blade_spin = 0.0f, blade_heat = 0.0f;
    // What the display on the launcher shows: the Attractor's saws and magnets (0..3), the Overheat's heat sink (0..1).
    int saw_count = 10;
    float magnet_charge = 3.0f, heat_sink = 1.0f;
    int weapon = 0;                        // 0 revolver, 1 shotgun, 2 railcannon, 3 sawblade launcher
    char weapon_note[40] = "";             // a line written under the weapon's picture (the launcher's saws, magnets and heat)
    bool alt = false;                      // the revolver is the alternate ("Slab") one
    int weapon_var = 0;                    // the shotgun's variation (0 Core Eject, 1 Pump Charge), the railcannon's (0 Electric, 1 Malicious), the launcher's (0 Attractor, 1 Overheat) or the rocket launcher's (0 Freezeframe, 1 S.R.S. Cannon)
    // the rocket launcher (weapon 4): the clock on it (how much of its ring is filled, where its hand points,
    // its colour and how strongly it is drawn) and how far the cannon is wound up, which shakes the gun
    float clock_fill = 1.0f, clock_alpha = 1.0f, clock_rgb[3] = {0.251f, 0.906f, 1.0f}, srs_charge = 0.0f;
    // Dark Souls' title screen: nothing of the HUD is drawn; the picture is looked at and, if it is the title
    // screen, dressed as ULTRAKILL's main menu. title_note: a line written under the logo.
    bool title = false;
    char title_note[64] = "";
    float rail_charge = 5.0f;              // 0..5: the railcannon's charge, which lights its pips one by one
    bool meter_rgb_set = false;            // the shotgun's meter in this colour instead of the Core Eject's (the Pump Charge's)
    float meter_rgb[3] = {1, 1, 1};
    bool rail_meter = false;               // show the railcannon's charge meter beside the weapon panel
    float rail_flash = 0.0f;               // 0..1: how far that meter's colour is towards white (it flashes as the charge completes)
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
    // ULTRAKILL's death sequence ('DeathSequence'): seconds since the player died, negative while alive. For
    // its first two seconds there is no HUD and no weapon, and the sequence's lines appear in the corner, one
    // every 0.05 s; after them the black screen with its own two lines. death_prompt: whether the second of
    // those, the key to restart with, is shown. The words come from ultrasouls_text.txt beside the packs.
    float death_time = -1.0f;
    bool death_prompt = true;
    // boss health bars across the top: name, health 0..1
    enum { MAX_BOSSES = 2, BOSS_NAME_CHARS = 48 };
    int boss_count = 0;
    struct Boss { char name[BOSS_NAME_CHARS]; float hp; } bosses[MAX_BOSSES];
    float red_tint = 0;                    // 0..1: the whole picture washed in red (the visceral attack)
    float arm_roll = 0;                    // and turned by this much about the line of sight, at the middle of the view
    float arm_push[3] = {0, 0, 0};         // the Feedbacker moved from its place by this much (right, up, forward): the visceral attack's thrust
    const char *arm_clip = nullptr;
    double arm_clip_start = -100.0;
    const char *arm2_clip = nullptr;       // the Knuckleblaster
    double arm2_clip_start = -100.0;
    float arm2_clip_speed = 1.0f;
    // The whiplash arm: its clip (Throw, Pull, Catch), whether the clip's last frame is held once it has
    // run out (the arm stays out for as long as the hook is), and whether the hook is away from the hand
    // (then the model's own hook is left out: the DLL draws the cable and the hook in the world).
    const char *whip_clip = nullptr;
    double whip_clip_start = -100.0;
    bool whip_hold = false, whip_out = false;
    float flash = 0.0f;                    // 0..1: a white flash over the whole screen (the parry's)
    // Blood on the screen after a heal ('ScreenBlood'): where its middle is from the middle of the HUD canvas,
    // in the canvas's units, which of the five pictures it is, and its alpha now.
    enum { MAX_SCREEN_BLOOD = 12 };
    int screen_blood_count = 0;
    struct ScreenBloodMark { float x, y; int sprite; float alpha; float scale = 1.0f; } screen_blood[MAX_SCREEN_BLOOD];   // scale: of its usual size (half the canvas's height)
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
    // plain: the line alone, no soft glow round it. grad: the colour runs from r, g, b at the first end to
    // r1, g1, b1 at the other. sprite: drawn with that picture, u0 to u1 of it along the line.
    enum { MAX_TRACERS = 1100 };
    int tracer_count = 0;
    struct TracerLine {
        float x0, y0, x1, y1, w0, w1, r, g, b, a;
        bool plain, grad;
        float r1, g1, b1, a1;              // (a1: the far end's alpha, when grad is set)
        const char *sprite;
        float u0, u1;
        float z0, z1;                      // how far each end is from the eye along the view, in metres (0: not known), for the world to hide it
    } tracers[MAX_TRACERS];
    // Trails (Unity's TrailRenderer): runs of points, already projected like a tracer's ends (centre in -1..1,
    // half-width as a fraction of the screen's height, distance along the view), each run drawn as one strip
    // whose pieces share their corners, so a bend has no gaps and nothing is drawn twice. additive: the
    // colour is added to the picture (the 'Additive' particle material the saws' trails have) instead of
    // laid over it. Filled in with hud_trail_begin and hud_trail_point.
    enum { MAX_RIBBON_POINTS = 2400, MAX_RIBBONS = 96 };
    int ribbon_point_count = 0, ribbon_count = 0;
    struct RibbonPoint { float x, y, w, r, g, b, a, z; } ribbon_points[MAX_RIBBON_POINTS];
    struct Ribbon { int first, count; bool additive; } ribbons[MAX_RIBBONS];
    bool trail_open = false, trail_has_prev = false, trail_additive = false;
    float trail_prev[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // the last point given: where the camera sees it (3), width, colour, alpha
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
    struct Blast { float x, y, size, ring, r, g, b, a, z; } blasts[MAX_BLASTS];        // z: distance along the view, as for tracers
    // ULTRAKILL's own effect meshes, placed in the world: the game camera (eye, its three axes, the
    // tangents of half the field of view across and up), then each mesh by the name of its model in the
    // pack, where it is, its radius, a turn about the camera's right axis (the coin's flip), how far its
    // texture has scrolled, and its colour with alpha.
    bool cam_valid = false;
    float cam_eye[3] = {0, 0, 0}, cam_right[3] = {1, 0, 0}, cam_up[3] = {0, 1, 0}, cam_fwd[3] = {0, 0, 1}, cam_tan_x = 1, cam_tan_y = 1;
    enum { MAX_WORLD_MESHES = 96 };
    int world_mesh_count = 0;
    // limits: for an explosion's ball, how far each vertex of the mesh may go from the middle before it
    // meets the game's world (one distance per vertex, in the order hud_model_dirs gives them); the ball
    // is then drawn pressed against walls and floors instead of through them, and is not turned with the camera.
    // oriented: the model's own x, y and z axes are put along ax, ay and az (unit vectors of the world) and
    // `radius` is the size of one of its units, instead of the turn with the camera. spin: its second
    // node is turned by this much about that node's own z (a sawblade's teeth). cutout: the picture's
    // transparent parts are left out.
    struct WorldMesh {
        const char *model;
        float p[3], radius, flip, uv[2], rgba[4];
        const float *limits;
        int limit_count;
        bool oriented = false;
        float ax[3] = {1, 0, 0}, ay[3] = {0, 1, 0}, az[3] = {0, 0, 1}, spin = 0;
        bool cutout = false;
    } world_meshes[MAX_WORLD_MESHES];
    // effect sprites, already projected like the blasts: centre in -1..1, size as a fraction of the
    // screen height, a turn in degrees, colour with alpha. A null sprite is a plain square.
    enum { MAX_FX_SPRITES = 360 };
    int fx_sprite_count = 0;
    struct FxSprite { const char *sprite; float x, y, size, rot, r, g, b, a, z; } fx_sprites[MAX_FX_SPRITES];   // z: distance along the view
};

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
enum { BEAM_NONE = 0, BEAM_REVOLVER, BEAM_SLAB, BEAM_SUPER, BEAM_SHARP, BEAM_RAIL, BEAM_MALICIOUS, BEAM_KINDS };
struct BeamLook { float width_u, c0[3], c1[3]; };
extern const BeamLook BEAM_LOOKS[BEAM_KINDS];

// A strip between two points of the world, added to `st` as its camera (cam_*) sees it: projected, cut
// at the eye. c0, c1: its colour at each end. sprite: the picture it is drawn with, u0 to u1 of it along
// the strip. wmax: the most half its width may come to, as a share of the screen's height.
void hud_strip(HudState &st, const float *pa, const float *pb, float width, const float *c0, const float *c1, float alpha, const char *sprite, float u0,
               float u1, float wmax);
// A trail through points of the world, added to `st` as its camera sees it: begin one, then give its points
// in order, each with the trail's width (metres), colour and alpha there. A stretch behind the eye is cut out.
void hud_trail_begin(HudState &st, bool additive);
void hud_trail_point(HudState &st, const float *p, float width, const float *rgb, float alpha);
// One beam of a BEAM_ kind from a to b, `width` metres wide now, with whatever lines go round it. `unit` is
// how many metres an ULTRAKILL unit is; `seen` has a bit for each eighth of the line that may be drawn;
// `seed` tells beams apart for what is picked at random.
void hud_beam(HudState &st, const float *a, const float *b, int kind, float width, float unit, unsigned seen, int seed);

// The game's own depth for the picture the HUD is about to be drawn over, so that what belongs to the world
// (tracers, particles, effect meshes) is hidden where something of the game stands nearer. `srv` gives the
// stored depth of each pixel of the target; the distance along the view is b / (stored - a). Null turns
// it off. The view is kept (with a reference) until the next call.
void hud_set_depth(ID3D11ShaderResourceView *srv, float a, float b);

// How many lines the death sequence has (0 if the text file is not there), and whether line `i` is one of
// the orange ones (a warning; the rest are errors): the DLL plays a sound as each appears.
int hud_death_lines();
bool hud_death_line_warning(int i);

// Whether the model pack has a model of this name (the effect meshes are optional).
bool hud_has_model(const char *name);

// The direction from a model's middle to each vertex of its first mesh (unit length, x y z each), for
// working out a WorldMesh's limits. Returns how many were written; 0 if the model is not there.
int hud_model_dirs(const char *name, float *xyz, int max_verts);

// Where the held weapon's muzzle was drawn last frame, in -1..1 from the screen centre (y up).
bool hud_muzzle(float *x, float *y);

// Where the whiplash arm's cable leaves the hand ("Wire Start"), as drawn last frame, in the same terms.
bool hud_whip_hand(float *x, float *y);

// Loads the pack and creates GPU resources. Safe to call repeatedly; returns false if it cannot draw.
bool hud_init(ID3D11Device *device, const wchar_t *pack_path);

// Draws onto `target` (the back buffer). Saves and restores every piece of pipeline state it touches.
void hud_draw(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *target, const HudState &state);

// The title menu drawn as ULTRAKILL's column of buttons (HudState.title). Dark Souls still runs the menu and
// goes by where the pointer is, so while the column is up the pointer's place is translated for it: on button
// i it is given as on Dark Souls' row i, on one of the two arrow marks as on Dark Souls' arrow, and over the
// rows that are blacked out as beside them. x, y: the pointer in the window (pixels, y down), w, h: the
// window's size. Returns false, changing nothing, when the column was not on the last picture or the pointer
// is nowhere that matters.
bool hud_title_cursor(float w, float h, float *x, float *y);
// Whether the title menu is drawn that way at all (the ini's title_column; off, the rows are dressed where they are).
void hud_title_column(bool on);
// The column as last drawn, for the log: its buttons' words, the chosen one in brackets ("" with no column).
const char *hud_title_state();

// The parry's freeze: keep a copy of what is on `target` now, and put it back later.
bool hud_keep_frame(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *target);
void hud_put_frame(ID3D11DeviceContext *ctx, ID3D11Texture2D *target);

// Why the last hud_init failed, for the log.
const char *hud_error();
