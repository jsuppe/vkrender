#pragma once

// M13: vkrenderer as a minimal Wayland compositor (Linux only) - the desktop
// equivalent of the Android Binder/BufferQueue architecture. Producer apps
// are ordinary Wayland clients: they connect to our private socket, create an
// xdg_toplevel surface, and attach dma-buf buffers (zwp_linux_dmabuf, sampled
// zero-copy) or wl_shm buffers (CPU upload fallback - also the VM story).
// Each client surface becomes one stream on the two-cube vaporwave scene,
// exactly like the Android per-UID registry.
//
//   WAYLAND_DISPLAY=vkrender-0 gst-launch-1.0 rtspsrc ... ! vah264dec ! waylandsink
//
// Buffer lifecycle mirrors Android: wl_buffer.release is deferred until the
// frame fence that last sampled the buffer has retired; frame callbacks fire
// once per composited frame, giving clients vsync pacing for free.

#ifndef _WIN32

#include <string>
#include <vector>

#include "scene_host.h"

namespace vkr {

struct WaylandOptions {
    std::vector<std::string> sceneOutputs;
    std::string socketName = "vkrender-0";
    int headlessFrames = 0; // 0 = windowed
    std::string dumpPath = "headless.ppm";
    std::string shaderDir;
    std::vector<WindowSpec> windows; // M15: canvas-spanning windows
};

// Runs the compositor until the window closes (or headlessFrames adopted in
// headless mode). Returns a process exit code.
int runWaylandCompositor(const WaylandOptions& options);

} // namespace vkr

#endif // !_WIN32
