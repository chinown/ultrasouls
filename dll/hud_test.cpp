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
    return 0;
}
