#include <cstring>
#include <string>

#include "d3d11va_decoder.h"
#include "ipc.h"
#include "log.h"

// vkdecoder --url rtsp://host:port/path [--pipe vkrender]
//
// Process 1 of the zero-copy pipeline: receives an RTSP stream, decodes it
// with D3D11VA, and publishes decoded frames to the renderer process through
// shared GPU textures (no cross-process copies).

int main(int argc, char** argv) {
    std::string url;
    std::string pipe = "vkrender";
    // 4 slots: one being written, one displayed (may still be referenced by
    // in-flight GPU work), plus fence-latency slack. Latest-frame-wins on the
    // renderer side means pool depth never adds display latency.
    uint32_t poolSize = 4;

    const char* usage = "usage: vkdecoder --url rtsp://... [--pipe name] [--pool-size 2-8]\n";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--url") && i + 1 < argc) url = argv[++i];
        else if (!strcmp(argv[i], "--pipe") && i + 1 < argc) pipe = argv[++i];
        else if (!strcmp(argv[i], "--pool-size") && i + 1 < argc) poolSize = (uint32_t)atoi(argv[++i]);
        else {
            fprintf(stderr, "%s", usage);
            return 1;
        }
    }
    if (url.empty()) {
        fprintf(stderr, "%s", usage);
        return 1;
    }

    vkr::IpcChannel ch = vkr::IpcChannel::listen(pipe);
    if (!ch.connected()) return 1;

    auto hello = ch.recv();
    if (!hello || hello->type != vkr::proto::MsgType::Hello) {
        VKR_ERR("expected Hello from renderer");
        return 1;
    }
    const auto* msg = hello->as<vkr::proto::HelloMsg>();
    if (!msg) {
        VKR_ERR("malformed Hello");
        return 1;
    }
    VKR_LOG("decoder: renderer pid=%u luidValid=%u", msg->rendererPid, msg->luidValid);

    vkr::D3D11VADecoder decoder(*msg, poolSize);
    return decoder.run(url, ch);
}
