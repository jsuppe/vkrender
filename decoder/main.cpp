#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include "d3d11va_decoder.h"
using PlatformDecoder = vkr::D3D11VADecoder;
#else
#include "vaapi_decoder.h"
using PlatformDecoder = vkr::VAAPIDecoder;
#endif

#include "ipc.h"
#include "log.h"

// vkdecoder --url rtsp://... [--pipe vkrender] [--pool-size 2-8] [--device /dev/dri/renderD128]
//
// Process 1 of the zero-copy pipeline: receives an RTSP stream, decodes it in
// hardware (D3D11VA on Windows, VAAPI on Linux), and publishes decoded frames
// to the renderer process through shared GPU memory (no cross-process copies).

int main(int argc, char** argv) {
    std::string url;
    std::string pipe = "vkrender";
    std::string device; // Linux: DRM render node; empty = driver default
    // Windows: shared texture ring depth. Linux: max decode surfaces held
    // outstanding for the renderer. Latest-frame-wins on the renderer side
    // means this depth never adds display latency.
    uint32_t poolSize = 4;

    const char* usage =
        "usage: vkdecoder --url rtsp://... [--pipe name] [--pool-size 2-8] "
        "[--device /dev/dri/renderD128]\n";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--url") && i + 1 < argc) url = argv[++i];
        else if (!strcmp(argv[i], "--pipe") && i + 1 < argc) pipe = argv[++i];
        else if (!strcmp(argv[i], "--pool-size") && i + 1 < argc) poolSize = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) device = argv[++i];
        else {
            fprintf(stderr, "%s", usage);
            return 1;
        }
    }
    if (url.empty()) {
        fprintf(stderr, "%s", usage);
        return 1;
    }

    // Session loop: serve one renderer until it goes away, then wait for the
    // next one. RTSP reconnection is handled inside run(); this loop handles
    // renderer restarts. Ctrl+C to exit.
    for (;;) {
        vkr::IpcChannel ch = vkr::IpcChannel::listen(pipe);
        if (!ch.connected()) return 1;

        auto hello = ch.recv();
        if (!hello || hello->type != vkr::proto::MsgType::Hello) {
            VKR_ERR("expected Hello from renderer, dropping session");
            continue;
        }
        const auto* msg = hello->as<vkr::proto::HelloMsg>();
        if (!msg) {
            VKR_ERR("malformed Hello, dropping session");
            continue;
        }
        VKR_LOG("decoder: renderer pid=%u luidValid=%u", msg->rendererPid, msg->luidValid);

#ifdef _WIN32
        PlatformDecoder decoder(*msg, poolSize);
#else
        PlatformDecoder decoder(*msg, poolSize, device);
#endif
        int rc = decoder.run(url, ch);
        if (rc != 0) return rc;
        VKR_LOG("decoder: session ended, waiting for a new renderer");
    }
}
