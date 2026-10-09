// ULTRAKILL-style HUD and revolver viewmodel. See hud.h.
//
// The layout is not eyeballed: every rectangle, colour and font size below is the value stored in
// ULTRAKILL's own HUD objects (dumped by tools/uk_hud_dump.py), resolved with the same RectTransform
// rules Unity uses. The main panel is a tilted plane in front of a 90-degree camera, as in the game.
#include "hud.h"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- pack file

struct Tex {
    std::string name;
    int w = 0, h = 0;
    ID3D11ShaderResourceView *srv = nullptr;
};
struct Sprite {
    std::string name;
    int tex = -1;
    float border[4] = {};   // left, bottom, right, top, in pixels
    float ppu = 100.0f;
};
struct Glyph {
    uint32_t code;
    float x, y, w, h, bx, by, adv;   // atlas rect (y from the top), bearings and advance at the font's point size
};
struct Font {
    int tex = -1;
    float point = 1, line = 1, ascent = 0, descent = 0, baseline = 0, padding = 0, aw = 1, ah = 1;
    std::vector<Glyph> glyphs;
    const Glyph *find(uint32_t code) const {
        for (const Glyph &g : glyphs)
            if (g.code == code) return &g;
        return nullptr;
    }
};
struct Mesh {
    std::string name;
    int tex = -1;
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    UINT index_count = 0;
};

// An animated model: a tree of transforms, meshes bound to some of them, and clips that hold every
// transform's local position, rotation and scale for each sampled frame.
struct SkinMesh {
    std::string name;
    ID3D11ShaderResourceView *srv = nullptr;
    ID3D11ShaderResourceView *emis = nullptr;      // the material's emissive picture (the gun's lights), if it has one
    UINT vertex_count = 0, index_count = 0;
    std::vector<float> verts;        // rest position 3, normal 3, uv 2
    std::vector<uint16_t> slots;     // 4 bone slots per vertex
    std::vector<float> weights;      // 4 per vertex
    std::vector<int> bone_node;      // slot -> node
    std::vector<float> bind;         // slot -> 3x4 bind matrix
    std::vector<float> skinned;      // what is uploaded each frame
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
};
struct Clip {
    std::string name;
    float fps = 60.0f;
    UINT frames = 0;
    std::vector<float> data;         // frames x nodes x 10
};
// A flat panel fixed to one node: a display on a weapon. Corners are bottom-left, top-left, top-right,
// bottom-right, each position 3 and uv 2, in the node's own space. `fill` is the edge a meter's filled
// part starts from: 0 none, 1 left, 2 right, 3 bottom, 4 top.
struct Screen {
    std::string name;
    int node = 0, tex = -1, fill = 0;
    float rgba[4] = {1, 1, 1, 1};
    float corner[4][5] = {};
};
struct Model {
    std::string name;
    std::vector<std::string> tex_names;
    std::vector<ID3D11ShaderResourceView *> tex_srvs;
    std::vector<Screen> screens;
    int texture(const char *n) const {
        for (size_t i = 0; i < tex_names.size(); i++)
            if (tex_names[i] == n) return (int)i;
        return -1;
    }
    int nodes = 0;
    std::vector<std::string> node_names;
    std::vector<int> parent;
    int node(const char *n) const {
        for (size_t i = 0; i < node_names.size(); i++)
            if (node_names[i] == n) return (int)i;
        return -1;
    }
    std::vector<float> rest;         // nodes x 10
    std::vector<SkinMesh> meshes;
    std::vector<Clip> clips;
    const Clip *clip(const char *n) const {
        if (!n) return nullptr;
        for (const Clip &c : clips)
            if (c.name == n) return &c;
        return nullptr;
    }
};

struct UiVert {
    float pos[4];
    float uv[2];
    float col[4];
    float inv_z;       // 1 / (distance along the view in metres) for something in the game's world; 0 for the HUD itself
};
struct UiDraw {
    int tex;      // -1: plain colour
    int sdf;      // 1: distance-field text
    UINT start, count;
    int add = 0;  // 1: added to the picture instead of laid over it
    int world = 0;  // 1: something of the game's world (a trail, a beam, a spark), which goes under the HUD's own panels
    int fx = 0;     // 1: the whole picture, redrawn through the death shader
};

// Dark Souls' title menu as read off the picture (title_probe): the chosen one of its four rows (-1: none),
// which entry each row shows (an index into TITLE_WORDS; -1 words that were not recognised, -2 an empty row),
// and whether its arrows are up, which say that the list goes on above or below.
struct TitleMenu {
    int row = -1;
    int items[4] = {-2, -2, -2, -2};
    bool up = false, down = false;
};

struct Gfx {
    bool ready = false;
    std::string error;
    // the death sequence's words (ultrasouls_text.txt): its lines, which of them are orange, the screen after
    std::vector<std::string> death_lines, dead_lines;
    std::vector<char> death_orange;
    std::vector<Tex> textures;
    std::vector<Sprite> sprites;
    std::vector<Font> fonts;
    std::vector<Mesh> meshes;
    std::vector<Model> models;
    ID3D11ShaderResourceView *white = nullptr;
    ID3D11ShaderResourceView *scene_depth = nullptr;    // the game's depth for this frame (hud_set_depth), and how to read it
    float depth_a = 0, depth_b = 0;

    ID3D11VertexShader *ui_vs = nullptr, *mesh_vs = nullptr;
    ID3D11PixelShader *ui_ps = nullptr, *mesh_ps = nullptr, *death_ps = nullptr, *menu_ps = nullptr;
    ID3D11Texture2D *probe_tex = nullptr;    // the picture copied where it can be read, to tell the title screen by
    TitleMenu title_menu;                    // the title menu as it is drawn: what was last read, or what is held over a gap
    bool title_col = false;                  // it is drawn as ULTRAKILL's column of buttons (every row's words were read)
    double title_good = -100.0;              // when every row's words were last read
    ID3D11Texture2D *fx_tex = nullptr;       // the picture as it stood, for the death shader to read
    ID3D11ShaderResourceView *fx_srv = nullptr;
    ID3D11InputLayout *ui_il = nullptr, *mesh_il = nullptr;
    ID3D11Buffer *ui_cb = nullptr, *mesh_cb = nullptr, *ui_vb = nullptr, *screen_vb = nullptr;
    ID3D11Texture2D *kept = nullptr;         // the frame kept aside during a parry's freeze
    UINT ui_vb_cap = 0;
    ID3D11BlendState *blend = nullptr, *opaque = nullptr, *blend_add = nullptr;
    ID3D11RasterizerState *raster = nullptr, *raster_cull = nullptr;
    ID3D11DepthStencilState *depth_off = nullptr, *depth_on = nullptr, *depth_read = nullptr;
    ID3D11SamplerState *linear = nullptr, *point = nullptr, *wrap = nullptr;
    ID3D11Texture2D *depth_tex = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    UINT depth_w = 0, depth_h = 0, depth_samples = 0;

    std::vector<UiVert> verts;
    std::vector<UiDraw> draws;

    int sprite(const char *name) const {
        for (size_t i = 0; i < sprites.size(); i++)
            if (sprites[i].name == name) return (int)i;
        return -1;
    }
};
Gfx g;

struct Reader {
    const uint8_t *p, *end;
    bool ok = true;
    template <typename T> T get() {
        T v{};
        if (p + sizeof(T) > end) { ok = false; return v; }
        memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    const uint8_t *bytes(size_t n) {
        if (p + n > end) { ok = false; return nullptr; }
        const uint8_t *r = p;
        p += n;
        return r;
    }
    std::string name() {
        const uint8_t *b = bytes(40);
        return b ? std::string((const char *)b, strnlen((const char *)b, 40)) : std::string();
    }
};

bool load_pack(ID3D11Device *dev, const wchar_t *path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { g.error = "asset pack not found"; return false; }
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    std::vector<uint8_t> data((size_t)size.QuadPart);
    DWORD got = 0;
    BOOL read = ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr);
    CloseHandle(f);
    if (!read || got != data.size() || data.size() < 12 || memcmp(data.data(), "USPACK01", 8) != 0) { g.error = "asset pack unreadable"; return false; }

    Reader r{data.data() + 8, data.data() + data.size()};
    uint32_t n = r.get<uint32_t>();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        Tex t;
        t.name = r.name();
        t.w = (int)r.get<uint32_t>();
        t.h = (int)r.get<uint32_t>();
        const uint8_t *px = r.bytes((size_t)t.w * t.h * 4);
        if (!px) break;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = t.w;
        d.Height = t.h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{px, (UINT)t.w * 4, 0};
        ID3D11Texture2D *tex = nullptr;
        if (FAILED(dev->CreateTexture2D(&d, &sd, &tex))) { g.error = "texture creation failed"; return false; }
        dev->CreateShaderResourceView(tex, nullptr, &t.srv);
        tex->Release();
        g.textures.push_back(t);
    }
    n = r.get<uint32_t>();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        Sprite s;
        s.name = r.name();
        s.tex = (int)r.get<uint32_t>();
        for (float &b : s.border) b = r.get<float>();
        s.ppu = r.get<float>();
        g.sprites.push_back(s);
    }
    {
        // A round glow made here: ULTRAKILL's "glow" sprite is still an eighth opaque at the middle of
        // its edges (it is meant for additive particles), which showed as a faint square around every
        // fireball. This one falls to nothing at the rim.
        enum { GLOW = 32 };
        static uint8_t px[GLOW * GLOW * 4];
        for (int y = 0; y < GLOW; y++)
            for (int x = 0; x < GLOW; x++) {
                float dx = (x + 0.5f) / GLOW * 2 - 1, dy = (y + 0.5f) / GLOW * 2 - 1, rr = sqrtf(dx * dx + dy * dy);
                float a = rr >= 1 ? 0 : powf(1 - rr, 1.5f) * 1.6f;
                uint8_t *o = px + (y * GLOW + x) * 4;
                o[0] = o[1] = o[2] = 255;
                o[3] = (uint8_t)(fminf(a, 1.0f) * 255.0f);
            }
        Tex t;
        t.name = "softglow";
        t.w = t.h = GLOW;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = GLOW;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{px, GLOW * 4, 0};
        ID3D11Texture2D *tex = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&d, &sd, &tex))) {
            dev->CreateShaderResourceView(tex, nullptr, &t.srv);
            tex->Release();
            g.textures.push_back(t);
            Sprite sp;
            sp.name = "softglow";
            sp.tex = (int)g.textures.size() - 1;
            g.sprites.push_back(sp);
        }
    }
    n = r.get<uint32_t>();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        Font ft;
        r.name();
        ft.tex = (int)r.get<uint32_t>();
        ft.point = r.get<float>();
        ft.line = r.get<float>();
        ft.ascent = r.get<float>();
        ft.descent = r.get<float>();
        ft.baseline = r.get<float>();
        ft.padding = r.get<float>();
        ft.aw = r.get<float>();
        ft.ah = r.get<float>();
        uint32_t ng = r.get<uint32_t>();
        for (uint32_t k = 0; k < ng && r.ok; k++) {
            Glyph gl;
            gl.code = r.get<uint32_t>();
            gl.x = r.get<float>();
            gl.y = r.get<float>();
            gl.w = r.get<float>();
            gl.h = r.get<float>();
            gl.bx = r.get<float>();
            gl.by = r.get<float>();
            gl.adv = r.get<float>();
            ft.glyphs.push_back(gl);
        }
        g.fonts.push_back(ft);
    }
    n = r.get<uint32_t>();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        Mesh m;
        m.name = r.name();
        m.tex = (int)r.get<uint32_t>();
        uint32_t nv = r.get<uint32_t>(), ni = r.get<uint32_t>();
        const uint8_t *vb = r.bytes((size_t)nv * 32), *ib = r.bytes((size_t)ni * 4);
        if (!vb || !ib) break;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = nv * 32;
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{vb, 0, 0};
        dev->CreateBuffer(&bd, &sd, &m.vb);
        bd.ByteWidth = ni * 4;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = ib;
        dev->CreateBuffer(&bd, &sd, &m.ib);
        m.index_count = ni;
        g.meshes.push_back(m);
    }
    if (!r.ok) { g.error = "asset pack truncated"; return false; }
    return true;
}

// The animated models' own pack ("USMDL001"; layout in tools/uk_models.py). Optional: without it the
// revolver is drawn from the asset pack in its fixed pose.
// What ULTRAKILL writes when the player dies, from the text file tools/uk_models.py makes: a section
// [death] of lines that appear one by one (TextMeshPro's <color=orange> marks the warnings) and a section
// [dead] for the screen after them.
void load_text(const wchar_t *path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    std::string data((size_t)(size.QuadPart < 65536 ? size.QuadPart : 65536), '\0');
    DWORD got = 0;
    BOOL read = ReadFile(f, &data[0], (DWORD)data.size(), &got, nullptr);
    CloseHandle(f);
    if (!read) return;
    data.resize(got);
    int section = 0;
    size_t at = 0;
    while (at <= data.size()) {
        size_t end = data.find('\n', at);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[death]") { section = 1; continue; }
        if (line == "[dead]") { section = 2; continue; }
        if (section == 1) {
            const std::string open = "<color=orange>", close = "</color>";
            bool orange = line.compare(0, open.size(), open) == 0;
            for (const std::string &tag : {open, close})
                for (size_t k; (k = line.find(tag)) != std::string::npos;) line.erase(k, tag.size());
            g.death_lines.push_back(line);
            g.death_orange.push_back(orange ? 1 : 0);
        } else if (section == 2) {
            g.dead_lines.push_back(line);
        }
    }
    while (!g.death_lines.empty() && g.death_lines.back().empty()) { g.death_lines.pop_back(); g.death_orange.pop_back(); }
    while (!g.dead_lines.empty() && g.dead_lines.back().empty()) g.dead_lines.pop_back();
}

bool load_models(ID3D11Device *dev, const wchar_t *path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    std::vector<uint8_t> data((size_t)size.QuadPart);
    DWORD got = 0;
    BOOL read = ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr);
    CloseHandle(f);
    bool v1 = data.size() >= 12 && memcmp(data.data(), "USMDL001", 8) == 0, v2 = data.size() >= 12 && memcmp(data.data(), "USMDL002", 8) == 0;
    if (!read || got != data.size() || (!v1 && !v2)) return false;
    Reader r{data.data() + 8, data.data() + data.size()};
    uint32_t model_count = r.get<uint32_t>();
    for (uint32_t mi = 0; mi < model_count && r.ok; mi++) {
        Model m;
        m.name = r.name();
        m.nodes = (int)r.get<uint32_t>();
        if (m.nodes <= 0 || m.nodes > 512) return false;
        for (int i = 0; i < m.nodes && r.ok; i++) {
            m.node_names.push_back(r.name());
            m.parent.push_back(r.get<int32_t>());
            for (int k = 0; k < 10; k++) m.rest.push_back(r.get<float>());
        }
        std::vector<ID3D11ShaderResourceView *> srvs;
        uint32_t tex_count = r.get<uint32_t>();
        for (uint32_t i = 0; i < tex_count && r.ok; i++) {
            m.tex_names.push_back(r.name());
            uint32_t w = r.get<uint32_t>(), h = r.get<uint32_t>();
            const uint8_t *px = r.bytes((size_t)w * h * 4);
            if (!px) return false;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w;
            d.Height = h;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_IMMUTABLE;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sd{px, w * 4, 0};
            ID3D11Texture2D *tex = nullptr;
            ID3D11ShaderResourceView *srv = nullptr;
            if (SUCCEEDED(dev->CreateTexture2D(&d, &sd, &tex))) {
                dev->CreateShaderResourceView(tex, nullptr, &srv);
                tex->Release();
            }
            srvs.push_back(srv);
        }
        uint32_t mesh_count = r.get<uint32_t>();
        for (uint32_t i = 0; i < mesh_count && r.ok; i++) {
            SkinMesh sm;
            sm.name = r.name();
            int tex = r.get<int32_t>();
            sm.srv = tex >= 0 && tex < (int)srvs.size() ? srvs[tex] : nullptr;
            if (tex >= 0 && tex < (int)m.tex_names.size()) {
                int e = m.texture((m.tex_names[tex] + "__emissive").c_str());
                if (e >= 0 && e < (int)srvs.size()) sm.emis = srvs[e];
            }
            sm.vertex_count = r.get<uint32_t>();
            sm.index_count = r.get<uint32_t>();
            uint32_t bones = r.get<uint32_t>();
            const uint8_t *v = r.bytes((size_t)sm.vertex_count * 32), *s = r.bytes((size_t)sm.vertex_count * 8);
            const uint8_t *w = r.bytes((size_t)sm.vertex_count * 16), *ib = r.bytes((size_t)sm.index_count * 4);
            if (!v || !s || !w || !ib) return false;
            sm.verts.resize((size_t)sm.vertex_count * 8);
            memcpy(sm.verts.data(), v, sm.verts.size() * 4);
            sm.slots.resize((size_t)sm.vertex_count * 4);
            memcpy(sm.slots.data(), s, sm.slots.size() * 2);
            sm.weights.resize((size_t)sm.vertex_count * 4);
            memcpy(sm.weights.data(), w, sm.weights.size() * 4);
            for (uint32_t b = 0; b < bones && r.ok; b++) {
                int node = r.get<int32_t>();
                sm.bone_node.push_back(node >= 0 && node < m.nodes ? node : 0);
                for (int k = 0; k < 12; k++) sm.bind.push_back(r.get<float>());
            }
            for (uint16_t &slot : sm.slots)
                if (slot >= bones) slot = 0;
            sm.skinned = sm.verts;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sm.vertex_count * 32;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            dev->CreateBuffer(&bd, nullptr, &sm.vb);
            D3D11_BUFFER_DESC id{};
            id.ByteWidth = sm.index_count * 4;
            id.Usage = D3D11_USAGE_IMMUTABLE;
            id.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA isd{ib, 0, 0};
            dev->CreateBuffer(&id, &isd, &sm.ib);
            m.meshes.push_back(std::move(sm));
        }
        uint32_t clip_count = r.get<uint32_t>();
        for (uint32_t i = 0; i < clip_count && r.ok; i++) {
            Clip c;
            c.name = r.name();
            c.fps = r.get<float>();
            c.frames = r.get<uint32_t>();
            size_t n = (size_t)c.frames * m.nodes * 10;
            const uint8_t *d = r.bytes(n * 4);
            if (!d) return false;
            c.data.resize(n);
            memcpy(c.data.data(), d, n * 4);
            m.clips.push_back(std::move(c));
        }
        m.tex_srvs = srvs;
        uint32_t screen_count = v2 ? r.get<uint32_t>() : 0;
        for (uint32_t i = 0; i < screen_count && r.ok; i++) {
            Screen sc;
            sc.name = r.name();
            sc.node = r.get<int32_t>();
            sc.tex = r.get<int32_t>();
            sc.fill = r.get<int32_t>();
            for (float &v : sc.rgba) v = r.get<float>();
            for (auto &corner : sc.corner)
                for (float &v : corner) v = r.get<float>();
            if (sc.node < 0 || sc.node >= m.nodes) sc.node = 0;
            m.screens.push_back(std::move(sc));
        }
        if (r.ok) g.models.push_back(std::move(m));
    }
    return r.ok && !g.models.empty();
}

// ---------------------------------------------------------------- shaders

const char UI_SHADER[] = R"(
cbuffer C : register(b0) { float4 mode; float4 dpar; }      // mode x: 1 = distance-field text, y: 1 = target stores sRGB. dpar: a, b, on, -
Texture2D tex : register(t0);
Texture2D<float> dtex : register(t2);
SamplerState smp : register(s0);
struct VI { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float invz : TEXCOORD1; };
struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float invz : TEXCOORD1; };
// What belongs to the game's world (invz > 0) is put at the very back of the weapon's depth buffer, and the
// HUD itself at the very front: with that buffer bound, the weapon and the arms cover the first and not the second.
VO vs(VI i) { VO o; o.pos = i.pos; o.pos.z = i.invz > 0 ? 0.99995 * i.pos.w : 0.0; o.uv = i.uv; o.col = i.col; o.invz = i.invz; return o; }
float4 ps(VO i) : SV_TARGET {
    if (dpar.z > 0.5 && i.invz > 0) {
        // the game's picture is nearer here than this is: hidden (a little slack, so what lies on a surface shows)
        float scene = dpar.y / (dtex.Load(int3(i.pos.xy, 0)) - dpar.x);
        if (scene > 0 && 1.0 / i.invz > scene * 1.03 + 0.08) discard;
    }
    float4 t = tex.Sample(smp, i.uv);
    float4 c;
    if (mode.x > 0.5) {
        float w = max(fwidth(t.a), 0.0001);
        c = float4(i.col.rgb, i.col.a * smoothstep(0.5 - w, 0.5 + w, t.a));
    } else {
        c = t * i.col;
    }
    if (mode.y > 0.5) c.rgb = pow(abs(c.rgb), 2.2);
    return c;
}
)";

const char MESH_SHADER[] = R"(
cbuffer C : register(b0) { row_major float4x4 mvp; row_major float4x4 world; float4 light; float4 mode; float4 tint; float4 uvoff; float4 emis; float4 dpar; }
Texture2D tex : register(t0);
Texture2D etex : register(t1);
Texture2D<float> dtex : register(t2);
SamplerState smp : register(s0);
struct VI { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float shade : TEXCOORD1; float vz : TEXCOORD2; };
VO vs(VI i) {
    VO o;
    o.pos = mul(float4(i.pos, 1), mvp);
    float3 n = normalize(mul(float4(i.nrm, 0), world).xyz);
    o.shade = light.w + (1 - light.w) * saturate(dot(n, light.xyz));
    o.uv = i.uv + uvoff.xy;
    o.vz = o.pos.w;                                   // the distance along the view (the projections here put it in w)
    return o;
}
float4 ps(VO i) : SV_TARGET {
    if (dpar.z > 0.5) {
        float scene = dpar.y / (dtex.Load(int3(i.pos.xy, 0)) - dpar.x);
        if (scene > 0 && i.vz > scene * 1.02 + 0.05) discard;     // an effect mesh behind something of the game's
    }
    float4 t = tex.Sample(smp, i.uv);
    if (mode.x > 0.5 && t.a < 0.5) discard;          // a display's picture: its transparent parts are cut out
    float4 c = float4(t.rgb * i.shade * tint.rgb, 1);
    if (emis.a > 0.5) c.rgb += etex.Sample(smp, i.uv).rgb * emis.rgb;   // the lights: the emissive picture in the colour asked for, unlit
    if (mode.z > 0.5) c = float4(tint.rgb, t.a * tint.a);   // a glow: the tint, as see-through as the picture
    if (mode.w > 0.5) c = float4(t.rgb * tint.rgb, tint.a); // an effect mesh: its own colours, unlit, as see-through as asked
    if (mode.y > 0.5) c.rgb = pow(abs(c.rgb), 2.2);
    return c;
}
)";

