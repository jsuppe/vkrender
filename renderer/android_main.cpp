// NativeActivity entry for the vkrender renderer on Android.
//
// The activity (see android/app/src/main/java) starts the DecoderService in
// its own process, then this native loop connects to the decoder over an
// abstract-namespace Unix socket, imports the AHardwareBuffers it announces,
// and renders them zero-copy - the same pipeline as desktop with the GLFW
// loop replaced by android_native_app_glue and AHB import instead of
// dma-buf/NT-handle import.

#ifdef __ANDROID__

#include <android/asset_manager.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>

#include <volk.h>

#include "external_video_image.h"
#include "ipc.h"
#include "log.h"
#include "video_pass.h"
#include "vk_context.h"

namespace {

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The .spv shaders ship as APK assets; VideoPass loads from the filesystem,
// so extract them to the app's internal files dir once at startup.
bool extractShaders(android_app* app, const std::string& outDir) {
    const char* names[] = {"fullscreen.vert.spv", "video_ycbcr.frag.spv", "nv12_planar.frag.spv"};
    for (const char* name : names) {
        std::string assetPath = std::string("shaders/") + name;
        AAsset* asset =
            AAssetManager_open(app->activity->assetManager, assetPath.c_str(), AASSET_MODE_BUFFER);
        if (!asset) {
            VKR_ERR("missing asset %s", assetPath.c_str());
            return false;
        }
        std::string outPath = outDir + "/" + name;
        FILE* f = fopen(outPath.c_str(), "wb");
        if (!f) {
            AAsset_close(asset);
            return false;
        }
        fwrite(AAsset_getBuffer(asset), 1, (size_t)AAsset_getLength(asset), f);
        fclose(f);
        AAsset_close(asset);
    }
    return true;
}

struct FrameSync {
    VkSemaphore acquireSem = VK_NULL_HANDLE;
    VkSemaphore renderDoneSem = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::optional<uint64_t> pendingConsumed;
};

struct RendererState {
    bool windowReady = false;
    bool destroyed = false;
};

void onAppCmd(android_app* app, int32_t cmd) {
    auto* state = (RendererState*)app->userData;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: state->windowReady = app->window != nullptr; break;
        case APP_CMD_TERM_WINDOW: state->windowReady = false; break;
        case APP_CMD_DESTROY: state->destroyed = true; break;
    }
}

} // namespace

