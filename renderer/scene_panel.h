#pragma once

// On-screen controls for the desktop scene (Dear ImGui, on the primary window).
//
// The panel is one more client of the control plane, beside the web console:
// it reads the ControlSnapshot the render loop publishes and emits the same
// ControlCommands the HTTP API does, so there is a single path that changes
// the renderer. The service loop applies them where it applies the console's.
//
// Drawn after the camera and scene-output captures, so it never appears in
// those feeds. The moderator preview reads the finished image and does show it.

#ifndef __ANDROID__

#include <cstddef>
#include <vector>

#include <volk.h>

#include "control_api.h"
#include "vk_context.h"

namespace vkr {

class ScenePanel {
public:
    struct Bindings {
        float* speed = nullptr;
        bool* backdrop = nullptr;
        bool* quit = nullptr;
        const ControlSnapshot* snapshot = nullptr;
        std::vector<ControlCommand>* commands = nullptr;
        float canvasWidth = 0, canvasHeight = 0;
        size_t windows = 0;
    };

    bool init(VkContext& vk);
    void destroy(VkContext& vk);
    // Once per frame, after input is polled. Must not be skipped between calls
    // to record: the draw data lives until the next build.
    void build(const Bindings& b);
    // Into an image in COLOR_ATTACHMENT_OPTIMAL holding the finished scene.
    void record(VkCommandBuffer cmd, VkImage image, VkImageView view, VkExtent2D extent);

    bool enabled() const { return ready_; }
    bool drawable() const;
    bool wantsKeyboard() const;
    void toggle() { visible_ = !visible_; }

private:
    void drawWindow(const Bindings& b);
    bool ready_ = false;
    bool visible_ = true;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
};

} // namespace vkr

#endif // !__ANDROID__
