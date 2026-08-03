#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <optional>
#include <string>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <volk.h>

#include "external_video_image.h"
#include "ipc.h"
#include "log.h"
#include "video_pass.h"
#include "vk_context.h"

// vkrenderer [--pipe vkrender]
//
// Process 2 of the zero-copy pipeline: imports the decoder's shared GPU
// textures and renders the video. Never copies pixel data - it samples the
// exact memory the decoder wrote.

namespace {

std::string exeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
    size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? "." : path.substr(0, pos);
}

// Debug: copy a corner of the imported luma plane to a host buffer and dump
// bytes. Distinguishes "imported memory is unmapped -> any GPU access faults"
// from "shader sampling path specifically faults".
bool debugCopyReadback(vkr::VkContext& vk, VkCommandPool cmdPool,
                       const vkr::ExternalVideoPool& pool, int slot) {
    const uint32_t kW = 64, kH = 4;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = kW * kH;
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
    // Bottom-right corner (single-plane pools only).
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {(int32_t)(pool.width - kW / 4), (int32_t)(pool.height - kH), 0};
    region.imageExtent = {kW / 4, kH, 1};
    vkCmdCopyImageToBuffer(cmd, pool.slots[slot].image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &region);
    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(vk.device, &fci, nullptr, &fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult sr = vkQueueSubmit(vk.queue, 1, &si, fence);
    VkResult wr = sr == VK_SUCCESS
                      ? vkWaitForFences(vk.device, 1, &fence, VK_TRUE, 2000000000ull)
                      : VK_ERROR_UNKNOWN;
    bool ok = sr == VK_SUCCESS && wr == VK_SUCCESS;
    if (ok) {
        void* data = nullptr;
        vkMapMemory(vk.device, mem, 0, VK_WHOLE_SIZE, 0, &data);
        const uint8_t* p = (const uint8_t*)data;
        VKR_LOG("debug copy test OK, luma bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        vkUnmapMemory(vk.device, mem);
    } else {
        VKR_ERR("debug copy test FAILED: submit=%d wait=%d", sr, wr);
    }
    vkDestroyFence(vk.device, fence, nullptr);
    vkFreeCommandBuffers(vk.device, cmdPool, 1, &cmd);
    vkDestroyBuffer(vk.device, buf, nullptr);
    vkFreeMemory(vk.device, mem, nullptr);
    return ok;
}

struct FrameSync {
    VkSemaphore acquireSem = VK_NULL_HANDLE;
    VkSemaphore renderDoneSem = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

} // namespace

int main(int argc, char** argv) {
    std::string pipe = "vkrender";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--pipe") && i + 1 < argc) pipe = argv[++i];
    }
    // Debug bisection switches
    const bool dbgNoTimelineWait = GetEnvironmentVariableA("VKR_NO_TIMELINE_WAIT", nullptr, 0) > 0;
    const bool dbgNoVideoDraw = GetEnvironmentVariableA("VKR_NO_VIDEO_DRAW", nullptr, 0) > 0;
    if (dbgNoTimelineWait) VKR_WARN("debug: timeline wait disabled");
    if (dbgNoVideoDraw) VKR_WARN("debug: video draw disabled");

    vkr::VkContext vk;
    if (!vk.init(1280, 720, "vkrender - waiting for stream")) return 1;

    vkr::VideoPass pass;
    if (!pass.init(vk, exeDir() + "/shaders")) return 1;

    vkr::IpcChannel ch = vkr::IpcChannel::connect(pipe, 30000);
    if (!ch.connected()) {
        VKR_ERR("could not reach vkdecoder on pipe '%s' (start it first)", pipe.c_str());
        return 1;
    }
    vkr::proto::HelloMsg hello{};
    hello.rendererPid = GetCurrentProcessId();
    hello.luidValid = vk.luidValid ? 1 : 0;
    memcpy(hello.deviceLUID, vk.deviceLUID, sizeof(hello.deviceLUID));
    if (!ch.send(vkr::proto::MsgType::Hello, hello)) return 1;

    // Per-frame-in-flight sync objects
    constexpr uint32_t kFramesInFlight = 2;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = vk.queueFamily;
    vkCreateCommandPool(vk.device, &cpci, nullptr, &cmdPool);

    FrameSync frames[kFramesInFlight];
    for (auto& f : frames) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(vk.device, &sci, nullptr, &f.acquireSem);
        vkCreateSemaphore(vk.device, &sci, nullptr, &f.renderDoneSem);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(vk.device, &fci, nullptr, &f.inFlight);
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        vkAllocateCommandBuffers(vk.device, &cbai, &f.cmd);
    }

    vkr::ExternalVideoPool pool;
    int currentSlot = -1;
    uint64_t currentReady = 0;        // readyValue of the frame we display
    uint64_t lastConsumedSignal = 0;  // last value signaled on consumedSem
    std::optional<uint64_t> pendingConsumedSignal;
    uint64_t frameIndex = 0;
    bool decoderGone = false;

    while (!glfwWindowShouldClose(vk.window)) {
        glfwPollEvents();

        // Drain IPC; frame messages coalesce (latest wins).
        std::optional<vkr::proto::FrameReadyMsg> newest;
        while (auto msg = ch.tryRecv()) {
            switch (msg->type) {
                case vkr::proto::MsgType::PoolDesc: {
                    if (const auto* d = msg->as<vkr::proto::PoolDescMsg>()) {
                        vkDeviceWaitIdle(vk.device);
                        pass.unbindPool(vk);
                        pool.destroy(vk);
                        currentSlot = -1;
                        currentReady = 0;
                        lastConsumedSignal = 0;
                        pendingConsumedSignal.reset();
                        newest.reset();
                        if (!pool.create(vk, *d) || !pass.bindPool(vk, pool)) {
                            VKR_ERR("failed to import shared pool");
                            return 1;
                        }
                        glfwSetWindowTitle(vk.window, "vkrender - zero-copy stream");
                    }
                    break;
                }
                case vkr::proto::MsgType::FrameReady:
                    if (const auto* fr = msg->as<vkr::proto::FrameReadyMsg>()) newest = *fr;
                    break;
                case vkr::proto::MsgType::StreamReset:
                    // Old pool must not be touched anymore; wait for the new PoolDesc.
                    vkDeviceWaitIdle(vk.device);
                    pass.unbindPool(vk);
                    pool.destroy(vk);
                    currentSlot = -1;
                    newest.reset();
                    break;
                case vkr::proto::MsgType::Bye:
                    VKR_LOG("renderer: decoder said goodbye");
                    break;
                default:
                    break;
            }
        }
        if (!ch.connected() && !decoderGone) {
            decoderGone = true;
            glfwSetWindowTitle(vk.window, "vkrender - stream ended (showing last frame)");
        }

        // Adopt the newest decoded frame. Signaling consumed = V promises we
        // will never again read any frame with readyValue <= V, which frees
        // every pool slot except the one we are adopting.
        if (newest && pool.valid()) {
            static bool copyTested = false;
            if (!copyTested && GetEnvironmentVariableA("VKR_DEBUG_COPY_TEST", nullptr, 0) > 0) {
                copyTested = true;
                debugCopyReadback(vk, cmdPool, pool, (int)newest->slot);
            }
            currentSlot = (int)newest->slot;
            currentReady = newest->readyValue;
            if (currentReady > 0 && currentReady - 1 > lastConsumedSignal)
                pendingConsumedSignal = currentReady - 1;
        }

        FrameSync& f = frames[frameIndex % kFramesInFlight];
        ++frameIndex;
        vkWaitForFences(vk.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        VkResult ar = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, f.acquireSem,
                                            VK_NULL_HANDLE, &imageIndex);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
            if (!vk.recreateSwapchain()) {
                glfwWaitEventsTimeout(0.1); // minimized
                continue;
            }
            if (ar == VK_ERROR_OUT_OF_DATE_KHR) continue;
        } else if (ar != VK_SUCCESS) {
            VKR_ERR("vkAcquireNextImageKHR failed: %d", ar);
            break;
        }
        vkResetFences(vk.device, 1, &f.inFlight);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &bi);

        // swapchain image: UNDEFINED -> COLOR_ATTACHMENT
        VkImageMemoryBarrier2 toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toColor.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.image = vk.swapImages[imageIndex];
        toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toColor;
        vkCmdPipelineBarrier2(f.cmd, &dep);

        pass.record(f.cmd, pool, (pool.valid() && !dbgNoVideoDraw) ? currentSlot : -1,
                    vk.swapViews[imageIndex], vk.swapExtent, vk.queueFamily);

        VkImageMemoryBarrier2 toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toPresent.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.image = vk.swapImages[imageIndex];
        toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dep.pImageMemoryBarriers = &toPresent;
        vkCmdPipelineBarrier2(f.cmd, &dep);

        vkEndCommandBuffer(f.cmd);

        // Submit: wait for the swapchain image AND (if showing video) for the
        // decoder's ready fence to pass the frame's value. Optionally signal
        // the consumed fence to release older slots back to the decoder.
        VkSemaphore waitSems[2] = {f.acquireSem, VK_NULL_HANDLE};
        VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
        uint64_t waitValues[2] = {0, 0};
        uint32_t waitCount = 1;
        if (pool.valid() && currentSlot >= 0 && !dbgNoTimelineWait) {
            waitSems[waitCount] = pool.readySem;
            waitValues[waitCount] = currentReady;
            ++waitCount;
        }
        VkSemaphore signalSems[2] = {f.renderDoneSem, VK_NULL_HANDLE};
        uint64_t signalValues[2] = {0, 0};
        uint32_t signalCount = 1;
        if (pool.valid() && pendingConsumedSignal) {
            signalSems[signalCount] = pool.consumedSem;
            signalValues[signalCount] = *pendingConsumedSignal;
            lastConsumedSignal = *pendingConsumedSignal;
            pendingConsumedSignal.reset();
            ++signalCount;
        }

        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = waitCount;
        timeline.pWaitSemaphoreValues = waitValues;
        timeline.signalSemaphoreValueCount = signalCount;
        timeline.pSignalSemaphoreValues = signalValues;

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.pNext = &timeline;
        si.waitSemaphoreCount = waitCount;
        si.pWaitSemaphores = waitSems;
        si.pWaitDstStageMask = waitStages;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        si.signalSemaphoreCount = signalCount;
        si.pSignalSemaphores = signalSems;
        VkResult sr = vkQueueSubmit(vk.queue, 1, &si, f.inFlight);
        if (sr != VK_SUCCESS) {
            VKR_ERR("vkQueueSubmit failed: %d (slot=%d ready=%llu waits=%u signals=%u)", sr,
                    currentSlot, (unsigned long long)currentReady, waitCount, signalCount);
            break;
        }

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &f.renderDoneSem;
        pi.swapchainCount = 1;
        pi.pSwapchains = &vk.swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(vk.queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) vk.recreateSwapchain();
    }

    if (ch.connected()) ch.send(vkr::proto::MsgType::Bye);

    vkDeviceWaitIdle(vk.device);
    for (auto& f : frames) {
        vkDestroySemaphore(vk.device, f.acquireSem, nullptr);
        vkDestroySemaphore(vk.device, f.renderDoneSem, nullptr);
        vkDestroyFence(vk.device, f.inFlight, nullptr);
    }
    vkDestroyCommandPool(vk.device, cmdPool, nullptr);
    pass.destroy(vk);
    pool.destroy(vk);
    vk.destroy();
    return 0;
}
