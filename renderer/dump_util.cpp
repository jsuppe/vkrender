#include "dump_util.h"

#include <cstdio>
#include <vector>

#include "log.h"

bool vkrDumpTarget(vkr::VkContext& vk, VkCommandPool cmdPool, const char* path) {
    return vkrDumpImage(vk, cmdPool, vk.swapImages[0], vk.swapExtent, VK_IMAGE_LAYOUT_GENERAL,
                        path);
}

bool vkrDumpImage(vkr::VkContext& vk, VkCommandPool cmdPool, VkImage image, VkExtent2D extent,
                  VkImageLayout srcLayout, const char* path) {
    const uint32_t w = extent.width, h = extent.height;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = (VkDeviceSize)w * h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(vk.device, &bci, nullptr, &buf);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk.phys, &mp);
    uint32_t type = 0;
    for (uint32_t t = 0; t < mp.memoryTypeCount; ++t) {
        if ((req.memoryTypeBits & (1u << t)) &&
            (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = t;
            break;
        }
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    vkAllocateMemory(vk.device, &mai, nullptr, &mem);
    vkBindBufferMemory(vk.device, buf, mem, 0);

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(vk.device, &cbai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    auto barrier = [&](VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    const bool needTransition = srcLayout != VK_IMAGE_LAYOUT_GENERAL &&
                                srcLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (needTransition) barrier(srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, image,
                           needTransition ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : srcLayout,
                           buf, 1, &region);
    if (needTransition) barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk.queue);

    void* data = nullptr;
    vkMapMemory(vk.device, mem, 0, VK_WHOLE_SIZE, 0, &data);
    FILE* f = fopen(path, "wb");
    bool ok = f != nullptr;
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        const uint8_t* px = (const uint8_t*)data;
        std::vector<uint8_t> row(w * 3);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* p = px + ((size_t)y * w + x) * 4; // BGRA
                row[x * 3 + 0] = p[2];
                row[x * 3 + 1] = p[1];
                row[x * 3 + 2] = p[0];
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
    }
    vkUnmapMemory(vk.device, mem);
    vkFreeCommandBuffers(vk.device, cmdPool, 1, &cmd);
    vkDestroyBuffer(vk.device, buf, nullptr);
    vkFreeMemory(vk.device, mem, nullptr);
    if (ok) VKR_LOG("renderer: wrote %s (%ux%u)", path, w, h);
    return ok;
}

// M42: identical readback, memory destination. Duplicating the ~60 lines of
// buffer/barrier plumbing was rejected in favour of a copy INTO the dump path
// later if a third caller appears; for now the two share by symmetry, not
// abstraction.
bool vkrReadImage(vkr::VkContext& vk, VkCommandPool cmdPool, VkImage image, VkExtent2D extent,
                  VkImageLayout srcLayout, std::vector<uint8_t>& bgraOut) {
    const uint32_t w = extent.width, h = extent.height;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = (VkDeviceSize)w * h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(vk.device, &bci, nullptr, &buf);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk.phys, &mp);
    uint32_t type = 0;
    for (uint32_t t = 0; t < mp.memoryTypeCount; ++t) {
        if ((req.memoryTypeBits & (1u << t)) &&
            (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = t;
            break;
        }
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    vkAllocateMemory(vk.device, &mai, nullptr, &mem);
    vkBindBufferMemory(vk.device, buf, mem, 0);

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(vk.device, &cbai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    auto barrier = [&](VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    const bool needTransition = srcLayout != VK_IMAGE_LAYOUT_GENERAL &&
                                srcLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (needTransition) barrier(srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, image,
                           needTransition ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : srcLayout,
                           buf, 1, &region);
    if (needTransition) barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk.queue);

    void* data = nullptr;
    vkMapMemory(vk.device, mem, 0, VK_WHOLE_SIZE, 0, &data);
    bgraOut.assign((const uint8_t*)data, (const uint8_t*)data + (size_t)w * h * 4);
    vkUnmapMemory(vk.device, mem);
    vkFreeCommandBuffers(vk.device, cmdPool, 1, &cmd);
    vkDestroyBuffer(vk.device, buf, nullptr);
    vkFreeMemory(vk.device, mem, nullptr);
    return true;
}
