// NativeActivity entry for the vkrender renderer on Android - M8 architecture.
//
// The renderer OWNS an AImageReader; its Surface is handed to producers (the
// DecoderService, in another process) over Binder via RendererService.
// BufferQueue does all frame transport: queueing, fences, timestamps,
// backpressure. This loop just acquires the latest ready image each display
// vsync, imports its AHardwareBuffer into Vulkan once per distinct buffer
// (they recycle), samples it zero-copy, and defers each image's release
// until the GPU work that sampled it has provably retired.

#ifdef __ANDROID__

#include <android/asset_manager.h>
#include <android/choreographer.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <volk.h>

#include "external_video_image.h"
#include "log.h"
#include "video_pass.h"
#include "vk_context.h"

namespace {

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---- Surface bridge to RendererService (Binder thread) ---------------------
std::mutex g_readerMutex;
std::condition_variable g_readerCv;
AImageReader* g_reader = nullptr;
ANativeWindow* g_readerWindow = nullptr;

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
    // Images whose last GPU read is fenced by this frame; deleted (returned
    // to the BufferQueue) once the fence is waited.
    std::vector<AImage*> retiring;
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

// Called from RendererService's binder thread (decoder_jni.cpp).
ANativeWindow* vkrGetRendererInputWindow() {
    std::unique_lock<std::mutex> lock(g_readerMutex);
    g_readerCv.wait_for(lock, std::chrono::seconds(5), [] { return g_readerWindow != nullptr; });
    return g_readerWindow;
}

void android_main(android_app* app) {
    RendererState state;
    app->userData = &state;
    app->onAppCmd = onAppCmd;

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

    // The producer-facing input: created up-front at a nominal size (the
    // producer overrides buffer dimensions; per-image sizes are authoritative).
    {
        std::lock_guard<std::mutex> lock(g_readerMutex);
        media_status_t ms = AImageReader_newWithUsage(1920, 1080, AIMAGE_FORMAT_PRIVATE,
                                                      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 6,
                                                      &g_reader);
        if (ms != AMEDIA_OK || !g_reader) {
            VKR_ERR("AImageReader_newWithUsage failed: %d", ms);
            return;
        }
        AImageReader_getWindow(g_reader, &g_readerWindow);
    }
    g_readerCv.notify_all();
    VKR_LOG("renderer: input surface ready (BufferQueue)");

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
    bool passBound = false;
    int64_t currentSlot = -1;
    AImage* currentImage = nullptr; // the image currentSlot samples from
    uint64_t frameIndex = 0;
    ANativeWindow* vkWindow = app->window;
    uint64_t adoptions = 0, renders = 0;
    int64_t rateWindowStart = nowNs();

    auto keyForBuffer = [](AHardwareBuffer* ahb) {
        uint64_t v = (uint64_t)(uintptr_t)ahb;
        return (uint32_t)(v ^ (v >> 32));
    };

    AChoreographer* choreographer = AChoreographer_getInstance();
    bool frameDue = false;

    while (!state.destroyed) {
        // Vsync-paced event pump (see M7.1 notes: unpaced loops free-run).
        if (state.windowReady && choreographer) {
            frameDue = false;
            AChoreographer_postFrameCallback64(
                choreographer, [](int64_t, void* data) { *(bool*)data = true; }, &frameDue);
            while (!frameDue && !state.destroyed) {
                android_poll_source* source = nullptr;
                int events = 0;
                if (ALooper_pollOnce(50, nullptr, &events, (void**)&source) >= 0 && source)
                    source->process(app, source);
            }
        } else {
            android_poll_source* source = nullptr;
            int events = 0;
            while (ALooper_pollOnce(100, nullptr, &events, (void**)&source) >= 0 && source)
                source->process(app, source);
        }
        if (state.destroyed) break;

        if (state.windowReady && app->window != vkWindow) {
            VKR_LOG("renderer: window replaced, recreating surface");
            if (!vk.recreateSurface(app->window)) break;
            vkWindow = app->window;
        }
        if (!state.windowReady) continue;

        // Latest ready frame from the BufferQueue; older queued frames are
        // discarded by acquireLatestImage itself (latest-wins, fences
        // respected by the sync acquire).
        AImage* image = nullptr;
        if (AImageReader_acquireLatestImage(g_reader, &image) == AMEDIA_OK && image) {
            AHardwareBuffer* ahb = nullptr;
            if (AImage_getHardwareBuffer(image, &ahb) == AMEDIA_OK && ahb) {
                if (!pool.headerValid) {
                    int32_t w = 0, h = 0;
                    AImage_getWidth(image, &w);
                    AImage_getHeight(image, &h);
                    vkr::proto::PoolDescMsg header{};
                    header.width = (uint32_t)w & ~1u;
                    header.height = (uint32_t)h & ~1u;
                    header.format = vkr::proto::SharedFormat::NV12;
                    header.matrix = h >= 720 ? vkr::proto::Matrix::BT709 : vkr::proto::Matrix::BT601;
                    header.range = vkr::proto::Range::Limited;
                    header.planeCount = 2;
                    if (!pool.create(vk, header)) break;
                }
                uint32_t key = keyForBuffer(ahb);
                bool ok = pool.find(key) != nullptr || pool.addSurfaceAHB(vk, key, ahb);
                if (ok && !passBound) {
                    ok = pass.bindPool(vk, pool);
                    passBound = ok;
                } else if (ok) {
                    ok = pass.addSlot(vk, pool, key);
                }
                if (ok) {
                    // Adopt: previous image retires behind this frame's fence.
                    if (currentImage)
                        frames[frameIndex % kFramesInFlight].retiring.push_back(currentImage);
                    currentImage = image;
                    currentSlot = (int64_t)key;
                    ++adoptions;
                    image = nullptr; // ownership moved to currentImage
                }
            }
            if (image) AImage_delete(image); // import failed: return the buffer
        }

        FrameSync& f = frames[frameIndex % kFramesInFlight];
        ++frameIndex;
        vkWaitForFences(vk.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
        // Fence retired: every submission recorded before it is done; images
        // parked on this frame can go back to the BufferQueue.
        for (AImage* retired : f.retiring) AImage_delete(retired);
        f.retiring.clear();

        uint32_t imageIndex = 0;
        VkResult ar = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, f.acquireSem,
                                            VK_NULL_HANDLE, &imageIndex);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
            vk.recreateSwapchain();
            continue;
        }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) continue;
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