// The picture while the player dies: ULTRAKILL's post-process shader ('ULTRAKILL/PostProcessV2') with its DEAD
// keyword, written back out of the compiled program (build/_dead_shader.py disassembles it). Two numbers
// drive it, and DeathSequence runs both from 0 to 1 over its two seconds: "_Deathness" and "_Sharpness".
//   Every point of the picture is read through a glitch. The screen is cut into 12.8 coarse rows and 204.8
//   fine ones, each with a number from 0 to 1 that changes with the clock. A row is "broken" when its coarse
//   number is under deathness^2, fully so when its fine number is also under the root of the deathness.
//   A broken row is pushed sideways by the sine of (time x deathness^4 / 10000, more or less by row) and
//   read a second time up to a twentieth of the screen lower; red comes from the first reading (x 1.1),
//   green and blue from the second, and in a quarter x deathness^2 of the screen's sixteen blocks green
//   and blue are cut down (to 0.75 - 0.55 deathness^2), which is the red cast.
//   Then the sharpening: the point, plus 100 x sharpness^3 times its difference from the mean of its four
//   neighbours (each read through the glitch too). At full sharpness every edge is blown out.
// Left out of the port: the shader's ordinary work, which goes on under it (dithering, colour depth, gamma).
const char DEATH_SHADER[] = R"(
cbuffer C : register(b0) { float4 par; float4 px; }         // par: deathness, sharpness, time, -. px: one pixel in uv
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float invz : TEXCOORD1; };
float hash3(float3 v) {
    float3 p = 17.0 * frac(v * 0.3183099 + float3(0.71, 0.113, 0.419));
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float3 glitch(float2 p) {
    float D = saturate(par.x), D2 = D * D, T = par.z;
    float t1 = T * 0.0001, t2 = T * 0.00005, t3 = T * 0.00001;
    float2 a = float2(t1, floor(p.y * 12.8 + 0.00001) * 10.0 + t1), b = float2(t2, floor(p.y * 204.8 + 0.00001) * 10.0 + t2);
    float hA = hash3(a.xyx), hB = hash3(b.xyx);
    float m = 1.0 - saturate(0.5 * ceil(hB - sqrt(D)) + ceil(hA - (D2 - 0.001)));
    float w = saturate(lerp(sin(hA * 37.189), 1.0, 1.0 / 3.0) + lerp(sin(hB * 37.189), 1.0, 1.0 / 3.0)) - 1.0;
    float2 q = float2(p.x + sin((1.0 + m * w) * (m * T) * D2 * D2 * 0.0001), p.y);
    float hC = hash3(float3(floor(q * 4.0 + 0.00001) * 10.0 + t3, t3));
    float block = max(ceil(hC - (1.0 - 0.25 * D2)), 0.0);
    float2 q2 = float2(q.x, q.y + 0.1 * frac(m * ceil(hB * 5.0) * 0.1));
    float3 A = tex.Sample(smp, clamp(q, 0.0, 0.9999)).rgb, B = tex.Sample(smp, clamp(q2, 0.0, 0.9999)).rgb;
    float3 c = float3(1.1 * A.r, B.g, B.b);
    c = lerp(c, c * float3(1.0, 0.75 - 0.55 * D2, 0.75 - 0.55 * D2), block);
    c.r = c.g < 0.9 ? max(c.g, c.r) : c.r;
    return lerp(A, c, ceil(m));
}
float4 ps(VO i) : SV_TARGET {
    float3 c = glitch(i.uv);
    float3 n = glitch(saturate(i.uv + float2(px.x, 0))) + glitch(saturate(i.uv - float2(px.x, 0))) + glitch(saturate(i.uv + float2(0, px.y))) +
               glitch(saturate(i.uv - float2(0, px.y)));
    float S = par.y;
    return float4(saturate(c + (c - n * 0.25) * S * S * S * 100.0), 1.0);
}
)";

// The chosen row of the title menu, redrawn as ULTRAKILL's chosen button: Dark Souls writes it in white on a
// dark orange bar; here the bar and all round it are white and the words black.
const char MENU_SHADER[] = R"(
cbuffer C : register(b0) { float4 par; float4 px; }
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float invz : TEXCOORD1; };
float4 ps(VO i) : SV_TARGET {
    float3 c = tex.Sample(smp, i.pos.xy * px.xy).rgb;
    float words = smoothstep(0.25, 0.7, c.b);
    float v = 1.0 - words;
    return float4(v, v, v, 1.0);
}
)";

typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

ID3DBlob *compile(CompileFn fn, const char *src, size_t len, const char *entry, const char *profile) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = fn(src, len, nullptr, nullptr, nullptr, entry, profile, 0, 0, &code, &err);
    if (FAILED(hr)) {
        g.error = std::string("shader compile failed: ") + (err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

bool create_pipeline(ID3D11Device *dev) {
    HMODULE lib = LoadLibraryA("d3dcompiler_47.dll");
    CompileFn fn = lib ? (CompileFn)GetProcAddress(lib, "D3DCompile") : nullptr;
    if (!fn) { g.error = "d3dcompiler_47.dll not available"; return false; }

    ID3DBlob *vs = compile(fn, UI_SHADER, sizeof(UI_SHADER) - 1, "vs", "vs_4_0");
    ID3DBlob *ps = compile(fn, UI_SHADER, sizeof(UI_SHADER) - 1, "ps", "ps_4_0");
    if (!vs || !ps) return false;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.ui_vs);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.ui_ps);
    if (ID3DBlob *dps = compile(fn, DEATH_SHADER, sizeof(DEATH_SHADER) - 1, "ps", "ps_4_0")) {
        dev->CreatePixelShader(dps->GetBufferPointer(), dps->GetBufferSize(), nullptr, &g.death_ps);
        dps->Release();
    }
    if (ID3DBlob *mps = compile(fn, MENU_SHADER, sizeof(MENU_SHADER) - 1, "ps", "ps_4_0")) {
        dev->CreatePixelShader(mps->GetBufferPointer(), mps->GetBufferSize(), nullptr, &g.menu_ps);
        mps->Release();
    }
    g.error.clear();                         // (the death shader is not needed for the rest to work)
    const D3D11_INPUT_ELEMENT_DESC ui_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(ui_layout, 4, vs->GetBufferPointer(), vs->GetBufferSize(), &g.ui_il);
    vs->Release();
    ps->Release();

    vs = compile(fn, MESH_SHADER, sizeof(MESH_SHADER) - 1, "vs", "vs_4_0");
    ps = compile(fn, MESH_SHADER, sizeof(MESH_SHADER) - 1, "ps", "ps_4_0");
    if (!vs || !ps) return false;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.mesh_vs);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.mesh_ps);
    const D3D11_INPUT_ELEMENT_DESC mesh_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(mesh_layout, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &g.mesh_il);
    vs->Release();
    ps->Release();

    D3D11_BUFFER_DESC cb{};
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb.ByteWidth = 32;
    dev->CreateBuffer(&cb, nullptr, &g.ui_cb);
    cb.ByteWidth = 224;
    dev->CreateBuffer(&cb, nullptr, &g.mesh_cb);

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    dev->CreateBlendState(&bd, &g.blend);
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    dev->CreateBlendState(&bd, &g.blend_add);
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendEnable = FALSE;
    dev->CreateBlendState(&bd, &g.opaque);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g.raster);
    // For the weapon and arm models: faces turned away from the eye are not drawn, as in ULTRAKILL. With
    // both sides drawn, the inside of the shotgun's barrel housing showed as two flat slabs beside the barrels.
    rd.CullMode = D3D11_CULL_BACK;
    dev->CreateRasterizerState(&rd, &g.raster_cull);

    D3D11_DEPTH_STENCIL_DESC dd{};
    dev->CreateDepthStencilState(&dd, &g.depth_off);
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    dev->CreateDepthStencilState(&dd, &g.depth_on);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;     // tested against what is there, but leaves no mark: see-through things
    dev->CreateDepthStencilState(&dd, &g.depth_read);

    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    dev->CreateSamplerState(&sd, &g.linear);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    dev->CreateSamplerState(&sd, &g.point);
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;   // for textures that scroll
    dev->CreateSamplerState(&sd, &g.wrap);

    const uint32_t white_px = 0xFFFFFFFF;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA wd{&white_px, 4, 0};
    ID3D11Texture2D *wt = nullptr;
    dev->CreateTexture2D(&td, &wd, &wt);
    if (wt) {
        dev->CreateShaderResourceView(wt, nullptr, &g.white);
        wt->Release();
    }
    return g.ui_vs && g.ui_ps && g.ui_il && g.mesh_vs && g.mesh_ps && g.mesh_il && g.white && g.blend && g.linear;
}

// ---------------------------------------------------------------- 2D layout, Unity's RectTransform rules

struct Xf {
    float ox, oy, sx, sy, rot;   // local -> canvas: rotate by rot (radians, about the pivot), scale, translate
};
struct Box {
    Xf xf;
    float x0, y0, x1, y1;        // the rectangle in its own local space; the origin is its pivot
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};
struct RT {
    float amin_x, amin_y, amax_x, amax_y;   // anchors, as fractions of the parent rect
    float ax, ay;                           // anchored position: pivot relative to the anchor reference point
    float w, h;                             // size delta: added to the size the anchors span
    float px, py;                           // pivot, as fractions of this rect
    float sx, sy;                           // local scale
    float rot;                              // rotation about Z in degrees
};
RT rt(float aminx, float aminy, float amaxx, float amaxy, float ax, float ay, float w, float h, float px = 0.5f, float py = 0.5f,
      float sx = 1, float sy = 1, float rot = 0) {
    return RT{aminx, aminy, amaxx, amaxy, ax, ay, w, h, px, py, sx, sy, rot};
}
const RT FILL = {0, 0, 1, 1, 0, 0, 0, 0, 0.5f, 0.5f, 1, 1, 0};

void to_canvas(const Xf &xf, float x, float y, float &cx, float &cy) {
    float c = cosf(xf.rot), s = sinf(xf.rot);
    float lx = x * xf.sx, ly = y * xf.sy;
    cx = xf.ox + lx * c - ly * s;
    cy = xf.oy + lx * s + ly * c;
}

Box child(const Box &parent, const RT &r) {
    float ax0 = parent.x0 + r.amin_x * parent.w(), ax1 = parent.x0 + r.amax_x * parent.w();
    float ay0 = parent.y0 + r.amin_y * parent.h(), ay1 = parent.y0 + r.amax_y * parent.h();
    float w = (ax1 - ax0) + r.w, h = (ay1 - ay0) + r.h;
    float pivx = ax0 + (ax1 - ax0) * r.px + r.ax, pivy = ay0 + (ay1 - ay0) * r.py + r.ay;
    Box b;
    to_canvas(parent.xf, pivx, pivy, b.xf.ox, b.xf.oy);
    b.xf.sx = parent.xf.sx * r.sx;
    b.xf.sy = parent.xf.sy * r.sy;
    b.xf.rot = parent.xf.rot + r.rot * 3.14159265f / 180.0f;
    b.x0 = -r.px * w;
    b.x1 = (1 - r.px) * w;
    b.y0 = -r.py * h;
    b.y1 = (1 - r.py) * h;
    return b;
}

// Where a canvas ends up on screen.
struct Surface {
    bool world;                 // true: a plane in front of the HUD camera; false: screen space
    float pos[3], yaw, scale[2];            // world: position, rotation about Y (radians), units per canvas unit
    float proj_x, proj_y;                   // world: projection scale (1 / (aspect * tan), 1 / tan)
    float px_per_unit, screen_w, screen_h;  // screen: canvas units to pixels
    void clip(float cx, float cy, float out[4]) const {
        if (world) {
            float lx = cx * scale[0], ly = cy * scale[1];
            float c = cosf(yaw), s = sinf(yaw);
            float x = pos[0] + lx * c, y = pos[1] + ly, z = pos[2] - lx * s;   // Unity's left-handed rotation about Y
            out[0] = x * proj_x;
            out[1] = y * proj_y;
            out[2] = 0.5f * z;
            out[3] = z;
        } else {
            out[0] = cx * px_per_unit / (screen_w * 0.5f);
            out[1] = cy * px_per_unit / (screen_h * 0.5f);
            out[2] = 0.5f;
            out[3] = 1.0f;
        }
    }
};

struct Color {
    float r, g, b, a;
};

int g_ui_add = 0;        // what push_quad marks its quads with: 1 while the additive trails are being built
int g_ui_world = 0;      // and 1 while what is being built belongs to the world and not to the HUD
bool g_title_column = false;   // the title menu as ULTRAKILL's column of buttons (v0.83, not yet tried in the game) and not as Dark Souls' rows in place
int g_ui_fx = 0;         // 1 for the one quad that redraws the picture through the death shader, 2 through the title menu's
void push_quad(const Surface &sf, const Xf &xf, int tex, int sdf, const float lx[4], const float ly[4], const float u[4], const float v[4], Color c) {
    // corners in order: (0) bottom-left, (1) top-left, (2) top-right, (3) bottom-right
    UiVert q[4];
    for (int i = 0; i < 4; i++) {
        float cx, cy;
        to_canvas(xf, lx[i], ly[i], cx, cy);
        sf.clip(cx, cy, q[i].pos);
        q[i].uv[0] = u[i];
        q[i].uv[1] = v[i];
        q[i].col[0] = c.r;
        q[i].col[1] = c.g;
        q[i].col[2] = c.b;
        q[i].col[3] = c.a;
        q[i].inv_z = 0;
    }
    UINT start = (UINT)g.verts.size();
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int k : order) g.verts.push_back(q[k]);
    if (!g.draws.empty() && g.draws.back().tex == tex && g.draws.back().sdf == sdf && g.draws.back().add == g_ui_add && g.draws.back().world == g_ui_world &&
        !g_ui_fx && !g.draws.back().fx)
        g.draws.back().count += 6;
    else
        g.draws.push_back({tex, sdf, start, 6, g_ui_add, g_ui_world, g_ui_fx});
}

// Marks the vertices pushed since `first` as being in the game's world, `z` metres along the view.
void world_depth(UINT first, float z) {
    if (!(z > 0)) return;
    for (size_t i = first; i < g.verts.size(); i++) g.verts[i].inv_z = 1.0f / z;
}

void rect_quad(const Surface &sf, const Xf &xf, int tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color c) {
    if (x1 <= x0 || y1 <= y0) return;
    const float lx[4] = {x0, x0, x1, x1}, ly[4] = {y0, y1, y1, y0};
    const float u[4] = {u0, u0, u1, u1}, v[4] = {v1, v0, v0, v1};   // v0 is the top of the image
    push_quad(sf, xf, tex, 0, lx, ly, u, v, c);
}

// Image.Type.Simple, optionally keeping the sprite's aspect ratio (fitted inside the rect, centred).
// `match_scale` > 0 draws the sprite at that many units per pixel instead of fitting it.
void image_simple(const Surface &sf, const Box &b, int sprite, Color c, bool preserve = false, float match_scale = 0) {
    float x0 = b.x0, y0 = b.y0, x1 = b.x1, y1 = b.y1;
    int tex = -1;
    if (sprite >= 0) {
        const Tex &t = g.textures[g.sprites[sprite].tex];
        tex = g.sprites[sprite].tex;
        if (preserve || match_scale > 0) {
            float s = match_scale > 0 ? match_scale : fminf(b.w() / t.w, b.h() / t.h);
            float w = t.w * s, h = t.h * s, cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
            x0 = cx - w * 0.5f;
            x1 = cx + w * 0.5f;
            y0 = cy - h * 0.5f;
            y1 = cy + h * 0.5f;
        }
    }
    rect_quad(sf, b.xf, tex, x0, y0, x1, y1, 0, 0, 1, 1, c);
}

// Image.Type.Sliced: the nine-patch Unity builds, including shrinking the borders when the rect is
// smaller than they are. Border sizes in units are pixels / (sprite ppu / 100 * multiplier).
void image_sliced(const Surface &sf, const Box &b, int sprite, Color c, float ppu_mult = 5.4f) {
    if (sprite < 0) {
        rect_quad(sf, b.xf, -1, b.x0, b.y0, b.x1, b.y1, 0, 0, 1, 1, c);
        return;
    }
    const Sprite &sp = g.sprites[sprite];
    const Tex &t = g.textures[sp.tex];
    float unit = (sp.ppu / 100.0f) * ppu_mult;
    float bl = sp.border[0] / unit, bb = sp.border[1] / unit, br = sp.border[2] / unit, bt = sp.border[3] / unit;
    float w = b.w(), h = b.h();
    if (w <= 0 || h <= 0) return;
    if (bl + br > w && bl + br > 0) { float k = w / (bl + br); bl *= k; br *= k; }
    if (bb + bt > h && bb + bt > 0) { float k = h / (bb + bt); bb *= k; bt *= k; }
    const float xs[4] = {b.x0, b.x0 + bl, b.x1 - br, b.x1};
    const float ys[4] = {b.y0, b.y0 + bb, b.y1 - bt, b.y1};
    const float us[4] = {0, sp.border[0] / t.w, 1 - sp.border[2] / t.w, 1};
    const float vs[4] = {1, 1 - sp.border[1] / t.h, sp.border[3] / t.h, 0};   // bottom to top
    for (int iy = 0; iy < 3; iy++)
        for (int ix = 0; ix < 3; ix++)
            rect_quad(sf, b.xf, sp.tex, xs[ix], ys[iy], xs[ix + 1], ys[iy + 1], us[ix], vs[iy + 1], us[ix + 1], vs[iy], c);
}

// Image.Type.Filled, Radial360 from the bottom: a pie slice of the sprite.
void image_radial(const Surface &sf, const Box &b, int sprite, Color c, float amount, bool clockwise) {
    if (sprite < 0 || amount <= 0) return;
    if (amount > 1) amount = 1;
    int tex = g.sprites[sprite].tex;
    float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f, hw = b.w() * 0.5f, hh = b.h() * 0.5f;
    const int steps = 48;
    int n = (int)ceilf(steps * amount);
    for (int i = 0; i < n; i++) {
        float f0 = (float)i / steps, f1 = fminf((float)(i + 1) / steps, amount);
        float px[3] = {0, 0, 0}, py[3] = {0, 0, 0};
        const float fr[2] = {f0, f1};
        for (int k = 0; k < 2; k++) {
            // angle measured from straight down; clockwise as seen on screen means towards the left first
            float a = fr[k] * 6.2831853f * (clockwise ? -1.0f : 1.0f);
            float dx = sinf(a), dy = -cosf(a);
            float m = fmaxf(fabsf(dx), fabsf(dy));
            px[k + 1] = dx / m;
            py[k + 1] = dy / m;
        }
        const float lx[4] = {cx, cx + px[1] * hw, cx + px[2] * hw, cx}, ly[4] = {cy, cy + py[1] * hh, cy + py[2] * hh, cy};
        const float u[4] = {0.5f, 0.5f + px[1] * 0.5f, 0.5f + px[2] * 0.5f, 0.5f}, v[4] = {0.5f, 0.5f - py[1] * 0.5f, 0.5f - py[2] * 0.5f, 0.5f};
        push_quad(sf, b.xf, tex, 0, lx, ly, u, v, c);
    }
}

// TextMeshPro-style text from the distance-field atlas. halign: 0 left, 1 centre, 2 right.
// Vertically the glyphs themselves are centred in the box ("geometry" alignment, which the HUD uses).
void text(const Surface &sf, const Box &b, const char *str, float size, Color c, int halign) {
    if (g.fonts.empty()) return;
    const Font &f = g.fonts[0];
    float s = size / f.point, width = 0, top = -1e9f, bottom = 1e9f;
    for (const char *p = str; *p; p++) {
        const Glyph *gl = f.find((uint8_t)*p);
        if (!gl) continue;
        width += gl->adv * s;
        if (gl->w > 0) {
            top = fmaxf(top, gl->by * s);
            bottom = fminf(bottom, (gl->by - gl->h) * s);
        }
    }
    if (top < bottom) return;
    float pen = halign == 0 ? b.x0 : halign == 1 ? (b.x0 + b.x1 - width) * 0.5f : b.x1 - width;
    float baseline = (b.y0 + b.y1) * 0.5f - (top + bottom) * 0.5f;
    float pad = f.padding;
    for (const char *p = str; *p; p++) {
        const Glyph *gl = f.find((uint8_t)*p);
        if (!gl) continue;
        if (gl->w > 0) {
            float x0 = pen + (gl->bx - pad) * s, x1 = x0 + (gl->w + 2 * pad) * s;
            float y1 = baseline + (gl->by + pad) * s, y0 = y1 - (gl->h + 2 * pad) * s;
            float u0 = (gl->x - pad) / f.aw, u1 = (gl->x + gl->w + pad) / f.aw;
            float v0 = (gl->y - pad) / f.ah, v1 = (gl->y + gl->h + pad) / f.ah;
            const float lx[4] = {x0, x0, x1, x1}, ly[4] = {y0, y1, y1, y0};
            const float u[4] = {u0, u0, u1, u1}, v[4] = {v1, v0, v0, v1};
            push_quad(sf, b.xf, f.tex, 1, lx, ly, u, v, c);
        }
        pen += gl->adv * s;
    }
}

// ---------------------------------------------------------------- the HUD itself

// values that ease towards the real ones, like ULTRAKILL's after-image bars
struct Anim {
    double last = 0;
    float after_health = 100, shown_health = 100, shown_stamina = 300;
    float cross_alpha = 0;
    double cross_until = 0;
    float prev_health = 100, prev_stamina = 300;
} anim;

const Color PANEL = {0, 0, 0, 0.4902f}, PANEL2 = {0, 0, 0, 0.502f}, TRACK = {0, 0, 0, 0.6863f};
const Color MARKSMAN_GREEN = {0.2667f, 1, 0.2706f, 1};   // the green ULTRAKILL's HUD uses; the variation colour itself is a user setting
const Color PIERCER_BLUE = {0, 0.8759f, 1, 1}, FIST_BLUE = {0.251f, 0.9059f, 1, 1};
const Color SHARP_RED = {1, 0.2392f, 0.2392f, 1};         // ours: a red in the same key as the other two
float g_muzzle[2] = {0.26f, -0.32f};     // where the held weapon's muzzle was drawn last frame (-1..1, y up)
bool g_muzzle_known = false;
float g_whip_hand[2] = {-0.45f, -0.55f}; // the same for the point of the whiplash arm its cable starts at
bool g_whip_hand_known = false;
const char *g_skip_mesh = nullptr;       // a mesh draw_model leaves out, by name (the whiplash's hook while it is thrown)
const char *g_only_mesh = nullptr;       // the one mesh draw_model draws, by name (the Overheat's glowing blade, in a pass of its own)

// ULTRAKILL's death, on its screen-space canvas (reference 1280 x 720, "expand").
//   For two seconds the picture goes through the death shader (above) and 'DeathSequence/Text (TMP)' writes
//   its lines over it: 16 units high in the HUD's own font, from the top left corner, 50 units in from every
//   edge; red, the warnings orange. TextAppearByLines shows one more every 0.05 s.
//   Then 'BlackScreen':
//     the 'crtbg' picture in (0.05, 0.05, 0.05) over everything;
//     'YouDiedText', a legacy Text set to "best fit" in a box the width of the canvas and 40 short of its
//     height, centred: its seven lines (the first and the last have words) are made as large as fills the
//     box, which is 97 units a line on a 720-unit canvas, so "[YOU ARE DEAD]" stands across the top and the
//     line about the key across the bottom (measured on Davi's screenshot of the game: 98);
//     'LaughingSkull', half the canvas's height square in the middle, its animation changing between its
//     two pictures every half second of a 1.017 s loop (the DLL plays its laugh as the second comes up);
//     'Flash': a red panel the size of the canvas that masks the 'ISeeYou' picture and is closed at once, top
//     and bottom first, by HudOpenEffect (speed 30): its height goes down at 30 x (what is left + 0.1) a
//     second, which takes 0.08 s.
void build_death(const HudState &st, float screen_w, float screen_h) {
    Surface sc{};
    sc.world = false;
    sc.screen_w = screen_w;
    sc.screen_h = screen_h;
    sc.px_per_unit = fminf(screen_w / 1280.0f, screen_h / 720.0f);
    float hw = screen_w * 0.5f / sc.px_per_unit, hh = screen_h * 0.5f / sc.px_per_unit;
    Xf id{0, 0, 1, 1, 0};
    float line_scale = !g.fonts.empty() && g.fonts[0].point > 0 && g.fonts[0].line > 0 ? g.fonts[0].line / g.fonts[0].point : 1.17f;
    if (st.death_time < 2.0f) {
        g_ui_fx = 1;
        rect_quad(sc, id, -1, -hw, -hh, hw, hh, 0, 0, 1, 1, {1, 1, 1, 1});
        g_ui_fx = 0;
        int shown = (int)(st.death_time / 0.05f) + 1, count = (int)g.death_lines.size();
        if (shown > count) shown = count;
        float lh = 16.0f * line_scale;
        for (int i = 0; i < shown; i++) {
            Box row{id, -hw + 50.0f, hh - 50.0f - lh * (i + 1), hw - 50.0f, hh - 50.0f - lh * i};
            if (row.y1 < -hh) break;
            text(sc, row, g.death_lines[i].c_str(), 16.0f, g.death_orange[i] ? Color{1.0f, 0.5f, 0.0f, 1.0f} : Color{1.0f, 0.0f, 0.0f, 1.0f}, 0);
        }
        return;
    }
    int bg = g.sprite("crtbg");
    if (bg >= 0) rect_quad(sc, id, g.sprites[bg].tex, -hw, -hh, hw, hh, 0, 0, 1, 1, {0.05f, 0.05f, 0.05f, 1.0f});
    else rect_quad(sc, id, -1, -hw, -hh, hw, hh, 0, 0, 1, 1, {0.0025f, 0.0025f, 0.0025f, 1.0f});
    {
        // best fit: the size at which the lines fill the box's height, or its width if that is less
        int count = (int)g.dead_lines.size();
        size_t widest = 1;
        for (const std::string &l : g.dead_lines) widest = l.size() > widest ? l.size() : widest;
        float box_h = hh * 2.0f - 40.0f, size = count > 0 ? box_h / count : 14.0f, advance = 0.6f;
        if (!g.fonts.empty() && g.fonts[0].point > 0)
            if (const Glyph *gl = g.fonts[0].find('M')) advance = gl->adv / g.fonts[0].point;
        if (advance > 0 && size * advance * widest > hw * 2.0f) size = hw * 2.0f / (advance * widest);
        if (size > 200.0f) size = 200.0f;
        float top = size * count * 0.5f;
        for (int i = 0; i < count; i++) {
            if (g.dead_lines[i].empty() || (i == count - 1 && count > 1 && !st.death_prompt)) continue;
            Box row{id, -hw, top - size * (i + 1), hw, top - size * i};
            text(sc, row, g.dead_lines[i].c_str(), size, {1, 1, 1, 1}, 1);
        }
    }
    {
        float loop = fmodf(st.death_time - 2.0f, 61.0f / 60.0f);
        int skull = g.sprite(loop >= 0.5f && loop < 1.0f ? "SkullFrameDoubleB" : "SkullFrameDoubleA");
        if (skull >= 0) rect_quad(sc, id, g.sprites[skull].tex, -hh * 0.5f, -hh * 0.5f, hh * 0.5f, hh * 0.5f, 0, 0, 1, 1, {1, 1, 1, 1});
    }
    float open = 1.1f * expf(-30.0f * (st.death_time - 2.0f)) - 0.1f;
    if (open > 0) {
        int pic = g.sprite("ISeeYou");
        float h = 360.0f * open;
        rect_quad(sc, id, -1, -640.0f, -h, 640.0f, h, 0, 0, 1, 1, {1, 0, 0, 1});
        if (pic >= 0) rect_quad(sc, id, g.sprites[pic].tex, -640.0f, -h, 640.0f, h, 0, 0.5f - 0.5f * open, 1, 0.5f + 0.5f * open, {1, 1, 1, 1});
    }
}

