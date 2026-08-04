#include "video_pass.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "log.h"

namespace vkr {

namespace {
// dma-buf producers (VAAPI) are a different driver "instance": FOREIGN.
// Same-GPU cross-API sharing on Windows (D3D11) uses EXTERNAL.
#ifdef _WIN32
constexpr uint32_t kExternalQueueFamily = VK_QUEUE_FAMILY_EXTERNAL;
#else
constexpr uint32_t kExternalQueueFamily = VK_QUEUE_FAMILY_FOREIGN_EXT;
#endif
} // namespace

VkShaderModule VideoPass::loadShader(VkDevice device, const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        VKR_ERR("cannot open shader %s", path.c_str());
        return VK_NULL_HANDLE;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint32_t> code((size + 3) / 4);
    fread(code.data(), 1, size, f);
    fclose(f);

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = (size_t)size;
    smci.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smci, nullptr, &mod) != VK_SUCCESS)
        VKR_ERR("vkCreateShaderModule(%s) failed", path.c_str());
    return mod;
}

bool VideoPass::init(VkContext& vk, const std::string& shaderDir, Geometry geometry) {
    shaderDir_ = shaderDir;
    targetFormat_ = vk.swapFormat;
    geometry_ = geometry;
    return true;
}

bool VideoPass::bindPool(VkContext& vk, const ExternalVideoPool& pool) {
    unbindPool(vk);
    planar_ = pool.planar;
    pcMatrix_ = pool.matrix == proto::Matrix::BT709 ? 1 : 0;
    pcFullRange_ = pool.range == proto::Range::Full ? 1 : 0;

    // Combined mode: one binding whose immutable sampler carries the YCbCr
    // conversion (NV12) or is plain (RGB). Planar mode: two bindings (R8 luma
    // + R8G8 chroma) with a plain sampler and YUV math in the shader.
    VkSamplerYcbcrConversionInfo convInfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    convInfo.conversion = pool.conversion;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    if (pool.conversion && !planar_) sci.pNext = &convInfo;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(vk.device, &sci, nullptr, &sampler_) != VK_SUCCESS) return false;

    const uint32_t bindingCount = planar_ ? 2 : 1;
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < bindingCount; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[i].pImmutableSamplers = &sampler_;
    }
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = bindingCount;
    dslci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(vk.device, &dslci, nullptr, &setLayout_) != VK_SUCCESS)
        return false;

    if (geometry_ == Geometry::Cube && planar_) {
        // cube.vert claims push-constant offset 0 for the MVP; the planar
        // fragment shader's constants would collide. The cube path is Android,
        // where AHB import always yields combined mode, so this cannot happen.
        VKR_ERR("cube geometry requires combined-sampler mode");
        return false;
    }
    VkPushConstantRange pcRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 2 * sizeof(int32_t)};
    if (geometry_ == Geometry::Cube)
        pcRange = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof(float)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout_;
    if (planar_ || geometry_ == Geometry::Cube) {
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcRange;
    }
    if (vkCreatePipelineLayout(vk.device, &plci, nullptr, &pipelineLayout_) != VK_SUCCESS)
        return false;

    VkShaderModule vert = loadShader(
        vk.device,
        shaderDir_ + (geometry_ == Geometry::Cube ? "/cube.vert.spv" : "/fullscreen.vert.spv"));
    VkShaderModule frag = loadShader(
        vk.device, shaderDir_ + (planar_ ? "/nv12_planar.frag.spv" : "/video_ycbcr.frag.spv"));
    if (!vert || !frag) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // Cube: back-face culling stands in for depth (solo convex object).
    // Winding verified empirically on device: with the projection's Y-flip the
    // outward faces rasterize counter-clockwise (mirrored timestamp text on
    // the faces is the tell when this is wrong).
    rs.cullMode = geometry_ == Geometry::Cube ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAtt;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &targetFormat_;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.pNext = &rendering;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vin;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &blend;
    gpci.pDynamicState = &dyn;
    gpci.layout = pipelineLayout_;

    VkResult r = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline_);
    vkDestroyShaderModule(vk.device, vert, nullptr);
    vkDestroyShaderModule(vk.device, frag, nullptr);
    if (r != VK_SUCCESS) {
        VKR_ERR("vkCreateGraphicsPipelines failed");
        return false;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  proto::kMaxSurfaces * 2};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = proto::kMaxSurfaces;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(vk.device, &dpci, nullptr, &descriptorPool_) != VK_SUCCESS)
        return false;

    for (const auto& [key, slot] : pool.slots) {
        if (!addSlot(vk, pool, key)) return false;
    }
    return true;
}

