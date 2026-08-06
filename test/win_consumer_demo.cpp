// M24 test consumer: negotiate a scene view, acquire frames, dump one to PPM.
// Verifies the consumer protocol, ring import, fencing and blit content
// headlessly:  vkr_consumer_demo [pipe] [width] [height] [fps] [frames]

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "vkrender_client.h"

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pipe = argc > 1 ? argv[1] : "vkrender";
    unsigned w = argc > 2 ? (unsigned)atoi(argv[2]) : 1280;
    unsigned h = argc > 3 ? (unsigned)atoi(argv[3]) : 720;
    unsigned fps = argc > 4 ? (unsigned)atoi(argv[4]) : 30;
    int frames = argc > 5 ? atoi(argv[5]) : 60;

    vkrClient* c = vkrConnectConsumer(pipe, nullptr, &w, &h, fps);
    if (!c) {
        fprintf(stderr, "consumer: cannot connect to '%s'\n", pipe);
        return 1;
    }
    printf("consumer: negotiated view %ux%u @%u fps\n", w, h, fps);

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

    int got = 0;
    bool dumped = false;
    while (got < frames) {
        vkrFrame frame;
        int r = vkrAcquireFrame(c, &frame, 2000);
        if (r == 1) {
            fprintf(stderr, "consumer: timeout waiting for frame (%d so far)\n", got);
            continue;
        }
        if (r != 0) {
            fprintf(stderr, "consumer: disconnected\n");
            break;
        }
        ++got;
        if (got % 30 == 1) printf("consumer: %d frames (slot %u)\n", got, frame.slot);

        if (!dumped && got >= 10) { // let the scene animate a moment
            ctx->CopyResource(staging.Get(), frame.texture);
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                FILE* f = fopen("consumer-view.ppm", "wb");
                if (f) {
                    fprintf(f, "P6\n%u %u\n255\n", w, h);
                    std::vector<unsigned char> row(w * 3);
                    for (unsigned y = 0; y < h; ++y) {
                        const unsigned char* src =
                            (const unsigned char*)map.pData + y * map.RowPitch;
                        for (unsigned x = 0; x < w; ++x) {
                            row[x * 3 + 0] = src[x * 4 + 2]; // BGRA -> RGB
                            row[x * 3 + 1] = src[x * 4 + 1];
                            row[x * 3 + 2] = src[x * 4 + 0];
                        }
                        fwrite(row.data(), 1, row.size(), f);
                    }
                    fclose(f);
                    printf("consumer: wrote consumer-view.ppm (frame %d)\n", got);
                    dumped = true;
                }
                ctx->Unmap(staging.Get(), 0);
            }
        }
        vkrReleaseFrame(c, &frame);
    }
    printf("consumer: done (%d frames)\n", got);
    vkrDisconnect(c);
    return got > 0 ? 0 : 2;
}

#endif // _WIN32