// Dark Souls' title screen, told by the picture itself (measured on a 1920 x 1080 one, as shares of its size):
//   its logo is white letters between 36% and 55% of the way down, from 7% to 92% across, on black, with
//   "REMASTERED" under it down to 64%; a line across at 45% is about a third white, one at 30% all black;
//   its menu is four rows in the middle, 3.47% of the height apart, the first centred 73.15% of the way down,
//   the chosen one on a dark orange bar (98, 46, 12) from 40.7% to 59.3% across and 3.1% tall;
//   a notice (the offline one, the "not closed properly" one) is a grey box over the middle: the two points
//   at 35% and 65% across, 56% down, are black without one.
// The words of a row are told by their shape. They are centred, white (grey in a row at the edge of the list)
// and, at 1080 lines, within 13 pixels of the row's middle line; the blue of the picture alone separates them
// from the black and from the orange bar. Measured there, in pixels (they are scaled by the height):
//   Continue    104 wide, one word
//   System       81 wide, one word
//   Log In       73 wide, the gap between its words right of the middle (7 to 15)
//   Load Game   132 wide, its first word 57, the gap left of the middle (-8 to 1)
//   New Game    125 wide, its first word 50, and nothing of it lower than 6 under the middle line
//   Quit Game   122 wide, its first word 47, the tail of its Q down to 10
// (a gap between words is 9 to 11 wide, one between letters under 5). Two arrows, at 67% and 87% of the way
// down, are there while the list goes on above or below.
// Returns whether the title screen is up with no notice over it, and in `menu` what its menu shows.
const char *const TITLE_WORDS[6] = {"CONTINUE", "LOAD GAME", "NEW GAME", "SYSTEM", "LOG IN", "QUIT GAME"};
const float TITLE_ROW0 = 0.7315f, TITLE_ROW_STEP = 0.03472f;      // the rows' middle lines, as shares of the height
const float TITLE_ARROW_UP = 0.6704f, TITLE_ARROW_DOWN = 0.8736f; // and the arrows'

bool title_probe(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *target, const D3D11_TEXTURE2D_DESC &td, TitleMenu &menu) {
    menu = TitleMenu();
    int &row = menu.row;
    bool bgra = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    bool rgba = td.Format == DXGI_FORMAT_R8G8B8A8_UNORM || td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if (td.SampleDesc.Count != 1 || (!bgra && !rgba) || td.Width < 320 || td.Height < 180) return false;
    D3D11_TEXTURE2D_DESC pd{};
    if (g.probe_tex) g.probe_tex->GetDesc(&pd);
    if (!g.probe_tex || pd.Width != td.Width || pd.Height != td.Height || pd.Format != td.Format) {
        if (g.probe_tex) g.probe_tex->Release();
        g.probe_tex = nullptr;
        pd = td;
        pd.BindFlags = 0;
        pd.Usage = D3D11_USAGE_STAGING;
        pd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        pd.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&pd, nullptr, &g.probe_tex))) return false;
    }
    ctx->CopyResource(g.probe_tex, target);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ctx->Map(g.probe_tex, 0, D3D11_MAP_READ, 0, &ms))) return false;
    auto px = [&](float fx, float fy, int *rgb) {
        int x = (int)(fx * td.Width), y = (int)(fy * td.Height);
        x = x < 0 ? 0 : x >= (int)td.Width ? (int)td.Width - 1 : x;
        y = y < 0 ? 0 : y >= (int)td.Height ? (int)td.Height - 1 : y;
        const uint8_t *p = (const uint8_t *)ms.pData + (size_t)y * ms.RowPitch + (size_t)x * 4;
        rgb[0] = p[bgra ? 2 : 0];
        rgb[1] = p[1];
        rgb[2] = p[bgra ? 0 : 2];
    };
    int c[3], white = 0, above = 0, n = 0;
    for (float fx = 0.19f; fx < 0.81f; fx += 0.002f, n++) {
        px(fx, 0.45f, c);
        if (c[0] > 200 && c[1] > 200 && c[2] > 200) white++;
        px(fx, 0.30f, c);
        if (c[0] > 60 || c[1] > 60 || c[2] > 60) above++;
    }
    int a[3], b[3];
    px(0.35f, 0.56f, a);
    px(0.65f, 0.56f, b);
    bool notice = a[0] + a[1] + a[2] > 12 || b[0] + b[1] + b[2] > 12;
    bool ok = white * 100 > n * 15 && white * 100 < n * 60 && above * 100 < n * 2 && !notice;
    const float u = td.Height / 1080.0f;
    const int xc = (int)td.Width / 2;
    auto blue = [&](int x, int y) { return (int)((const uint8_t *)ms.pData + (size_t)y * ms.RowPitch + (size_t)x * 4)[bgra ? 0 : 2]; };
    auto words = [&](int i) -> int {
        const int yc = (int)((TITLE_ROW0 + TITLE_ROW_STEP * i) * td.Height), hy = (int)(13.0f * u + 0.5f), hx = (int)(100.0f * u + 0.5f);
        static int low[1024];                    // per column: how far under the middle line its lowest ink is
        if (yc - hy < 0 || yc + hy >= (int)td.Height || xc - hx < 0 || xc + hx >= (int)td.Width || 2 * hx + 1 > 1024) return -1;
        int top = 0;
        for (int y = yc - hy; y <= yc + hy; y++)
            for (int x = xc - hx; x <= xc + hx; x++) top = blue(x, y) > top ? blue(x, y) : top;
        if (top < 60) return -2;
        const int thr = top * 45 / 100, none = -1000;
        int x0 = -1, x1 = -1;
        for (int k = 0; k <= 2 * hx; k++) {
            low[k] = none;
            for (int y = yc + hy; y >= yc - hy; y--)
                if (blue(xc - hx + k, y) >= thr) { low[k] = y - yc; break; }
            if (low[k] != none) {
                if (x0 < 0) x0 = k;
                x1 = k;
            }
        }
        if (x0 < 0) return -2;
        int gaps = 0, gap_at = 0, gap_len = 0, run = 0;
        for (int k = x0; k <= x1; k++) {
            if (low[k] == none) { run++; continue; }
            if (run >= 7.0f * u) {
                if (!gaps) { gap_at = k - run; gap_len = run; }
                gaps++;
            }
            run = 0;
        }
        const float w = (x1 - x0 + 1) / u;
        if (!gaps) return w > 94 && w < 114 ? 0 : w > 72 && w < 92 ? 3 : -1;
        if (gaps > 1) return -1;
        const float gap_mid = (gap_at + gap_len * 0.5f - hx) / u, first = (gap_at - x0) / u;
        if (gap_mid > 3.0f) return w > 63 && w < 83 ? 4 : -1;
        if (first >= 53.5f) return w > 122 && w < 142 ? 1 : -1;
        if (first < 41.0f || w < 112 || w > 135) return -1;
        int tail = none;
        for (int k = x0; k < gap_at; k++) tail = low[k] > tail ? low[k] : tail;
        return tail >= 8.5f * u ? 5 : 2;
    };
    auto arrow = [&](float fy) {
        for (int y = (int)((fy - 0.004f) * td.Height); y <= (int)((fy + 0.004f) * td.Height); y++)
            for (int x = xc - (int)(26.0f * u); x <= xc + (int)(26.0f * u); x++)
                if (y >= 0 && y < (int)td.Height && x >= 0 && x < (int)td.Width && blue(x, y) > 40) return true;
        return false;
    };
    if (ok) {
        for (int i = 0; i < 4; i++) {
            px(0.42f, TITLE_ROW0 + TITLE_ROW_STEP * i, c);
            if (c[0] > 60 && c[0] < 150 && c[1] > 20 && c[1] < 90 && c[2] < 45 && c[0] - c[2] > 45) row = i;
            menu.items[i] = words(i);
        }
        menu.up = arrow(TITLE_ARROW_UP);
        menu.down = arrow(TITLE_ARROW_DOWN);
    }
    ctx->Unmap(g.probe_tex, 0);
    return ok;
}

// What of the menu is drawn, from what was just read: ULTRAKILL's column of buttons while every row that has
// words was recognised (two at the least: "PRESS ANY BUTTON" is one unknown row), and over a gap of up to
// 0.35 s after that (rows moving as the list scrolls); otherwise the rows as Dark Souls draws them.
volatile ULONGLONG g_title_col_tick = 0;     // GetTickCount64 of the last picture drawn with the column on it
void title_settle(const TitleMenu &seen, double time) {
    int known = 0, unknown = 0;
    for (int item : seen.items) {
        known += item >= 0;
        unknown += item == -1;
    }
    if (known >= 2 && !unknown) {
        g.title_menu = seen;
        g.title_col = g_title_column;        // (only where the column is asked for: title_column in the ini)
        g.title_good = time;
    } else if (!(g.title_col && time >= g.title_good && time - g.title_good < 0.35)) {
        g.title_menu = seen;
        g.title_col = false;
    }
}

// ULTRAKILL's four buttons ('Continue', 'Options', 'Credits', 'Quit' under 'LeftSide'): 420 x 70, their top
// left corners 555 left of the middle and 70 above it, 75 apart; and the marks this draws for Dark Souls'
// arrows, over the first and under the last.
const float TITLE_BTN_X = -555.0f, TITLE_BTN_TOP = 70.0f, TITLE_BTN_W = 420.0f, TITLE_BTN_H = 70.0f, TITLE_BTN_STEP = 75.0f;
const float TITLE_MARK_UP = TITLE_BTN_TOP + 17.0f, TITLE_MARK_DOWN = TITLE_BTN_TOP - 3 * TITLE_BTN_STEP - TITLE_BTN_H - 17.0f;

// ULTRAKILL's main menu over Dark Souls' title screen ('Main Menu (1)' in its menu scene, on the 1280 x 720
// canvas the HUD's flat parts use): the frame round the screen ('Border', 3 units in), the logo top left
// ('Title': 'TextmodeLogo', 968 x 160 at 0.6, its top left corner 555 left of the middle and 290 above it)
// and the picture of V1 on the right ('V1': 'TextmodeV1', 740 x 825 at 0.7; 320 right of the middle there,
// 380 here and 22 up, to stand clear of the menu and of the line of small print along the bottom). Dark Souls' own logo and the lines of text in its corner are
// blacked out. The menu is still Dark Souls' own, which answers the keys and the mouse. Its four rows are
// blacked out too and drawn again as ULTRAKILL's four buttons in the left column, in the HUD's font
// (size 40, capitals): the chosen one filled white with black words, as 'Continue' is there, the others
// framed ('Round_BorderLargeBlack', sliced at 4.05). hud_title_cursor tells Dark Souls the pointer is on
// row i while it is on button i. Where the rows' words cannot be read (another language, another layout) the
// menu stays where Dark Souls draws it, as before v0.83: each row in a button's frame, the chosen one
// redrawn white with black words, in Dark Souls' own letters.
void build_title(const HudState &st, float screen_w, float screen_h) {
    Surface sc{};
    sc.world = false;
    sc.screen_w = screen_w;
    sc.screen_h = screen_h;
    sc.px_per_unit = fminf(screen_w / 1280.0f, screen_h / 720.0f);
    float hw = screen_w * 0.5f / sc.px_per_unit, hh = screen_h * 0.5f / sc.px_per_unit;
    Xf id{0, 0, 1, 1, 0};
    auto X = [&](float f) { return (f - 0.5f) * screen_w / sc.px_per_unit; };
    auto Y = [&](float f) { return (0.5f - f) * screen_h / sc.px_per_unit; };
    const Color black{0, 0, 0, 1}, white{1, 1, 1, 1};
    rect_quad(sc, id, -1, X(0.04f), Y(0.665f), X(0.96f), Y(0.33f), 0, 0, 1, 1, black);
    rect_quad(sc, id, -1, X(0.04f), Y(0.23f), X(0.26f), Y(0.05f), 0, 0, 1, 1, black);
    if (g.title_col) rect_quad(sc, id, -1, X(0.38f), Y(0.892f), X(0.62f), Y(0.665f), 0, 0, 1, 1, black);
    Box screen{{0, 0, 1, 1, 0}, -hw, -hh, hw, hh};
    image_simple(sc, child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, 380, 22, 518, 577.5f)), g.sprite("TextmodeV1"), white, true);
    image_sliced(sc, child(screen, rt(0, 0, 1, 1, 0, 0, -6, -6)), g.sprite("Round_BorderLarge"), white);
    image_simple(sc, child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, -555, 290, 580.8f, 96, 0, 1)), g.sprite("TextmodeLogo"), white, true);
    if (st.title_note[0]) text(sc, child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, -555, 188, 968, 24, 0, 1)), st.title_note, 20.0f, white, 0);
    const TitleMenu &m = g.title_menu;
    if (g.title_col) {
        for (int i = 0; i < 4; i++) {
            if (m.items[i] < 0) continue;
            Box b = child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, TITLE_BTN_X, TITLE_BTN_TOP - TITLE_BTN_STEP * i, TITLE_BTN_W, TITLE_BTN_H, 0, 1));
            image_sliced(sc, b, g.sprite(i == m.row ? "Round_FillLarge" : "Round_BorderLargeBlack"), white, 4.05f);
            text(sc, child(b, rt(0, 0, 1, 1, 0, 1, 0, -2)), TITLE_WORDS[m.items[i]], 40.0f, i == m.row ? black : white, 1);
        }
        const float mx = TITLE_BTN_X + TITLE_BTN_W * 0.5f;
        for (int k = 0; k < 2; k++) {
            if (!(k ? m.down : m.up)) continue;
            float base = k ? TITLE_MARK_DOWN + 6.0f : TITLE_MARK_UP - 6.0f, tip = k ? TITLE_MARK_DOWN - 6.0f : TITLE_MARK_UP + 6.0f;
            const float lx[4] = {mx - 14.0f, mx, mx, mx + 14.0f}, ly[4] = {base, tip, tip, base}, uv[4] = {0, 0, 1, 1};
            push_quad(sc, id, -1, 0, lx, ly, uv, uv, white);
        }
        return;
    }
    if (m.row < 0) return;
    float half = 0.0153f * screen_h / sc.px_per_unit + 1.5f, x0 = X(0.4068f) - 6.0f, x1 = X(0.5927f) + 6.0f;
    for (int i = 0; i < 4; i++) {
        float yc = Y(TITLE_ROW0 + TITLE_ROW_STEP * i);
        if (i == m.row && g.menu_ps) {
            g_ui_fx = 2;
            rect_quad(sc, id, -1, x0, yc - half, x1, yc + half, 0, 0, 1, 1, white);
            g_ui_fx = 0;
        }
        Box frame{{0, 0, 1, 1, 0}, x0, yc - half, x1, yc + half};
        image_sliced(sc, frame, g.sprite("Round_BorderLarge"), white, 12.0f);
    }
}