bool VideoPass::addSlot(VkContext& vk, const ExternalVideoPool& pool, uint32_t key) {
    if (sets_.count(key)) return true;
    const auto* slot = pool.find(key);
    if (!slot || !descriptorPool_) return false;

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = descriptorPool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &setLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vk.device, &dsai, &set) != VK_SUCCESS) {
        VKR_ERR("descriptor allocation failed (surface %u)", key);
        return false;
    }
    const uint32_t viewCount = planar_ ? 2 : 1;
    VkDescriptorImageInfo imgs[2]{};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < viewCount; ++i) {
        imgs[i] = {VK_NULL_HANDLE, slot->view[i], VK_IMAGE_LAYOUT_GENERAL};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imgs[i];
    }
    vkUpdateDescriptorSets(vk.device, viewCount, writes, 0, nullptr);
    sets_.emplace(key, set);
    return true;
}

void VideoPass::unbindPool(VkContext& vk) {
    if (descriptorPool_) vkDestroyDescriptorPool(vk.device, descriptorPool_, nullptr);
    if (pipeline_) vkDestroyPipeline(vk.device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(vk.device, pipelineLayout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(vk.device, setLayout_, nullptr);
    if (sampler_) vkDestroySampler(vk.device, sampler_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    sets_.clear();
}

void VideoPass::record(VkCommandBuffer cmd, const ExternalVideoPool& pool, int64_t slotKey,
                       VkImageView target, VkExtent2D extent, uint32_t queueFamily,
                       const float* mvp16) {
    const ExternalVideoPool::Slot* slot =
        slotKey >= 0 ? pool.find((uint32_t)slotKey) : nullptr;
    auto setIt = slotKey >= 0 ? sets_.find((uint32_t)slotKey) : sets_.end();
    const bool haveVideo = slot && setIt != sets_.end() && pipeline_ != VK_NULL_HANDLE;
    static const bool noExternalBarrier = getenv("VKR_NO_EXTERNAL_BARRIER") != nullptr;

    // Acquire the sampled image from the external producer. Layout stays
    // GENERAL both ways: transitioning from UNDEFINED would legally discard
    // the decoder's pixels.
    if (haveVideo && !noExternalBarrier) {
        VkImageMemoryBarrier2 acquire[2]{};
        uint32_t n = 0;
        for (uint32_t i = 0; i < 2 && slot->image[i]; ++i, ++n) {
            acquire[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            acquire[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            acquire[i].srcAccessMask = VK_ACCESS_2_NONE;
            acquire[i].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            acquire[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            acquire[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            acquire[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            acquire[i].srcQueueFamilyIndex = kExternalQueueFamily;
            acquire[i].dstQueueFamilyIndex = queueFamily;
            acquire[i].image = slot->image[i];
            acquire[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = n;
        dep.pImageMemoryBarriers = acquire;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.05f, 0.05f, 0.08f, 1.0f}};

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);

    if (haveVideo) {
        VkViewport viewport{0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f};
        if (geometry_ == Geometry::Fullscreen) {
            // Letterbox: fit the video into the target preserving aspect ratio.
            float scale = std::min((float)extent.width / (float)pool.width,
                                   (float)extent.height / (float)pool.height);
            float vw = pool.width * scale, vh = pool.height * scale;
            viewport = {(extent.width - vw) * 0.5f, (extent.height - vh) * 0.5f,
                        vw, vh, 0.0f, 1.0f};
        }
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &setIt->second, 0, nullptr);
        if (geometry_ == Geometry::Cube && mvp16) {
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               16 * sizeof(float), mvp16);
        } else if (planar_) {
            int32_t pc[2] = {pcMatrix_, pcFullRange_};
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                               pc);
        }
        vkCmdDraw(cmd, geometry_ == Geometry::Cube ? 36 : 3, 1, 0, 0);
    }

    vkCmdEndRendering(cmd);

    if (haveVideo && !noExternalBarrier) {
        VkImageMemoryBarrier2 release[2]{};
        uint32_t n = 0;
        for (uint32_t i = 0; i < 2 && slot->image[i]; ++i, ++n) {
            release[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            release[i].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            release[i].srcAccessMask = VK_ACCESS_2_NONE; // reads need no flush
            release[i].dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            release[i].dstAccessMask = VK_ACCESS_2_NONE;
            release[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            release[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            release[i].srcQueueFamilyIndex = queueFamily;
            release[i].dstQueueFamilyIndex = kExternalQueueFamily;
            release[i].image = slot->image[i];
            release[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = n;
        dep.pImageMemoryBarriers = release;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

void VideoPass::destroy(VkContext& vk) {
    unbindPool(vk);
}

} // namespace vkr
