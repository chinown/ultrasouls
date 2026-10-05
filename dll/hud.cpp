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
};
struct UiDraw {
    int tex;      // -1: plain colour
    int sdf;      // 1: distance-field text
    UINT start, count;
};

struct Gfx {
    bool ready = false;
    std::string error;
    std::vector<Tex> textures;
    std::vector<Sprite> sprites;
    std::vector<Font> fonts;
    std::vector<Mesh> meshes;
    std::vector<Model> models;
    ID3D11ShaderResourceView *white = nullptr;

    ID3D11VertexShader *ui_vs = nullptr, *mesh_vs = nullptr;
    ID3D11PixelShader *ui_ps = nullptr, *mesh_ps = nullptr;
    ID3D11InputLayout *ui_il = nullptr, *mesh_il = nullptr;
    ID3D11Buffer *ui_cb = nullptr, *mesh_cb = nullptr, *ui_vb = nullptr, *screen_vb = nullptr;
    ID3D11Texture2D *kept = nullptr;         // the frame kept aside during a parry's freeze
    UINT ui_vb_cap = 0;
    ID3D11BlendState *blend = nullptr, *opaque = nullptr;
    ID3D11RasterizerState *raster = nullptr;
    ID3D11DepthStencilState *depth_off = nullptr, *depth_on = nullptr;
    ID3D11SamplerState *linear = nullptr, *point = nullptr;
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
cbuffer C : register(b0) { float4 mode; }      // x: 1 = distance-field text, y: 1 = target stores sRGB
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VI { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
VO vs(VI i) { VO o; o.pos = i.pos; o.uv = i.uv; o.col = i.col; return o; }
float4 ps(VO i) : SV_TARGET {
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
cbuffer C : register(b0) { row_major float4x4 mvp; row_major float4x4 world; float4 light; float4 mode; float4 tint; }
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VI { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float shade : TEXCOORD1; };
VO vs(VI i) {
    VO o;
    o.pos = mul(float4(i.pos, 1), mvp);
    float3 n = normalize(mul(float4(i.nrm, 0), world).xyz);
    o.shade = light.w + (1 - light.w) * saturate(dot(n, light.xyz));
    o.uv = i.uv;
    return o;
}
float4 ps(VO i) : SV_TARGET {
    float4 t = tex.Sample(smp, i.uv);
    if (mode.x > 0.5 && t.a < 0.5) discard;          // a display's picture: its transparent parts are cut out
    float4 c = float4(t.rgb * i.shade * tint.rgb, 1);
    if (mode.y > 0.5) c.rgb = pow(abs(c.rgb), 2.2);
    return c;
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
    const D3D11_INPUT_ELEMENT_DESC ui_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(ui_layout, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &g.ui_il);
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
    cb.ByteWidth = 16;
    dev->CreateBuffer(&cb, nullptr, &g.ui_cb);
    cb.ByteWidth = 176;
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
    bd.RenderTarget[0].BlendEnable = FALSE;
    dev->CreateBlendState(&bd, &g.opaque);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g.raster);

    D3D11_DEPTH_STENCIL_DESC dd{};
    dev->CreateDepthStencilState(&dd, &g.depth_off);
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    dev->CreateDepthStencilState(&dd, &g.depth_on);

    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    dev->CreateSamplerState(&sd, &g.linear);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    dev->CreateSamplerState(&sd, &g.point);

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
    }
    UINT start = (UINT)g.verts.size();
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int k : order) g.verts.push_back(q[k]);
    if (!g.draws.empty() && g.draws.back().tex == tex && g.draws.back().sdf == sdf)
        g.draws.back().count += 6;
    else
        g.draws.push_back({tex, sdf, start, 6});
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
    float after_health = 100, shown_stamina = 300;
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

void build_hud(const HudState &st, float screen_w, float screen_h) {
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
    // the orange bar lags behind the red one when health drops, and snaps up when it rises
    if (anim.after_health < st.health) anim.after_health = st.health;
    else anim.after_health = fmaxf(st.health, anim.after_health - 30.0f * dt);

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
        float after = fminf(anim.after_health, 100.0f) / 100.0f, hp = fminf(fmaxf(st.health, 0.0f), 100.0f) / 100.0f;
        image_sliced(sf, child(track, rt(0, 0, after, 1, 0, 0, 0, 0)), g.sprite("Round_FillSmall"), {1, 0.3931f, 0, 1});
        image_sliced(sf, child(track, rt(0, 0, hp, 1, 0, 0, 0, 0)), g.sprite("Round_FillSmall"), {1, 0, 0, 1});
        text(sf, child(inner, rt(0, 0, 0, 1, 5.0002f, 0, 10, -78, 0, 0.5f)), "+", 17.1f, {1, 1, 1, 1}, 1);
        char num[16];
        snprintf(num, sizeof(num), "%d", (int)ceilf(fmaxf(st.health, 0.0f)));
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

    Box gun = child(canvas, rt(0, 0, 0, 0, -79, 187, 200, 100, 0, 0, 4, 4));
    image_sliced(sf, gun, g.sprite("Round_FillLarge"), PANEL);
    {
        Box icon = child(child(gun, rt(0, 0, 1, 1, 0, 0, -10, -10)), FILL);
        // each weapon and variation has its own drawing; the shotgun's first variation is the Core Eject
        static const char *const rev_icons[3] = {"SingleRevolver", "RevolverSpecial", "RevolverSharp"};
        static const char *const rev_glows[3] = {"SingleRevolverGlow", "RevolverSpecialGlow", "RevolverSharpGlow"};
        int var = st.variation < 0 ? 0 : st.variation > 2 ? 2 : st.variation;
        int rev = g.sprite(st.weapon == 1 ? "Shotgun" : rev_icons[var]);
        int glow = g.sprite(st.weapon == 1 ? "ShotgunGlow" : rev_glows[var]);
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
        int ref = g.sprite("RevolverSpecialGlow");
        if (ref >= 0) s = fminf(s, fminf(icon.w() / g.textures[g.sprites[ref].tex].w, icon.h() / g.textures[g.sprites[ref].tex].h));
        // ULTRAKILL tints the icon with the variation's colour: blue, green, red
        const Color tint = st.weapon == 1 ? PIERCER_BLUE : var == 1 ? MARKSMAN_GREEN : var == 2 ? SHARP_RED : PIERCER_BLUE;
        image_simple(sf, icon, rev, tint, false, s);
        image_simple(sf, icon, glow, {tint.r, tint.g, tint.b, 0.749f}, false, s);
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
        float hp = fminf(fmaxf(st.health, 0.0f), 100.0f) / 100.0f, after = fminf(anim.after_health, 100.0f) / 100.0f;
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
    // tracers: a bright core inside a wider, fainter glow, each a quad along the projected line
    for (int i = 0; i < st.tracer_count && i < HudState::MAX_TRACERS; i++) {
        const HudState::TracerLine &t = st.tracers[i];
        float hw = screen_w * 0.5f / sc.px_per_unit, hh = screen_h * 0.5f / sc.px_per_unit;
        float ax = t.x0 * hw, ay = t.y0 * hh, bx = t.x1 * hw, by = t.y1 * hh;
        float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
        if (len < 0.01f) continue;
        float px = -dy / len, py = dx / len;
        for (int pass = 0; pass < 2; pass++) {
            float k = (pass == 0 ? 3.0f : 1.0f) * 2.0f * hh;       // half-width fraction of the height -> canvas units
            float wa = t.w0 * k, wb = t.w1 * k;
            const float lx[4] = {ax - px * wa, ax + px * wa, bx + px * wb, bx - px * wb};
            const float ly[4] = {ay - py * wa, ay + py * wa, by + py * wb, by - py * wb};
            const float uv[4] = {0, 0, 0, 0};
            Color c = pass == 0 ? Color{t.r, t.g, t.b, t.a * 0.25f} : Color{fminf(t.r + 0.5f, 1.0f), fminf(t.g + 0.5f, 1.0f), fminf(t.b + 0.5f, 1.0f), t.a};
            push_quad(sc, screen.xf, -1, 0, lx, ly, uv, uv, c);
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
    // explosions and glowing projectiles: a soft ball, and a shock ring that runs ahead of it
    for (int i = 0; i < st.blast_count && i < HudState::MAX_BLASTS; i++) {
        const HudState::Blast &b = st.blasts[i];
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
    }
    {
        // the muzzle flash, over the barrel for a few frames
        float since = (float)(st.time - st.muzzle_flash);
        if (since >= 0 && since < 0.07f && g_muzzle_known && st.show_viewmodel) {
            float d = (st.weapon == 1 ? 0.34f : 0.22f) * screen_h / sc.px_per_unit * (1.0f - since * 5.0f);
            Box fl = child(screen, rt(0.5f, 0.5f, 0.5f, 0.5f, g_muzzle[0] * screen_w * 0.5f / sc.px_per_unit, g_muzzle[1] * screen_h * 0.5f / sc.px_per_unit,
                                      d, d, 0.5f, 0.5f, 1, 1, (float)((int)(st.muzzle_flash * 1000.0) % 90)));
            image_simple(sc, fl, g.sprite(st.weapon == 1 ? "muzzleflashshotgun" : "muzzleflash"), {1, 1, 1, 1.0f - since * 8.0f});
        }
    }
    if (st.flash > 0.001f) rect_quad(sc, screen.xf, -1, screen.x0, screen.y0, screen.x1, screen.y1, 0, 0, 1, 1, {1, 1, 1, fminf(st.flash, 1.0f) * 0.85f});
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
    float mvp[16], world[16], light[4], mode[4], tint[4];
};

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
void pose_model(const Model &m, const Clip *clip, float time, bool loop, std::vector<float> &world, const Clip *base = nullptr,
                float base_time = 0, float weight = 1.0f, int spin_node = -1, float spin = 0) {
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
        if (n == spin_node && spin != 0) {
            float s = sinf(spin * 0.5f), c = cosf(spin * 0.5f), x = t[3], y = t[4], z = t[5], w = t[6];
            t[3] = x * c + y * s;
            t[4] = y * c - x * s;
            t[5] = w * s + z * c;
            t[6] = w * c - z * s;
        }
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

// `at_node` (if not negative) gets that node's position in the model's space, for finding the muzzle.
void draw_model(ID3D11DeviceContext *ctx, Model &m, const Clip *clip, float time, bool loop, const Clip *base = nullptr, float base_time = 0,
                float weight = 1.0f, int spin_node = -1, float spin = 0, int at_node = -1, float *at = nullptr) {
    std::vector<float> &world = g_pose;
    pose_model(m, clip, time, loop, world, base, base_time, weight, spin_node, spin);
    if (at && at_node >= 0 && at_node < m.nodes)
        for (int i = 0; i < 3; i++) at[i] = world[(size_t)at_node * 12 + i * 4 + 3];
    for (SkinMesh &sm : m.meshes) {
        if (!sm.vb || !sm.ib) continue;
        skin_mesh(ctx, sm, world);
        UINT stride = 32, offset = 0;
        ctx->IASetVertexBuffers(0, 1, &sm.vb, &stride, &offset);
        ctx->IASetIndexBuffer(sm.ib, DXGI_FORMAT_R32_UINT, 0);
        ID3D11ShaderResourceView *srv = sm.srv ? sm.srv : g.white;
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->DrawIndexed(sm.index_count, 0, 0);
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
void draw_screens(ID3D11DeviceContext *ctx, Model &m, const HudState &st, const MeshConsts &base) {
    if (m.screens.empty() || g_pose.size() < (size_t)m.nodes * 12) return;
    if (!g.screen_vb) {
        ID3D11Device *dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 6 * 32;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &g.screen_vb);
        dev->Release();
        if (!g.screen_vb) return;
    }
    const char *prefix = st.weapon == 1 ? "core." : st.variation == 1 ? "marksman." : st.variation == 2 ? "sharp." : "pierce.";
    size_t plen = strlen(prefix);
    const float blue[3] = {0.25f, 0.91f, 1.0f}, green[3] = {0.2667f, 1.0f, 0.2706f}, red[3] = {1.0f, 0.2392f, 0.2392f};
    MeshConsts c = base;
    c.light[3] = 1.0f;                                // all ambient: the displays light themselves
    c.mode[0] = 1.0f;
    int layer = 0;
    for (const Screen &sc : m.screens) {
        if (sc.name.compare(0, plen, prefix) != 0) continue;
        const char *what = sc.name.c_str() + plen;
        float fill = 1.0f, rgb[3] = {sc.rgba[0], sc.rgba[1], sc.rgba[2]};
        int tex = sc.tex;
        if (!strcmp(what, "monitor")) {
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
            fill = fminf(fmaxf((sharp ? st.sharp_charge : st.coin_charge) / 100.0f - i, 0.0f), 1.0f);
            if (fill >= 1.0f) memcpy(rgb, sharp ? red : green, sizeof(rgb));
            else if (sharp) rgb[0] = rgb[1] = rgb[2] = 0.5f;
            else { rgb[0] = 1; rgb[1] = 0; rgb[2] = 0; }
        } else if (!strcmp(what, "fill")) {
            fill = fminf(fmaxf(st.core_meter, 0.0f), 1.0f);
            float k = fminf(fmaxf(st.core_meter_red, 0.0f), 1.0f);
            rgb[0] = blue[0] + (1.0f - blue[0]) * k;
            rgb[1] = blue[1] + (0.25f - blue[1]) * k;
            rgb[2] = blue[2] + (0.25f - blue[2]) * k;
        }
        layer++;
        if (fill <= 0.001f) continue;
        float p[4][3], uv[4][2];
        for (int k = 0; k < 4; k++) {
            memcpy(p[k], sc.corner[k], 12);
            uv[k][0] = sc.corner[k][3];
            uv[k][1] = sc.corner[k][4];
        }
        // a meter keeps the edge it fills from and draws the rest in proportion
        static const int moved[5][4] = {{0, 0, 0, 0}, {2, 1, 3, 0}, {1, 2, 0, 3}, {1, 0, 2, 3}, {0, 1, 3, 2}};   // pairs: corner, the corner it shrinks towards
        if (sc.fill >= 1 && sc.fill <= 4 && fill < 1.0f)
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
        float verts[6][8];
        static const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int v = 0; v < 6; v++) {
            int k = order[v];
            for (int i = 0; i < 3; i++) {
                verts[v][i] = q[k][i] + n[i] * lift;
                verts[v][3 + i] = n[i] / nl;
            }
            verts[v][6] = uv[k][0];
            verts[v][7] = uv[k][1];
        }
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(ctx->Map(g.screen_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
        memcpy(ms.pData, verts, sizeof(verts));
        ctx->Unmap(g.screen_vb, 0);
        c.tint[0] = rgb[0];
        c.tint[1] = rgb[1];
        c.tint[2] = rgb[2];
        upload_consts(ctx, c);
        UINT stride = 32, offset = 0;
        ctx->IASetVertexBuffers(0, 1, &g.screen_vb, &stride, &offset);
        ID3D11ShaderResourceView *srv = tex >= 0 && tex < (int)m.tex_srvs.size() && m.tex_srvs[tex] ? m.tex_srvs[tex] : g.white;
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->Draw(6, 0);
    }
    upload_consts(ctx, base);
}

const float SHOT_RECOIL_SCALE = 0.8f;
Model *find_model(const char *name) {
    for (Model &m : g.models)
        if (m.name == name) return &m;
    return nullptr;
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
    float shake = charge > 0 ? sinf((float)st.time * 90.0f) * 0.004f * charge : 0;

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
    ctx->PSSetSamplers(0, 1, &g.point);
    ctx->OMSetBlendState(g.opaque, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g.depth_on, 0);
    if (revolver) {
        Model *shotgun = st.weapon == 1 ? find_model("shotgun") : nullptr;
        Model *weapon = shotgun ? shotgun : revolver;
        // the named clip while it lasts, otherwise the idle loop
        const Clip *clip = weapon->clip(st.revolver_clip);
        float t = (float)(st.time - st.revolver_clip_start);
        bool loop = false;
        if (!clip || t < 0 || t > (clip->frames - 1) / clip->fps) {
            clip = weapon->clip("Idle");
            t = (float)st.time;
            loop = true;
        }
        // The shot clips are played at 80% of their travel away from the idle pose: at full strength
        // the recoil read as stronger than ULTRAKILL's (a tuning choice, from Davi's comparison).
        const Clip *idle = weapon->clip("Idle");
        bool shot = !shotgun && clip && !loop && clip->name.compare(0, 5, "Shoot") == 0;
        // While the Sharpshooter spins, the hand goes over to the Twirl clip's pose (the gun lying on
        // its side, turning about the finger); ULTRAKILL's Animator blends towards it with the spin's speed.
        const Clip *twirl = shotgun ? nullptr : weapon->clip("Twirl");
        float blend_weight = SHOT_RECOIL_SCALE;
        if (twirl && loop && st.twirl_blend > 0.001f) {
            clip = twirl;
            shot = true;                                  // blended from the idle pose, like a shot clip
            blend_weight = fminf(st.twirl_blend, 1.0f);
        }
        float muzzle[3] = {0, 0, 0};
        int muzzle_node = weapon->node(shotgun ? "ShootPoint L" : "ShootPoint");
        draw_model(ctx, *weapon, clip, t, loop, shot ? idle : nullptr, (float)st.time, blend_weight,
                   shotgun ? -1 : weapon->node("Revolver_Bone"), st.twirl, muzzle_node, muzzle);
        draw_screens(ctx, *weapon, st, c);
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
        // the arm is only in view while one of its clips plays
        const Clip *ac = arm ? arm->clip(st.arm_clip) : nullptr;
        float at = (float)(st.time - st.arm_clip_start);
        if (ac && at >= 0 && at <= (ac->frames - 1) / ac->fps) draw_model(ctx, *arm, ac, at, false);
        Model *arm2 = find_model("knuckleblaster");
        const Clip *bc = arm2 ? arm2->clip(st.arm2_clip) : nullptr;
        float bt = (float)(st.time - st.arm2_clip_start);
        if (bc && bt >= 0 && bt <= (bc->frames - 1) / bc->fps) draw_model(ctx, *arm2, bc, bt, false);
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
    ID3D11ShaderResourceView *srv = nullptr;
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
        rel(rtv); rel(dsv); rel(rs); rel(bs); rel(dss); rel(srv); rel(smp); rel(ps); rel(vs); rel(gs); rel(hs); rel(ds);
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
    }
    return g.ready;
}

void hud_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *target, const HudState &st) {
    if (!g.ready) return;
    D3D11_TEXTURE2D_DESC td;
    target->GetDesc(&td);
    bool srgb = td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Format = td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM
                : td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : td.Format;
    ID3D11RenderTargetView *rtv = nullptr;
    if (FAILED(dev->CreateRenderTargetView(target, &rd, &rtv)) || !rtv) return;

    g.verts.clear();
    g.draws.clear();
    build_hud(st, (float)td.Width, (float)td.Height);

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

    if (!st.flash_only && st.show_viewmodel && (!g.meshes.empty() || !g.models.empty()) && ensure_depth(dev, td.Width, td.Height, td.SampleDesc.Count)) {
        ctx->ClearDepthStencilView(g.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ctx->OMSetRenderTargets(1, &rtv, g.dsv);
        draw_viewmodel(ctx, st, (float)td.Width / (float)td.Height, srgb);
    }

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(g.blend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g.depth_off, 0);
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
            for (const UiDraw &d : g.draws) {
                float mode[4] = {(float)d.sdf, srgb ? 1.0f : 0.0f, 0, 0};
                if (SUCCEEDED(ctx->Map(g.ui_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                    memcpy(ms.pData, mode, sizeof(mode));
                    ctx->Unmap(g.ui_cb, 0);
                }
                ID3D11ShaderResourceView *srv = d.tex >= 0 ? g.textures[d.tex].srv : g.white;
                ctx->PSSetShaderResources(0, 1, &srv);
                ctx->Draw(d.count, d.start);
            }
        }
    }

    saved.restore(ctx);
    rtv->Release();
}
