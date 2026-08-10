// framelink spike consumer: own the ring, read frames back, verify the pixels.
//
// Verification is the point. The producer derives each frame's colour from its
// frame number and sends that number as the pts; this reads the pixel back off
// the GPU and checks it against the colour the pts implies. A mismatch means a
// frame's pixels were paired with another frame's metadata - slot aliasing, or
// a missing fence wait letting us read a buffer mid-write. "Some pixels
// arrived" would not catch either.
//
//   fl_consume [channel] [frames-to-verify]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>

#include "framelink.h"
#include "framelink_d3d11.h"

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* name = argc > 1 ? argv[1] : "flspike";
    const int want = argc > 2 ? atoi(argv[2]) : 30;
    // Per-frame delay, to model a slow consumer. This is how the fan-out
    // back-pressure question gets answered: does a slow reader throttle the
    // producer, or does the producer run free and the reader sample the newest?
    const int slowMs = argc > 3 ? atoi(argv[3]) : 0;
    const uint32_t W = 256, H = 128;

    flChannel* ch = nullptr;
    flResult r = flCreateChannel(name, W, H, FL_FORMAT_BGRA8, 4, &ch);
    if (r != FL_OK) {
        printf("consume: flCreateChannel -> %s\n", flResultString(r));
        return 1;
    }
    printf("consume: channel '%s' %ux%u pool 4, waiting for a producer\n", name, W, H);

    ID3D11Device* dev = flChannelD3D11Device(ch);
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = W;
    sd.Height = H;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) {
        printf("consume: staging texture failed\n");
        flClose(ch);
        return 1;
    }

    int verified = 0, mismatched = 0;
    uint64_t lastSeq = 0, skipped = 0;
    const DWORD deadline = GetTickCount() + 30000;

    while (verified + mismatched < want && GetTickCount() < deadline) {
        flFrame f{};
        r = flAcquireFrame(ch, &f, 500);
        if (r == FL_TIMEOUT) continue;
        if (r == FL_DISCONNECTED) {
            printf("consume: producer disconnected\n");
            break;
        }
        if (r != FL_OK) {
            printf("consume: flAcquireFrame -> %s\n", flResultString(r));
            break;
        }
        if (lastSeq && f.sequence > lastSeq + 1) skipped += f.sequence - lastSeq - 1;
        lastSeq = f.sequence;

        ctx->CopyResource(staging.Get(), flImageD3D11(f.image));
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
            const uint8_t* px = (const uint8_t*)map.pData; // BGRA
            const int i = (int)f.ptsNs;
            const uint8_t wantB = (uint8_t)((i * 13) & 0xFF);
            const uint8_t wantG = (uint8_t)((i * 7) & 0xFF);
            const uint8_t wantR = (uint8_t)(i & 0xFF);
            // +/-1 tolerance: the float clear round-trips through UNORM.
            const bool ok = (abs((int)px[0] - wantB) <= 1) && (abs((int)px[1] - wantG) <= 1) &&
                            (abs((int)px[2] - wantR) <= 1);
            if (ok) {
                ++verified;
            } else {
                ++mismatched;
                printf("consume: MISMATCH pts=%d slot=%u got BGR %u,%u,%u want %u,%u,%u\n", i,
                       f.slot, px[0], px[1], px[2], wantB, wantG, wantR);
            }
            ctx->Unmap(staging.Get(), 0);
        }
        if (slowMs) Sleep(slowMs);
        flRelease(ch, &f);
    }

    printf("consume: %d verified, %d mismatched, %llu skipped (latest-frame-wins)\n", verified,
           mismatched, (unsigned long long)skipped);
    printf("consume: producer submitted %llu frames while we read %d\n",
           (unsigned long long)lastSeq, verified + mismatched);
    flClose(ch);
    const bool pass = verified > 0 && mismatched == 0;
    printf("consume: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
