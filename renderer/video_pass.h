#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <volk.h>

#include "external_video_image.h"
#include "vk_context.h"

namespace vkr {

// Fullscreen video pass over the imported images, with external-queue-family
// acquire/release barriers around each sample (EXTERNAL on Windows, FOREIGN on
// Linux dma-bufs). The pipeline is (re)built per stream because the immutable
// sampler (carrying the YCbCr conversion for NV12) is baked into the
// descriptor set layout. Descriptor sets are added per surface as the decoder
// announces them (Linux) or all at once (Windows).
class VideoPass {
public:
    bool init(VkContext& vk, const std::string& shaderDir);
    void destroy(VkContext& vk);

    // Builds sampler/pipeline for a stream header and descriptors for any
    // slots the pool already has.
    bool bindPool(VkContext& vk, const ExternalVideoPool& pool);
    // Adds the descriptor set for one newly imported slot.
    bool addSlot(VkContext& vk, const ExternalVideoPool& pool, uint32_t key);
    void unbindPool(VkContext& vk);

    // Records: acquire slot from the external queue family, letterboxed draw
    // into target, release back. Unknown key records a plain clear.
    void record(VkCommandBuffer cmd, const ExternalVideoPool& pool, int64_t slotKey,
                VkImageView target, VkExtent2D extent, uint32_t queueFamily);

private:
    VkShaderModule loadShader(VkDevice device, const std::string& path);

    std::string shaderDir_;
    VkFormat targetFormat_ = VK_FORMAT_UNDEFINED;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkDescriptorSet> sets_;
    bool planar_ = false;    // per-plane R8/R8G8 sampling with shader YUV
    int32_t pcMatrix_ = 1;   // push constants for the planar shader
    int32_t pcFullRange_ = 0;
};

} // namespace vkr