void build_hud(const HudState &st, float screen_w, float screen_h) {
    if (st.title) {
        build_title(st, screen_w, screen_h);
        return;
    }
    if (st.death_time >= 0 && !st.flash_only) {
        build_death(st, screen_w, screen_h);
        return;
    }
    if (st.flash_only) {
        Surface fs{};
        fs.world = false;
        fs.screen_w = screen_w;
        fs.screen_h = screen_h;
        fs.px_per_unit = 1.0f;
        Xf id{0, 0, 1, 1, 0};
        rect_quad(fs, id, -1, -screen_w * 0.5f, -screen_h * 0.5f, screen_w * 0.5f, screen_h * 0.5f, 0, 0, 1, 1, {1, 1, 1, fminf(st.flash, 1.0f)});
        return;
    }
    float dt = (float)(st.time - anim.last);
    if (dt < 0 || dt > 0.5f) dt = 0;
    anim.last = st.time;
    // HealthBar.Update. The red bar and the number climb to a higher health at 5 a second plus five times
    // what is still to go, and drop to a lower one at once; the orange bar behind follows the red one up at
    // once and down at that same easing rate, so a hit shows as an orange piece that runs out after it.
    // (Until v0.71 the red bar jumped both ways and the orange one fell at a flat 30 a second.)
    if (dt <= 0) {
        // (a first frame, or the clock stepping: nothing to ease from)
        if (anim.shown_health > st.health) anim.shown_health = st.health;
    } else if (anim.shown_health < st.health) {
        anim.shown_health = fminf(st.health, anim.shown_health + dt * ((st.health - anim.shown_health) * 5.0f + 5.0f));
    } else {
        anim.shown_health = st.health;
    }
    if (anim.after_health < anim.shown_health) anim.after_health = anim.shown_health;
    else anim.after_health = fmaxf(anim.shown_health, anim.after_health - dt * ((anim.after_health - anim.shown_health) * 5.0f + 5.0f));

    // ---- main panel: "GunCanvas", a world-space canvas seen by the 90-degree HUD camera
    Surface sf{};
    sf.world = true;
    sf.pos[0] = -1.06f + st.hud_sway[0];
    sf.pos[1] = -0.53f + st.hud_sway[1];
    sf.pos[2] = 1.0f + st.hud_sway[2];
    sf.yaw = -30.0f * 3.14159265f / 180.0f;
    sf.scale[0] = 0.0007f;
    sf.scale[1] = 0.001f;
    sf.proj_y = 1.0f;                       // 1 / tan(90 / 2)
    sf.proj_x = screen_h / screen_w;
    Box canvas{{0, 0, 1, 1, 0}, -441, -248, 441, 248};

    Box stats = child(canvas, rt(0, 0, 0, 0, -79, 0, 200, 46, 0, 0, 4, 4));
    image_sliced(sf, stats, g.sprite("Round_FillLarge"), PANEL);
    Box filler = child(stats, rt(0, 0, 1, 1, 0, 0, -10, -10));
    {
        Box panel = child(filler, rt(0, 0, 1, 0, 0, 18, 0, 18, 0.5f, 0));
        image_sliced(sf, panel, g.sprite("Round_FillSmall"), PANEL2);
        Box inner = child(panel, rt(0, 0.5f, 1, 0.5f, 0, 0, 0, 100));
        Box slider = child(inner, rt(0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 190, 18));
        Box track = child(slider, rt(0, 0.25f, 1, 0.75f, 0, 0, 0, 9));
        image_sliced(sf, track, g.sprite("Round_Meter"), TRACK);
        float after = fminf(anim.after_health, 100.0f) / 100.0f, hp = fminf(fmaxf(anim.shown_health, 0.0f), 100.0f) / 100.0f;
        image_sliced(sf, child(track, rt(0, 0, after, 1, 0, 0, 0, 0)), g.sprite("Round_FillSmall"), {1, 0.3931f, 0, 1});
        image_sliced(sf, child(track, rt(0, 0, hp, 1, 0, 0, 0, 0)), g.sprite("Round_FillSmall"), {1, 0, 0, 1});
        text(sf, child(inner, rt(0, 0, 0, 1, 5.0002f, 0, 10, -78, 0, 0.5f)), "+", 17.1f, {1, 1, 1, 1}, 1);
        char num[16];
        snprintf(num, sizeof(num), "%d", (int)lroundf(fmaxf(anim.shown_health, 0.0f)));          // hp.ToString("F0")
        text(sf, child(inner, rt(0, 0.5f, 0, 0.5f, 15.0001f, 0, 154.5f, 15, 0, 0.5f)), num, 17.05f, {1, 1, 1, 1}, 0);
    }
    {
        Box panel = child(filler, rt(0, 0, 1, 0, 0, 0, 0, 16, 0.5f, 0));
        image_sliced(sf, panel, g.sprite("Round_FillSmall"), PANEL);
        const RT seg[3] = {rt(0, 0, 0, 0, 0, 0, 61, 16, 0, 0), rt(0.5f, 0, 0.5f, 0, 0, 0, 62, 16, 0.5f, 0), rt(1, 0, 1, 0, 0, 0, 61, 16, 1, 0)};
        const int spr[3] = {g.sprite("Round_MeterLeft"), -1, g.sprite("Round_MeterRight")};
        const Color blue[3] = {{0, 0.8759f, 1, 1}, {0.2074f, 0.908f, 1, 1}, {0.3235f, 0.9332f, 1, 1}};
        for (int i = 0; i < 3; i++) {
            Box s = child(panel, seg[i]);
            image_sliced(sf, s, spr[i], TRACK);
            Box area = child(s, rt(0, 0.25f, 1, 0.75f, 0, 0, 0, 8));
            float part = fminf(fmaxf((st.stamina - 100.0f * i) / 100.0f, 0.0f), 1.0f);
            Color col = blue[i];
            if (part < 1.0f) { col.r = 1; col.g = 0; col.b = 0; }   // a bar that cannot be spent yet shows red
            image_sliced(sf, child(area, rt(0, 0, part, 1, 0, 0, 0, 0)), spr[i], col);
        }
    }
    {
        Box fist = child(filler, rt(1, 0.5f, 1, 0.5f, 52, 0, 46, 46, 1, 0.5f));
        image_sliced(sf, fist, g.sprite("Round_FillLarge"), PANEL2);
        Box icon = child(fist, rt(0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 36, 36));
        // HudController: the picture is the arm in use (blue Feedbacker, red Knuckleblaster). A dark copy
        // lies behind; over it the picture is filled from the bottom by punch stamina / 2, at 60%
        // brightness while there is not enough for a punch.
        int fist_spr = g.sprite(st.arm == 1 ? "ArmKnuckleblaster" : "ArmFeedbacker");
        if (fist_spr < 0) fist_spr = g.sprite("FistIcon");
        const Color arm_col = st.arm == 1 ? SHARP_RED : FIST_BLUE;
        image_simple(sf, icon, fist_spr, {arm_col.r * 0.2f, arm_col.g * 0.2f, arm_col.b * 0.2f, 1}, true);
        float amount = fminf(fmaxf(st.punch_stamina / 2.0f, 0.0f), 1.0f), dim = st.punch_stamina >= 1.0f ? 1.0f : 0.6f;
        if (fist_spr >= 0 && amount > 0) {
            const Tex &ft = g.textures[g.sprites[fist_spr].tex];
            float fs = fminf(icon.w() / ft.w, icon.h() / ft.h), fw = ft.w * fs, fh = ft.h * fs;
            float cx = (icon.x0 + icon.x1) * 0.5f, cy = (icon.y0 + icon.y1) * 0.5f;
            rect_quad(sf, icon.xf, g.sprites[fist_spr].tex, cx - fw * 0.5f, cy - fh * 0.5f, cx + fw * 0.5f, cy - fh * 0.5f + fh * amount, 0, 1.0f - amount, 1, 1,
                      {arm_col.r * dim, arm_col.g * dim, arm_col.b * dim, 1});
        }
    }

    if (st.rail_meter) {
        // RailcannonChargePanel: a 46 x 100 panel hung on the right of the stats panel with a lightning bolt
        // in it. The bolt fills from the bottom by charge / 4 in the charging colour (red) over a dark copy
        // of itself; full, it is the full colour (blue), flashing white as it gets there.
        Box panel = child(stats, rt(1, 0.5f, 1, 0.5f, 47, 73.75f, 46, 100, 1, 0.5f));
        image_sliced(sf, panel, g.sprite("Round_FillLarge"), PANEL2);
        Box bolt = child(panel, rt(0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 36, 90));
        int spr = g.sprite("lightningboltbigvector");
        bool full = st.rail_charge > 4.0f;
        float amount = full ? 1.0f : fminf(fmaxf(st.rail_charge / 4.0f, 0.0f), 1.0f), k = full ? fminf(fmaxf(st.rail_flash, 0.0f), 1.0f) : 0.0f;
        Color col = full ? Color{0.251f + 0.749f * k, 0.906f + 0.094f * k, 1, 1} : Color{1, 0, 0, 1};
        if (spr >= 0) {
            if (!full) image_simple(sf, bolt, spr, {0, 0, 0, 0.6784f});
            if (amount > 0) rect_quad(sf, bolt.xf, g.sprites[spr].tex, bolt.x0, bolt.y0, bolt.x1, bolt.y0 + bolt.h() * amount, 0, 1.0f - amount, 1, 1, col);
        } else {
            rect_quad(sf, bolt.xf, -1, bolt.x0, bolt.y0, bolt.x1, bolt.y0 + bolt.h() * amount, 0, 0, 1, 1, col);
        }
    }

    Box gun = child(canvas, rt(0, 0, 0, 0, -79, 187, 200, 100, 0, 0, 4, 4));
    image_sliced(sf, gun, g.sprite("Round_FillLarge"), PANEL);
    {
        Box icon = child(child(gun, rt(0, 0, 1, 1, 0, 0, -10, -10)), FILL);
        // each weapon and variation has its own drawing; the shotgun's first variation is the Core Eject
        static const char *const rev_icons[3] = {"SingleRevolver", "RevolverSpecial", "RevolverSharp"};
        static const char *const rev_glows[3] = {"SingleRevolverGlow", "RevolverSpecialGlow", "RevolverSharpGlow"};
        int var = st.variation < 0 ? 0 : st.variation > 2 ? 2 : st.variation;
        static const char *const alt_icons[3] = {"RevolverAltSingle", "RevolverAltSpecial", "RevolverAltSharp"};
        static const char *const alt_glows[3] = {"RevolverAltSingleGlow", "RevolverAltSpecialGlow", "RevolverAltSharpGlow"};
        const char *icon_name = rev_icons[var], *glow_name = rev_glows[var];
        if (st.weapon == 1) {
            icon_name = st.weapon_var == 1 ? "Shotgun1" : "Shotgun";
            glow_name = st.weapon_var == 1 ? "Shotgun1Glow" : "ShotgunGlow";
        } else if (st.weapon == 2) {
            icon_name = st.weapon_var == 1 ? "railcannonmalicious" : "Railcannon";
            glow_name = st.weapon_var == 1 ? "railcannonmaliciousglow" : "RailcannonGlow";
        } else if (st.weapon == 3) {
            icon_name = st.weapon_var == 1 ? "SawbladeLauncherOverheat" : "SawbladeLauncher";
            glow_name = st.weapon_var == 1 ? "SawbladeLauncherOverheatGlow" : "SawbladeLauncherGlow";
        } else if (st.weapon == 4) {
            icon_name = st.weapon_var == 1 ? "rocketlaunchercannon" : "rocketlauncher";
            glow_name = st.weapon_var == 1 ? "rocketlaunchercannonglow" : "rocketlauncherglow";
        } else if (st.alt) {
            icon_name = alt_icons[var];
            glow_name = alt_glows[var];
        }
        int rev = g.sprite(icon_name);
        int glow = g.sprite(glow_name);
        if (rev < 0) { rev = g.sprite("SingleRevolver"); glow = g.sprite("SingleRevolverGlow"); }
        // ULTRAKILL fits the drawing and its glow into this box separately. The glow sprite is the same
        // drawing with a soft margin, so here both are drawn at the scale at which the glow fits: the
        // drawing stays inside its glow and nothing reaches past the box (v0.59 fitted the drawing and
        // let the glow spill over the panel's edge on every icon but the Marksman's).
        float s = rev >= 0 ? fminf(icon.w() / g.textures[g.sprites[rev].tex].w, icon.h() / g.textures[g.sprites[rev].tex].h) : 0;
        if (glow >= 0) s = fminf(s, fminf(icon.w() / g.textures[g.sprites[glow].tex].w, icon.h() / g.textures[g.sprites[glow].tex].h));
        // The three revolver drawings are the same gun at the same size in their pictures, but the
        // Marksman's picture is taller (it has the coin over the gun), so fitted one by one the other two
        // came out a third larger than it. They are held to the Marksman's scale (Davi's comparison with
        // ULTRAKILL: the Marksman's size is the right one).
        int ref = g.sprite(st.weapon == 0 && st.alt ? "RevolverAltSpecialGlow" : "RevolverSpecialGlow");
        if (ref >= 0) s = fminf(s, fminf(icon.w() / g.textures[g.sprites[ref].tex].w, icon.h() / g.textures[g.sprites[ref].tex].h));
        // ULTRAKILL tints the icon with the variation's colour: blue, green, red
        // (variation 0 blue, 1 green, 2 red: the Pump Charge is the shotgun's 1, the Malicious the railcannon's 2)
        const Color tint = st.weapon == 1 ? (st.weapon_var == 1 ? MARKSMAN_GREEN : PIERCER_BLUE)
                           : st.weapon == 2 ? (st.weapon_var == 1 ? SHARP_RED : PIERCER_BLUE)
                           : st.weapon == 3 || st.weapon == 4 ? (st.weapon_var == 1 ? MARKSMAN_GREEN : PIERCER_BLUE)
                           : var == 1 ? MARKSMAN_GREEN : var == 2 ? SHARP_RED : PIERCER_BLUE;
        image_simple(sf, icon, rev, tint, false, s);
        image_simple(sf, icon, glow, {tint.r, tint.g, tint.b, 0.749f}, false, s);
        // (ours) a line along the bottom of the weapon's picture, for what the weapon's own displays would say
        if (st.weapon_note[0]) text(sf, child(icon, rt(0, 0, 1, 0, 0, 2, 0, 16, 0.5f, 0)), st.weapon_note, 13.0f, {1, 1, 1, 0.9f}, 1);
    }

    // ---- style meter: "StyleCanvas", the same kind of world-space canvas on the right (882 x 496 at
    // (1.3, 0.3, 1), turned 30 degrees the other way, scale 0.001 x 0.002). Its right-hand panel is 442
    // wide: a 150-high strip with the rank letter, the meter under it, and the list of bonuses below.
    if (st.style_rank >= 0) {
        Surface ss{};
        ss.world = true;
        ss.pos[0] = 1.3f + st.hud_sway[0];
        ss.pos[1] = 0.3f + st.hud_sway[1];
        ss.pos[2] = 1.0f + st.hud_sway[2];
        ss.yaw = 30.0f * 3.14159265f / 180.0f;
        ss.scale[0] = 0.001f;
        ss.scale[1] = 0.002f;
        ss.proj_y = 1.0f;
        ss.proj_x = screen_h / screen_w;
        Box scanvas{{0, 0, 1, 1, 0}, -441, -248, 441, 248};
        Box panel = child(scanvas, rt(0, 0, 1, 1, 0, 0, -440, 0));
        Box strip = child(panel, rt(0, 1, 1, 1, 0, 0, 0, 150, 0.5f, 1));
        rect_quad(ss, strip.xf, -1, strip.x0, strip.y0, strip.x1, strip.y1, 0, 0, 1, 1, {0, 0, 0, 0.5f});
        static const char *const rank_sprites[8] = {"RankD", "RankC", "RankB", "RankA", "RankS", "RankSS", "RankSSS", "RankU"};
        image_simple(ss, child(strip, rt(0, 0, 1, 1, 0, 0, -30, -30)), g.sprite(rank_sprites[st.style_rank > 7 ? 7 : st.style_rank]), {1, 1, 1, 1}, true);
        Box slider = child(strip, rt(0, 0.5f, 1, 0.5f, 0, -93.0003f, 0, 50));
        rect_quad(ss, slider.xf, -1, slider.x0, slider.y0, slider.x1, slider.y1, 0, 0, 1, 1, {0, 0, 0, 0.6863f});
        float fill = fminf(fmaxf(st.style_meter, 0.0f), 1.0f);
        Box bar = child(slider, rt(0, 0, fill, 1, 0, 0, 0, 0));
        rect_quad(ss, bar.xf, -1, bar.x0, bar.y0, bar.x1, bar.y1, 0, 0, 1, 1, {1, 1, 1, 1});
        Box list = child(panel, rt(0, 0, 1, 1, 0, -90, 0, -190));
        rect_quad(ss, list.xf, -1, list.x0, list.y0, list.x1, list.y1, 0, 0, 1, 1, {0, 0, 0, 0.5f});
        // one bonus per line, newest at the bottom, 48 units tall as the original's text
        for (int i = 0; i < st.style_line_count && i < HudState::STYLE_LINES; i++) {
            const HudState::StyleLine &ln = st.style_lines[i];
            Box row = child(list, rt(0, 1, 1, 1, 0, -12.0f - 48.0f * i, -24, 48, 0.5f, 1));
            text(ss, row, ln.text, 44.0f, {ln.r, ln.g, ln.b, 1}, 0);
        }
    }

    // ---- crosshair: the screen-space canvas (reference 1280x720, "expand")
    Surface sc{};
    sc.world = false;
    sc.screen_w = screen_w;
    sc.screen_h = screen_h;
    sc.px_per_unit = fminf(screen_w / 1280.0f, screen_h / 720.0f);
    Box screen{{0, 0, 1, 1, 0}, -screen_w * 0.5f / sc.px_per_unit, -screen_h * 0.5f / sc.px_per_unit, screen_w * 0.5f / sc.px_per_unit,
               screen_h * 0.5f / sc.px_per_unit};
    Box dot = child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 5, 5));
    image_sliced(sf.world ? sc : sc, dot, g.sprite("meter"), {1, 1, 1, 1}, 1.0f);

    // the arcs around the crosshair appear while health or stamina is changing or not full, then fade
    bool changed = fabsf(st.health - anim.prev_health) > 0.01f || fabsf(st.stamina - anim.prev_stamina) > 0.01f;
    anim.prev_health = st.health;
    anim.prev_stamina = st.stamina;
    if (changed || st.stamina < 299.5f) anim.cross_until = st.time + 1.0;
    float target = st.time < anim.cross_until ? 1.0f : 0.0f;
    anim.cross_alpha += (target - anim.cross_alpha) * fminf(1.0f, dt * 8.0f);
    float a = anim.cross_alpha;
    if (a > 0.01f) {
        int ring = g.sprite("circlethick");
        float hp = fminf(fmaxf(anim.shown_health, 0.0f), 100.0f) / 100.0f, after = fminf(anim.after_health, 100.0f) / 100.0f;
        Box left = child(dot, rt(0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 32, 32, 0.5f, 0.5f, 1, 1, -3.0f));
        image_radial(sc, left, ring, {1, 0, 0, a}, 0.485f * after, true);
        image_radial(sc, left, ring, {0.2667f, 1, 0.2706f, a}, 0.485f * hp, true);
        const float rot[3] = {3.0f, 62.5f, 122.0f};
        for (int i = 0; i < 3; i++) {
            float part = fminf(fmaxf((st.stamina - 100.0f * i) / 100.0f, 0.0f), 1.0f);
            Box arc = child(dot, rt(0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 32, 32, 0.5f, 0.5f, 1, 1, rot[i]));
            Color col = part < 1.0f ? Color{1, 0, 0, a} : Color{1, 1, 1, a};
            image_radial(sc, arc, ring, col, 0.15f * part, false);
        }
    }
    // From here on, but for the boss's health, it is the world's: trails, beams, coins, sparks, explosions.
    // They are built after the HUD's panels but drawn before them (hud_draw), so a trail or a blast that
    // crosses a panel passes behind it, as everything does in ULTRAKILL, whose HUD has a camera of its own.
    // (Until v0.77 they lay over the panels.)
    g_ui_world = 1;
    // Trails: each run of points is one strip. A point's two corners are shared by the piece before it and
    // the piece after, set across the bend there (the mean of the two pieces' own directions across, made
    // longer so the strip keeps its width); only at a turn sharper than about 140 degrees, a saw coming back
    // off a wall, does each piece keep corners of its own. (v0.76 drew every piece as a rectangle of its own:
    // round a magnet they overlapped on the inside of the curve and gaped on the outside, and looked like
    // a row of cards.)
    for (int ri = 0; ri < st.ribbon_count && ri < HudState::MAX_RIBBONS; ri++) {
        const HudState::Ribbon &rb = st.ribbons[ri];
        if (rb.count < 2 || rb.first < 0 || rb.first + rb.count > HudState::MAX_RIBBON_POINTS) continue;
        const HudState::RibbonPoint *pt = st.ribbon_points + rb.first;
        float hw = screen_w * 0.5f / sc.px_per_unit, hh = screen_h * 0.5f / sc.px_per_unit;
        g_ui_add = rb.additive ? 1 : 0;
        // the direction across each piece (zero for one with no length)
        static float nx[HudState::MAX_RIBBON_POINTS], ny[HudState::MAX_RIBBON_POINTS];
        for (int i = 0; i + 1 < rb.count; i++) {
            float dx = (pt[i + 1].x - pt[i].x) * hw, dy = (pt[i + 1].y - pt[i].y) * hh, len = sqrtf(dx * dx + dy * dy);
            nx[i] = len > 0.01f ? -dy / len : 0.0f;
            ny[i] = len > 0.01f ? dx / len : 0.0f;
        }
        // (a piece with no length borrows its neighbour's)
        for (int i = 1; i + 1 < rb.count; i++)
            if (nx[i] == 0 && ny[i] == 0) { nx[i] = nx[i - 1]; ny[i] = ny[i - 1]; }
        for (int i = rb.count - 3; i >= 0; i--)
            if (nx[i] == 0 && ny[i] == 0) { nx[i] = nx[i + 1]; ny[i] = ny[i + 1]; }
        // corner(i, piece): the direction across the strip at point i, as piece `piece` (i - 1 or i) uses it
        auto corner = [&](int i, int piece, float &ox, float &oy) {
            int before = i - 1, after = i;
            ox = nx[piece];
            oy = ny[piece];
            if (before < 0 || after > rb.count - 2) return;
            float mx = nx[before] + nx[after], my = ny[before] + ny[after], ml = sqrtf(mx * mx + my * my);
            if (ml < 0.7f) return;                        // a sharp turn back: no shared corner
            mx /= ml;
            my /= ml;
            float along = mx * nx[piece] + my * ny[piece], grow = along > 0.5f ? 1.0f / along : 2.0f;
            ox = mx * grow;
            oy = my * grow;
        };
        for (int i = 0; i + 1 < rb.count; i++) {
            const HudState::RibbonPoint &a = pt[i], &b = pt[i + 1];
            if (nx[i] == 0 && ny[i] == 0) continue;
            float ax = a.x * hw, ay = a.y * hh, bx = b.x * hw, by = b.y * hh, wa = a.w * 2.0f * hh, wb = b.w * 2.0f * hh, oax, oay, obx, oby;
            corner(i, i, oax, oay);
            corner(i + 1, i, obx, oby);
            const float lx[4] = {ax - oax * wa, ax + oax * wa, bx + obx * wb, bx - obx * wb};
            const float ly[4] = {ay - oay * wa, ay + oay * wa, by + oby * wb, by - oby * wb};
            const float uv[4] = {0, 0, 0, 0};
            UINT first = (UINT)g.verts.size();
            push_quad(sc, screen.xf, -1, 0, lx, ly, uv, uv, {a.r, a.g, a.b, a.a});
            for (UINT k : {2u, 4u, 5u}) {                 // the quad's vertices 2, 4 and 5 are its far end
                g.verts[first + k].col[0] = b.r;
                g.verts[first + k].col[1] = b.g;
                g.verts[first + k].col[2] = b.b;
                g.verts[first + k].col[3] = b.a;
            }
            if (a.z > 0 && b.z > 0)
                for (UINT k = 0; k < 6; k++) g.verts[first + k].inv_z = 1.0f / (k == 2 || k == 4 || k == 5 ? b.z : a.z);
        }
        g_ui_add = 0;
    }
    // tracers: a bright core inside a wider, fainter glow, each a quad along the projected line
    for (int i = 0; i < st.tracer_count && i < HudState::MAX_TRACERS; i++) {
        const HudState::TracerLine &t = st.tracers[i];
        float hw = screen_w * 0.5f / sc.px_per_unit, hh = screen_h * 0.5f / sc.px_per_unit;
        float ax = t.x0 * hw, ay = t.y0 * hh, bx = t.x1 * hw, by = t.y1 * hh;
        float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
        if (len < 0.01f) continue;
        float px = -dy / len, py = dx / len;
        if (t.sprite || t.grad) {
            int tex = -1;
            if (t.sprite) {
                int spr = g.sprite(t.sprite);
                if (spr < 0) continue;
                tex = g.sprites[spr].tex;
            }
            float wa = t.w0 * 2.0f * hh, wb = t.w1 * 2.0f * hh;
            const float lx[4] = {ax - px * wa, ax + px * wa, bx + px * wb, bx - px * wb};
            const float ly[4] = {ay - py * wa, ay + py * wa, by + py * wb, by - py * wb};
            const float u[4] = {t.u0, t.u0, t.u1, t.u1}, v[4] = {1, 0, 0, 1};
            Color c0 = {t.r, t.g, t.b, t.a}, c1 = t.grad ? Color{t.r1, t.g1, t.b1, t.a1} : c0;
            UINT first = (UINT)g.verts.size();
            push_quad(sc, screen.xf, tex, 0, lx, ly, u, v, c0);
            // the quad's six vertices are corners 0 1 2 0 2 3: 2 and 3 are the far end
            for (UINT k : {2u, 4u, 5u}) {
                g.verts[first + k].col[0] = c1.r;
                g.verts[first + k].col[1] = c1.g;
                g.verts[first + k].col[2] = c1.b;
                g.verts[first + k].col[3] = c1.a;
            }
            if (t.z0 > 0 && t.z1 > 0)
                for (UINT k = 0; k < 6; k++) g.verts[first + k].inv_z = 1.0f / (k == 2 || k == 4 || k == 5 ? t.z1 : t.z0);
            continue;
        }
        UINT plain_first = (UINT)g.verts.size();
        for (int pass = t.plain ? 1 : 0; pass < 2; pass++) {
            float k = (pass == 0 ? 3.0f : 1.0f) * 2.0f * hh;       // half-width fraction of the height -> canvas units
            float wa = t.w0 * k, wb = t.w1 * k;
            const float lx[4] = {ax - px * wa, ax + px * wa, bx + px * wb, bx - px * wb};
            const float ly[4] = {ay - py * wa, ay + py * wa, by + py * wb, by - py * wb};
            const float uv[4] = {0, 0, 0, 0};
            Color c = pass == 0 ? Color{t.r, t.g, t.b, t.a * 0.25f} : t.plain ? Color{t.r, t.g, t.b, t.a} : Color{fminf(t.r + 0.5f, 1.0f), fminf(t.g + 0.5f, 1.0f), fminf(t.b + 0.5f, 1.0f), t.a};
            push_quad(sc, screen.xf, -1, 0, lx, ly, uv, uv, c);
        }
        if (t.z0 > 0 && t.z1 > 0)
            for (UINT k = plain_first; k < (UINT)g.verts.size(); k++) {
                UINT corner = (k - plain_first) % 6;
                g.verts[k].inv_z = 1.0f / (corner == 2 || corner == 4 || corner == 5 ? t.z1 : t.z0);
            }
    }
    // coins in flight: a gold ring that narrows and widens as the coin spins, white during the split window
    for (int i = 0; i < st.coin_count && i < HudState::MAX_COINS; i++) {
        const HudState::CoinDot &c = st.coins[i];
        float d = c.size * screen_h / sc.px_per_unit;
        float squash = fmaxf(fabsf(cosf(c.phase)), 0.18f);
        Box coin = child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, c.x * screen_w * 0.5f / sc.px_per_unit, c.y * screen_h * 0.5f / sc.px_per_unit, d, d, 0.5f,
                                    0.5f, squash, 1));
        Color col = c.flash ? Color{1, 1, 1, 1} : Color{1, 0.82f, 0.18f, 1};
        image_radial(sc, coin, g.sprite("circlethick"), col, 1.0f, true);
        image_sliced(sc, child(coin, rt(0.5f, 0.5f, 0.5f, 0.5f, 0, 0, d * 0.45f, d * 0.45f)), g.sprite("meter"), col, 1.0f);
    }
    g_ui_world = 0;
    // boss health ("Boss Healths"): a 1200 x 56 panel 10 under the top edge, the bar inside it, the name
    // over the bar. Behind the red there is a slower orange bar that shows what was just lost.
    {
        static float after[HudState::MAX_BOSSES] = {1, 1};
        for (int i = 0; i < st.boss_count && i < HudState::MAX_BOSSES; i++) {
            const HudState::Boss &b = st.bosses[i];
            float hp = fminf(fmaxf(b.hp, 0.0f), 1.0f);
            after[i] = after[i] < hp ? hp : fmaxf(hp, after[i] - 0.3f * dt);
            Box panel = child(screen, rt(0.5f, 1, 0.5f, 1, 0, -10.0f - 57.0f * i, 1200, 56, 0.5f, 1));
            image_sliced(sc, panel, g.sprite("Round_FillLarge"), {0, 0, 0, 0.3922f});
            Box slider = child(panel, rt(0, 0.5f, 1, 0.5f, 0, 0, -16, 40));
            Box track = child(slider, rt(0, 0.25f, 1, 0.75f, -5, 0, -20, 20.604f));
            image_sliced(sc, track, g.sprite("Round_FillLarge"), {0, 0, 0, 0.6863f});
            Box area = child(slider, rt(0, 0.25f, 1, 0.75f, 0, 0, 0, 20.604f));
            if (after[i] > hp) image_sliced(sc, child(area, rt(0, 0, after[i], 1, 0, 0, 0, 0)), g.sprite("Round_FillSmall"), {1, 0.3931f, 0, 1});
            if (hp > 0) image_sliced(sc, child(area, rt(0, 0, hp, 1, 0, 0, 0, 0)), g.sprite("Round_FillSmall"), {1, 0, 0, 1});
            text(sc, child(slider, rt(0, 0.5f, 1, 0.5f, 0, 0.396f, 0, 25)), b.name, 28.4f, {1, 1, 1, 1}, 1);
        }
        for (int i = st.boss_count; i < HudState::MAX_BOSSES; i++) after[i] = 1.0f;
    }
    g_ui_world = 1;
    // effect sprites (sparks, rings, flashes), where the DLL has projected them
    for (int i = 0; i < st.fx_sprite_count && i < HudState::MAX_FX_SPRITES; i++) {
        const HudState::FxSprite &fx = st.fx_sprites[i];
        float d = fx.size * screen_h / sc.px_per_unit;
        Box b = child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, fx.x * screen_w * 0.5f / sc.px_per_unit, fx.y * screen_h * 0.5f / sc.px_per_unit, d, d, 0.5f, 0.5f, 1, 1, fx.rot));
        UINT fx_first = (UINT)g.verts.size();
        image_simple(sc, b, fx.sprite ? g.sprite(fx.sprite) : -1, {fx.r, fx.g, fx.b, fx.a});
        world_depth(fx_first, fx.z);
    }
    // explosions and glowing projectiles: a soft ball, and a shock ring that runs ahead of it
    for (int i = 0; i < st.blast_count && i < HudState::MAX_BLASTS; i++) {
        const HudState::Blast &b = st.blasts[i];
        UINT blast_first = (UINT)g.verts.size();
        float cx = b.x * screen_w * 0.5f / sc.px_per_unit, cy = b.y * screen_h * 0.5f / sc.px_per_unit;
        float d = b.size * screen_h / sc.px_per_unit;
        // the ring first, so the ball covers its middle; the glow sprite fades to nothing at its edge, so
        // it is drawn larger than the ball it stands for
        if (b.ring > 0) {
            float rd = b.ring * screen_h / sc.px_per_unit;
            image_simple(sc, child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, cx, cy, rd, rd)), g.sprite("Shockwave"), {1, 0.9f, 0.7f, b.a * 0.35f});
        }
        Box ball = child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, cx, cy, d * 2.2f, d * 2.2f));
        image_simple(sc, ball, g.sprite("softglow"), {b.r, b.g, b.b, b.a});
        Box core = child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, cx, cy, d * 1.2f, d * 1.2f));
        image_simple(sc, core, g.sprite("softglow"), {1, 0.95f, 0.6f, b.a});
        world_depth(blast_first, b.z);
    }
    {
        // the muzzle flash, over the barrel for a few frames
        float since = (float)(st.time - st.muzzle_flash);
        if (since >= 0 && since < 0.07f && g_muzzle_known && st.show_viewmodel) {
            float d = (st.weapon == 1 ? 0.34f : st.weapon == 2 ? 0.40f : 0.22f) * screen_h / sc.px_per_unit * (1.0f - since * 5.0f);
            Box fl = child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, g_muzzle[0] * screen_w * 0.5f / sc.px_per_unit, g_muzzle[1] * screen_h * 0.5f / sc.px_per_unit,
                                      d, d, 0.5f, 0.5f, 1, 1, (float)((int)(st.muzzle_flash * 1000.0) % 90)));
            image_simple(sc, fl, g.sprite(st.weapon == 1 ? "muzzleflashshotgun" : "muzzleflash"), {1, 1, 1, 1.0f - since * 8.0f});
        }
    }
    g_ui_world = 0;
    // Blood on the screen ('ScreenBlood', a square half the canvas's height on a side; its material multiplies
    // what is behind it by its dark red, which is drawn here as that red laid over at the picture's alpha).
    {
        static const char *const SPLATS[5] = {"Bloodsplatter6", "Bloodsplatter7", "Bloodsplatter8", "Bloodsplatter9", "Bloodsplatter10"};
        float side = (screen.y1 - screen.y0) * 0.5f;
        for (int i = 0; i < st.screen_blood_count && i < HudState::MAX_SCREEN_BLOOD; i++) {
            const HudState::ScreenBloodMark &b = st.screen_blood[i];
            int spr = g.sprite(SPLATS[b.sprite < 0 ? 0 : b.sprite > 4 ? 4 : b.sprite]);
            if (spr < 0) continue;
            image_simple(sc, child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, b.x, b.y, side, side)), spr, {0.6765f * 0.6f, 0, 0, fminf(b.alpha * 1.6f, 1.0f)});
        }
    }
    if (st.flash > 0.001f) rect_quad(sc, screen.xf, -1, screen.x0, screen.y0, screen.x1, screen.y1, 0, 0, 1, 1, {1, 1, 1, fminf(st.flash, 1.0f) * 0.4902f});
}

