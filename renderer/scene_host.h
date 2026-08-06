#pragma once

// M15: the desktop scene host - renders the shared scene across N windows
// spanning a virtual canvas, with runtime layouts and controls. Used by both
// producer front doors (--serve on Windows, --wayland on Linux); Android has
// its own single-window loop.
//
// Windows are specified as WxH+X+Y: the rect places the window on the OS
// desktop AND defines its region of the virtual canvas, so windows arranged
// across displays show one seamless scene (per-window off-center projections
// + canvas-mapped backdrop).
//
// Controls (any window):
//   L         cycle layout: two-cubes -> single-cube -> wall
//   SPACE     pause/resume animation        -/=  slow down / speed up
//   B         toggle vaporwave backdrop
//   1..9      toggle stream N in the scene  0    re-enable all streams
//   ESC       quit

#ifndef __ANDROID__

#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

#include "background_pass.h"
#include "external_video_image.h"
#include "video_pass.h"
#include "vk_context.h"

namespace vkr {

struct WindowSpec {
    int width = 1280, height = 720;
    int x = 0, y = 0; // canvas + desktop position
};

enum class SceneLayout { TwoCubes = 0, SingleCube, Wall, Count };

class SceneHost {
public:
    // vk must already be initialized: window mode with the FIRST spec's size
    // (the host repositions it), or headless (then specs beyond the first are
    // ignored and the offscreen target is the whole canvas).
    bool init(VkContext& vk, const std::string& shaderDir, std::vector<WindowSpec> specs);
    void destroy(VkContext& vk);

    // One frame: poll input, animate, render every window, present.
    // streams: (id, slot) per connected producer, in stable order - the host
    // applies its enable/disable set and the active layout. Extra timeline
    // waits/signals (the Windows service's per-client fences) join the
    // frame's single submit.
    struct Extras {
        std::vector<VkSemaphore> waitSems;
        std::vector<uint64_t> waitValues;
        std::vector<VkSemaphore> signalSems;
        std::vector<uint64_t> signalValues;
        // M24: consumer-view deliveries. After window 0 renders, its image is
        // blitted (scaled) into each target - an imported shared D3D11
        // texture a consumer process reads. The corresponding readyFence
        // signal rides signalSems/signalValues above.
        struct Blit {
            VkImage image;
            uint32_t width, height;
        };
        std::vector<Blit> blits;
    };
    struct StreamRef {
        uint64_t id;
        int64_t slot;
    };
    // Returns false on fatal error; sets *quit when the user closed a window
    // or pressed ESC.
    bool frame(VkContext& vk, ExternalVideoPool& pool, VideoPass& pass, bool passBound,
               const std::vector<StreamRef>& streams, const Extras& extras, bool* quit);

    // For the headless dump path (primary target is vk.swapImages[0]).
    VkCommandPool commandPool() const { return cmdPool_; }

    // Called from the GLFW key callback; public for the C callback trampoline.
    void handleKey(int key);

    // An extra (non-primary) window with its own surface + swapchain (index 0
    // is the VkContext-owned primary window).
    struct Window {
        GLFWwindow* glfw = nullptr; // fwd-declared via vk_context.h
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        std::vector<VkImage> images;
        std::vector<VkImageView> views;
    };

private:
    struct WindowRects {
        float u0, v0, u1, v1; // canvas region
    };

    bool recreateWindow(VkContext& vk, size_t index);

    std::vector<WindowSpec> specs_;
    std::vector<Window> extra_;
    std::vector<WindowRects> rects_; // per window incl. primary
    float canvasAspect_ = 16.0f / 9.0f;
    BackgroundPass background_;
    bool backgroundOn_ = true;

    SceneLayout layout_ = SceneLayout::TwoCubes;
    bool paused_ = false;
    float speed_ = 1.0f;
    double sceneTime_ = 0.0;
    int64_t lastFrameNs_ = 0;
    std::vector<uint64_t> disabledIds_;
    std::vector<uint64_t> lastStreamIds_; // order snapshot for number keys

    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kFramesInFlight = 2;
    struct FrameSync {
        VkFence inFlight = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        std::vector<VkSemaphore> acquireSems;    // per window
        std::vector<VkSemaphore> renderDoneSems; // per window
    };
    FrameSync frames_[kFramesInFlight];
    uint64_t frameIndex_ = 0;
    uint64_t renders_ = 0;
    int64_t rateWindowStart_ = 0;

    // Snapshot control (P key, or VKR_SNAPSHOT_SECONDS for periodic dumps):
    // writes each window's last presented frame to scene-w<N>.ppm.
    bool snapshotRequested_ = false;
    double snapshotEvery_ = 0.0;
    int64_t lastSnapshotNs_ = 0;
    std::vector<uint32_t> lastImageIndex_;
    void snapshot(VkContext& vk);
};

} // namespace vkr

#endif // !__ANDROID__
