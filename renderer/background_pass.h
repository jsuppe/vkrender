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
    void draw(VkCommandBuffer cmd, VkExtent2D extent, float timeSec);

    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

private:
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace vkr
