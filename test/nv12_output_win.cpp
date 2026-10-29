#include "external_video_image.h"
#include "framelink.h"
#include "framelink_d3d11.h"
#include "nv12_output.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
int main(int argc, char **argv) {
  CHECK(argc == 4);
  uint32_t w = atoi(argv[2]), h = atoi(argv[3]);
  vkr::VkContext vk;
  CHECK(vk.init(64, 32, "NV12 GPU verification", true));
  char name[80];
  snprintf(name, sizeof(name), "nv12-gpu-%lu", GetCurrentProcessId());
  flChannel *c = nullptr, *p = nullptr;
  CHECK(flCreateChannelOnAdapter(name, w, h, FL_FORMAT_NV12, 2, 0,
                                 vk.deviceLUID, &c) == FL_OK);
  for (int i = 0; i < 100; i++) {
    if (flOpenProducer(name, &p) == FL_OK)
      break;
    Sleep(10);
  }
  CHECK(p);

  VkCommandPool pool{};
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.queueFamilyIndex = vk.queueFamily;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  CHECK(vkCreateCommandPool(vk.device, &pci, nullptr, &pool) == VK_SUCCESS);
  VkCommandBuffer cmd{};
  VkCommandBufferAllocateInfo ca{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = pool;
  ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount = 1;
  CHECK(vkAllocateCommandBuffers(vk.device, &ca, &cmd) == VK_SUCCESS);
  vkr::Nv12Output converters[2];
  ComPtr<ID3D11DeviceContext> ctx;
  flChannelD3D11Device(c)->GetImmediateContext(&ctx);
  ComPtr<ID3D11Texture2D> staging;
  const float colors[][3] = {{0, 0, 0}, {1, 1, 1}, {1, 0, 0},
                             {0, 1, 0}, {0, 0, 1}, {.25f, .5f, .75f},
                             {0, 1, 1}, {1, 1, 0}};
  for (unsigned n = 0; n < 24; n++) {
    flBuffer b{};
    CHECK(flAcquireBuffer(p, &b, 2000) == FL_OK);
    unsigned slot = b.slot;
    if (!converters[slot].image)
      CHECK(converters[slot].init(vk, flChannelD3D11Device(p),
                                  flImageD3D11(b.image), w, h));
    CHECK(vkResetCommandBuffer(cmd, 0) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
    VkImageMemoryBarrier2 bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    bar.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    bar.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    bar.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = vk.swapImages[0];
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &bar;
    vkCmdPipelineBarrier2(cmd, &dep);
    auto color = colors[n % 8];
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = vk.swapViews[0];
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {{color[0], color[1], color[2], 1}};
    VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};
    render.renderArea.extent = vk.swapExtent;
    render.layerCount = 1;
    render.colorAttachmentCount = 1;
    render.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &render);
    bool checker = w == 64 && h == 32 && n >= 16;
    if (checker) {
      const float pattern[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}};
      for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 64; x++) {
          auto rgb = pattern[(y & 1) * 2 + (x & 1)];
          VkClearAttachment clear{};
          clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          clear.clearValue.color = {{rgb[0], rgb[1], rgb[2], 1}};
          VkClearRect rect{{{(int)x, (int)y}, {1, 1}}, 0, 1};
          vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
        }
    }
    vkCmdEndRendering(cmd);
    bar.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    bar.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    bar.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    bar.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkImageMemoryBarrier2 dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    dst.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    dst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    dst.dstQueueFamilyIndex = vk.queueFamily;
    dst.image = converters[slot].image;
    dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dep.pImageMemoryBarriers = &dst;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkImageBlit region{};
    region.srcSubresource =
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {64, 32, 1};
    region.dstOffsets[1] = {(int)w, (int)h, 1};
    vkCmdBlitImage(cmd, vk.swapImages[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   converters[slot].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_LINEAR);
    dst.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    dst.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    dst.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    dst.dstAccessMask = 0;
    dst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dst.srcQueueFamilyIndex = vk.queueFamily;
    dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
    uint64_t value = n + 1;
    VkTimelineSemaphoreSubmitInfo timeline{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.signalSemaphoreValueCount = 1;
    timeline.pSignalSemaphoreValues = &value;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &timeline;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &converters[slot].inputReady;
    CHECK(vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    CHECK(converters[slot].finish(value));
    CHECK(flSubmit(p, &b, n) == FL_OK);
    flFrame f{};
    CHECK(flAcquireFrame(c, &f, 3000) == FL_OK);
    CHECK(f.ptsNs == n);
    auto *tex = flImageD3D11(f.image);
    if (!staging) {
      D3D11_TEXTURE2D_DESC d{};
      tex->GetDesc(&d);
      d.Usage = D3D11_USAGE_STAGING;
      d.BindFlags = 0;
      d.MiscFlags = 0;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(SUCCEEDED(
          flChannelD3D11Device(c)->CreateTexture2D(&d, nullptr, &staging)));
    }
    ctx->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CHECK(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    double r = round(color[0] * 255) / 255, g = round(color[1] * 255) / 255,
           blue = round(color[2] * 255) / 255;
    int expected[3] = {
        (int)round(16 + 219 * (.2126 * r + .7152 * g + .0722 * blue)),
        (int)round(128 + 224 * (-.114572 * r - .385428 * g + .5 * blue)),
        (int)round(128 + 224 * (.5 * r - .454153 * g - .045847 * blue))};
    for (unsigned plane = 0; plane < 2; plane++)
      for (unsigned y = 0; y < (plane ? h / 2 : h); y++)
        for (unsigned x = 0; x < w; x++) {
          int got =
              ((unsigned char *)
                   mapped.pData)[(plane ? (size_t)mapped.RowPitch * h : 0) +
                                 (size_t)y * mapped.RowPitch + x];
          int want = expected[plane ? 1 + (x & 1) : 0];
          if (checker) {
            const double luma[4] = {.2126, .7152, .0722, 1};
            want = plane ? 128
                         : (int)round(16 + 219 * luma[(y & 1) * 2 + (x & 1)]);
          }
          if (abs(got - want) > 1) {
            fprintf(stderr, "frame%u plane%u (%u,%u): got%d expected%d\n", n,
                    plane, x, y, got, want);
            return 1;
          }
        }
    ctx->Unmap(staging.Get(), 0);
    CHECK(flRelease(c, &f) == FL_OK);
    CHECK(vkQueueWaitIdle(vk.queue) == VK_SUCCESS);
  }
  vkDeviceWaitIdle(vk.device);
  for (int i = 0; i < 2; i++) {
    converters[i].destroy(vk);
  }
  vkDestroyCommandPool(vk.device, pool, nullptr);
  flClose(p);
  flClose(c);
  vk.destroy();
  puts("24 GPU RGB -> FrameLink NV12 frames: BT.709 limited Y/UV bytes "
       "verified");
}
