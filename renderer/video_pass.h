#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

#include "external_video_image.h"
#include "vk_context.h"

namespace vkr {

// Fullscreen video pass over the imported images, with EXTERNAL-queue-family
// acquire/release barriers around each sample. The pipeline is (re)built per
// imported pool because the immutable YCbCr sampler (carrying the stream's
// colorimetry) is baked into the descriptor set layout.
class VideoPass {
public:
    bool init(VkContext& vk, const std::string& shaderDir);
    void destroy(VkContext& vk);

    // Builds sampler/pipeline/descriptors for a freshly imported pool.
    bool bindPool(VkContext& vk, const ExternalVideoPool& pool);
    void unbindPool(VkContext& vk);

    // Records: acquire slot from EXTERNAL, letterboxed draw into target,
    // release back to EXTERNAL. slot < 0 records a plain clear.
    void record(VkCommandBuffer cmd, const ExternalVideoPool& pool, int slot,
                VkImageView target, VkExtent2D extent, uint32_t queueFamily);

private:
    VkShaderModule loadShader(VkDevice device, const std::string& path);

    std::string shaderDir_;
    VkFormat targetFormat_ = VK_FORMAT_UNDEFINED;

    // Per-pool objects (immutable sampler bakes the YCbCr conversion)
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets_;
};

} // namespace vkr
