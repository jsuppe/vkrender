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

bool VideoPass::init(VkContext& vk, const std::string& shaderDir) {
    shaderDir_ = shaderDir;
    targetFormat_ = vk.swapFormat;
    return true;
}

bool VideoPass::bindPool(VkContext& vk, const ExternalVideoPool& pool) {
    unbindPool(vk);

    // For NV12 streams the sampler carries the YCbCr conversion (filters must
    // match its chromaFilter); RGB streams use a plain sampler.
    VkSamplerYcbcrConversionInfo convInfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    convInfo.conversion = pool.conversion;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    if (pool.conversion) sci.pNext = &convInfo;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(vk.device, &sci, nullptr, &sampler_) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = &sampler_;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(vk.device, &dslci, nullptr, &setLayout_) != VK_SUCCESS)
        return false;

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout_;
    if (vkCreatePipelineLayout(vk.device, &plci, nullptr, &pipelineLayout_) != VK_SUCCESS)
        return false;

    VkShaderModule vert = loadShader(vk.device, shaderDir_ + "/fullscreen.vert.spv");
    VkShaderModule frag = loadShader(vk.device, shaderDir_ + "/video_ycbcr.frag.spv");
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
    rs.cullMode = VK_CULL_MODE_NONE;
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

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, proto::kMaxSurfaces};
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
    VkDescriptorImageInfo img{VK_NULL_HANDLE, slot->view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &img;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, nullptr);
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
                       VkImageView target, VkExtent2D extent, uint32_t queueFamily) {
    const ExternalVideoPool::Slot* slot =
        slotKey >= 0 ? pool.find((uint32_t)slotKey) : nullptr;
    auto setIt = slotKey >= 0 ? sets_.find((uint32_t)slotKey) : sets_.end();
    const bool haveVideo = slot && setIt != sets_.end() && pipeline_ != VK_NULL_HANDLE;
    static const bool noExternalBarrier = getenv("VKR_NO_EXTERNAL_BARRIER") != nullptr;

    // Acquire the sampled image from the external producer. Layout stays
    // GENERAL both ways: transitioning from UNDEFINED would legally discard
    // the decoder's pixels.
    if (haveVideo && !noExternalBarrier) {
        VkImageMemoryBarrier2 acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        acquire.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        acquire.srcAccessMask = VK_ACCESS_2_NONE;
        acquire.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        acquire.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.srcQueueFamilyIndex = kExternalQueueFamily;
        acquire.dstQueueFamilyIndex = queueFamily;
        acquire.image = slot->image;
        acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &acquire;
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
        // Letterbox: fit the video into the target preserving aspect ratio.
        float scale = std::min((float)extent.width / (float)pool.width,
                               (float)extent.height / (float)pool.height);
        float vw = pool.width * scale, vh = pool.height * scale;
        VkViewport viewport{(extent.width - vw) * 0.5f, (extent.height - vh) * 0.5f,
                            vw, vh, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &setIt->second, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    vkCmdEndRendering(cmd);

    if (haveVideo && !noExternalBarrier) {
        VkImageMemoryBarrier2 release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        release.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        release.srcAccessMask = VK_ACCESS_2_NONE; // reads need no flush
        release.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        release.dstAccessMask = VK_ACCESS_2_NONE;
        release.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.srcQueueFamilyIndex = queueFamily;
        release.dstQueueFamilyIndex = kExternalQueueFamily;
        release.image = slot->image;
        release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &release;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

void VideoPass::destroy(VkContext& vk) {
    unbindPool(vk);
}

} // namespace vkr
