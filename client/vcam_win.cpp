// M24: virtual camera fed from the scene. Negotiates a view with the
// renderer (consumer API), reads each delivered frame back and pumps it into
// the softcam DirectShow virtual camera - the vkrender scene becomes a
// webcam in Chrome / OBS / Zoom / classic Teams (DirectShow enumeration;
// MF-only UWP apps need the future MFCreateVirtualCamera source).
//
//   vkr_vcam [pipe] [width] [height] [fps]
//
// One-time setup (elevated):  regsvr32 softcam.dll

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "softcam.h"
#include "vkrender_client.h"

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pipe = argc > 1 ? argv[1] : "vkrender";
    unsigned w = argc > 2 ? (unsigned)atoi(argv[2]) : 1280;
    unsigned h = argc > 3 ? (unsigned)atoi(argv[3]) : 720;
    unsigned fps = argc > 4 ? (unsigned)atoi(argv[4]) : 30;

    vkrClient* c = vkrConnectConsumer(pipe, nullptr, &w, &h, fps);
    if (!c) {
        fprintf(stderr, "vcam: cannot reach vkrenderer service on pipe '%s'\n", pipe);
        return 1;
    }
    printf("vcam: negotiated scene view %ux%u @%u fps\n", w, h, fps);

    // softcam wants multiples of four.
    if ((w & 3) || (h & 3)) {
        fprintf(stderr, "vcam: view %ux%u not a multiple of 4\n", w, h);
        return 1;
    }
    // framerate 0: we push in real time as frames arrive (the renderer paces).
    scCamera cam = scCreateCamera((int)w, (int)h, 0.0f);
    if (!cam) {
        fprintf(stderr,
                "vcam: scCreateCamera failed - is softcam.dll registered "
                "(elevated: regsvr32 softcam.dll) and no other instance running?\n");
        return 1;
    }
    printf("vcam: virtual camera 'DirectShow Softcam' up - open it in a camera app\n");

    ID3D11Device* dev = vkrGetDevice(c);
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = w;
    sd.Height = h;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) return 1;

    std::vector<unsigned char> bgr((size_t)w * h * 3);
    uint64_t frames = 0, lastLog = 0;

    for (;;) {
        vkrFrame frame;
        int r = vkrAcquireFrame(c, &frame, 2000);
        if (r == 1) continue; // renderer idle; keep the camera alive
        if (r != 0) {
            fprintf(stderr, "vcam: renderer disconnected\n");
            break;
        }
        ctx->CopyResource(staging.Get(), frame.texture);
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
            // BGRA -> tight 24-bit BGR (softcam's shared-memory format).
            for (unsigned y = 0; y < h; ++y) {
                const unsigned char* src = (const unsigned char*)map.pData + y * map.RowPitch;
                unsigned char* dst = bgr.data() + (size_t)y * w * 3;
                for (unsigned x = 0; x < w; ++x) {
                    dst[x * 3 + 0] = src[x * 4 + 0];
                    dst[x * 3 + 1] = src[x * 4 + 1];
                    dst[x * 3 + 2] = src[x * 4 + 2];
                }
            }
            ctx->Unmap(staging.Get(), 0);
            scSendFrame(cam, bgr.data());
            ++frames;
        }
        vkrReleaseFrame(c, &frame);

        if (frames - lastLog >= 150) {
            printf("vcam: %llu frames -> camera (%s)\n", (unsigned long long)frames,
                   scIsConnected(cam) ? "an app is watching" : "no app connected");
            lastLog = frames;
        }
    }

    scDeleteCamera(cam);
    vkrDisconnect(c);
    return 0;
}

#endif // _WIN32