void android_main(android_app* app) {
    RendererState state;
    app->userData = &state;
    app->onAppCmd = onAppCmd;

    // Wait for the window before any Vulkan work.
    while (!state.windowReady && !state.destroyed) {
        android_poll_source* source = nullptr;
        int events = 0;
        if (ALooper_pollOnce(100, nullptr, &events, (void**)&source) >= 0 && source)
            source->process(app, source);
    }
    if (state.destroyed) return;

    std::string filesDir = app->activity->internalDataPath;
    if (!extractShaders(app, filesDir)) return;

    vkr::VkContext vk;
    vk.nativeWindow = app->window;
    if (!vk.init(0, 0, "vkrender", false)) {
        VKR_ERR("Vulkan init failed");
        return;
    }
    vkr::VideoPass pass;
    if (!pass.init(vk, filesDir)) return;

    // The decoder service may still be starting; keep dialing.
    vkr::IpcChannel ch;
    auto sendHello = [&](vkr::IpcChannel& c) {
        vkr::proto::HelloMsg hello{};
        hello.rendererPid = (uint32_t)getpid();
        return c.send(vkr::proto::MsgType::Hello, hello);
    };

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
    bool passNeedsBind = false;
    int64_t currentSlot = -1;
    uint64_t currentReady = 0;
    uint64_t lastConsumedSignal = 0;
    std::optional<uint64_t> pendingConsumedSignal;
    uint64_t frameIndex = 0;
    int64_t lastDialNs = 0;

    auto adoptFrame = [&](const vkr::proto::FrameReadyMsg& fr) {
        currentSlot = (int64_t)fr.slot;
        currentReady = fr.readyValue;
        if (currentReady > 0 && currentReady - 1 > lastConsumedSignal)
            pendingConsumedSignal = currentReady - 1;
    };

    while (!state.destroyed) {
        // Android event pump
        android_poll_source* source = nullptr;
        int events = 0;
        while (ALooper_pollOnce(0, nullptr, &events, (void**)&source) >= 0 && source)
            source->process(app, source);
        if (state.destroyed) break;
        if (!state.windowReady) continue;

        // (Re)dial the decoder service.
        if (!ch.connected() && nowNs() - lastDialNs > 1000000000ll) {
            lastDialNs = nowNs();
            vkr::IpcChannel next = vkr::IpcChannel::connect("vkrender", 1, /*quiet=*/true);
            if (next.connected() && sendHello(next)) {
                ch = std::move(next);
                VKR_LOG("renderer: connected to decoder service");
            }
        }

        // Message pump: latest-frame-wins.
        std::optional<vkr::proto::FrameReadyMsg> newest;
        while (auto msg = ch.tryRecv()) {
            switch (msg->type) {
                case vkr::proto::MsgType::PoolDesc:
                    if (const auto* d = msg->as<vkr::proto::PoolDescMsg>()) {
                        vkDeviceWaitIdle(vk.device);
                        pass.unbindPool(vk);
                        pool.destroy(vk);
                        currentSlot = -1;
                        currentReady = 0;
                        lastConsumedSignal = 0;
                        pendingConsumedSignal.reset();
                        newest.reset();
                        if (!pool.create(vk, *d)) return;
                        passNeedsBind = true;
                    }
                    break;
                case vkr::proto::MsgType::SurfaceDesc:
                    if (const auto* d = msg->as<vkr::proto::SurfaceDescMsg>()) {
                        // numObjects == 0: the AHardwareBuffer follows
                        // out-of-band on this socket.
                        AHardwareBuffer* ahb = nullptr;
                        if (d->numObjects == 0 &&
                            AHardwareBuffer_recvHandleFromUnixSocket(ch.nativeSocket(), &ahb) == 0 &&
                            ahb) {
                            bool ok = pool.addSurfaceAHB(vk, d->slotKey, ahb);
                            AHardwareBuffer_release(ahb); // pool holds its own ref via import
                            if (ok) {
                                ok = passNeedsBind ? pass.bindPool(vk, pool)
                                                   : pass.addSlot(vk, pool, d->slotKey);
                                passNeedsBind = false;
                            }
                            if (!ok) VKR_ERR("failed to bind surface %u", d->slotKey);
                        } else {
                            VKR_ERR("failed to receive AHardwareBuffer for surface %u", d->slotKey);
                        }
                    }
                    break;
                case vkr::proto::MsgType::FrameReady:
                    if (const auto* fr = msg->as<vkr::proto::FrameReadyMsg>()) newest = *fr;
                    break;
                case vkr::proto::MsgType::StreamReset:
                    vkDeviceWaitIdle(vk.device);
                    pass.unbindPool(vk);
                    pool.destroy(vk);
                    currentSlot = -1;
                    newest.reset();
                    break;
                default:
                    break;
            }
        }
        if (newest && pool.valid()) adoptFrame(*newest);

        // Render
        FrameSync& f = frames[frameIndex % kFramesInFlight];
        ++frameIndex;
        vkWaitForFences(vk.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
        if (f.pendingConsumed) {
            vkr::proto::FrameConsumedMsg fc{};
            fc.readyValue = *f.pendingConsumed;
            if (ch.connected()) ch.send(vkr::proto::MsgType::FrameConsumed, fc);
            f.pendingConsumed.reset();
        }

        uint32_t imageIndex = 0;
        VkResult ar = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, f.acquireSem,
                                            VK_NULL_HANDLE, &imageIndex);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
            vk.recreateSwapchain();
            continue;
        }
        if (ar != VK_SUCCESS) continue;
        vkResetFences(vk.device, 1, &f.inFlight);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &bi);

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

        pass.record(f.cmd, pool, pool.valid() ? currentSlot : -1, vk.swapViews[imageIndex],
                    vk.swapExtent, vk.queueFamily);

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

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore signalSem = f.renderDoneSem;
        if (pendingConsumedSignal) {
            f.pendingConsumed = *pendingConsumedSignal;
            lastConsumedSignal = *pendingConsumedSignal;
            pendingConsumedSignal.reset();
        }
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquireSem;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signalSem;
        if (vkQueueSubmit(vk.queue, 1, &si, f.inFlight) != VK_SUCCESS) break;

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
}

#endif // __ANDROID__
