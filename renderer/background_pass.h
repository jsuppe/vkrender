#pragma once

#include <string>

#include <volk.h>

#include "vk_context.h"

namespace vkr {

// Fullscreen procedural backdrop (M12: the vaporwave scene) drawn inside an
// already-begun dynamic-rendering pass, before the video geometry. No
// descriptors; push constants carry time + aspect.
class BackgroundPass {
public:
    bool init(VkContext& vk, const std::string& shaderDir);
    void destroy(VkContext& vk);

    // Records the fullscreen draw. Call between vkCmdBeginRendering/End.
    // canvasAspect/uvOff/uvScale map this window into the shared virtual
    // canvas (M15 multi-window spanning); the defaults render the whole
    // backdrop into one target.
    // `extent` is the area to fill; `originPx` offsets it, so a panel with a
    // bezel draws the backdrop inset by the same amount as the rest of the
    // scene (M29).
    void draw(VkCommandBuffer cmd, VkExtent2D extent, float timeSec, float canvasAspect = 0.0f,
              float uvOffX = 0.0f, float uvOffY = 0.0f, float uvScaleX = 1.0f,
              float uvScaleY = 1.0f, uint32_t originPx = 0);

    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

private:
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace vkr