// ---------------------------------------------------------------- viewmodel

void mul44(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            r[i * 4 + j] = 0;
            for (int k = 0; k < 4; k++) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
        }
    memcpy(out, r, sizeof(r));
}

// row-vector convention: v' = v * M
void identity(float m[16]) {
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
}
void translate(float m[16], float x, float y, float z) {
    identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}
void rotate_x(float m[16], float a) {
    identity(m);
    m[5] = cosf(a);
    m[6] = sinf(a);
    m[9] = -sinf(a);
    m[10] = cosf(a);
}
void rotate_z(float m[16], float a) {
    identity(m);
    m[0] = cosf(a);
    m[1] = sinf(a);
    m[4] = -sinf(a);
    m[5] = cosf(a);
}

struct MeshConsts {
    float mvp[16], world[16], light[4], mode[4], tint[4], uvoff[4], emis[4], dpar[4];
};
static_assert(sizeof(MeshConsts) == 224, "must match the shader's cbuffer and the buffer's size");

// How the model being drawn is lit up and moved by the weapon's own code, set by draw_viewmodel around
// its draw_model call.
//   Lights: a mesh whose material has an emissive picture gets it added in the variation's colour
//   (WeaponIcon sets _EmissiveColor on the renderers listed in its variationColoredRenderers: the standard
//   revolver's cylinder, the alternate revolver's cylinder and body; other meshes keep white).
//   Railcannon.SetMaterialIntensity: its body's lights are as strong as the charge (a fifth each), and each
//   of the five pips (Pips_1..4, BigPip) fades in over its own fifth; all are full when the gun is.
//   RailCannonPip: the eight capsules along the prongs and the main one each have a charge level (1..4, 5).
//   Below it the capsule sits pushed 0.03 (0.05) out of place; from it on it is back and spins about its
//   own axis at 2400 degrees a second.
//   RevolverCylinder: the cylinder is turned about its axis (0, -1, 0) by `cylinder`.
//   Spin, on the sawblade launcher's 'Blade': the blade in its jaws turns about its own z, as fast as
//   Nailgun.Update says (the DLL adds the angle up).
struct ModelLook {
    bool on = false, rail = false, alt = false;
    float charge = 0, rgb[3] = {1, 1, 1}, time = 0, cylinder = 0, blade = 0;
    MeshConsts base;
} g_look;
// t (position 3, rotation xyzw, scale 3) turned by `angle` about its own y axis
void turn_y(float *t, float angle) {
    float s = sinf(angle * 0.5f), c = cosf(angle * 0.5f), x = t[3], y = t[4], z = t[5], w = t[6];
    t[3] = x * c - z * s;
    t[4] = w * s + y * c;
    t[5] = x * s + z * c;
    t[6] = w * c - y * s;
}
void turn_z(float *t, float angle) {
    float s = sinf(angle * 0.5f), c = cosf(angle * 0.5f), x = t[3], y = t[4], z = t[5], w = t[6];
    t[3] = x * c + y * s;
    t[4] = y * c - x * s;
    t[5] = w * s + z * c;
    t[6] = w * c - z * s;
}
void look_node(const Model &m, int n, float *t) {
    const std::string &nm = m.node_names[n];
    if (g_look.blade != 0 && nm == "Blade") {
        turn_z(t, g_look.blade);
    } else if (g_look.rail) {
        float level = 0, push = 0;
        if (nm == "Main_Capsule") { level = 5; push = -5; }
        else if (nm.size() == 18 && nm.compare(0, 13, "ProngCapsule_") == 0) { level = (float)(nm[17] - '0'); push = nm[13] == 'L' ? 3.0f : -3.0f; }
        if (level <= 0) return;
        if (g_look.charge >= level) turn_y(t, -fmodf(g_look.time * 2400.0f, 360.0f) * 3.14159265f / 180.0f);
        else t[0] += push * 0.01f;
    } else if (g_look.cylinder != 0 && nm == "Cylinder_Bone") {
        turn_y(t, -g_look.cylinder);
    }
}


