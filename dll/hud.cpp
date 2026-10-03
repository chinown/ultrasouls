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
    ID3D11ShaderResourceView *white = nullptr;

    ID3D11VertexShader *ui_vs = nullptr, *mesh_vs = nullptr;
    ID3D11PixelShader *ui_ps = nullptr, *mesh_ps = nullptr;
    ID3D11InputLayout *ui_il = nullptr, *mesh_il = nullptr;
    ID3D11Buffer *ui_cb = nullptr, *mesh_cb = nullptr, *ui_vb = nullptr;
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
cbuffer C : register(b0) { row_major float4x4 mvp; row_major float4x4 world; float4 light; float4 mode; }
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
    float4 c = float4(t.rgb * i.shade, 1);
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
    cb.ByteWidth = 160;
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
const Color PIERCER_BLUE = {0, 0.8759f, 1, 1}, FIST_BLUE = {0.251f, 0.9059f, 1, 1};

void build_hud(const HudState &st, float screen_w, float screen_h) {
    float dt = (float)(st.time - anim.last);
    if (dt < 0 || dt > 0.5f) dt = 0;
    anim.last = st.time;
    // the orange bar lags behind the red one when health drops, and snaps up when it rises
    if (anim.after_health < st.health) anim.after_health = st.health;
    else anim.after_health = fmaxf(st.health, anim.after_health - 30.0f * dt);

    // ---- main panel: "GunCanvas", a world-space canvas seen by the 90-degree HUD camera
    Surface sf{};
    sf.world = true;
    sf.pos[0] = -1.06f;
    sf.pos[1] = -0.53f;
    sf.pos[2] = 1.0f;
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
        image_simple(sf, icon, g.sprite("FistIcon"), {0, 0, 0, 1});
        image_simple(sf, icon, g.sprite("FistIcon"), FIST_BLUE);
    }

    Box gun = child(canvas, rt(0, 0, 0, 0, -79, 187, 200, 100, 0, 0, 4, 4));
    image_sliced(sf, gun, g.sprite("Round_FillLarge"), PANEL);
    {
        Box icon = child(child(gun, rt(0, 0, 1, 1, 0, 0, -10, -10)), FILL);
        int rev = g.sprite("SingleRevolver");
        float s = rev >= 0 ? fminf(icon.w() / g.textures[g.sprites[rev].tex].w, icon.h() / g.textures[g.sprites[rev].tex].h) : 0;
        image_simple(sf, icon, rev, PIERCER_BLUE, true);
        // the glow sprite is the same drawing with a soft margin, so it is drawn at the icon's scale
        image_simple(sf, icon, g.sprite("SingleRevolverGlow"), {0, 0.8759f, 1, 0.749f}, false, s);
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
    if (changed || st.stamina < 299.5f || st.pierce_charge > 0) anim.cross_until = st.time + 1.0;
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
    // the Piercer's charge, on the wider ring ULTRAKILL uses for power-ups
    if (st.pierce_charge > 0 || st.pierce_ready < 100.0f) {
        Box big = child(child(dot, rt(0.5f, 0.5f, 0.5f, 0.5f, 0.0001f, 0, 100, 100, 0.5f, 0.5f, 0.8195f, 0.8195f)),
                        rt(0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 55, 55, 0.5f, 0.5f, 1, 1, 180.0f));
        if (st.pierce_charge > 0)
            image_radial(sc, big, g.sprite("circle"), st.pierce_charge >= 100.0f ? Color{1, 1, 1, 1} : Color{0, 0.8759f, 1, 1}, st.pierce_charge / 100.0f, true);
        else
            image_radial(sc, big, g.sprite("circle"), {0.35f, 0.4f, 0.45f, 0.8f}, st.pierce_ready / 100.0f, true);
    }
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
    float mvp[16], world[16], light[4], mode[4];
};

void draw_viewmodel(ID3D11DeviceContext *ctx, const HudState &st, float aspect, bool srgb) {
    // The meshes are stored already posed in the HUD camera's space (x right, y up, z forward), in the
    // pose the Piercer prefab is saved in. Recoil is a short kick about the grip.
    const float grip[3] = {0.42f, -0.6f, 1.49f};
    float since = (float)(st.time - st.last_shot), since_p = (float)(st.time - st.last_pierce);
    float kick = 0;
    if (since >= 0 && since < 0.35f) kick = fmaxf(kick, expf(-since * 14.0f) * 0.30f);
    if (since_p >= 0 && since_p < 0.6f) kick = fmaxf(kick, expf(-since_p * 8.0f) * 0.55f);
    float charge = st.pierce_charge / 100.0f;
    float shake = charge > 0 ? sinf((float)st.time * 90.0f) * 0.004f * charge : 0;

    float t0[16], rx[16], rz[16], t1[16], world[16], tmp[16];
    translate(t0, -grip[0], -grip[1], -grip[2]);
    rotate_x(rx, -kick);                              // barrel up
    rotate_z(rz, charge * 0.12f);                     // tilts while charging
    translate(t1, grip[0] + shake, grip[1] + shake * 0.5f, grip[2] - kick * 0.25f);
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
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx->Map(g.mesh_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        memcpy(ms.pData, &c, sizeof(c));
        ctx->Unmap(g.mesh_cb, 0);
    }
    ctx->IASetInputLayout(g.mesh_il);
    ctx->VSSetShader(g.mesh_vs, nullptr, 0);
    ctx->PSSetShader(g.mesh_ps, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g.mesh_cb);
    ctx->PSSetConstantBuffers(0, 1, &g.mesh_cb);
    ctx->PSSetSamplers(0, 1, &g.point);
    ctx->OMSetBlendState(g.opaque, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g.depth_on, 0);
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

bool hud_init(ID3D11Device *device, const wchar_t *pack_path) {
    static bool tried = false;
    if (tried) return g.ready;
    tried = true;
    g.ready = load_pack(device, pack_path) && create_pipeline(device);
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

    if (st.show_viewmodel && !g.meshes.empty() && ensure_depth(dev, td.Width, td.Height, td.SampleDesc.Count)) {
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
