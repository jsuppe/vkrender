#include "gpu_output.h"
#include "framelink.h"
#include "framelink_sync_fd.h"
#include "gpu_trace.h"
#include "log.h"
#include "output_nv12_spv.h"
#include "perf_trace.h"
#ifdef __ANDROID__
#include "output_fullscreen_spv.h"
#include "output_yuv_spv.h"
#endif
#include <algorithm>
#include <chrono>
#include <map>
#ifdef __ANDROID__
#include "framelink_ahb.h"
#include <android/hardware_buffer.h>
#else
#include "framelink_dmabuf.h"
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace vkr {
namespace {
uint32_t memoryType(VkContext &v, uint32_t bits) {
  VkPhysicalDeviceMemoryProperties p{};
  vkGetPhysicalDeviceMemoryProperties(v.phys, &p);
  for (uint32_t i = 0; i < p.memoryTypeCount; i++)
    if (bits & (1u << i))
      return i;
  return UINT32_MAX;
}
void barrier(VkCommandBuffer c, VkImage im, VkImageLayout old,
             VkImageLayout next, uint32_t from = VK_QUEUE_FAMILY_IGNORED,
             uint32_t to = VK_QUEUE_FAMILY_IGNORED) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = b.dstAccessMask =
      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = old;
  b.newLayout = next;
  b.srcQueueFamilyIndex = from;
  b.dstQueueFamilyIndex = to;
  b.image = im;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  d.imageMemoryBarrierCount = 1;
  d.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(c, &d);
}
struct Buffer {
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  VkDeviceSize size{};
  void clear(VkContext &v) {
    if (buffer)
      vkDestroyBuffer(v.device, buffer, nullptr);
    if (memory)
      vkFreeMemory(v.device, memory, nullptr);
    *this = {};
  }
  bool ensure(VkContext &v, VkDeviceSize n) {
    if (buffer && size == n)
      return true;
    clear(v);
    VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    b.size = n;
    b.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (vkCreateBuffer(v.device, &b, nullptr, &buffer) != VK_SUCCESS)
      return false;
    VkMemoryRequirements r{};
    vkGetBufferMemoryRequirements(v.device, buffer, &r);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    a.allocationSize = r.size;
    a.memoryTypeIndex = memoryType(v, r.memoryTypeBits);
    if (a.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(v.device, &a, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(v.device, buffer, memory, 0) != VK_SUCCESS) {
      clear(v);
      return false;
    }
    size = n;
    return true;
  }
};
struct Image {
  VkImage image[2]{};
  VkDeviceMemory memory[2]{};
  unsigned planes = 0;
  VkFormat format{};
  uint64_t externalFormat = 0;
  VkFormat resolveFormat{};
  VkImageView views[2]{};
  VkPipeline resolvePipeline{};
  void clear(VkContext &v) {
    if (resolvePipeline)
      vkDestroyPipeline(v.device, resolvePipeline, nullptr);
    for (auto view : views)
      if (view)
        vkDestroyImageView(v.device, view, nullptr);
    for (auto i : image)
      if (i)
        vkDestroyImage(v.device, i, nullptr);
    for (auto m : memory)
      if (m)
        vkFreeMemory(v.device, m, nullptr);
    *this = {};
  }
#ifdef __ANDROID__
  bool resolveInit(VkContext &v, VkPipelineLayout layout) {
    if (resolvePipeline)
      return true;
    VkShaderModule modules[2]{};
    const uint32_t *codes[] = {outputFullscreenSpv, outputYuvSpv};
    size_t sizes[] = {sizeof(outputFullscreenSpv), sizeof(outputYuvSpv)};
    for (unsigned i = 0; i < 2; i++) {
      VkShaderModuleCreateInfo c{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      c.codeSize = sizes[i];
      c.pCode = codes[i];
      if (vkCreateShaderModule(v.device, &c, nullptr, &modules[i]) !=
          VK_SUCCESS) {
        for (auto m : modules)
          if (m)
            vkDestroyShaderModule(v.device, m, nullptr);
        return false;
      }
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (unsigned i = 0; i < 2; i++) {
      stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      stages[i].stage =
          i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
      stages[i].module = modules[i];
      stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vertex{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynamic;
    VkExternalFormatANDROID ef{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
    ef.externalFormat = externalFormat;
    VkFormat color =
        v.nullExternalResolveAttachment ? VK_FORMAT_UNDEFINED : resolveFormat;
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.pNext = &ef;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color;
    VkGraphicsPipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertex;
    ci.pInputAssemblyState = &assembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &samples;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &ds;
    ci.layout = layout;
    auto r = vkCreateGraphicsPipelines(v.device, VK_NULL_HANDLE, 1, &ci,
                                       nullptr, &resolvePipeline);
    for (auto m : modules)
      vkDestroyShaderModule(v.device, m, nullptr);
    return r == VK_SUCCESS;
  }
#endif
  bool import(VkContext &v, flImage *source, const flChannelInfo &info) {
    format = info.format == FL_FORMAT_BGRA8 ? VK_FORMAT_B8G8R8A8_UNORM
             : info.format == FL_FORMAT_RGB10A2
                 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32
                 : VK_FORMAT_R8G8B8A8_UNORM;
#ifdef __ANDROID__
    auto ahb = flImageAHardwareBuffer(source);
    if (!ahb)
      return false;
    VkAndroidHardwareBufferFormatPropertiesANDROID f{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID p{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    p.pNext = &f;
    VkAndroidHardwareBufferFormatResolvePropertiesANDROID rp{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_RESOLVE_PROPERTIES_ANDROID};
    if (v.hasExternalResolve)
      f.pNext = &rp;
    if (vkGetAndroidHardwareBufferPropertiesANDROID(v.device, ahb, &p) !=
        VK_SUCCESS)
      return false;
    bool resolve = f.format == VK_FORMAT_UNDEFINED;
    // Native YUV can also expose a defined multiplanar format. That requires
    // plane-specific copies; it must not enter the single-color RGB path.
    if (info.format == FL_FORMAT_NV12 && !resolve) {
      VKR_WARN("GPU output: defined-format Android YUV transfer is not supported (VkFormat=%u)", f.format);
      return false;
    }
    if (resolve && (!v.hasExternalResolve ||
                    rp.colorAttachmentFormat == VK_FORMAT_UNDEFINED))
      return false;
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);
    if (!(desc.usage & AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT) ||
        (!resolve && !(f.formatFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)))
      return false;
    format = f.format;
    planes = 1;
    externalFormat = resolve ? f.externalFormat : 0;
    resolveFormat = rp.colorAttachmentFormat;
    VkExternalMemoryImageCreateInfo ex{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ex.handleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkExternalFormatANDROID ef{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
    ef.externalFormat = externalFormat;
    if (resolve)
      ex.pNext = &ef;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = &ex;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {info.width, info.height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = resolve ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(v.device, &ci, nullptr, &image[0]) != VK_SUCCESS)
      return false;
    VkImportAndroidHardwareBufferInfoANDROID imp{
        VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    imp.buffer = ahb;
    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &imp;
    dedicated.image = image[0];
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &dedicated;
    ai.allocationSize = p.allocationSize;
    ai.memoryTypeIndex = memoryType(v, p.memoryTypeBits);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(v.device, &ai, nullptr, &memory[0]) != VK_SUCCESS ||
        vkBindImageMemory(v.device, image[0], memory[0], 0) != VK_SUCCESS)
      return false;
    if (resolve) {
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = image[0];
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = VK_FORMAT_UNDEFINED;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (vkCreateImageView(v.device, &vi, nullptr, &views[0]) != VK_SUCCESS)
        return false;
      if (!v.nullExternalResolveAttachment) {
        ci.pNext = nullptr;
        ci.format = resolveFormat;
        if (vkCreateImage(v.device, &ci, nullptr, &image[1]) != VK_SUCCESS)
          return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(v.device, image[1], &req);
        ai.pNext = nullptr;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(v, req.memoryTypeBits);
        if (ai.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(v.device, &ai, nullptr, &memory[1]) !=
                VK_SUCCESS ||
            vkBindImageMemory(v.device, image[1], memory[1], 0) != VK_SUCCESS)
          return false;
        vi.image = image[1];
        vi.format = resolveFormat;
        if (vkCreateImageView(v.device, &vi, nullptr, &views[1]) != VK_SUCCESS)
          return false;
      }
    }
    return true;
#else
    flDmabuf dm{};
    if (flImageDmabuf(source, &dm) != FL_OK)
      return false;
    planes = info.format == FL_FORMAT_NV12 ? 2 : 1;
    if (dm.planes != planes)
      return false;
    // Import the complete native image. Plane copies preserve its native
    // offsets; separate R8/RG8 aliases can lose offsets on dedicated-image
    // imports.
    for (unsigned p = 0; p < 1; p++) {
      VkFormat fmt = planes == 2 ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : format;
      VkSubresourceLayout layouts[2]{};
      for (unsigned j = 0; j < planes; j++) {
        layouts[j].offset = dm.offset[j];
        layouts[j].rowPitch = dm.stride[j];
      }
      if (planes == 2) {
        struct stat a{}, b{};
        if (fstat(dm.fd[0], &a) || fstat(dm.fd[1], &b) ||
            a.st_ino != b.st_ino || a.st_dev != b.st_dev)
          return false;
      }
      VkImageDrmFormatModifierExplicitCreateInfoEXT mod{
          VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
      mod.drmFormatModifier = dm.modifier;
      mod.drmFormatModifierPlaneCount = planes;
      mod.pPlaneLayouts = layouts;
      VkExternalMemoryImageCreateInfo ex{
          VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      ex.pNext = &mod;
      ex.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      VkPhysicalDeviceImageDrmFormatModifierInfoEXT modQuery{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
      modQuery.drmFormatModifier = dm.modifier;
      modQuery.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      VkPhysicalDeviceExternalImageFormatInfo extQuery{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
      extQuery.pNext = &modQuery;
      extQuery.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      VkPhysicalDeviceImageFormatInfo2 q{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
      q.pNext = &extQuery;
      q.format = fmt;
      q.type = VK_IMAGE_TYPE_2D;
      q.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
      q.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      VkExternalImageFormatProperties extProps{
          VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
      VkImageFormatProperties2 props{
          VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
      props.pNext = &extProps;
      if (vkGetPhysicalDeviceImageFormatProperties2(v.phys, &q, &props) !=
              VK_SUCCESS ||
          !(extProps.externalMemoryProperties.externalMemoryFeatures &
            VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
        return false;
      VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ci.pNext = &ex;
      ci.imageType = VK_IMAGE_TYPE_2D;
      ci.format = fmt;
      ci.extent = {p ? info.width / 2 : info.width,
                   p ? info.height / 2 : info.height, 1};
      ci.mipLevels = ci.arrayLayers = 1;
      ci.samples = VK_SAMPLE_COUNT_1_BIT;
      ci.tiling = q.tiling;
      ci.usage = q.usage;
      if (vkCreateImage(v.device, &ci, nullptr, &image[p]) != VK_SUCCESS)
        return false;
      VkMemoryRequirements req{};
      vkGetImageMemoryRequirements(v.device, image[p], &req);
      VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
      if (vkGetMemoryFdPropertiesKHR(
              v.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
              dm.fd[p], &fp) != VK_SUCCESS)
        return false;
      struct stat st{};
      if (fstat(dm.fd[p], &st) != 0 || st.st_size <= 0)
        return false;
      VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
      imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      imp.fd = dup(dm.fd[p]);
      if (imp.fd < 0)
        return false;
      VkMemoryDedicatedAllocateInfo dedicated{
          VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
      dedicated.image = image[p];
      dedicated.pNext = &imp;
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.pNext = (extProps.externalMemoryProperties.externalMemoryFeatures &
                  VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)
                     ? (void *)&dedicated
                     : (void *)&imp;
      ai.allocationSize = st.st_size;
      ai.memoryTypeIndex =
          memoryType(v, fp.memoryTypeBits & req.memoryTypeBits);
      if (ai.memoryTypeIndex == UINT32_MAX ||
          vkAllocateMemory(v.device, &ai, nullptr, &memory[p]) != VK_SUCCESS) {
        close(imp.fd);
        return false;
      }
      if (vkBindImageMemory(v.device, image[p], memory[p], 0) != VK_SUCCESS)
        return false;
    }
    return true;
#endif
  }
};
} // namespace
struct GpuOutput::Impl {
  struct Frame {
    Buffer source, converted;
    VkDescriptorSet set{};
    flBuffer acquired{};
    int64_t pts = 0;
    bool pending = false, armed = false;
    FrameGpuTrace trace;
  };
  struct Output {
    std::string name;
    flChannel *channel = nullptr;
    int64_t retry = 0;
    bool failed = false;
    std::map<flImage *, Image> images;
    Frame frames[2];
  };
  bool attempted = false;
  VkDeviceSize maxStorageRange = 0;
  std::vector<Output> outputs;
  VkDescriptorSetLayout sets{};
  VkDescriptorPool descriptors{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  bool init(VkContext &v) {
    if (pipeline)
      return true;
    if (attempted)
      return false;
    attempted = true;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(v.phys, &properties);
    maxStorageRange = properties.limits.maxStorageBufferRange;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(v.phys, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(v.phys, &count, queues.data());
    if (!(queues[v.queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT))
      return false;
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (unsigned i = 0; i < 2; i++) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags =
          VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo si{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = 2;
    si.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(v.device, &si, nullptr, &sets) !=
        VK_SUCCESS)
      return false;
    VkPushConstantRange push{
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
    VkPipelineLayoutCreateInfo li{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &sets;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(v.device, &li, nullptr, &layout) != VK_SUCCESS)
      return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              (uint32_t)outputs.size() * 4};
    VkDescriptorPoolCreateInfo pi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = outputs.size() * 2;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (vkCreateDescriptorPool(v.device, &pi, nullptr, &descriptors) !=
        VK_SUCCESS)
      return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(outputNv12Spv);
    mi.pCode = outputNv12Spv;
    VkShaderModule module{};
    if (vkCreateShaderModule(v.device, &mi, nullptr, &module) != VK_SUCCESS)
      return false;
    VkComputePipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.layout = layout;
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    auto r = vkCreateComputePipelines(v.device, VK_NULL_HANDLE, 1, &ci, nullptr,
                                      &pipeline);
    vkDestroyShaderModule(v.device, module, nullptr);
    return r == VK_SUCCESS;
  }
  void close(VkContext &v, Output &o) {
    for (auto &[k, i] : o.images)
      i.clear(v);
    o.images.clear();
    if (o.channel)
      flClose(o.channel);
    o.channel = nullptr;
    o.failed = false;
  }
};
GpuOutput::GpuOutput() : impl(new Impl) {}
GpuOutput::~GpuOutput() = default;
void GpuOutput::configure(const std::vector<std::string> &names) {
  for (auto &n : names)
    if (!n.empty()) {
      impl->outputs.emplace_back();
      impl->outputs.back().name = n;
    }
  VKR_LOG("scene output: strict GPU-only mode; unsupported imports are refused "
          "(no CPU fallback)");
}
void GpuOutput::submitted(unsigned slot) {
  for (auto &o : impl->outputs) {
    auto &f = o.frames[slot % 2];
    if (f.armed) {
      f.pending = true;
      f.armed = false;
      f.trace.submitted((uint64_t)f.pts);
    }
  }
}
void GpuOutput::retire(VkContext &v, unsigned slot) {
  vkrtrace::Scope scope("output.gpu_publish");
  for (auto &o : impl->outputs) {
    auto &f = o.frames[slot % 2];
    if (f.pending) {
      f.trace.collect(v);
      if (flSubmitSyncFd(o.channel, &f.acquired, -1, f.pts) != FL_OK)
        o.failed = true;
      f.pending = false;
    }
    if (o.failed && !o.frames[0].pending && !o.frames[1].pending)
      impl->close(v, o);
  }
}
void GpuOutput::record(VkContext &v, unsigned slot, VkCommandBuffer cmd,
                       VkImage source, VkExtent2D extent, VkFormat format,
                       VkImageLayout sourceLayout, int64_t pts) {
  vkrtrace::Scope scope("output.gpu_record");
  if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB &&
      format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_B8G8R8A8_SRGB)
    return;
  bool attempted = impl->attempted;
  if (!impl->init(v)) {
    if (!attempted)
      VKR_ERR("GPU output initialization failed; no CPU fallback");
    return;
  }
  for (auto &o : impl->outputs) {
    auto &f = o.frames[slot % 2];
    if (f.pending || f.armed || o.failed)
      continue;
    int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    if (!o.channel && now >= o.retry) {
      o.retry = now + 1000000000ll;
      flOpenProducer(o.name.c_str(), &o.channel);
    }
    if (!o.channel)
      continue;
    flChannelInfo info{};
    if (flQuery(o.channel, &info) != FL_OK) {
      o.failed = true;
      continue;
    }
    flBuffer b{};
    auto r = flAcquireBuffer(o.channel, &b, 0);
    if (r == FL_DISCONNECTED) {
      o.failed = true;
      continue;
    }
    if (r != FL_OK)
      continue;
    auto it = o.images.find(b.image);
    if (it == o.images.end()) {
      auto &image = o.images[b.image];
      if (!image.import(v, b.image, info)) {
        VKR_WARN(
            "GPU output '%s' format %u: writable native import unsupported",
            o.name.c_str(), info.format);
        image.clear(v);
        o.failed = true;
        continue;
      }
      it = o.images.find(b.image);
      VKR_LOG("GPU output '%s': imported native buffer format=%u planes=%u",
              o.name.c_str(), info.format, it->second.planes);
    }
    auto &image = it->second;
#ifdef __ANDROID__
    if (image.externalFormat && !image.resolveInit(v, impl->layout)) {
      o.failed = true;
      continue;
    }
#endif
    {
      const VkDeviceSize sourceBytes =
          (VkDeviceSize)extent.width * extent.height * 4;
      const VkDeviceSize outputBytes =
          image.externalFormat
              ? 4
              : ((((VkDeviceSize)info.width * info.height *
                   (info.format == FL_FORMAT_NV12 ? 3 : 8) / 2) +
                  3) &
                 ~VkDeviceSize(3));
      if (sourceBytes > impl->maxStorageRange ||
          outputBytes > impl->maxStorageRange) {
        VKR_WARN("GPU output '%s': storage buffer range exceeds device limit",
                 o.name.c_str());
        o.failed = true;
        continue;
      }
      if (!f.source.ensure(v, sourceBytes) ||
          !f.converted.ensure(v, outputBytes)) {
        o.failed = true;
        continue;
      }
      if (!f.set) {
        VkDescriptorSetAllocateInfo a{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        a.descriptorPool = impl->descriptors;
        a.descriptorSetCount = 1;
        a.pSetLayouts = &impl->sets;
        if (vkAllocateDescriptorSets(v.device, &a, &f.set) != VK_SUCCESS) {
          o.failed = true;
          continue;
        }
      }
      VkDescriptorBufferInfo buffers[2] = {
          {f.source.buffer, 0, f.source.size},
          {f.converted.buffer, 0, f.converted.size}};
      VkWriteDescriptorSet writes[2]{};
      for (unsigned i = 0; i < 2; i++) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = f.set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffers[i];
      }
      vkUpdateDescriptorSets(v.device, 2, writes, 0, nullptr);
    }
    f.trace.durationName = "gpu.output_duration";
    f.trace.object = (uint64_t)(uintptr_t)o.channel;
    f.trace.prepare(v);
    f.trace.begin(cmd);
    barrier(cmd, source, sourceLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    for (unsigned p = 0; p < 1; p++)
      barrier(cmd, image.image[p], VK_IMAGE_LAYOUT_UNDEFINED,
              image.externalFormat ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                   : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_QUEUE_FAMILY_FOREIGN_EXT, v.queueFamily);
    {
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {extent.width, extent.height, 1};
      vkCmdCopyImageToBuffer(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             f.source.buffer, 1, &copy);
      VkMemoryBarrier2 mem{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      mem.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
      mem.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      mem.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.memoryBarrierCount = 1;
      dep.pMemoryBarriers = &mem;
      vkCmdPipelineBarrier2(cmd, &dep);
      uint32_t params[6] = {extent.width,
                            extent.height,
                            info.width,
                            info.height,
                            (uint32_t)(format == VK_FORMAT_B8G8R8A8_UNORM ||
                                       format == VK_FORMAT_B8G8R8A8_SRGB),
                            (uint32_t)info.format};
#ifdef __ANDROID__
      if (info.format == FL_FORMAT_BGRA8)
        params[5] =
            FL_FORMAT_RGBA8; // native AHB storage, CPU mapping swizzles BGRA
      if (image.externalFormat) {
        if (image.image[1])
          barrier(cmd, image.image[1], VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo attachment{
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = image.views[1];
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.resolveMode =
            VK_RESOLVE_MODE_EXTERNAL_FORMAT_DOWNSAMPLE_ANDROID;
        attachment.resolveImageView = image.views[0];
        attachment.resolveImageLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.extent = {info.width, info.height};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &attachment;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport viewport{0, 0, (float)info.width, (float)info.height, 0, 1};
        VkRect2D scissor{{0, 0}, {info.width, info.height}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          image.resolvePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                impl->layout, 0, 1, &f.set, 0, nullptr);
        vkCmdPushConstants(cmd, impl->layout,
                           VK_SHADER_STAGE_COMPUTE_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(params), params);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
      } else
#endif
      {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                impl->layout, 0, 1, &f.set, 0, nullptr);
        vkCmdPushConstants(cmd, impl->layout,
                           VK_SHADER_STAGE_COMPUTE_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(params), params);
        const uint32_t groups = (uint32_t)((f.converted.size / 4 + 63) / 64);
        const uint32_t groupsX = std::min(groups, 65535u);
        vkCmdDispatch(cmd, groupsX, (groups + groupsX - 1) / groupsX, 1);
        mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mem.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mem.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mem.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier2(cmd, &dep);
        for (unsigned p = 0; p < image.planes; p++) {
          copy.bufferOffset = p ? (VkDeviceSize)info.width * info.height : 0;
          copy.imageExtent = {p ? info.width / 2 : info.width,
                              p ? info.height / 2 : info.height, 1};
          copy.imageSubresource.aspectMask =
              image.planes == 2 ? (p ? VK_IMAGE_ASPECT_PLANE_1_BIT
                                     : VK_IMAGE_ASPECT_PLANE_0_BIT)
                                : VK_IMAGE_ASPECT_COLOR_BIT;
          vkCmdCopyBufferToImage(cmd, f.converted.buffer, image.image[0],
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &copy);
        }
      }
    }
    for (unsigned p = 0; p < 1; p++)
      barrier(cmd, image.image[p],
              image.externalFormat ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                   : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_GENERAL, v.queueFamily,
              VK_QUEUE_FAMILY_FOREIGN_EXT);
    barrier(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sourceLayout);
    f.acquired = b;
    f.pts = pts;
    f.armed = true;
    f.trace.end(cmd);
  }
}
void GpuOutput::destroy(VkContext &v) {
  for (auto &o : impl->outputs) {
    impl->close(v, o);
    for (auto &f : o.frames) {
      f.trace.destroy(v);
      f.source.clear(v);
      f.converted.clear(v);
    }
  }
  if (impl->pipeline)
    vkDestroyPipeline(v.device, impl->pipeline, nullptr);
  if (impl->layout)
    vkDestroyPipelineLayout(v.device, impl->layout, nullptr);
  if (impl->descriptors)
    vkDestroyDescriptorPool(v.device, impl->descriptors, nullptr);
  if (impl->sets)
    vkDestroyDescriptorSetLayout(v.device, impl->sets, nullptr);
  impl.reset(new Impl);
}
} // namespace vkr
