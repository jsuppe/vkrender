// M14 demo producer: connects to the vkrenderer service through the client
// library and renders an animated pattern (hue-cycling background + sweeping
// bars) into dequeued shared textures with its own D3D11 device.
//
//   win_client_demo [pipe] [seconds] [hueOffsetDegrees]
//
// Run several instances with different hue offsets to fill cube faces.

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
#include <thread>

#include "vkrender_client.h"

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
    const unsigned W = 640, H = 360;

    vkrClient* client = vkrConnect(pipe, nullptr, W, H);
    if (!client) {
        fprintf(stderr, "demo: cannot connect to service '%s'\n", pipe);
        return 1;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    vkrGetDevice(client)->GetImmediateContext(&ctx);
    ComPtr<ID3D11DeviceContext1> ctx1;
    ctx.As(&ctx1);

    std::map<ID3D11Texture2D*, ComPtr<ID3D11RenderTargetView>> rtvs;
    auto start = std::chrono::steady_clock::now();
    unsigned frames = 0;
    for (;;) {
        float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        if (t >= (float)seconds) break;

        vkrBuffer buf;
        int rc = vkrDequeueBuffer(client, &buf, 100);
        if (rc == -1) break;
        if (rc == 1) continue; // backpressure

        auto& rtv = rtvs[buf.texture];
        if (!rtv) vkrGetDevice(client)->CreateRenderTargetView(buf.texture, nullptr, &rtv);

        float r, g, b;
        hsv(hueOffset + t * 40.0f, &r, &g, &b);
        float bg[4] = {r * 0.35f, g * 0.35f, b * 0.35f, 1.0f};
        ctx->ClearRenderTargetView(rtv.Get(), bg);
        if (ctx1) {
            // Sweeping vertical bar + counter-sweeping horizontal bar.
            float fg[4] = {r, g, b, 1.0f};
            LONG x = (LONG)(fmodf(t * 300.0f, (float)W));
            D3D11_RECT vbar{x, 0, x + 60 > (LONG)W ? (LONG)W : x + 60, (LONG)H};
            ctx1->ClearView(rtv.Get(), fg, &vbar, 1);
            float white[4] = {1, 1, 1, 1};
            LONG y = (LONG)(H - 1 - fmodf(t * 180.0f, (float)H));
            D3D11_RECT hbar{0, y, (LONG)W, y + 24 > (LONG)H ? (LONG)H : y + 24};
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
        if (vkrQueueBuffer(client, &buf, submitNs) != 0) break;
        ++frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 fps
    }
    printf("demo: produced %u frames\n", frames);
    vkrDisconnect(client);
    return 0;
}

#else
int main() { return 0; }
#endif
