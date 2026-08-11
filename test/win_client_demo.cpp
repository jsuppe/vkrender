// M14 demo producer: renders an animated pattern (hue-cycling background +
// sweeping bars) into the renderer's shared textures with its own D3D11 device.
//
//   win_client_demo [pipe] [seconds] [hueOffsetDegrees] [width] [height]
//
// Run several instances with different hue offsets to fill cube faces.
//
// M38: this is a framelink producer now, and the first receiver ported off the
// legacy vkrender_client protocol (docs/MIGRATION.md). It goes first because
// its failures are the cheapest to diagnose - no capture hardware, no sender,
// no network. It is also the harness for the latency baseline the migration
// must not regress: twelve instances against `vkrenderer --serve`.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11_1.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

#include "fl_slot.h"
#include "framelink.h"
#include "framelink_d3d11.h"

using Microsoft::WRL::ComPtr;

static void hsv(float h, float* r, float* g, float* b) {
    h = fmodf(h, 360.0f) / 60.0f;
    float x = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f);
    switch ((int)h) {
        case 0: *r = 1; *g = x; *b = 0; break;
        case 1: *r = x; *g = 1; *b = 0; break;
        case 2: *r = 0; *g = 1; *b = x; break;
        case 3: *r = 0; *g = x; *b = 1; break;
        case 4: *r = x; *g = 0; *b = 1; break;
        default: *r = 1; *g = 0; *b = x; break;
    }
}

int main(int argc, char** argv) {
    const char* pipe = argc > 1 ? argv[1] : "vkrender";
    int seconds = argc > 2 ? atoi(argv[2]) : 30;
    float hueOffset = argc > 3 ? (float)atof(argv[3]) : 0.0f;
    // Sizes are a parameter because real producers differ and the renderer's
    // ring starts at a placeholder: asking for something else is what exercises
    // flRequestGeometry, and a mirroring iPad's 752x1080 is not hypothetical.
    const unsigned W = argc > 4 ? (unsigned)atoi(argv[4]) : 640;
    const unsigned H = argc > 5 ? (unsigned)atoi(argv[5]) : 360;

    std::string slotName;
    flChannel* ch = vkr::flSlotAttach(pipe, W, H, 12, 10000, &slotName);
    if (!ch) return 1;
    flChannelInfo info{};
    flQuery(ch, &info);
    printf("demo: on '%s' %ux%u ring of %u\n", slotName.c_str(), info.width, info.height,
           info.poolSize);

    // The channel's device, not one of our own: framelink allocated the ring on
    // it, and flSubmit signals from its context. A second device would need
    // flBeginSubmit/flEndSubmit and its own fence signal - which is what the
    // renderer does, because it composites with Vulkan.
    ID3D11Device* dev = flChannelD3D11Device(ch);
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    ComPtr<ID3D11DeviceContext1> ctx1;
    ctx.As(&ctx1);

    std::map<ID3D11Texture2D*, ComPtr<ID3D11RenderTargetView>> rtvs;
    auto start = std::chrono::steady_clock::now();
    unsigned frames = 0;
    for (;;) {
        float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        if (t >= (float)seconds) break;

        flBuffer buf{};
        const flResult r = flAcquireBuffer(ch, &buf, 100);
        if (r == FL_TIMEOUT) continue; // back-pressure: the ring is full
        if (r != FL_OK) {
            fprintf(stderr, "demo: flAcquireBuffer -> %s\n", flResultString(r));
            break;
        }

        ID3D11Texture2D* tex = flImageD3D11(buf.image);
        auto& rtv = rtvs[tex];
        if (!rtv) dev->CreateRenderTargetView(tex, nullptr, &rtv);

        float rr, gg, bb;
        hsv(hueOffset + t * 40.0f, &rr, &gg, &bb);
        float bg[4] = {rr * 0.35f, gg * 0.35f, bb * 0.35f, 1.0f};
        ctx->ClearRenderTargetView(rtv.Get(), bg);
        if (ctx1) {
            // Sweeping vertical bar + counter-sweeping horizontal bar.
            float fg[4] = {rr, gg, bb, 1.0f};
            LONG x = (LONG)(fmodf(t * 300.0f, (float)info.width));
            D3D11_RECT vbar{x, 0, x + 60 > (LONG)info.width ? (LONG)info.width : x + 60,
                            (LONG)info.height};
            ctx1->ClearView(rtv.Get(), fg, &vbar, 1);
            float white[4] = {1, 1, 1, 1};
            LONG y = (LONG)(info.height - 1 - fmodf(t * 180.0f, (float)info.height));
            D3D11_RECT hbar{0, y, (LONG)info.width,
                            y + 24 > (LONG)info.height ? (LONG)info.height : y + 24};
            ctx1->ClearView(rtv.Get(), white, &hbar, 1);
        }

        // Stamp a CLOCK, not scene time: the service subtracts this from its
        // own steady_clock to measure the producer -> renderer hop, which is
        // the number the framelink migration must not regress. Nothing else
        // consumes this demo's pts.
        const long long submitNs =
            (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (flSubmit(ch, &buf, submitNs) != FL_OK) break;
        ++frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 fps
    }
    printf("demo: produced %u frames\n", frames);
    flClose(ch);
    return 0;
}

#else
int main() { return 0; }
#endif
