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
    // Fullscreen: letterboxed quad (desktop). Cube: spinning cube sampling the
    // video on all six faces — combined-sampler mode only, caller supplies the
    // MVP each frame via record()'s mvp16 (Android M10).
    enum class Geometry { Fullscreen, Cube };

    bool init(VkContext& vk, const std::string& shaderDir,
              Geometry geometry = Geometry::Fullscreen);
    void destroy(VkContext& vk);

    // Builds sampler/pipeline for a stream header and descriptors for any
    // slots the pool already has.
    bool bindPool(VkContext& vk, const ExternalVideoPool& pool);
    // Adds the descriptor set for one newly imported slot.
    bool addSlot(VkContext& vk, const ExternalVideoPool& pool, uint32_t key);
    // Frees one slot's descriptor set (pair with ExternalVideoPool::removeSlot).
    void removeSlot(VkContext& vk, uint32_t key);
    void unbindPool(VkContext& vk);

    // Records: acquire slot from the external queue family, draw into target
    // (letterboxed quad, or the cube when geometry is Cube and mvp16 is a
    // column-major mat4), release back. Unknown key records a plain clear.
    void record(VkCommandBuffer cmd, const ExternalVideoPool& pool, int64_t slotKey,
                VkImageView target, VkExtent2D extent, uint32_t queueFamily,
                const float* mvp16 = nullptr);

    // Cube scene with per-face streams (M11/M12): cubeCount cubes, each with
    // its own column-major mat4 in mvps (16 floats per cube) and six entries
    // in faceSlots (-1 = face not drawn). Barriers cover each unique slot.
    // background, if non-null, is drawn first inside the render pass.
    void recordScene(VkCommandBuffer cmd, const ExternalVideoPool& pool,
                     const int64_t* faceSlots, uint32_t cubeCount, const float* mvps,
                     VkImageView target, VkExtent2D extent, uint32_t queueFamily,
                     class BackgroundPass* background = nullptr, float timeSec = 0.0f);

private:
    VkShaderModule loadShader(VkDevice device, const std::string& path);

    std::string shaderDir_;
    VkFormat targetFormat_ = VK_FORMAT_UNDEFINED;
    Geometry geometry_ = Geometry::Fullscreen;

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
