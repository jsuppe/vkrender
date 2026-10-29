#include "framelink.h"
#include "gpu_output.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)
int main(int argc, char **argv) {
  flFormat format = argc > 1 ? (flFormat)atoi(argv[1]) : FL_FORMAT_RGBA8;
  bool large = argc > 2 && std::string(argv[2]) == "large";
  unsigned w = large ? 3840 : 130, h = large ? 2160 : 66;
  const unsigned iterations = large ? 4 : 24, recreate = large ? 2 : 12;
  vkr::VkContext v;
  CHECK(v.init(64, 32, "GPU output pixels", true));
  const std::string channel = "gpu.pixel.test." + std::to_string(getpid());
  flChannel *c = nullptr;
  auto cr = flCreateChannelEx(channel.c_str(), w, h, format, 3,
                              FL_MAP_CPU | FL_GPU_RENDER, &c);
  if (cr != FL_OK) {
    printf("SKIP allocation unsupported: %s\n", flResultString(cr));
    v.destroy();
    return 77;
  }
  vkr::GpuOutput output;
  output.configure({channel});
  VkImage image{};
  VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  im.imageType = VK_IMAGE_TYPE_2D;
  im.format = VK_FORMAT_R8G8B8A8_UNORM;
  im.extent = {64, 32, 1};
  im.mipLevels = im.arrayLayers = 1;
  im.samples = VK_SAMPLE_COUNT_1_BIT;
  im.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  CHECK(vkCreateImage(v.device, &im, nullptr, &image) == VK_SUCCESS);
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(v.device, image, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = __builtin_ctz(req.memoryTypeBits);
  VkDeviceMemory memory{};
  CHECK(vkAllocateMemory(v.device, &ai, nullptr, &memory) == VK_SUCCESS);
  CHECK(vkBindImageMemory(v.device, image, memory, 0) == VK_SUCCESS);
  VkCommandPool pool{};
  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pc.queueFamilyIndex = v.queueFamily;
  pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  CHECK(vkCreateCommandPool(v.device, &pc, nullptr, &pool) == VK_SUCCESS);
  VkCommandBuffer cmd{};
  VkCommandBufferAllocateInfo ca{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = pool;
  ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount = 1;
  CHECK(vkAllocateCommandBuffers(v.device, &ca, &cmd) == VK_SUCCESS);
  VkFence fence{};
  VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  CHECK(vkCreateFence(v.device, &fc, nullptr, &fence) == VK_SUCCESS);
  VkBuffer upload{};
  VkBufferCreateInfo ub{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ub.size = 64 * 32 * 4;
  ub.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  CHECK(vkCreateBuffer(v.device, &ub, nullptr, &upload) == VK_SUCCESS);
  vkGetBufferMemoryRequirements(v.device, upload, &req);
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(v.phys, &mp);
  ai.memoryTypeIndex = UINT32_MAX;
  for (unsigned i = 0; i < mp.memoryTypeCount; i++)
    if ((req.memoryTypeBits & (1u << i)) &&
        (mp.memoryTypes[i].propertyFlags &
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      ai.memoryTypeIndex = i;
      break;
    }
  CHECK(ai.memoryTypeIndex != UINT32_MAX);
  ai.allocationSize = req.size;
  VkDeviceMemory uploadMemory{};
  CHECK(vkAllocateMemory(v.device, &ai, nullptr, &uploadMemory) == VK_SUCCESS);
  CHECK(vkBindBufferMemory(v.device, upload, uploadMemory, 0) == VK_SUCCESS);
  void *pixels = nullptr;
  CHECK(vkMapMemory(v.device, uploadMemory, 0, VK_WHOLE_SIZE, 0, &pixels) ==
        VK_SUCCESS);
  unsigned received = 0;
  int worst = 0;
  for (unsigned frame = 0; frame < iterations; frame++) {
    CHECK(vkResetCommandBuffer(cmd, 0) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
    VkImageMemoryBarrier2 bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    bar.srcStageMask = bar.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    bar.srcAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.image = image;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &bar;
    vkCmdPipelineBarrier2(cmd, &dep);
    auto sourceRgb = [&](unsigned x, unsigned y, unsigned k) {
      return (int)((frame * (k == 0   ? 41
                             : k == 1 ? 73
                                      : 19) +
                    x * (k == 0   ? 17
                         : k == 1 ? 3
                                  : 11) +
                    y * (k == 0   ? 5
                         : k == 1 ? 23
                                  : 7)) %
                   256);
    };
    auto expectedRgb = [&](unsigned x, unsigned y, unsigned k) {
      return sourceRgb(((2 * x + 1) * 64) / (2 * w),
                       ((2 * y + 1) * 32) / (2 * h), k);
    };
    for (unsigned y = 0; y < 32; y++)
      for (unsigned x = 0; x < 64; x++) {
        auto *p = (unsigned char *)pixels + (y * 64 + x) * 4;
        for (unsigned k = 0; k < 3; k++)
          p[k] = sourceRgb(x, y, k);
        p[3] = 255;
      }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {64, 32, 1};
    vkCmdCopyBufferToImage(cmd, upload, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    output.record(v, frame % 2, cmd, image, {64, 32}, im.format,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, frame + 1);
    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
    flFrame unpublished{};
    auto before = flAcquireFrame(c, &unpublished, 0);
    CHECK(before == FL_TIMEOUT || before == FL_DISCONNECTED);
    if (frame == recreate) {
      flClose(c);
      c = nullptr;
      w += 4;
      h += 4;
      CHECK(flCreateChannelEx(channel.c_str(), w, h, format, 3,
                              FL_MAP_CPU | FL_GPU_RENDER, &c) == FL_OK);
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    CHECK(vkQueueSubmit(v.queue, 1, &submit, fence) == VK_SUCCESS);
    output.submitted(frame % 2);
    CHECK(vkWaitForFences(v.device, 1, &fence, VK_TRUE, 5000000000ull) ==
          VK_SUCCESS);
    vkResetFences(v.device, 1, &fence);
    output.retire(v, frame % 2);
    if (frame == recreate) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1100));
      continue;
    }
    flFrame f{};
    auto r = flAcquireFrame(c, &f, 100);
    if (r != FL_OK)
      continue;
    CHECK(f.ptsNs == frame + 1);
    flMappedImage mapped{};
    CHECK(flImageMapPlanes(f.image, &mapped) == FL_OK);
    // Copy mapped rows once. Repeated byte loads from uncached imported GPU
    // memory turn a 4K correctness check into a PCIe latency benchmark.
    std::vector<unsigned char> owned[2];
    for (unsigned plane = 0; plane < mapped.planes; plane++) {
      auto &m = mapped.plane[plane];
      owned[plane].resize((size_t)m.rows * m.rowBytes);
      for (unsigned row = 0; row < m.rows; row++)
        memcpy(owned[plane].data() + (size_t)row * m.rowBytes,
               (const unsigned char *)m.pixels + (size_t)row * m.stride,
               m.rowBytes);
      m.pixels = owned[plane].data();
      m.stride = m.rowBytes;
    }
    flImageUnmap(f.image);
    for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++) {
        int rgb[3] = {expectedRgb(x, y, 0), expectedRgb(x, y, 1),
                      expectedRgb(x, y, 2)};
        auto *p = (unsigned char *)mapped.plane[0].pixels +
                  y * mapped.plane[0].stride;
        if (format == FL_FORMAT_NV12) {
          int e = 16 + ((47 * rgb[0] + 157 * rgb[1] + 16 * rgb[2] + 128) >> 8);
          worst = std::max(worst, std::abs(e - p[x]));
          if (!(x & 1) && !(y & 1)) {
            auto *uv = (unsigned char *)mapped.plane[1].pixels +
                       (y / 2) * mapped.plane[1].stride + x;
            int sum[3]{};
            for (unsigned dy = 0; dy < 2; dy++)
              for (unsigned dx = 0; dx < 2; dx++)
                for (unsigned k = 0; k < 3; k++)
                  sum[k] += expectedRgb(x + dx, y + dy, k);
            int u = 128 +
                    ((-26 * sum[0] - 87 * sum[1] + 112 * sum[2] + 512) >> 10),
                vv = 128 +
                     ((112 * sum[0] - 102 * sum[1] - 10 * sum[2] + 512) >> 10);
            worst = std::max(worst, std::abs(u - uv[0]));
            worst = std::max(worst, std::abs(vv - uv[1]));
          }
        } else if (format == FL_FORMAT_RGB10A2) {
          uint32_t word;
          memcpy(&word, p + x * 4, 4);
          for (unsigned k = 0; k < 3; k++)
            worst = std::max(worst, std::abs((int)((word >> (10 * k)) & 1023) -
                                             rgb[k] * 1023 / 255));
        } else
          for (unsigned k = 0; k < 3; k++)
            worst = std::max(
                worst,
                std::abs(rgb[k] -
                         p[x * 4 + (format == FL_FORMAT_BGRA8 ? 2 - k : k)]));
      }
    CHECK(flRelease(c, &f) == FL_OK);
    received++;
  }
  vkUnmapMemory(v.device, uploadMemory);
  vkDestroyBuffer(v.device, upload, nullptr);
  vkFreeMemory(v.device, uploadMemory, nullptr);
  // A recorded-but-never-submitted output must not be published at shutdown.
  CHECK(vkResetCommandBuffer(cmd, 0) == VK_SUCCESS);
  VkCommandBufferBeginInfo abandoned{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(vkBeginCommandBuffer(cmd, &abandoned) == VK_SUCCESS);
  output.record(v, 0, cmd, image, {64, 32}, im.format,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 99999);
  CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
  output.destroy(v);
  flFrame unpublished{};
  auto closed = flAcquireFrame(c, &unpublished, 10);
  CHECK(closed == FL_DISCONNECTED || closed == FL_TIMEOUT);
  flClose(c);
  vkDestroyFence(v.device, fence, nullptr);
  vkDestroyCommandPool(v.device, pool, nullptr);
  vkDestroyImage(v.device, image, nullptr);
  vkFreeMemory(v.device, memory, nullptr);
  v.destroy();
  printf("format=%u frames=%u worst_byte_error=%d\n", format, received, worst);
  if (!received)
    return 77;
  return received == iterations - 1 && worst <= 1 ? 0 : 1;
}