        pass.record(f.cmd, pool, passBound ? currentSlot : -1, vk.swapViews[imageIndex],
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
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquireSem;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &f.renderDoneSem;
        if (vkQueueSubmit(vk.queue, 1, &si, f.inFlight) != VK_SUCCESS) break;

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &f.renderDoneSem;
        pi.swapchainCount = 1;
        pi.pSwapchains = &vk.swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(vk.queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR) vk.recreateSwapchain();

        ++renders;
        if (nowNs() - rateWindowStart > 5000000000ll) {
            VKR_LOG("renderer: %llu adoptions / %llu presents in 5s (pool %zu buffers)",
                    (unsigned long long)adoptions, (unsigned long long)renders,
                    pool.slots.size());
            adoptions = renders = 0;
            rateWindowStart = nowNs();
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_readerMutex);
        g_readerWindow = nullptr;
    }
    vkDeviceWaitIdle(vk.device);
    for (auto& f : frames) {
        for (AImage* retired : f.retiring) AImage_delete(retired);
        vkDestroySemaphore(vk.device, f.acquireSem, nullptr);
        vkDestroySemaphore(vk.device, f.renderDoneSem, nullptr);
        vkDestroyFence(vk.device, f.inFlight, nullptr);
    }
    if (currentImage) AImage_delete(currentImage);
    vkDestroyCommandPool(vk.device, cmdPool, nullptr);
    pass.destroy(vk);
    pool.destroy(vk);
    if (g_reader) AImageReader_delete(g_reader);
    g_reader = nullptr;
    vk.destroy();
}

#endif // __ANDROID__
