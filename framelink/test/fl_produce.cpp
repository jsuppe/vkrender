// framelink spike producer: attach, draw a frame-derived colour, submit.
//
// The colour is derived from the frame number, and the SAME number is sent as
// the pts. The consumer recomputes the colour from the pts it receives, so the
// test fails if a frame's pixels are ever paired with another frame's metadata
// - i.e. it catches slot aliasing and missing fence waits, not merely "some
// pixels arrived".
//
//   fl_produce [channel] [frames]

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
    const int frames = argc > 2 ? atoi(argv[2]) : 60;

    flChannel* ch = nullptr;
    flResult r = flOpenProducer(name, &ch);
    if (r != FL_OK) {
        printf("produce: flOpenProducer('%s') -> %s\n", name, flResultString(r));
        return 1;
    }
    flChannelInfo info{};
    flQuery(ch, &info);
    printf("produce: attached to '%s' %ux%u pool %u (geometry is the consumer's)\n", name,
           info.width, info.height, info.poolSize);

    ID3D11Device* dev = flChannelD3D11Device(ch);
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);

    int submitted = 0;
    for (int i = 1; i <= frames; ++i) {
        flBuffer buf{};
        r = flAcquireBuffer(ch, &buf, 1000);
        if (r != FL_OK) {
            printf("produce: flAcquireBuffer -> %s\n", flResultString(r));
            break;
        }
        // Draw on the GPU rather than uploading from the CPU: this is what
        // makes the fence wait meaningful on the other side.
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(dev->CreateRenderTargetView(flImageD3D11(buf.image), nullptr, &rtv))) {
            printf("produce: CreateRenderTargetView failed\n");
            break;
        }
        const float rgba[4] = {(float)(i & 0xFF) / 255.0f, (float)((i * 7) & 0xFF) / 255.0f,
                               (float)((i * 13) & 0xFF) / 255.0f, 1.0f};
        ctx->ClearRenderTargetView(rtv.Get(), rgba);

        r = flSubmit(ch, &buf, (int64_t)i); // pts carries the frame number
        if (r != FL_OK) {
            printf("produce: flSubmit -> %s\n", flResultString(r));
            break;
        }
        ++submitted;
        Sleep(8);
    }
    printf("produce: submitted %d frames\n", submitted);
    flClose(ch);
    return submitted > 0 ? 0 : 1;
}
