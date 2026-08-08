#include "background_pass.h"

#include <cstdio>
#include <vector>

#include "log.h"

namespace vkr {

namespace {
VkShaderModule loadModule(VkDevice device, const std::string& path) {
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
} // namespace

bool BackgroundPass::init(VkContext& vk, const std::string& shaderDir) {
    VkPushConstantRange pcRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 6 * sizeof(float)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(vk.device, &plci, nullptr, &layout_) != VK_SUCCESS) return false;

    VkShaderModule vert = loadModule(vk.device, shaderDir + "/fullscreen.vert.spv");
    VkShaderModule frag = loadModule(vk.device, shaderDir + "/vaporwave.frag.spv");
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

    VkPipelineVertexInputStateCreateInfo vin{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAtt;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat targetFormat = vk.swapFormat;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &targetFormat;

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
    gpci.layout = layout_;

    VkResult r = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &gpci, nullptr,
                                           &pipeline_);
    vkDestroyShaderModule(vk.device, vert, nullptr);
    vkDestroyShaderModule(vk.device, frag, nullptr);
    if (r != VK_SUCCESS) {
        VKR_ERR("background pipeline creation failed");
        return false;
    }
    return true;
}

void BackgroundPass::draw(VkCommandBuffer cmd, VkExtent2D extent, float timeSec,
                          float canvasAspect, float uvOffX, float uvOffY, float uvScaleX,
                          float uvScaleY, uint32_t originPx) {
    if (!pipeline_) return;
    VkViewport viewport{(float)originPx, (float)originPx, (float)extent.width,
                        (float)extent.height, 0.0f, 1.0f};
    VkRect2D scissor{{(int32_t)originPx, (int32_t)originPx}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    if (canvasAspect <= 0.0f)
        canvasAspect = extent.height ? (float)extent.width / (float)extent.height : 1.0f;
    float pc[6] = {timeSec, canvasAspect, uvOffX, uvOffY, uvScaleX, uvScaleY};
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void BackgroundPass::destroy(VkContext& vk) {
    if (pipeline_) vkDestroyPipeline(vk.device, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(vk.device, layout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
}

} // namespace vkr
