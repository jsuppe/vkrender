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

#include "gpu_trace.h"
#include "portable_output.h"
#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

#include "background_pass.h"
#include "external_video_image.h"
#include "scene_layout.h"
#include "video_pass.h"
#include "vk_context.h"

namespace vkr {

struct WindowSpec {
    int width = 1280, height = 720;
    int x = 0, y = 0; // canvas + desktop position
    // M29 bezel compensation, in pixels of this panel's own frame.
    //
    // The rect above is the panel's PHYSICAL footprint in canvas space - for a
    // real wall the config gives each screen's position relative to the others,
    // so the gaps between those rects already are the bezels. `bezel` then says
    // how much of this panel's own edge is hidden under its frame: the scene is
    // rendered into the rect inset by `bezel`, and whatever falls under the
    // frame is simply not drawn. Content therefore passes BEHIND the bezel
    // instead of jumping across it.
    //
    // On a single monitor (three windows on one screen) there is no real frame,
    // so this doubles as a simulator: the inset border is cleared to the bezel
    // colour, which is what makes the effect visible and tunable.
    int bezel = 0;
};

// M25: Presentation = flat tiles in fixed regions, count-adaptive (1 full
// screen, 2 top/bottom, 3 triangle, 4 quad), 16:9 aspect-fit per region.


class SceneHost {
public:
    // vk must already be initialized: window mode with the FIRST spec's size
    // (the host repositions it), or headless (then specs beyond the first are
    // ignored and the offscreen target is the whole canvas).
    void setOutputs(const std::vector<std::string>& names) { portableOutput_.configure(names); }
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

    // M31: is this stream still on screen? True while it is settled OR still
    // animating out. A producer whose pipe has closed must have its ring kept
    // alive until this goes false, otherwise the exit animation samples freed
    // memory - which is why the service defers teardown rather than freeing on
    // disconnect. Only the Presentation layout animates, so this is false
    // elsewhere and teardown is immediate as before.
    bool retainsStream(uint64_t id) const {
        return layout_ == SceneLayout::Presentation && motion_.has(id);
    }

    // Called from the GLFW key callback; public for the C callback trampoline.
    void handleKey(int key);

    // M42: the moderator console does programmatically what the keyboard does
    // - and in headless mode there is no keyboard, so this is the only way.
    void setLayout(SceneLayout l) { layout_ = l; }
    SceneLayout layout() const { return layout_; }
    void setPaused(bool p) { paused_ = p; }
    bool paused() const { return paused_; }
    void requestPreview() { previewRequested_=true; }
    bool takePreview(std::vector<uint8_t>& pixels) {
        pixels.swap(previewPixels_);previewPixels_.clear();return !pixels.empty();
    }
    void forgetStream(uint64_t id) {
        motion_.forget(id);
        for(size_t i=tileSlots_.size();i-->0;)if(tileSlots_[i].first==id)tileSlots_.erase(tileSlots_.begin()+i);
    }

    // M43: runtime display outputs (headless appliance mode). The offscreen
    // canvas stays the master - the camera always sees the whole scene - and
    // each activated monitor gets a borderless window showing an
    // aspect-correct subregion: the union of the active monitors' desktop
    // rects is aspect-fit into the canvas, and each monitor renders its slice
    // of that union (the same math the multi-window wall uses). Render thread
    // only - the moderator's commands arrive through the control queue.
    struct DisplayInfo {
        int id = 0; // index into glfwGetMonitors at enumeration time
        std::string name;
        int x = 0, y = 0, width = 0, height = 0; // desktop rect
        bool primary = false;
        bool active = false;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // canvas region when active
    };
    std::vector<DisplayInfo> listDisplays() const;
    bool setDisplayActive(VkContext& vk, int id, bool on);
    size_t activeDisplayCount() const { return displayOuts_.size(); }

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
    // M43 internals: displayOuts_ parallels extra_ in headless mode.
    struct DisplayOut {
        int monitorId = 0;
        std::string name;
        int x = 0, y = 0, width = 0, height = 0;
    };
    void recomputeDisplayRects();
    bool resizeFrameSync(VkContext& vk);

    std::vector<WindowSpec> specs_;
    std::vector<Window> extra_;
    std::vector<DisplayOut> displayOuts_; // headless only, parallel to extra_
    bool canDisplay_ = false;             // copied from vk.canDisplay at init
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
        FrameGpuTrace gpuTrace;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        std::vector<VkSemaphore> acquireSems;    // per window
        std::vector<VkSemaphore> renderDoneSems; // per window
    };
    FrameSync frames_[kFramesInFlight];
    PortableOutput portableOutput_;
    uint64_t frameIndex_ = 0;
    // M31: entry/reflow/exit animation for presentation tiles. Shared with the
    // Android wall demo (scene_motion.h) so the motion is written once.
    SceneMotion motion_;
    // Last known pool slot per stream id, so a stream that has left `streams`
    // can still be sampled while it animates out.
    std::vector<std::pair<uint64_t, int64_t>> tileSlots_;
    uint64_t renders_ = 0;
    int64_t rateWindowStart_ = 0;
    // M28: VKR_SCENE_TIMING breakdown, accumulated over the 5 s report window.
    int64_t fenceNs_ = 0;   // waiting on this slot's previous GPU work
    int64_t acquireNs_ = 0; // waiting for a swapchain image (vsync pacing)
    int64_t recordNs_ = 0;  // command buffer recording
    int64_t submitNs_ = 0;  // vkQueueSubmit
    int64_t presentNs_ = 0; // vkQueuePresentKHR

    // Snapshot control (P key, or VKR_SNAPSHOT_SECONDS for periodic dumps):
    // writes each window's last presented frame to scene-w<N>.ppm.
    bool previewRequested_ = false;
    std::vector<uint8_t> previewPixels_;
    bool snapshotRequested_ = false;
    double snapshotEvery_ = 0.0;
    int64_t lastSnapshotNs_ = 0;
    std::vector<uint32_t> lastImageIndex_;
    void snapshot(VkContext& vk);
};

} // namespace vkr

#endif // !__ANDROID__
