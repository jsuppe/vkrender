#pragma once

// M14: vkrenderer as a producer-facing service on Windows - the desktop
// equivalent of the Android Binder/BufferQueue architecture where no OS
// compositor protocol exists. The renderer OWNS each client's buffer ring
// (mirroring AImageReader): a client connects to the named pipe, asks for a
// ring (ClientHello), and receives shared D3D11 texture handles + a shared
// ready/consumed fence pair already duplicated into its process. The thin
// client library (client/vkrender_client.h) wraps this in a BufferQueue-shaped
// dequeue/queue API; each client becomes one stream on the two-cube scene.

#ifdef _WIN32

#include <string>
#include <vector>

#include "scene_host.h"
#include "av_playback.h"

namespace vkr {

struct WinServeOptions {
    std::string pipeName = "vkrender";
    av::PlaybackOptions avOptions;
    bool avExitOnEnd=false;
    std::vector<std::string> sceneOutputs; // empty: <pipeName>.scene; otherwise fan-out
    int headlessFrames = 0; // 0 = windowed
    std::string dumpPath = "headless.ppm";
    std::string shaderDir;
    std::vector<WindowSpec> windows; // M15: canvas-spanning windows
    bool fullscreen = false; // one borderless window per monitor
    int bezel = 0;           // applied to every fullscreen window
    bool panel = true;       // on-screen controls
    // M42: the moderation API's localhost port; 0 disables it.
    int controlPort = 8875;
};

int runWinService(const WinServeOptions& options);

} // namespace vkr

#endif // _WIN32