// 3x4 transforms in the usual column-vector form (p' = R p + t), rows stored one after the other.
void affine_mul(const float *a, const float *b, float *out) {      // apply b, then a
    float r[12];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        r[i * 4 + 3] = a[i * 4] * b[3] + a[i * 4 + 1] * b[7] + a[i * 4 + 2] * b[11] + a[i * 4 + 3];
    }
    memcpy(out, r, sizeof(r));
}
void trs_to_affine(const float *t, float *m) {                     // position 3, rotation xyzw, scale 3
    float x = t[3], y = t[4], z = t[5], w = t[6], sx = t[7], sy = t[8], sz = t[9];
    m[0] = (1 - 2 * (y * y + z * z)) * sx; m[1] = 2 * (x * y - z * w) * sy;     m[2] = 2 * (x * z + y * w) * sz;     m[3] = t[0];
    m[4] = 2 * (x * y + z * w) * sx;     m[5] = (1 - 2 * (x * x + z * z)) * sy; m[6] = 2 * (y * z - x * w) * sz;     m[7] = t[1];
    m[8] = 2 * (x * z - y * w) * sx;     m[9] = 2 * (y * z + x * w) * sy;     m[10] = (1 - 2 * (x * x + y * y)) * sz; m[11] = t[2];
}
// One node's position, rotation and scale between two stored poses (rotation by the shorter way round).
void blend_trs(const float *p, const float *q, float mix, float *t) {
    for (int k = 0; k < 3; k++) t[k] = p[k] + (q[k] - p[k]) * mix;
    float dot = p[3] * q[3] + p[4] * q[4] + p[5] * q[5] + p[6] * q[6], sign = dot < 0 ? -1.0f : 1.0f, len = 0;
    for (int k = 3; k < 7; k++) { t[k] = p[k] + (q[k] * sign - p[k]) * mix; len += t[k] * t[k]; }
    len = len > 1e-12f ? 1.0f / sqrtf(len) : 1.0f;
    for (int k = 3; k < 7; k++) t[k] *= len;
    for (int k = 7; k < 10; k++) t[k] = p[k] + (q[k] - p[k]) * mix;
}
// The two stored frames around `time` seconds into `clip`, and how far between them (the rest pose without a clip).
void clip_frames(const Model &m, const Clip *clip, float time, bool loop, const float *&a, const float *&b, float &mix) {
    a = b = m.rest.data();
    mix = 0;
    if (!clip || clip->frames == 0) return;
    float length = (clip->frames - 1) / clip->fps;
    if (loop && length > 0) time = fmodf(time, length);
    float f = fminf(fmaxf(time * clip->fps, 0.0f), (float)(clip->frames - 1));
    UINT i0 = (UINT)f, i1 = i0 + 1 < clip->frames ? i0 + 1 : i0;
    mix = f - i0;
    a = clip->data.data() + (size_t)i0 * m.nodes * 10;
    b = clip->data.data() + (size_t)i1 * m.nodes * 10;
}
// Every node's transform in the model's own space at `time` seconds into `clip`. With a `base` clip the
// pose is `weight` of the way from the base's pose to the clip's, which scales a motion down.
// `spin` turns one node: about its own z axis, or with `spin_axis` 1 the way the alternate revolver's spin
// does. ULTRAKILL sets the bone's rotation to Euler(its Euler angles + axis * angle), with the axis
// forward (z) for the standard revolver and left (-x) for the alternate one. Unity's Euler order is z,
// then x, then y, so adding to z is a turn about the bone's own z, and taking the angle off x is a turn by
// that much about the x axis as it stands before the bone's z turn: q * (qz^-1 * qx(-angle) * qz).
void pose_model(const Model &m, const Clip *clip, float time, bool loop, std::vector<float> &world, const Clip *base = nullptr,
                float base_time = 0, float weight = 1.0f, int spin_node = -1, float spin = 0, int spin_axis = 0) {
    world.resize((size_t)m.nodes * 12);
    const float *a, *b, *ba, *bb;
    float mix, bmix;
    clip_frames(m, clip, time, loop, a, b, mix);
    if (base) clip_frames(m, base, base_time, true, ba, bb, bmix);
    for (int n = 0; n < m.nodes; n++) {
        float t[10];
        blend_trs(a + n * 10, b + n * 10, mix, t);
        if (base) {
            float bt[10], full[10];
            blend_trs(ba + n * 10, bb + n * 10, bmix, bt);
            memcpy(full, t, sizeof(full));
            blend_trs(bt, full, weight, t);
        }
        if (n == spin_node && spin != 0 && spin_axis == 1) {
            float x = t[3], y = t[4], z = t[5], w = t[6];
            float ez = atan2f(2.0f * (x * y + w * z), 1.0f - 2.0f * (x * x + z * z));
            auto qmul = [](const float *a, const float *b, float *o) {   // x y z w
                o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
                o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
                o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
                o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
            };
            const float qz[4] = {0, 0, sinf(ez * 0.5f), cosf(ez * 0.5f)}, qzi[4] = {0, 0, -qz[2], qz[3]};
            const float qx[4] = {sinf(-spin * 0.5f), 0, 0, cosf(spin * 0.5f)}, q[4] = {x, y, z, w};
            float a[4], r[4], out[4];
            qmul(qzi, qx, a);
            qmul(a, qz, r);
            qmul(q, r, out);
            memcpy(t + 3, out, sizeof(out));
        } else if (n == spin_node && spin != 0) {
            float s = sinf(spin * 0.5f), c = cosf(spin * 0.5f), x = t[3], y = t[4], z = t[5], w = t[6];
            t[3] = x * c + y * s;
            t[4] = y * c - x * s;
            t[5] = w * s + z * c;
            t[6] = w * c - z * s;
        }
        if (g_look.on) look_node(m, n, t);
        float local[12];
        trs_to_affine(t, local);
        int parent = m.parent[n];
        if (parent >= 0 && parent < n) affine_mul(&world[(size_t)parent * 12], local, &world[(size_t)n * 12]);
        else memcpy(&world[(size_t)n * 12], local, sizeof(local));
    }
}
// Moves each vertex by its bones (bone transform times bind matrix, blended by weight) and uploads the result.
void skin_mesh(ID3D11DeviceContext *ctx, SkinMesh &sm, const std::vector<float> &world) {
    size_t bones = sm.bone_node.size();
    std::vector<float> mats(bones * 12);
    for (size_t b = 0; b < bones; b++) affine_mul(&world[(size_t)sm.bone_node[b] * 12], &sm.bind[b * 12], &mats[b * 12]);
    for (UINT v = 0; v < sm.vertex_count; v++) {
        const float *src = &sm.verts[(size_t)v * 8];
        float *dst = &sm.skinned[(size_t)v * 8];
        float p[3] = {0, 0, 0}, nrm[3] = {0, 0, 0};
        for (int k = 0; k < 4; k++) {
            float w = sm.weights[(size_t)v * 4 + k];
            if (w <= 0) continue;
            const float *mt = &mats[(size_t)sm.slots[(size_t)v * 4 + k] * 12];
            for (int i = 0; i < 3; i++) {
                p[i] += w * (mt[i * 4] * src[0] + mt[i * 4 + 1] * src[1] + mt[i * 4 + 2] * src[2] + mt[i * 4 + 3]);
                nrm[i] += w * (mt[i * 4] * src[3] + mt[i * 4 + 1] * src[4] + mt[i * 4 + 2] * src[5]);
            }
        }
        float nl = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
        nl = nl > 1e-9f ? 1.0f / nl : 1.0f;
        dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2];
        dst[3] = nrm[0] * nl; dst[4] = nrm[1] * nl; dst[5] = nrm[2] * nl;
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (sm.vb && SUCCEEDED(ctx->Map(sm.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        memcpy(ms.pData, sm.skinned.data(), sm.skinned.size() * 4);
        ctx->Unmap(sm.vb, 0);
    }
}
std::vector<float> g_pose;               // the last model's node transforms, as draw_model left them
void upload_consts(ID3D11DeviceContext *ctx, const MeshConsts &c);
// `at_node` (if not negative) gets that node's position in the model's space, for finding the muzzle.
void draw_model(ID3D11DeviceContext *ctx, Model &m, const Clip *clip, float time, bool loop, const Clip *base = nullptr, float base_time = 0,
                float weight = 1.0f, int spin_node = -1, float spin = 0, int at_node = -1, float *at = nullptr, int spin_axis = 0) {
    std::vector<float> &world = g_pose;
    pose_model(m, clip, time, loop, world, base, base_time, weight, spin_node, spin, spin_axis);
    if (at && at_node >= 0 && at_node < m.nodes)
        for (int i = 0; i < 3; i++) at[i] = world[(size_t)at_node * 12 + i * 4 + 3];
    for (SkinMesh &sm : m.meshes) {
        if (!sm.vb || !sm.ib) continue;
        if (g_skip_mesh && sm.name == g_skip_mesh) continue;
        if (g_only_mesh && sm.name != g_only_mesh) continue;
        skin_mesh(ctx, sm, world);
        UINT stride = 32, offset = 0;
        ctx->IASetVertexBuffers(0, 1, &sm.vb, &stride, &offset);
        ctx->IASetIndexBuffer(sm.ib, DXGI_FORMAT_R32_UINT, 0);
        ID3D11ShaderResourceView *srv = sm.srv ? sm.srv : g.white;
        ctx->PSSetShaderResources(0, 1, &srv);
        bool lit = g_look.on && sm.emis;
        if (lit) {
            const float white[3] = {1, 1, 1}, *rgb = g_look.rgb;
            float e = 1.0f;
            if (g_look.rail) {
                int pip = sm.name == "BigPip" ? 4 : sm.name.size() == 6 && sm.name.compare(0, 5, "Pips_") == 0 ? sm.name[5] - '1' : -1;
                if (g_look.charge >= 5.0f) e = 1.0f;
                else if (pip < 0) e = g_look.charge / 5.0f;
                else e = g_look.charge > pip + 1.0f ? 1.0f : fmaxf(g_look.charge - pip, 0.0f);
            } else if (g_look.alt) {
                e = 0.8f;                                 // the MinosRevolver material's _EmissiveIntensity
                if (sm.name == "MinosRevolver_Hammer") rgb = white;
            } else if (sm.name != "Revolver_Cylinder") {
                rgb = white;
            }
            MeshConsts k = g_look.base;
            for (int i = 0; i < 3; i++) k.emis[i] = rgb[i] * e;
            k.emis[3] = 1.0f;
            upload_consts(ctx, k);
            ctx->PSSetShaderResources(1, 1, &sm.emis);
        }
        ctx->DrawIndexed(sm.index_count, 0, 0);
        if (lit) upload_consts(ctx, g_look.base);
    }
}
void upload_consts(ID3D11DeviceContext *ctx, const MeshConsts &c) {
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx->Map(g.mesh_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        memcpy(ms.pData, &c, sizeof(c));
        ctx->Unmap(g.mesh_cb, 0);
    }
}

// The displays on the held weapon, drawn on their nodes of the pose draw_model has just left in g_pose.
//   Piercer: the battery monitor. Revolver.Update shows the charging pictures while the alt fire is held
//   (under 50, under 100, full), the low battery in red and the half one in yellow while it refills.
//   Marksman and Sharpshooter: one meter per coin or shot, filled by charge / 100 - index; red (grey on
//   the Sharpshooter) while filling, the variation's colour when full (Revolver.CheckCoinCharges).
//   Shotgun: the core meter (Shotgun.UpdateMeter).
//   Sawblade launcher (Nailgun.Update, RefreshHeatSinkFill). Attractor: the number of saws, and a meter for
//   each magnet, filled by charge - index, the variation's blue when full and grey (0.33) while it fills.
//   Overheat: the heat as a bar, in a frame that goes from that grey to orange with it and is not shown
//   while the heat sink is out and the gun cold; the heat sink's own meter, green when it is in.
enum { SCREEN_VERTS = 96 };
bool ensure_screen_vb(ID3D11DeviceContext *ctx) {
    if (g.screen_vb) return true;
    ID3D11Device *dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = SCREEN_VERTS * 32;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &g.screen_vb);
    dev->Release();
    return g.screen_vb != nullptr;
}
void draw_screens(ID3D11DeviceContext *ctx, Model &m, const HudState &st, const MeshConsts &base) {
    if (m.screens.empty() || g_pose.size() < (size_t)m.nodes * 12) return;
    if (!ensure_screen_vb(ctx)) return;
    const char *prefix = st.weapon == 4 ? "clock." : st.weapon == 3 ? (st.weapon_var == 1 ? "overheat." : "magnet.")
                         : st.weapon == 1 ? (st.weapon_var == 1 ? "pump." : "core.") : st.variation == 1 ? "marksman." : st.variation == 2 ? "sharp." : "pierce.";
    size_t plen = strlen(prefix);
    const float blue[3] = {0.25f, 0.91f, 1.0f}, green[3] = {0.2667f, 1.0f, 0.2706f}, red[3] = {1.0f, 0.2392f, 0.2392f};
    MeshConsts c = base;
    c.light[3] = 1.0f;                                // all ambient: the displays light themselves
    c.mode[0] = 1.0f;
    int layer = 0;
    // The rocket launcher's clock is a picture of light hung over the gun: each of its panels is as see-through
    // as its picture and its colour say, the ring empties clockwise from three o'clock as its own panel has it,
    // and the hand turns about the hub (RocketLauncher.Update: "timerArm.localRotation = Euler(0, 0, Lerp(360,
    // 0, amount))", "timerMeter.fillAmount = amount").
    const bool clock = st.weapon == 4;
    const Screen *hub = nullptr;
    if (clock) {
        for (const Screen &sc : m.screens)
            if (sc.name == "clock.hub") hub = &sc;
        c.mode[0] = 0.0f;
        ctx->OMSetBlendState(g.blend, nullptr, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(g.depth_read, 0);
    }
    for (const Screen &sc : m.screens) {
        if (sc.name.compare(0, plen, prefix) != 0) continue;
        const char *what = sc.name.c_str() + plen;
        float fill = 1.0f, rgb[3] = {sc.rgba[0], sc.rgba[1], sc.rgba[2]}, alpha = 1.0f;
        int tex = sc.tex;
        bool radial = false, ammo = false, pie = false, hand = false;
        if (clock) {
            bool white = !strcmp(what, "hub") || !strcmp(what, "arm");
            if (!white) memcpy(rgb, st.clock_rgb, sizeof(rgb));
            alpha = sc.rgba[3] * (white ? 1.0f : st.clock_alpha);
            if (!strcmp(what, "ring")) {
                pie = true;
                fill = fminf(fmaxf(st.clock_fill, 0.0f), 1.0f);
            }
            hand = !strcmp(what, "arm") && hub;
        } else if (!strcmp(what, "monitor")) {
            const char *pic = "battery_full";
            memcpy(rgb, blue, sizeof(rgb));
            if (st.pierce_charge > 0) pic = st.pierce_charge < 50.0f ? "battery_charge1" : st.pierce_charge < 100.0f ? "battery_charge2" : "battery_charge3";
            else if (st.pierce_ready < 50.0f) { pic = "battery_low"; rgb[0] = 1; rgb[1] = 0; rgb[2] = 0; }
            else if (st.pierce_ready < 100.0f) { pic = "battery_mid"; rgb[0] = 1; rgb[1] = 0.92f; rgb[2] = 0.016f; }
            int k = m.texture(pic);
            if (k >= 0) tex = k;
        } else if (!strncmp(what, "coin", 4)) {
            int i = what[4] - '0';
            bool sharp = st.variation == 2;
            radial = !sharp;                          // the Marksman's coins come back like a clock hand going round
            fill = fminf(fmaxf((sharp ? st.sharp_charge : st.coin_charge) / 100.0f - i, 0.0f), 1.0f);
            if (sharp && st.alt) fill = fminf(fmaxf(st.sharp_charge / 300.0f, 0.0f), 1.0f);   // the alternate one has a single panel for its one shot
            if (fill >= 1.0f) memcpy(rgb, sharp ? red : green, sizeof(rgb));
            else if (sharp) rgb[0] = rgb[1] = rgb[2] = 0.5f;
            else { rgb[0] = 1; rgb[1] = 0; rgb[2] = 0; }
        } else if (st.weapon == 3) {
            const float grey[3] = {0.33f, 0.33f, 0.33f}, orange[3] = {1.0f, 0.5f, 0.0f};
            float heat = fminf(fmaxf(st.blade_heat, 0.0f), 1.0f);
            if (!strncmp(what, "sink", 4)) {
                float charge = st.weapon_var == 1 ? st.heat_sink : st.magnet_charge;
                fill = fminf(fmaxf(charge - (what[4] - '0'), 0.0f), 1.0f);
                memcpy(rgb, fill >= 1.0f ? (st.weapon_var == 1 ? green : blue) : grey, sizeof(rgb));
            } else if (!strcmp(what, "fill")) {
                fill = heat;
            } else if (!strcmp(what, "heatbg")) {
                if (st.heat_sink < 1.0f && heat <= 0) fill = 0;
                for (int k = 0; k < 3; k++) rgb[k] = grey[k] + (orange[k] - grey[k]) * heat;
            } else if (!strcmp(what, "ammo")) {
                ammo = true;
                memcpy(rgb, blue, sizeof(rgb));
            } else if (sc.rgba[3] < 0.3f) {
                fill = 0;                                 // a faint wash over the display, which a solid panel is not
            }
        } else if (!strcmp(what, "fill")) {
            fill = fminf(fmaxf(st.core_meter, 0.0f), 1.0f);
            float k = fminf(fmaxf(st.core_meter_red, 0.0f), 1.0f);
            rgb[0] = blue[0] + (1.0f - blue[0]) * k;
            rgb[1] = blue[1] + (0.25f - blue[1]) * k;
            rgb[2] = blue[2] + (0.25f - blue[2]) * k;
            if (st.meter_rgb_set) memcpy(rgb, st.meter_rgb, sizeof(rgb));
        }
        layer++;
        if (fill <= 0.001f) continue;
        float p[4][3], uv[4][2];
        for (int k = 0; k < 4; k++) {
            memcpy(p[k], sc.corner[k], 12);
            uv[k][0] = sc.corner[k][3];
            uv[k][1] = sc.corner[k][4];
        }
        if (hand) {
            // turned about the hub's middle, in the display's own plane, anticlockwise as the display has it
            float mid[3] = {0, 0, 0}, ex[3], ey[3], lx = 0, ly = 0;
            for (int i = 0; i < 3; i++) {
                for (int k = 0; k < 4; k++) mid[i] += hub->corner[k][i] * 0.25f;
                ex[i] = hub->corner[3][i] - hub->corner[0][i];
                ey[i] = hub->corner[1][i] - hub->corner[0][i];
                lx += ex[i] * ex[i];
                ly += ey[i] * ey[i];
            }
            lx = sqrtf(lx);
            ly = sqrtf(ly);
            if (lx > 1e-9f && ly > 1e-9f) {
                float ang = 6.2831853f * (1.0f - fminf(fmaxf(st.clock_fill, 0.0f), 1.0f)), cs = cosf(ang), sn = sinf(ang);
                for (int k = 0; k < 4; k++) {
                    float x = 0, y = 0;
                    for (int i = 0; i < 3; i++) {
                        x += (p[k][i] - mid[i]) * ex[i] / lx;
                        y += (p[k][i] - mid[i]) * ey[i] / ly;
                    }
                    float x2 = x * cs - y * sn, y2 = x * sn + y * cs;
                    for (int i = 0; i < 3; i++) p[k][i] = mid[i] + ex[i] / lx * x2 + ey[i] / ly * y2;
                }
            }
        }
        // a meter keeps the edge it fills from and draws the rest in proportion
        static const int moved[5][4] = {{0, 0, 0, 0}, {2, 1, 3, 0}, {1, 2, 0, 3}, {1, 0, 2, 3}, {0, 1, 3, 2}};   // pairs: corner, the corner it shrinks towards
        if (!radial && !pie && sc.fill >= 1 && sc.fill <= 4 && fill < 1.0f)
            for (int pair = 0; pair < 2; pair++) {
                int a = moved[sc.fill][pair * 2], b = moved[sc.fill][pair * 2 + 1];
                for (int k = 0; k < 3; k++) p[a][k] = p[b][k] + (p[a][k] - p[b][k]) * fill;
                for (int k = 0; k < 2; k++) uv[a][k] = uv[b][k] + (uv[a][k] - uv[b][k]) * fill;
            }
        // into the model's space, then each later panel a hair nearer the eye than the one under it
        const float *w = &g_pose[(size_t)sc.node * 12];
        float q[4][3];
        for (int k = 0; k < 4; k++)
            for (int i = 0; i < 3; i++) q[k][i] = w[i * 4] * p[k][0] + w[i * 4 + 1] * p[k][1] + w[i * 4 + 2] * p[k][2] + w[i * 4 + 3];
        float e1[3] = {q[1][0] - q[0][0], q[1][1] - q[0][1], q[1][2] - q[0][2]}, e2[3] = {q[3][0] - q[0][0], q[3][1] - q[0][1], q[3][2] - q[0][2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 1e-12f) continue;
        float towards = (n[0] * q[0][0] + n[1] * q[0][1] + n[2] * q[0][2]) > 0 ? -1.0f : 1.0f;   // the eye is at the origin
        float lift = 0.0015f * layer * towards / nl;
        float verts[SCREEN_VERTS][8];
        int count = 0;
        // a point of the panel by how far across (a) and up (b) it is, both 0..1
        auto put = [&](float a, float b) {
            float *o = verts[count++];
            for (int i = 0; i < 3; i++) {
                o[i] = q[0][i] + (q[3][i] - q[0][i]) * a + (q[1][i] - q[0][i]) * b + n[i] * lift;
                o[3 + i] = n[i] / nl;
            }
            for (int i = 0; i < 2; i++) o[6 + i] = uv[0][i] + (uv[3][i] - uv[0][i]) * a + (uv[1][i] - uv[0][i]) * b;
        };
        ID3D11ShaderResourceView *letters = nullptr;
        if (ammo) {
            // The number of saws, in the middle of its box, in the HUD's own font (the game writes it in the same
            // one). The box's height is 1 here; sc.rgba[3] is how high the letters are in it.
            char num[8];
            snprintf(num, sizeof(num), "%d", st.saw_count < 0 ? 0 : st.saw_count > 99 ? 99 : st.saw_count);
            if (g.fonts.empty() || g.fonts[0].tex < 0) continue;
            const Font &f = g.fonts[0];
            letters = g.textures[f.tex].srv;
            float w_box = sqrtf((q[3][0] - q[0][0]) * (q[3][0] - q[0][0]) + (q[3][1] - q[0][1]) * (q[3][1] - q[0][1]) + (q[3][2] - q[0][2]) * (q[3][2] - q[0][2]));
            float h_box = sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
            if (h_box < 1e-9f || w_box < 1e-9f) continue;
            float s = sc.rgba[3] / f.point, sx = s * h_box / w_box, width = 0, top = -1e9f, bottom = 1e9f;
            for (const char *ch = num; *ch; ch++) {
                const Glyph *gl = f.find((uint8_t)*ch);
                if (!gl) continue;
                width += gl->adv * sx;
                top = fmaxf(top, gl->by * s);
                bottom = fminf(bottom, (gl->by - gl->h) * s);
            }
            float pen = 0.5f - width * 0.5f, base_y = 0.5f - (top + bottom) * 0.5f;
            for (const char *ch = num; *ch && count + 6 <= SCREEN_VERTS; ch++) {
                const Glyph *gl = f.find((uint8_t)*ch);
                if (!gl) continue;
                float a0 = pen + gl->bx * sx, a1 = a0 + gl->w * sx, b1 = base_y + gl->by * s, b0 = b1 - gl->h * s;
                float u0 = gl->x / f.aw, u1 = (gl->x + gl->w) / f.aw, v0 = gl->y / f.ah, v1 = (gl->y + gl->h) / f.ah;
                const float ab[6][4] = {{a0, b0, u0, v1}, {a0, b1, u0, v0}, {a1, b1, u1, v0}, {a0, b0, u0, v1}, {a1, b1, u1, v0}, {a1, b0, u1, v1}};
                for (const float *k : ab) {
                    put(k[0], k[1]);
                    verts[count - 1][6] = k[2];
                    verts[count - 1][7] = k[3];
                }
                pen += gl->adv * sx;
            }
        } else if (pie && fill < 1.0f) {
            // Unity's "Radial 360" from the right, clockwise, in the panel's own two directions: a fan about
            // the middle whose rim runs along the panel's edge
            int steps = (int)ceilf(30.0f * fill);
            auto rim = [&](float ang) {
                float dx = cosf(ang), dy = -sinf(ang), big = fmaxf(fabsf(dx), fabsf(dy));
                put(0.5f + 0.5f * dx / big, 0.5f + 0.5f * dy / big);
            };
            for (int k = 0; k < steps && count + 3 <= SCREEN_VERTS; k++) {
                put(0.5f, 0.5f);
                rim(6.2831853f * fill * k / steps);
                rim(6.2831853f * fill * (k + 1) / steps);
            }
        } else if (radial && fill < 1.0f) {
            // A pie from twelve o'clock, clockwise, as it is seen: the panel may be mounted turned or
            // flipped on the gun (the Marksman's is upside down), so each clock direction on the screen
            // is first put into the panel's own two directions. The picture's transparency rounds it off.
            float ax = q[3][0] - q[0][0], ay = q[3][1] - q[0][1], bx = q[1][0] - q[0][0], by = q[1][1] - q[0][1], det = ax * by - bx * ay;
            auto rim = [&](float ang) {
                float dx = sinf(ang), dy = cosf(ang), da = dx, db = dy;
                if (fabsf(det) > 1e-12f) {
                    da = (dx * by - bx * dy) / det;
                    db = (ax * dy - dx * ay) / det;
                }
                float len = sqrtf(da * da + db * db);
                if (len < 1e-9f) len = 1;
                put(0.5f + 0.5f * da / len, 0.5f + 0.5f * db / len);
            };
            int steps = (int)ceilf(28.0f * fill);
            for (int k = 0; k < steps && count + 3 <= SCREEN_VERTS; k++) {
                put(0.5f, 0.5f);
                rim(6.2831853f * fill * k / steps);
                rim(6.2831853f * fill * (k + 1) / steps);
            }
        } else {
            static const int order[6] = {0, 1, 2, 0, 2, 3};
            for (int v = 0; v < 6; v++) {
                // (the corners may have been moved by a straight fill, so they are used as they are)
                int k = order[v];
                float *o = verts[count++];
                for (int i = 0; i < 3; i++) {
                    o[i] = q[k][i] + n[i] * lift;
                    o[3 + i] = n[i] / nl;
                }
                o[6] = uv[k][0];
                o[7] = uv[k][1];
            }
        }
        if (!count) continue;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(ctx->Map(g.screen_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
        memcpy(ms.pData, verts, (size_t)count * 32);
        ctx->Unmap(g.screen_vb, 0);
        c.tint[0] = rgb[0];
        c.tint[1] = rgb[1];
        c.tint[2] = rgb[2];
        c.mode[2] = letters || clock ? 1.0f : 0.0f;       // letters: the tint alone, cut out where the font's picture says
        c.tint[3] = alpha;
        upload_consts(ctx, c);
        UINT stride = 32, offset = 0;
        ctx->IASetVertexBuffers(0, 1, &g.screen_vb, &stride, &offset);
        ID3D11ShaderResourceView *srv = letters ? letters : tex >= 0 && tex < (int)m.tex_srvs.size() && m.tex_srvs[tex] ? m.tex_srvs[tex] : g.white;
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->Draw((UINT)count, 0);
    }
    if (clock) {
        ctx->OMSetBlendState(g.opaque, nullptr, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(g.depth_on, 0);
    }
    upload_consts(ctx, base);
}

// The Piercer's charge gathering at the muzzle (ULTRAKILL's ChargeEffect, which grows with the charge):
// a glow on a panel facing the eye at the muzzle's own distance, drawn against the weapon's depth, so the
// gun stands in front of the part of it that is behind the barrel. (v0.60 drew it over the whole picture
// and it showed through the gun.)
void draw_muzzle_glow(ID3D11DeviceContext *ctx, const float *at, float charge, const MeshConsts &base) {
    int spr = g.sprite("softglow");
    if (spr < 0 || !ensure_screen_vb(ctx)) return;
    float k = fminf(fmaxf(charge, 0.0f), 1.0f), z = at[2] > 0.2f ? at[2] : 0.2f;
    MeshConsts c = base;
    c.mode[2] = 1.0f;
    ctx->OMSetBlendState(g.blend, nullptr, 0xFFFFFFFF);
    // tested against the weapon but leaving no depth of its own: its panel is mostly empty, and what is
    // drawn later against this buffer (the world's sparks and beams) must not be cut off by a square of it
    ctx->OMSetDepthStencilState(g.depth_read, 0);
    ctx->PSSetSamplers(0, 1, &g.linear);
    ID3D11ShaderResourceView *srv = g.textures[g.sprites[spr].tex].srv;
    ctx->PSSetShaderResources(0, 1, &srv);
    // outer blue ball, then a small white heart; sizes are the share of the picture's height they covered before
    const struct { float size, r, g, b, a; } layers[2] = {{(0.05f + 0.13f * k) * 1.8f, 0.25f, 0.85f, 1.0f, 0.55f + 0.35f * k}, {(0.05f + 0.13f * k) * 0.8f, 1, 1, 1, 0.9f}};
    for (const auto &l : layers) {
        float h = l.size * z;                         // half the panel's side: at 90 degrees the picture is 2z tall at depth z
        const float corners[6][4] = {{-1, -1, 0, 1}, {-1, 1, 0, 0}, {1, 1, 1, 0}, {-1, -1, 0, 1}, {1, 1, 1, 0}, {1, -1, 1, 1}};
        float verts[6][8];
        for (int v = 0; v < 6; v++) {
            verts[v][0] = at[0] + corners[v][0] * h;
            verts[v][1] = at[1] + corners[v][1] * h;
            verts[v][2] = at[2];
            verts[v][3] = 0; verts[v][4] = 0; verts[v][5] = -1;
            verts[v][6] = corners[v][2];
            verts[v][7] = corners[v][3];
        }
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(ctx->Map(g.screen_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) break;
        memcpy(ms.pData, verts, sizeof(verts));
        ctx->Unmap(g.screen_vb, 0);
        c.tint[0] = l.r; c.tint[1] = l.g; c.tint[2] = l.b; c.tint[3] = l.a;
        upload_consts(ctx, c);
        UINT stride = 32, offset = 0;
        ctx->IASetVertexBuffers(0, 1, &g.screen_vb, &stride, &offset);
        ctx->Draw(6, 0);
    }
    ctx->OMSetBlendState(g.opaque, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g.depth_on, 0);
    ctx->PSSetSamplers(0, 1, &g.point);
    upload_consts(ctx, base);
}

const float SHOT_RECOIL_SCALE = 0.8f;
Model *find_model(const char *name) {
    for (Model &m : g.models)
        if (m.name == name) return &m;
    return nullptr;
}

void rotate_y(float m[16], float a) {
    identity(m);
    m[0] = cosf(a);
    m[2] = -sinf(a);
    m[8] = sinf(a);
    m[10] = cosf(a);
}

// ULTRAKILL's effect meshes in the game's world, through the game camera the DLL reports. They are drawn
// over the finished picture with a depth buffer of their own, so they sort among themselves and the
// weapon covers them, but the game's walls do not.
void draw_world(ID3D11DeviceContext *ctx, const HudState &st, bool srgb) {
    if (!st.cam_valid || st.world_mesh_count <= 0) return;
    const float n = 0.1f, f = 600.0f;
    float view[16], proj[16] = {}, vp[16];
    identity(view);
    for (int i = 0; i < 3; i++) {
        view[i * 4 + 0] = st.cam_right[i];
        view[i * 4 + 1] = st.cam_up[i];
        view[i * 4 + 2] = st.cam_fwd[i];
    }
    for (int k = 0; k < 3; k++) {
        const float *axis = k == 0 ? st.cam_right : k == 1 ? st.cam_up : st.cam_fwd;
        view[12 + k] = -(st.cam_eye[0] * axis[0] + st.cam_eye[1] * axis[1] + st.cam_eye[2] * axis[2]);
    }
    proj[0] = 1.0f / st.cam_tan_x;
    proj[5] = 1.0f / st.cam_tan_y;
    proj[10] = f / (f - n);
    proj[11] = 1.0f;
    proj[14] = -n * f / (f - n);
    mul44(view, proj, vp);
    ctx->IASetInputLayout(g.mesh_il);
    ctx->VSSetShader(g.mesh_vs, nullptr, 0);
    ctx->PSSetShader(g.mesh_ps, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g.mesh_cb);
    ctx->PSSetConstantBuffers(0, 1, &g.mesh_cb);
    ctx->PSSetSamplers(0, 1, &g.wrap);
    ctx->PSSetShaderResources(2, 1, &g.scene_depth);
    ctx->OMSetBlendState(g.blend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g.depth_on, 0);
    float cam_yaw = atan2f(st.cam_fwd[0], st.cam_fwd[2]);
    for (int i = 0; i < st.world_mesh_count && i < HudState::MAX_WORLD_MESHES; i++) {
        const HudState::WorldMesh &wm = st.world_meshes[i];
        Model *m = wm.model ? find_model(wm.model) : nullptr;
        if (!m || wm.rgba[3] <= 0.003f) continue;
        ctx->OMSetDepthStencilState(wm.rgba[3] >= 0.99f ? g.depth_on : g.depth_read, 0);
        if (wm.limits && wm.limit_count > 0 && !m->meshes.empty()) {
            // An explosion's ball: every vertex goes out to the ball's radius, or to where the world stops it
            // if that is nearer. The vertices are moved here, and the mesh is only put in its place.
            float tr[16];
            translate(tr, wm.p[0], wm.p[1], wm.p[2]);
            MeshConsts c{};
            c.dpar[0] = g.depth_a;
            c.dpar[1] = g.depth_b;
            c.dpar[2] = g.scene_depth ? 1.0f : 0.0f;
            mul44(tr, vp, c.mvp);
            memcpy(c.world, tr, sizeof(c.world));
            c.light[1] = 1.0f;
            c.light[3] = 1.0f;
            c.mode[1] = srgb ? 1.0f : 0.0f;
            c.mode[3] = 1.0f;
            memcpy(c.tint, wm.rgba, sizeof(c.tint));
            c.uvoff[0] = wm.uv[0] - floorf(wm.uv[0]);
            c.uvoff[1] = wm.uv[1] - floorf(wm.uv[1]);
            upload_consts(ctx, c);
            SkinMesh &sm = m->meshes[0];
            if (!sm.vb || !sm.ib) continue;
            for (UINT v = 0; v < sm.vertex_count; v++) {
                const float *src = &sm.verts[(size_t)v * 8];
                float *dst = &sm.skinned[(size_t)v * 8];
                float len = sqrtf(src[0] * src[0] + src[1] * src[1] + src[2] * src[2]), r = len * wm.radius;
                if ((int)v < wm.limit_count && r > wm.limits[v]) r = wm.limits[v];
                float k = len > 1e-6f ? r / len : 0.0f;
                for (int i = 0; i < 3; i++) {
                    dst[i] = src[i] * k;
                    dst[3 + i] = src[3 + i];
                }
            }
            D3D11_MAPPED_SUBRESOURCE ms;
            if (FAILED(ctx->Map(sm.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
            memcpy(ms.pData, sm.skinned.data(), sm.skinned.size() * 4);
            ctx->Unmap(sm.vb, 0);
            UINT stride = 32, offset = 0;
            ctx->IASetVertexBuffers(0, 1, &sm.vb, &stride, &offset);
            ctx->IASetIndexBuffer(sm.ib, DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView *srv = sm.srv ? sm.srv : g.white;
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->DrawIndexed(sm.index_count, 0, 0);
            continue;
        }
        // size, the flip about the camera's right axis, then into place
        float sc[16], rx[16], ry[16], tr[16], world[16];
        if (wm.oriented) {
            // (a point is a row here: the rows of the matrix are where the model's axes go)
            identity(world);
            for (int k = 0; k < 3; k++) {
                world[k] = wm.ax[k] * wm.radius;
                world[4 + k] = wm.ay[k] * wm.radius;
                world[8 + k] = wm.az[k] * wm.radius;
                world[12 + k] = wm.p[k];
            }
        } else {
            identity(sc);
            sc[0] = sc[5] = sc[10] = wm.radius;
            rotate_x(rx, wm.flip);
            rotate_y(ry, cam_yaw);
            translate(tr, wm.p[0], wm.p[1], wm.p[2]);
            mul44(sc, rx, world);
            mul44(world, ry, world);
            mul44(world, tr, world);
        }
        MeshConsts c{};
        c.dpar[0] = g.depth_a;
        c.dpar[1] = g.depth_b;
        c.dpar[2] = g.scene_depth ? 1.0f : 0.0f;
        mul44(world, vp, c.mvp);
        memcpy(c.world, world, sizeof(c.world));
        c.light[1] = 1.0f;
        c.light[3] = 1.0f;
        c.mode[0] = wm.cutout ? 1.0f : 0.0f;
        c.mode[1] = srgb ? 1.0f : 0.0f;
        c.mode[3] = 1.0f;
        memcpy(c.tint, wm.rgba, sizeof(c.tint));
        c.uvoff[0] = wm.uv[0] - floorf(wm.uv[0]);
        c.uvoff[1] = wm.uv[1] - floorf(wm.uv[1]);
        upload_consts(ctx, c);
        draw_model(ctx, *m, nullptr, 0, false, nullptr, 0, 1.0f, wm.spin != 0 && m->nodes > 1 ? 1 : -1, wm.spin);
    }
    ctx->OMSetBlendState(g.opaque, nullptr, 0xFFFFFFFF);
    ctx->PSSetSamplers(0, 1, &g.point);
}

// The fully charged railcannon's crackle ("FullCharge", an object on the gun's Base bone that Railcannon.Update
// switches on at five charges). Its particle system sits 0.946 up the Base bone and 0.067 off it, between
// the prongs, and puts out ten pictures of an electric arc a second, each 1 u across, turned any way round,
// standing still for its one second of life somewhere in a rectangle 2 u across the gun by 0.5 u along
// it, in the variation's colour (Railcannon.Update sets the particles' start colour). The Malicious has
// the same in its orange and, with it, a still picture ('charge2', 0.45 u by 1.02 u) lying along the gun.
// Drawn against the weapon's depth, so the gun covers what is behind its own parts. How an arc comes
// and goes over its second is the prefab's colour-over-lifetime curve, which is not read: here it
// swells in and out (a sine).
void draw_rail_arcs(ID3D11DeviceContext *ctx, const HudState &st, Model &m, const MeshConsts &base) {
    static const char *const ARCS[10] = {"arc0", "arc1", "arc2", "arc3", "arc4", "arc5", "arc6", "arc7", "arc8", "arc9"};
    int bn = m.node("Base");
    if (bn < 0 || (size_t)bn * 12 + 12 > g_pose.size() || !ensure_screen_vb(ctx)) return;
    const float *w = &g_pose[(size_t)bn * 12];
    auto point = [&](float x, float y, float z, float *o) {
        for (int r = 0; r < 3; r++) o[r] = w[r * 4 + 0] * x + w[r * 4 + 1] * y + w[r * 4 + 2] * z + w[r * 4 + 3];
    };
    float along[3] = {w[1], w[5], w[9]}, across[3] = {w[2], w[6], w[10]};
    for (float *a : {along, across}) {
        float len = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (len < 1e-6f) return;
        for (int i = 0; i < 3; i++) a[i] /= len;
    }
    bool malicious = st.weapon_var == 1;
    const float rgb[3] = {malicious ? 1.0f : 0.251f, malicious ? 0.49f : 0.906f, malicious ? 0.25f : 1.0f};
    MeshConsts c = base;
    c.mode[2] = 1.0f;
    ctx->OMSetBlendState(g.blend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g.depth_read, 0);
    ctx->PSSetSamplers(0, 1, &g.linear);
    auto quad = [&](const char *name, const float *p, const float *ax, const float *ay, float alpha, const float *tint) {
        int spr = g.sprite(name);
        if (spr < 0 || alpha <= 0.003f) return;
        static const float corners[6][4] = {{-1, -1, 0, 1}, {-1, 1, 0, 0}, {1, 1, 1, 0}, {-1, -1, 0, 1}, {1, 1, 1, 0}, {1, -1, 1, 1}};
        float verts[6][8];
        for (int v = 0; v < 6; v++) {
            for (int i = 0; i < 3; i++) verts[v][i] = p[i] + ax[i] * corners[v][0] + ay[i] * corners[v][1];
            verts[v][3] = 0; verts[v][4] = 0; verts[v][5] = -1;
            verts[v][6] = corners[v][2];
            verts[v][7] = corners[v][3];
        }
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(ctx->Map(g.screen_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
        memcpy(ms.pData, verts, sizeof(verts));
        ctx->Unmap(g.screen_vb, 0);
        ID3D11ShaderResourceView *srv = g.textures[g.sprites[spr].tex].srv;
        ctx->PSSetShaderResources(0, 1, &srv);
        c.tint[0] = tint[0]; c.tint[1] = tint[1]; c.tint[2] = tint[2]; c.tint[3] = alpha;
        upload_consts(ctx, c);
        UINT stride = 32, offset = 0;
        ctx->IASetVertexBuffers(0, 1, &g.screen_vb, &stride, &offset);
        ctx->Draw(6, 0);
    };
    if (malicious) {
        // lying along the gun, turned to face the eye about that line
        float p[3], side[3] = {along[1], -along[0], 0.0f};
        point(0.0f, 0.919f, -0.013f, p);
        float len = sqrtf(side[0] * side[0] + side[1] * side[1]);
        if (len > 1e-4f) {
            float ax[3] = {side[0] / len * 0.224f, side[1] / len * 0.224f, 0.0f}, ay[3] = {along[0] * 0.512f, along[1] * 0.512f, along[2] * 0.512f};
            const float white[3] = {1, 1, 1};
            quad("charge2", p, ax, ay, 1.0f, white);
        }
    }
    float centre[3];
    point(0.0f, 0.946f, 0.067f, centre);
    long long now = (long long)floor(st.time * 10.0);
    for (int i = 0; i < 10; i++) {
        long long n = now - i;                           // the arc put out in that tenth of a second
        float age = (float)(st.time - (double)n / 10.0);
        if (age < 0 || age >= 1.0f) continue;
        uint32_t h = (uint32_t)(n * 2654435761u);
        auto rnd = [&]() {
            h ^= h << 13; h ^= h >> 17; h ^= h << 5;
            return (float)(h & 0xFFFF) / 65535.0f;
        };
        float u = rnd() * 2.0f - 1.0f, v = (rnd() - 0.5f) * 0.5f, turn = rnd() * 6.2831853f, p[3];
        for (int k = 0; k < 3; k++) p[k] = centre[k] + across[k] * u + along[k] * v;
        float cs = cosf(turn) * 0.5f, sn = sinf(turn) * 0.5f;
        float ax[3] = {cs, sn, 0}, ay[3] = {-sn, cs, 0};
        quad(ARCS[(uint32_t)(h >> 8) % 10], p, ax, ay, sinf(age * 3.14159265f), rgb);
    }
    ctx->OMSetBlendState(g.opaque, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g.depth_on, 0);
    ctx->PSSetSamplers(0, 1, &g.point);
    upload_consts(ctx, base);
}

void draw_viewmodel(ID3D11DeviceContext *ctx, const HudState &st, float aspect, bool srgb) {
    // The meshes are stored already posed in the HUD camera's space (x right, y up, z forward), in the
    // pose the Piercer prefab is saved in. Recoil is a short kick about the grip.
    const float grip[3] = {0.42f, -0.6f, 1.49f};
    float since = (float)(st.time - st.last_shot), since_p = (float)(st.time - st.last_pierce);
    float kick = 0;
    if (since >= 0 && since < 0.35f) kick = fmaxf(kick, expf(-since * 14.0f) * 0.30f);
    if (since_p >= 0 && since_p < 0.6f) kick = fmaxf(kick, expf(-since_p * 8.0f) * 0.55f);
    float charge = fmaxf(st.pierce_charge / 100.0f, st.core_charge);
    if (st.weapon == 4) charge = st.srs_charge * 2.5f;   // the cannon winding up: the gun is thrown about by up to a hundredth of a unit
    float shake = charge > 0 ? sinf((float)st.time * 90.0f) * 0.004f * charge : 0;
    if (st.weapon == 2 && st.rail_charge >= 5.0f) shake = sinf((float)st.time * 130.0f) * 0.004f;   // the full railcannon trembles (+-0.005)

    // With the animated models the recoil is the clip's own, so only the charge's shake is added.
    Model *revolver = find_model("revolver"), *arm = find_model("feedbacker");
    if (revolver) kick = 0;
    float t0[16], rx[16], rz[16], t1[16], world[16], tmp[16];
    translate(t0, -grip[0], -grip[1], -grip[2]);
    rotate_x(rx, -kick);                              // barrel up
    rotate_z(rz, revolver ? 0.0f : charge * 0.12f);   // tilts while charging
    translate(t1, grip[0] + shake + st.bob_x + st.weapon_sway[0], grip[1] + shake * 0.5f + st.bob_y + st.weapon_sway[1],
              grip[2] - kick * 0.25f + st.weapon_sway[2]);
    mul44(t0, rx, tmp);
    mul44(tmp, rz, tmp);
    mul44(tmp, t1, world);

    const float n = 0.05f, f = 50.0f;
    float proj[16] = {};
    proj[0] = 1.0f / aspect;                          // vertical field of view 90 degrees
    proj[5] = 1.0f;
    proj[10] = f / (f - n);
    proj[11] = 1.0f;
    proj[14] = -n * f / (f - n);

    MeshConsts c{};
    mul44(world, proj, c.mvp);
    memcpy(c.world, world, sizeof(c.world));
    float lx = -0.35f, ly = 0.75f, lz = -0.55f, ll = sqrtf(lx * lx + ly * ly + lz * lz);
    c.light[0] = lx / ll;
    c.light[1] = ly / ll;
    c.light[2] = lz / ll;
    c.light[3] = 0.55f;                               // ambient share
    c.mode[1] = srgb ? 1.0f : 0.0f;
    c.tint[0] = c.tint[1] = c.tint[2] = c.tint[3] = 1.0f;
    upload_consts(ctx, c);
    ctx->IASetInputLayout(g.mesh_il);
    ctx->VSSetShader(g.mesh_vs, nullptr, 0);
    ctx->PSSetShader(g.mesh_ps, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g.mesh_cb);
    ctx->PSSetConstantBuffers(0, 1, &g.mesh_cb);
    // textures repeat: the shotgun's mesh runs its texture coordinates up to 3 (clamped, those parts came out as flat
    // slabs of one colour beside the barrels)
    ctx->PSSetSamplers(0, 1, g.wrap ? &g.wrap : &g.point);
    ctx->OMSetBlendState(g.opaque, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g.depth_on, 0);
    if (revolver) {
        Model *shotgun = st.weapon == 1 ? find_model("shotgun") : nullptr;
        Model *rail = st.weapon == 2 ? find_model("railcannon") : nullptr;
        Model *slab = st.weapon == 0 && st.alt ? find_model("revolver_alt") : nullptr;
        Model *saw = st.weapon == 3 ? find_model("sawlauncher") : nullptr;
        Model *rocket = st.weapon == 4 ? find_model("rocketlauncher") : nullptr;
        Model *weapon = shotgun ? shotgun : rail ? rail : saw ? saw : rocket ? rocket : slab ? slab : revolver;
        bool is_revolver = !shotgun && !rail && !saw && !rocket;
        // the named clip while it lasts, otherwise the idle loop
        const Clip *clip = weapon->clip(st.revolver_clip);
        float t = (float)(st.time - st.revolver_clip_start) * st.revolver_clip_speed;
        bool loop = false;
        if (!clip || t < 0 || t > (clip->frames - 1) / clip->fps) {
            clip = weapon->clip("Idle");
            t = (float)st.time;
            loop = true;
            // The railcannon has no idle clip, and the pose its prefab is saved in is the stowed one,
            // out of view: at rest it is held as its Equip clip leaves it.
            const Clip *drawn = rail ? weapon->clip("Equip") : nullptr;
            if (drawn && drawn->frames > 0) {
                clip = drawn;
                t = (drawn->frames - 1) / drawn->fps;
                loop = false;
            }
        }
        // The shot clips are played at 80% of their travel away from the idle pose: at full strength
        // the recoil read as stronger than ULTRAKILL's (a tuning choice, from Davi's comparison).
        const Clip *idle = weapon->clip("Idle");
        bool shot = is_revolver && !slab && clip && !loop && clip->name.compare(0, 5, "Shoot") == 0;
        // While the Sharpshooter spins, the hand goes over to the Twirl clip's pose (the gun lying on
        // its side, turning about the finger); ULTRAKILL's Animator blends towards it with the spin's speed.
        const Clip *twirl = is_revolver ? weapon->clip("Twirl") : nullptr;
        float blend_weight = SHOT_RECOIL_SCALE;
        if (twirl && loop && st.twirl_blend > 0.001f) {
            clip = twirl;
            shot = true;                                  // blended from the idle pose, like a shot clip
            blend_weight = fminf(st.twirl_blend, 1.0f);
        }
        const Clip *base = shot ? idle : nullptr;
        float base_time = (float)st.time;
        if ((saw || rocket) && idle && idle->frames > 1) {
            // The sawblade launcher is played the way its Animator ('Nailgun2') plays it, which the plain
            // rule above (the clip, then the idle loop wherever the clock has it) is not:
            //   a clip goes over to Idle before its end, at a set share of its length, in a short blend
            //   (Shoot at 91.4% over 0.057 s, Equip at 94.9% over 0.051 s, ShootSuper at 93.2% over 0.032 s);
            //   Idle starts from its beginning as that blend starts, and plays at half speed;
            //   a second layer, whose weight Nailgun.UpdateAnimationWeight sets at every shot (heat x 0.6 on
            //   the Overheat, 0.9 with its heat sink out, nothing on the Attractor), holds the Idle clip's
            //   first frame over all of it: that much of every motion is taken away.
            // Until v0.76 the gun jumped from the end of a shot into the middle of an idle loop that ran at
            // twice its speed, and the Overheat kicked at full strength however hot it was.
            static const struct { const char *clip; float at, fade; } exits[] = {{"Shoot", 0.9143f, 0.0571f}, {"Equip", 0.9486f, 0.0514f}, {"ShootSuper", 0.9318f, 0.0318f}};
            const Clip *named = weapon->clip(st.revolver_clip);
            double since_start = (st.time - st.revolver_clip_start) * st.revolver_clip_speed;
            // (the rocket launcher is played the same way, with its Idle at its own speed and nothing on a second
            // layer; its clips go over to Idle in their last twentieth of a second)
            const float idle_rate = saw ? 0.5f : 1.0f;
            float muddle = saw ? fminf(fmaxf(st.clip_muddle, 0.0f), 1.0f) : 0.0f, idle_len = (idle->frames - 1) / idle->fps;
            clip = idle;
            loop = true;
            t = (float)fmod(st.time * idle_rate, idle_len);
            base = muddle > 0 ? idle : nullptr;
            base_time = 0;
            blend_weight = 1.0f - muddle;
            if (named && named != idle && since_start >= 0) {
                float len = (named->frames - 1) / named->fps, at = len - 0.05f, fade = 0.05f, nt = (float)fmin(since_start, 1.0e6);
                if (saw)
                    for (const auto &e : exits)
                        if (named->name == e.clip) { at = e.at * len; fade = e.fade; }
                if (nt < at) {
                    clip = named;
                    loop = false;
                    t = nt;
                } else if (nt < at + fade) {
                    clip = named;
                    loop = false;
                    t = fminf(nt, len);
                    base = idle;
                    base_time = (nt - at) * idle_rate;
                    blend_weight = (1.0f - (nt - at) / fade) * (1.0f - muddle);
                } else {
                    t = (float)fmod((since_start - at) * idle_rate, idle_len);
                }
            }
        }
        float muzzle[3] = {0, 0, 0};
        int muzzle_node = weapon->node(shotgun ? "ShootPoint L" : rail || saw || rocket ? "Shootpoint" : "ShootPoint");
        if (muzzle_node < 0 && slab) muzzle_node = weapon->node("ShootPoint (1)");
        if (g.raster_cull) ctx->RSSetState(g.raster_cull);
        {
            // the variation's colour (ColorBlindSettings.variationColors: blue, green, red); the Malicious is the railcannon's third
            static const float var_rgb[3][3] = {{0.251f, 0.906f, 1.0f}, {0.267f, 1.0f, 0.271f}, {1.0f, 0.235f, 0.235f}};
            int var = rail ? (st.weapon_var == 1 ? 2 : 0) : rocket ? (st.weapon_var == 1 ? 1 : 0) : st.variation < 0 ? 0 : st.variation > 2 ? 2 : st.variation;
            g_look.on = true;
            g_look.rail = rail != nullptr;
            g_look.alt = slab != nullptr;
            g_look.charge = st.rail_charge;
            g_look.time = (float)st.time;
            g_look.cylinder = is_revolver ? st.cylinder : 0.0f;
            g_look.blade = saw ? st.blade_spin : 0.0f;
            g_look.base = c;
            memcpy(g_look.rgb, var_rgb[var], sizeof(g_look.rgb));
        }
        if (saw) g_skip_mesh = "Blade (1)";
        draw_model(ctx, *weapon, clip, t, loop, base, base_time, blend_weight,
                   is_revolver ? weapon->node("Revolver_Bone") : -1, st.twirl, muzzle_node, muzzle, slab ? 1 : 0);
        g_skip_mesh = nullptr;
        if (saw && st.weapon_var == 1 && st.blade_heat > 0.01f) {
            // The Overheat's 'Blade (1)': the blade again, a tenth larger, in plain orange and as strong as
            // the gun is hot (Nailgun.SetHeat puts the heat in its material's alpha).
            MeshConsts k = c;
            k.mode[3] = 1.0f;
            k.tint[3] = fminf(st.blade_heat, 1.0f);
            upload_consts(ctx, k);
            ctx->OMSetBlendState(g.blend, nullptr, 0xFFFFFFFF);
            ctx->OMSetDepthStencilState(g.depth_read, 0);
            g_only_mesh = "Blade (1)";
            draw_model(ctx, *weapon, clip, t, loop, base, base_time, blend_weight, -1, 0, -1, nullptr, 0);
            g_only_mesh = nullptr;
            ctx->OMSetBlendState(g.opaque, nullptr, 0xFFFFFFFF);
            ctx->OMSetDepthStencilState(g.depth_on, 0);
            upload_consts(ctx, c);
        }
        g_look.on = false;
        g_look.blade = 0;
        ctx->RSSetState(g.raster);
        draw_screens(ctx, *weapon, st, c);
        if (rail && st.rail_charge >= 5.0f) draw_rail_arcs(ctx, st, *weapon, c);
        if (muzzle_node >= 0) {
            // the same transform the vertex shader applies: model space -> view -> clip
            float cx = muzzle[0] * c.mvp[0] + muzzle[1] * c.mvp[4] + muzzle[2] * c.mvp[8] + c.mvp[12];
            float cy = muzzle[0] * c.mvp[1] + muzzle[1] * c.mvp[5] + muzzle[2] * c.mvp[9] + c.mvp[13];
            float cw = muzzle[0] * c.mvp[3] + muzzle[1] * c.mvp[7] + muzzle[2] * c.mvp[11] + c.mvp[15];
            if (cw > 0.01f) {
                g_muzzle[0] = cx / cw;
                g_muzzle[1] = cy / cw;
                g_muzzle_known = true;
            }
        }
        // The arm is only in view while one of its clips plays. In ULTRAKILL the arms hang from the
        // "Punch" object, which sits 0.225 under the camera; the weapons hang from the camera itself.
        // (Until v0.61 the arms were drawn without that drop, so the punches came up too high.)
        MeshConsts ca = c;
        float arm_world[16];
        memcpy(arm_world, world, sizeof(arm_world));
        arm_world[13] -= 0.225f;
        mul44(arm_world, proj, ca.mvp);
        memcpy(ca.world, arm_world, sizeof(ca.world));
        upload_consts(ctx, ca);
        if (g.raster_cull) ctx->RSSetState(g.raster_cull);
        const Clip *ac = arm ? arm->clip(st.arm_clip) : nullptr;
        float at = (float)(st.time - st.arm_clip_start);
        if (ac && at >= 0 && at <= (ac->frames - 1) / ac->fps) draw_model(ctx, *arm, ac, at, false);
        Model *arm2 = find_model("knuckleblaster");
        const Clip *bc = arm2 ? arm2->clip(st.arm2_clip) : nullptr;
        float bt = (float)(st.time - st.arm2_clip_start) * st.arm2_clip_speed;
        if (bc && bt >= 0 && bt <= (bc->frames - 1) / bc->fps) draw_model(ctx, *arm2, bc, bt, false);
        // The whiplash ("Hook Arm", under the same Punch object). It is out from the throw until the Catch
        // clip's CatchOver event; between clips it holds the last frame of the one it has. While the hook
        // is away the model's hook is not drawn. Where its cable starts is kept for the DLL, as the muzzle is.
        Model *whip = find_model("whiplash");
        const Clip *wc = whip ? whip->clip(st.whip_clip) : nullptr;
        if (wc) {
            float wt = (float)(st.time - st.whip_clip_start), wend = (wc->frames - 1) / wc->fps;
            if (wt > wend && st.whip_hold) wt = wend;
            if (wt >= 0 && wt <= wend) {
                float hand[3] = {0, 0, 0};
                int hand_node = whip->node("Wire Start");
                g_skip_mesh = st.whip_out ? "Hook" : nullptr;
                draw_model(ctx, *whip, wc, wt, false, nullptr, 0, 1.0f, -1, 0, hand_node, hand);
                g_skip_mesh = nullptr;
                if (hand_node >= 0) {
                    float cx = hand[0] * ca.mvp[0] + hand[1] * ca.mvp[4] + hand[2] * ca.mvp[8] + ca.mvp[12];
                    float cy = hand[0] * ca.mvp[1] + hand[1] * ca.mvp[5] + hand[2] * ca.mvp[9] + ca.mvp[13];
                    float cw = hand[0] * ca.mvp[3] + hand[1] * ca.mvp[7] + hand[2] * ca.mvp[11] + ca.mvp[15];
                    if (cw > 0.01f) {
                        g_whip_hand[0] = cx / cw;
                        g_whip_hand[1] = cy / cw;
                        g_whip_hand_known = true;
                    }
                }
            }
        }
        ctx->RSSetState(g.raster);
        upload_consts(ctx, c);
        if (is_revolver && st.variation == 0 && st.pierce_charge > 0 && muzzle_node >= 0) draw_muzzle_glow(ctx, muzzle, st.pierce_charge / 100.0f, c);
        return;
    }
    for (const Mesh &m : g.meshes) {
        if (!m.vb || !m.ib) continue;
        UINT stride = 32, offset = 0;
        ctx->IASetVertexBuffers(0, 1, &m.vb, &stride, &offset);
        ctx->IASetIndexBuffer(m.ib, DXGI_FORMAT_R32_UINT, 0);
        ID3D11ShaderResourceView *srv = m.tex >= 0 ? g.textures[m.tex].srv : g.white;
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->DrawIndexed(m.index_count, 0, 0);
    }
}

// ---------------------------------------------------------------- pipeline state the game owns

struct Saved {
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    UINT vp_n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, sc_n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11RasterizerState *rs = nullptr;
    ID3D11BlendState *bs = nullptr;
    FLOAT bf[4];
    UINT mask = 0, sref = 0;
    ID3D11DepthStencilState *dss = nullptr;
    ID3D11ShaderResourceView *srv = nullptr, *srv1 = nullptr, *srv2 = nullptr;
    ID3D11SamplerState *smp = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11VertexShader *vs = nullptr;
    ID3D11GeometryShader *gs = nullptr;
    ID3D11HullShader *hs = nullptr;
    ID3D11DomainShader *ds = nullptr;
    ID3D11ClassInstance *ps_inst[32], *vs_inst[32], *gs_inst[32], *hs_inst[32], *ds_inst[32];
    UINT ps_n = 32, vs_n = 32, gs_n = 32, hs_n = 32, ds_n = 32;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer *ib = nullptr, *vb = nullptr, *vs_cb = nullptr, *ps_cb = nullptr;
    UINT ib_off = 0, vb_stride = 0, vb_off = 0;
    DXGI_FORMAT ib_fmt = DXGI_FORMAT_UNKNOWN;
    ID3D11InputLayout *il = nullptr;

    void save(ID3D11DeviceContext *c) {
        c->OMGetRenderTargets(1, &rtv, &dsv);
        c->RSGetViewports(&vp_n, vp);
        c->RSGetScissorRects(&sc_n, sc);
        c->RSGetState(&rs);
        c->OMGetBlendState(&bs, bf, &mask);
        c->OMGetDepthStencilState(&dss, &sref);
        c->PSGetShaderResources(0, 1, &srv);
        c->PSGetShaderResources(1, 1, &srv1);
        c->PSGetShaderResources(2, 1, &srv2);
        c->PSGetSamplers(0, 1, &smp);
        c->PSGetShader(&ps, ps_inst, &ps_n);
        c->VSGetShader(&vs, vs_inst, &vs_n);
        c->GSGetShader(&gs, gs_inst, &gs_n);
        c->HSGetShader(&hs, hs_inst, &hs_n);
        c->DSGetShader(&ds, ds_inst, &ds_n);
        c->VSGetConstantBuffers(0, 1, &vs_cb);
        c->PSGetConstantBuffers(0, 1, &ps_cb);
        c->IAGetPrimitiveTopology(&topo);
        c->IAGetIndexBuffer(&ib, &ib_fmt, &ib_off);
        c->IAGetVertexBuffers(0, 1, &vb, &vb_stride, &vb_off);
        c->IAGetInputLayout(&il);
    }
    template <typename T> static void rel(T *&p) {
        if (p) p->Release();
        p = nullptr;
    }
    static void rel_all(ID3D11ClassInstance **a, UINT n) {
        for (UINT i = 0; i < n; i++)
            if (a[i]) a[i]->Release();
    }
    void restore(ID3D11DeviceContext *c) {
        c->OMSetRenderTargets(1, &rtv, dsv);
        c->RSSetViewports(vp_n, vp);
        c->RSSetScissorRects(sc_n, sc);
        c->RSSetState(rs);
        c->OMSetBlendState(bs, bf, mask);
        c->OMSetDepthStencilState(dss, sref);
        c->PSSetShaderResources(0, 1, &srv);
        c->PSSetShaderResources(1, 1, &srv1);
        c->PSSetShaderResources(2, 1, &srv2);
        c->PSSetSamplers(0, 1, &smp);
        c->PSSetShader(ps, ps_inst, ps_n);
        c->VSSetShader(vs, vs_inst, vs_n);
        c->GSSetShader(gs, gs_inst, gs_n);
        c->HSSetShader(hs, hs_inst, hs_n);
        c->DSSetShader(ds, ds_inst, ds_n);
        c->VSSetConstantBuffers(0, 1, &vs_cb);
        c->PSSetConstantBuffers(0, 1, &ps_cb);
        c->IASetPrimitiveTopology(topo);
        c->IASetIndexBuffer(ib, ib_fmt, ib_off);
        c->IASetVertexBuffers(0, 1, &vb, &vb_stride, &vb_off);
        c->IASetInputLayout(il);
        rel(rtv); rel(dsv); rel(rs); rel(bs); rel(dss); rel(srv); rel(srv1); rel(srv2); rel(smp); rel(ps); rel(vs); rel(gs); rel(hs); rel(ds);
        rel(vs_cb); rel(ps_cb); rel(ib); rel(vb); rel(il);
        rel_all(ps_inst, ps_n); rel_all(vs_inst, vs_n); rel_all(gs_inst, gs_n); rel_all(hs_inst, hs_n); rel_all(ds_inst, ds_n);
    }
};

bool ensure_depth(ID3D11Device *dev, UINT w, UINT h, UINT samples) {
    if (g.dsv && g.depth_w == w && g.depth_h == h && g.depth_samples == samples) return true;
    if (g.dsv) g.dsv->Release();
    if (g.depth_tex) g.depth_tex->Release();
    g.dsv = nullptr;
    g.depth_tex = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_D32_FLOAT;
    d.SampleDesc.Count = samples;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g.depth_tex))) return false;
    if (FAILED(dev->CreateDepthStencilView(g.depth_tex, nullptr, &g.dsv))) return false;
    g.depth_w = w;
    g.depth_h = h;
    g.depth_samples = samples;
    return true;
}

}   // namespace

const char *hud_error() { return g.error.c_str(); }

const BeamLook BEAM_LOOKS[BEAM_KINDS] = {
    {0.0f, {1, 1, 1}, {1, 1, 1}},
    {0.25f, {1, 1, 1}, {1, 0.81f, 0}},
    {0.35f, {1, 0.72f, 0}, {1, 1, 1}},
    {0.5f, {1, 1, 1}, {0, 0.83f, 1}},
    {0.5f, {1, 1, 1}, {1, 0.24f, 0.24f}},
    {1.0f, {1, 1, 1}, {0, 0.83f, 1}},
    {1.0f, {1, 1, 1}, {1, 0.63f, 0}},
};

void hud_strip(HudState &st, const float *pa, const float *pb, float width, const float *c0, const float *c1, float alpha, const char *sprite, float u0,
               float u1, float wmax) {
    if (st.tracer_count >= HudState::MAX_TRACERS || !st.cam_valid) return;
    float v[2][3], col[2][3], uu[2] = {u0, u1};
    memcpy(col[0], c0, sizeof(col[0]));
    memcpy(col[1], c1, sizeof(col[1]));
    for (int e = 0; e < 2; e++) {
        const float *p = e ? pb : pa;
        float d[3] = {p[0] - st.cam_eye[0], p[1] - st.cam_eye[1], p[2] - st.cam_eye[2]};
        v[e][0] = d[0] * st.cam_right[0] + d[1] * st.cam_right[1] + d[2] * st.cam_right[2];
        v[e][1] = d[0] * st.cam_up[0] + d[1] * st.cam_up[1] + d[2] * st.cam_up[2];
        v[e][2] = d[0] * st.cam_fwd[0] + d[1] * st.cam_fwd[1] + d[2] * st.cam_fwd[2];
    }
    const float NEAR_Z = 0.1f;
    if (v[0][2] < NEAR_Z && v[1][2] < NEAR_Z) return;
    for (int e = 0; e < 2; e++) {                 // an end behind the eye is cut back to just in front of it
        if (v[e][2] >= NEAR_Z) continue;
        float k = (NEAR_Z - v[e][2]) / (v[1 - e][2] - v[e][2]);
        for (int i = 0; i < 3; i++) {
            v[e][i] += (v[1 - e][i] - v[e][i]) * k;
            col[e][i] += (col[1 - e][i] - col[e][i]) * k;
        }
        uu[e] += (uu[1 - e] - uu[e]) * k;
    }
    HudState::TracerLine &ln = st.tracers[st.tracer_count++];
    ln.x0 = v[0][0] / (v[0][2] * st.cam_tan_x);
    ln.y0 = v[0][1] / (v[0][2] * st.cam_tan_y);
    ln.x1 = v[1][0] / (v[1][2] * st.cam_tan_x);
    ln.y1 = v[1][1] / (v[1][2] * st.cam_tan_y);
    float w0 = width * 0.5f / (v[0][2] * st.cam_tan_y * 2.0f), w1 = width * 0.5f / (v[1][2] * st.cam_tan_y * 2.0f);
    ln.w0 = w0 < 0.0008f ? 0.0008f : w0 > wmax ? wmax : w0;
    ln.w1 = w1 < 0.0008f ? 0.0008f : w1 > wmax ? wmax : w1;
    ln.r = col[0][0]; ln.g = col[0][1]; ln.b = col[0][2];
    ln.r1 = col[1][0]; ln.g1 = col[1][1]; ln.b1 = col[1][2];
    ln.a = alpha;
    ln.a1 = alpha;
    ln.plain = true;
    ln.grad = true;
    ln.sprite = sprite;
    ln.u0 = uu[0];
    ln.u1 = uu[1];
    ln.z0 = v[0][2];
    ln.z1 = v[1][2];
}

void hud_trail_begin(HudState &st, bool additive) {
    st.trail_open = false;
    st.trail_has_prev = false;
    st.trail_additive = additive;
}

void hud_trail_point(HudState &st, const float *p, float width, const float *rgb, float alpha) {
    if (!st.cam_valid) return;
    const float NEAR_Z = 0.1f, WMAX = 0.12f;
    float d[3] = {p[0] - st.cam_eye[0], p[1] - st.cam_eye[1], p[2] - st.cam_eye[2]};
    float cur[8] = {d[0] * st.cam_right[0] + d[1] * st.cam_right[1] + d[2] * st.cam_right[2], d[0] * st.cam_up[0] + d[1] * st.cam_up[1] + d[2] * st.cam_up[2],
                    d[0] * st.cam_fwd[0] + d[1] * st.cam_fwd[1] + d[2] * st.cam_fwd[2], width, rgb[0], rgb[1], rgb[2], alpha};
    auto emit = [&](const float *v) {
        if (!st.trail_open) {
            if (st.ribbon_count >= HudState::MAX_RIBBONS) return;
            st.ribbons[st.ribbon_count++] = {st.ribbon_point_count, 0, st.trail_additive};
            st.trail_open = true;
        }
        if (st.ribbon_point_count >= HudState::MAX_RIBBON_POINTS) return;
        HudState::RibbonPoint &o = st.ribbon_points[st.ribbon_point_count++];
        o.x = v[0] / (v[2] * st.cam_tan_x);
        o.y = v[1] / (v[2] * st.cam_tan_y);
        float w = v[3] * 0.5f / (v[2] * st.cam_tan_y * 2.0f);
        o.w = w > WMAX ? WMAX : w;
        o.r = v[4]; o.g = v[5]; o.b = v[6]; o.a = v[7];
        o.z = v[2];
        st.ribbons[st.ribbon_count - 1].count++;
    };
    // where the line from the last point to this one comes through the plane just in front of the eye
    auto cut = [&](float *out) {
        const float *q = st.trail_prev;
        float k = (NEAR_Z - q[2]) / (cur[2] - q[2]);
        for (int i = 0; i < 8; i++) out[i] = q[i] + (cur[i] - q[i]) * k;
        out[2] = NEAR_Z;
    };
    bool in = cur[2] >= NEAR_Z;
    if (!st.trail_has_prev) {
        if (in) emit(cur);
    } else {
        bool was_in = st.trail_prev[2] >= NEAR_Z;
        float edge[8];
        if (was_in && in) {
            emit(cur);
        } else if (was_in) {                              // it goes behind the eye: the strip ends at the plane
            cut(edge);
            emit(edge);
            st.trail_open = false;
        } else if (in) {                                  // and comes back: a new strip from the plane
            cut(edge);
            st.trail_open = false;
            emit(edge);
            emit(cur);
        }
    }
    memcpy(st.trail_prev, cur, sizeof(cur));
    st.trail_has_prev = true;
}

void hud_beam(HudState &st, const float *a, const float *b, int kind, float width, float unit, unsigned seen, int seed) {
    static const char *const ARCS[10] = {"arc0", "arc1", "arc2", "arc3", "arc4", "arc5", "arc6", "arc7", "arc8", "arc9"};
    if (kind <= BEAM_NONE || kind >= BEAM_KINDS) return;
    const BeamLook &look = BEAM_LOOKS[kind];
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    auto at = [&](float f, float *p) {
        for (int i = 0; i < 3; i++) p[i] = a[i] + d[i] * f;
    };
    auto tone = [&](float f, float *c) {
        for (int i = 0; i < 3; i++) c[i] = look.c0[i] + (look.c1[i] - look.c0[i]) * f;
    };
    // The wide lines round a beam start from nothing at the gun (ours: seen from behind the gun, a strip
    // metres wide that begins a metre from the eye would open with a hard edge across the screen).
    auto soften = [&](int before, int k) {
        if (k == 0 && st.tracer_count > before) st.tracers[before].a = 0.0f;
    };
    if (kind == BEAM_RAIL && len > 0.01f) {
        // what is picked afresh every twentieth of a second: the arcs' picture, width and two colours
        uint32_t h = (uint32_t)(st.time / 0.05) * 2654435761u + (uint32_t)seed * 40503u;
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        float arcs_w = width * (15.0f + 5.0f * (h & 255) / 255.0f), ca[3], cb[3];
        tone(((h >> 8) & 255) / 255.0f, ca);
        tone(((h >> 16) & 255) / 255.0f, cb);
        const char *arc = ARCS[(h >> 24) % 10];
        const float quarter = 40.0f * unit / 4.0f;        // the picture repeats every 40 u; drawn a quarter at a time
        int n = (int)ceilf(len / quarter);
        n = n < 1 ? 1 : n > 40 ? 40 : n;
        for (int pass = 0; pass < 2; pass++)
            for (int k = 0; k < n; k++) {
                float f0 = k * quarter / len, f1 = fminf((k + 1) * quarter / len, 1.0f), pa[3], pb[3], c0[3], c1[3];
                if (f0 >= 1.0f) break;
                at(f0, pa);
                at(f1, pb);
                int before = st.tracer_count;
                if (pass == 0) {
                    tone(f0, c0);
                    tone(f1, c1);
                    hud_strip(st, pa, pb, width * 10.0f, c0, c1, 1.0f, "lineglow16", 0.0f, 1.0f, 0.25f);
                } else {
                    for (int i = 0; i < 3; i++) {
                        c0[i] = ca[i] + (cb[i] - ca[i]) * f0;
                        c1[i] = ca[i] + (cb[i] - ca[i]) * f1;
                    }
                    float u0 = (k % 4) * 0.25f;
                    hud_strip(st, pa, pb, arcs_w, c0, c1, 1.0f, arc, u0, u0 + 0.25f * (f1 - f0) * len / quarter, 0.4f);
                }
                soften(before, k);
            }
    } else if (kind == BEAM_MALICIOUS && len > 0.01f) {
        const float white[3] = {1, 1, 1};
        for (int k = 0; k < 12; k++) {
            float pa[3], pb[3];
            at(k / 12.0f, pa);
            at((k + 1) / 12.0f, pb);
            int before = st.tracer_count;
            hud_strip(st, pa, pb, width * 3.0f, white, white, 1.0f, "charge2", k / 12.0f, (k + 1) / 12.0f, 0.2f);
            soften(before, k);
        }
    }
    // The line itself, eighth by eighth where it may be seen, so that its two colours meet where they
    // would in depth and not halfway across the screen.
    for (int k = 0; k < 8; k++) {
        if (!(seen & (1u << k))) continue;
        float pa[3], pb[3], c0[3], c1[3];
        at(k / 8.0f, pa);
        at((k + 1) / 8.0f, pb);
        tone(k / 8.0f, c0);
        tone((k + 1) / 8.0f, c1);
        hud_strip(st, pa, pb, width, c0, c1, 1.0f, nullptr, 0, 0, kind >= BEAM_RAIL ? 0.05f : 0.02f);
    }
}

void hud_set_depth(ID3D11ShaderResourceView *srv, float a, float b) {
    if (srv) srv->AddRef();
    if (g.scene_depth) g.scene_depth->Release();
    g.scene_depth = srv;
    g.depth_a = a;
    g.depth_b = b;
}

int hud_model_dirs(const char *name, float *xyz, int max_verts) {
    Model *m = find_model(name);
    if (!m || m->meshes.empty()) return 0;
    const SkinMesh &sm = m->meshes[0];
    int n = (int)sm.vertex_count < max_verts ? (int)sm.vertex_count : max_verts;
    for (int v = 0; v < n; v++) {
        const float *src = &sm.verts[(size_t)v * 8];
        float len = sqrtf(src[0] * src[0] + src[1] * src[1] + src[2] * src[2]);
        for (int i = 0; i < 3; i++) xyz[v * 3 + i] = len > 1e-6f ? src[i] / len : (i == 1 ? 1.0f : 0.0f);
    }
    return n;
}

int hud_death_lines() { return (int)g.death_lines.size(); }
bool hud_death_line_warning(int i) { return i >= 0 && i < (int)g.death_orange.size() && g.death_orange[i]; }

bool hud_has_model(const char *name) {
    return find_model(name) != nullptr;
}

bool hud_keep_frame(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *target) {
    D3D11_TEXTURE2D_DESC td, kd{};
    target->GetDesc(&td);
    if (g.kept) g.kept->GetDesc(&kd);
    if (!g.kept || kd.Width != td.Width || kd.Height != td.Height || kd.Format != td.Format || kd.SampleDesc.Count != td.SampleDesc.Count) {
        if (g.kept) g.kept->Release();
        g.kept = nullptr;
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &g.kept))) return false;
    }
    ctx->CopyResource(g.kept, target);
    return true;
}

void hud_put_frame(ID3D11DeviceContext *ctx, ID3D11Texture2D *target) {
    if (!g.kept) return;
    D3D11_TEXTURE2D_DESC td, kd;
    target->GetDesc(&td);
    g.kept->GetDesc(&kd);
    if (kd.Width == td.Width && kd.Height == td.Height && kd.Format == td.Format && kd.SampleDesc.Count == td.SampleDesc.Count) ctx->CopyResource(target, g.kept);
}

bool hud_muzzle(float *x, float *y) {
    *x = g_muzzle[0];
    *y = g_muzzle[1];
    return g_muzzle_known;
}

bool hud_whip_hand(float *x, float *y) {
    if (!g_whip_hand_known) return false;
    // kept within the picture: a hand drawn off its edge would start the cable from nowhere
    *x = fminf(fmaxf(g_whip_hand[0], -1.0f), 1.0f);
    *y = fminf(fmaxf(g_whip_hand[1], -1.0f), 1.0f);
    return true;
}

void hud_title_column(bool on) { g_title_column = on; }
bool hud_title_cursor(float w, float h, float *x, float *y) {
    if (GetTickCount64() - g_title_col_tick > 250 || w < 1 || h < 1) return false;
    const float ppu = fminf(w / 1280.0f, h / 720.0f), cx = (*x - w * 0.5f) / ppu, cy = (h * 0.5f - *y) / ppu;
    const TitleMenu m = g.title_menu;
    if (cx >= TITLE_BTN_X && cx < TITLE_BTN_X + TITLE_BTN_W) {
        for (int i = 0; i < 4; i++) {
            float top = TITLE_BTN_TOP - TITLE_BTN_STEP * i;
            if (m.items[i] < 0 || cy > top || cy <= top - TITLE_BTN_H) continue;
            *x = w * (0.42f + 0.16f * (cx - TITLE_BTN_X) / TITLE_BTN_W);
            *y = h * (TITLE_ROW0 + TITLE_ROW_STEP * i + 0.022f * ((top - cy) / TITLE_BTN_H - 0.5f));
            return true;
        }
        for (int k = 0; k < 2; k++) {
            float mid = k ? TITLE_MARK_DOWN : TITLE_MARK_UP;
            if (!(k ? m.down : m.up) || fabsf(cy - mid) > 13.0f) continue;
            *x = w * 0.4995f;
            *y = h * (k ? TITLE_ARROW_DOWN : TITLE_ARROW_UP);
            return true;
        }
    }
    if (*x > w * 0.38f && *x < w * 0.62f && *y > h * 0.665f && *y < h * 0.892f) {
        *x = w * 0.30f;
        return true;
    }
    return false;
}

const char *hud_title_state() {
    static char out[96];
    out[0] = 0;
    if (!g.title_col) return out;
    const TitleMenu &m = g.title_menu;
    for (int i = 0; i < 4; i++) {
        if (m.items[i] < 0) continue;
        size_t n = strlen(out);
        snprintf(out + n, sizeof(out) - n, i == m.row ? "%s[%s]" : "%s%s", n ? " " : "", TITLE_WORDS[m.items[i]]);
    }
    size_t n = strlen(out);
    snprintf(out + n, sizeof(out) - n, "%s%s", m.up ? " ^" : "", m.down ? " v" : "");
    return out;
}

bool hud_init(ID3D11Device *device, const wchar_t *pack_path) {
    static bool tried = false;
    if (tried) return g.ready;
    tried = true;
    g.ready = load_pack(device, pack_path) && create_pipeline(device);
    if (g.ready) {
        std::wstring models(pack_path);
        size_t slash = models.find_last_of(L"\\/");
        models = (slash == std::wstring::npos ? std::wstring() : models.substr(0, slash + 1)) + L"ultrasouls_models.bin";
        load_models(device, models.c_str());
        std::wstring words(pack_path);
        words = (slash == std::wstring::npos ? std::wstring() : words.substr(0, slash + 1)) + L"ultrasouls_text.txt";
        load_text(words.c_str());
    }
    return g.ready;
}

void hud_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *target, const HudState &st) {
    if (!g.ready) return;
    D3D11_TEXTURE2D_DESC td;
    target->GetDesc(&td);
    bool srgb = td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (st.title) {
        TitleMenu seen;
        if (!title_probe(dev, ctx, target, td, seen)) {
            g.title_col = false;
            return;
        }
        title_settle(seen, st.time);
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Format = td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM
                : td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : td.Format;
    ID3D11RenderTargetView *rtv = nullptr;
    if (FAILED(dev->CreateRenderTargetView(target, &rd, &rtv)) || !rtv) return;

    g.verts.clear();
    g.draws.clear();
    build_hud(st, (float)td.Width, (float)td.Height);
    if (st.title && g.title_col) g_title_col_tick = GetTickCount64();

    Saved saved;
    saved.save(ctx);

    D3D11_VIEWPORT vp{0, 0, (float)td.Width, (float)td.Height, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetScissorRects(0, nullptr);
    ctx->RSSetState(g.raster);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    bool weapon_drawn = false;
    if (!st.flash_only && st.death_time < 0 && st.show_viewmodel && (!g.meshes.empty() || !g.models.empty()) && ensure_depth(dev, td.Width, td.Height, td.SampleDesc.Count)) {
        ctx->ClearDepthStencilView(g.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ctx->OMSetRenderTargets(1, &rtv, g.dsv);
        draw_world(ctx, st, srgb);
        ctx->ClearDepthStencilView(g.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        draw_viewmodel(ctx, st, (float)td.Width / (float)td.Height, srgb);
        weapon_drawn = true;
    }

    // The flat pass draws the HUD and, with it, everything of the world that is a picture or a line: sparks,
    // rings, glows, tracers, beams. Until v0.70 it was drawn with no depth at all, so those lay over the
    // weapon and the arms (an explosion's ring and sparks across a punching arm, in Davi's screenshot),
    // while the effect meshes, drawn before the weapon, lay under them. Now the weapon's depth buffer
    // stays bound, read only: see the vertex shader.
    ctx->OMSetRenderTargets(1, &rtv, weapon_drawn ? g.dsv : nullptr);
    ctx->OMSetBlendState(g.blend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(weapon_drawn ? g.depth_read : g.depth_off, 0);
    if (!g.verts.empty()) {
        UINT need = (UINT)g.verts.size();
        if (need > g.ui_vb_cap) {
            if (g.ui_vb) g.ui_vb->Release();
            g.ui_vb = nullptr;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = (need + 1024) * sizeof(UiVert);
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g.ui_vb))) g.ui_vb_cap = need + 1024;
        }
        D3D11_MAPPED_SUBRESOURCE ms;
        if (g.ui_vb && SUCCEEDED(ctx->Map(g.ui_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            memcpy(ms.pData, g.verts.data(), need * sizeof(UiVert));
            ctx->Unmap(g.ui_vb, 0);
            UINT stride = sizeof(UiVert), offset = 0;
            ctx->IASetVertexBuffers(0, 1, &g.ui_vb, &stride, &offset);
            ctx->IASetInputLayout(g.ui_il);
            ctx->VSSetShader(g.ui_vs, nullptr, 0);
            ctx->PSSetShader(g.ui_ps, nullptr, 0);
            ctx->PSSetConstantBuffers(0, 1, &g.ui_cb);
            ctx->PSSetSamplers(0, 1, &g.linear);
            ctx->PSSetShaderResources(2, 1, &g.scene_depth);
            bool fx_ready = false;
            for (const UiDraw &d : g.draws)
                if (d.fx && (d.fx == 2 ? g.menu_ps : g.death_ps) && td.SampleDesc.Count == 1 && !fx_ready) {
                    // the picture as it stands, for the death shader to read while it writes over it
                    D3D11_TEXTURE2D_DESC fd{};
                    if (g.fx_tex) g.fx_tex->GetDesc(&fd);
                    if (!g.fx_tex || fd.Width != td.Width || fd.Height != td.Height || fd.Format != td.Format) {
                        if (g.fx_srv) g.fx_srv->Release();
                        if (g.fx_tex) g.fx_tex->Release();
                        g.fx_srv = nullptr;
                        g.fx_tex = nullptr;
                        fd = td;
                        fd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        fd.Usage = D3D11_USAGE_DEFAULT;
                        fd.CPUAccessFlags = 0;
                        fd.MiscFlags = 0;
                        if (SUCCEEDED(dev->CreateTexture2D(&fd, nullptr, &g.fx_tex))) {
                            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                            vd.Format = rd.Format;
                            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                            vd.Texture2D.MipLevels = 1;
                            dev->CreateShaderResourceView(g.fx_tex, &vd, &g.fx_srv);
                        }
                    }
                    if (g.fx_tex && g.fx_srv) {
                        ctx->CopyResource(g.fx_tex, target);
                        fx_ready = true;
                    }
                }
            for (int pass = 1; pass >= 0; pass--)            // the world's first, then the HUD's own over it
            for (const UiDraw &d : g.draws) {
                if (d.world != pass) continue;
                if (d.fx) {
                    if (!fx_ready) continue;
                    float k = fminf(fmaxf(st.death_time * 0.5f, 0.0f), 1.0f);     // DeathSequence.Update: both run up to 1 in 2 s
                    float par[8] = {k, k, 300.0f + (float)fmod(st.time, 600.0), 0, 1.0f / (float)td.Width, 1.0f / (float)td.Height, 0, 0};
                    if (SUCCEEDED(ctx->Map(g.ui_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                        memcpy(ms.pData, par, sizeof(par));
                        ctx->Unmap(g.ui_cb, 0);
                    }
                    ctx->PSSetShader(d.fx == 2 ? g.menu_ps : g.death_ps, nullptr, 0);
                    ctx->PSSetShaderResources(0, 1, &g.fx_srv);
                    ctx->PSSetSamplers(0, 1, &g.point);
                    ctx->OMSetBlendState(g.opaque, nullptr, 0xFFFFFFFF);
                    ctx->Draw(d.count, d.start);
                    ctx->PSSetShader(g.ui_ps, nullptr, 0);
                    ctx->PSSetSamplers(0, 1, &g.linear);
                    continue;
                }
                float mode[8] = {(float)d.sdf, srgb ? 1.0f : 0.0f, 0, 0, g.depth_a, g.depth_b, g.scene_depth ? 1.0f : 0.0f, 0};
                if (SUCCEEDED(ctx->Map(g.ui_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                    memcpy(ms.pData, mode, sizeof(mode));
                    ctx->Unmap(g.ui_cb, 0);
                }
                ID3D11ShaderResourceView *srv = d.tex >= 0 ? g.textures[d.tex].srv : g.white;
                ctx->PSSetShaderResources(0, 1, &srv);
                ctx->OMSetBlendState(d.add && g.blend_add ? g.blend_add : g.blend, nullptr, 0xFFFFFFFF);
                ctx->Draw(d.count, d.start);
            }
        }
    }

    saved.restore(ctx);
    rtv->Release();
}
