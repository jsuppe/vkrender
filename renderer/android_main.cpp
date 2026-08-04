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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
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

// ---- Minimal column-major mat4 helpers for the cube MVP --------------------
struct Mat4 {
    float m[16]; // column-major, m[col*4+row]
};

Mat4 matMul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k) r.m[c * 4 + row] += a.m[k * 4 + row] * b.m[c * 4 + k];
    return r;
}

Mat4 matRotY(float a) {
    Mat4 r{};
    float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[2] = -s; r.m[5] = 1.0f; r.m[8] = s; r.m[10] = c; r.m[15] = 1.0f;
    return r;
}

Mat4 matRotX(float a) {
    Mat4 r{};
    float c = cosf(a), s = sinf(a);
    r.m[0] = 1.0f; r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c; r.m[15] = 1.0f;
    return r;
}

// Vulkan conventions: depth 0..1, clip Y down (the Y flip lives in m[5]).
Mat4 matPerspective(float fovyRad, float aspect, float zNear, float zFar) {
    Mat4 r{};
    float f = 1.0f / tanf(fovyRad * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = -f;
    r.m[10] = zFar / (zNear - zFar);
    r.m[11] = -1.0f;
    r.m[14] = (zNear * zFar) / (zNear - zFar);
    return r;
}

// Model spins around two axes, sits 4 units down -Z, 45° vertical FOV.
Mat4 cubeMvp(float tSec, VkExtent2D extent) {
    Mat4 model = matMul(matRotY(tSec * 0.9f), matRotX(tSec * 0.37f));
    model.m[14] -= 4.0f; // translate along -Z (view space)
    float aspect = extent.height ? (float)extent.width / (float)extent.height : 1.0f;
    return matMul(matPerspective(0.7854f, aspect, 0.1f, 100.0f), model);
}

// ---- Multi-producer stream registry (M11) ----------------------------------
// One AImageReader per client app, keyed by Binder calling UID. Created on
// demand from the Binder thread; images are acquired/imported/released only
// on the render thread. Up to one stream per cube face.
constexpr size_t kMaxStreams = 6;

struct InputStream {
    int uid = -1;
    AImageReader* reader = nullptr;
    ANativeWindow* window = nullptr; // owned by the reader
    AImage* currentImage = nullptr;  // render thread only
    int64_t currentSlot = -1;        // render thread only
    std::vector<uint32_t> keys;      // slots imported from this reader
};

std::mutex g_streamsMutex;
std::condition_variable g_readerCv; // renderer-readiness for binder waits
bool g_rendererReady = false;
std::vector<std::unique_ptr<InputStream>> g_streams;
std::vector<int> g_pendingRelease; // uids to tear down on the render thread

bool extractShaders(android_app* app, const std::string& outDir) {
    const char* names[] = {"fullscreen.vert.spv", "cube.vert.spv", "video_ycbcr.frag.spv",
                           "nv12_planar.frag.spv"};
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

// Called from RendererService's binder thread (renderer_jni.cpp). Returns the
// caller's existing stream window, or creates a fresh AImageReader for a new
// client (max one per cube face). Re-acquire after a client crash returns the
// same reader - BufferQueue accepts a new producer once the old one is gone.
ANativeWindow* vkrAcquireStreamWindow(int uid) {
    std::unique_lock<std::mutex> lock(g_streamsMutex);
    g_readerCv.wait_for(lock, std::chrono::seconds(5), [] { return g_rendererReady; });
    if (!g_rendererReady) return nullptr;
    // A release for this uid may still be queued (release + immediate
    // re-acquire): wait until the render thread has torn it down, else we
    // would hand back a window that is about to be destroyed.
    g_readerCv.wait_for(lock, std::chrono::seconds(2), [uid] {
        return std::find(g_pendingRelease.begin(), g_pendingRelease.end(), uid) ==
               g_pendingRelease.end();
    });
    if (std::find(g_pendingRelease.begin(), g_pendingRelease.end(), uid) !=
        g_pendingRelease.end())
        return nullptr; // render thread paused; client will retry
    for (auto& s : g_streams)
        if (s->uid == uid) return s->window;
    if (g_streams.size() >= kMaxStreams) {
        VKR_WARN("renderer: stream request from uid %d rejected (%zu faces occupied)", uid,
                 g_streams.size());
        return nullptr;
    }
    auto s = std::make_unique<InputStream>();
    s->uid = uid;
    media_status_t ms = AImageReader_newWithUsage(1920, 1080, AIMAGE_FORMAT_PRIVATE,
                                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 6,
                                                  &s->reader);
    if (ms != AMEDIA_OK || !s->reader) {
        VKR_ERR("AImageReader_newWithUsage failed for uid %d: %d", uid, ms);
        return nullptr;
    }
    AImageReader_getWindow(s->reader, &s->window);
    ANativeWindow* window = s->window;
    g_streams.push_back(std::move(s));
    VKR_LOG("renderer: stream created for uid %d (%zu total)", uid, g_streams.size());
    return window;
}

// Called from the binder thread; actual teardown happens on the render thread
// (it must sequence with in-flight GPU work).
void vkrReleaseStream(int uid) {
    std::lock_guard<std::mutex> lock(g_streamsMutex);
    g_pendingRelease.push_back(uid);
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
    if (!pass.init(vk, filesDir, vkr::VideoPass::Geometry::Cube)) return;

    // Producer streams are created on demand per client UID (binder thread);
    // from here on acquireInputSurface calls can be served.
    {
        std::lock_guard<std::mutex> lock(g_streamsMutex);
        g_rendererReady = true;
    }
    g_readerCv.notify_all();
    VKR_LOG("renderer: stream service ready (up to %zu producers)", kMaxStreams);

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
    uint64_t frameIndex = 0;
    ANativeWindow* vkWindow = app->window;
    uint64_t adoptions = 0, renders = 0;
    int64_t rateWindowStart = nowNs();
    const int64_t appStartNs = nowNs();

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

        int64_t faceSlots[6] = {-1, -1, -1, -1, -1, -1};
        size_t streamCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_streamsMutex);

            // Deferred stream teardown: quiesce the GPU (rare event), retire
            // every parked image, then drop the stream's slots - its buffers
            // are freed and their addresses (our keys) may be recycled.
            if (!g_pendingRelease.empty()) {
                vkDeviceWaitIdle(vk.device);
                for (auto& fr : frames) {
                    for (AImage* retired : fr.retiring) AImage_delete(retired);
                    fr.retiring.clear();
                }
                for (int uid : g_pendingRelease) {
                    for (auto it = g_streams.begin(); it != g_streams.end(); ++it) {
                        if ((*it)->uid != uid) continue;
                        InputStream& s = **it;
                        if (s.currentImage) AImage_delete(s.currentImage);
                        for (uint32_t key : s.keys) {
                            pass.removeSlot(vk, key);
                            pool.removeSlot(vk, key);
                        }
                        AImageReader_delete(s.reader);
                        VKR_LOG("renderer: stream released for uid %d (%zu slots freed)", uid,
                                s.keys.size());
                        g_streams.erase(it);
                        break;
                    }
                }
                g_pendingRelease.clear();
                g_readerCv.notify_all(); // unblock acquire-after-release waiters
            }

            // Latest ready frame per stream; older queued frames are discarded
            // by acquireLatestImage itself (latest-wins, fences respected by
            // the sync acquire).
            for (auto& sp : g_streams) {
                InputStream& s = *sp;
                AImage* image = nullptr;
                if (AImageReader_acquireLatestImage(s.reader, &image) != AMEDIA_OK || !image)
                    continue;
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
                        header.matrix =
                            h >= 720 ? vkr::proto::Matrix::BT709 : vkr::proto::Matrix::BT601;
                        header.range = vkr::proto::Range::Limited;
                        header.planeCount = 2;
                        if (!pool.create(vk, header)) {
                            AImage_delete(image);
                            continue;
                        }
                    }
                    uint32_t key = keyForBuffer(ahb);
                    bool isNew = pool.find(key) == nullptr;
                    bool ok = !isNew || pool.addSurfaceAHB(vk, key, ahb);
                    if (ok && !passBound) {
                        ok = pass.bindPool(vk, pool);
                        passBound = ok;
                    } else if (ok && isNew) {
                        ok = pass.addSlot(vk, pool, key);
                    }
                    if (ok) {
                        if (isNew) s.keys.push_back(key);
                        // Adopt: previous image retires behind this frame's fence.
                        if (s.currentImage)
                            frames[frameIndex % kFramesInFlight].retiring.push_back(
                                s.currentImage);
                        s.currentImage = image;
                        s.currentSlot = (int64_t)key;
                        ++adoptions;
                        image = nullptr; // ownership moved to the stream
                    }
                }
                if (image) AImage_delete(image); // import failed: return the buffer
            }

            // Face assignment: streams with content cycle across the six
            // faces in registry order (1 stream = all faces, 2 = alternating,
            // 6 = one each).
            std::vector<int64_t> active;
            for (auto& sp : g_streams)
                if (sp->currentSlot >= 0) active.push_back(sp->currentSlot);
            if (!active.empty())
                for (int f = 0; f < 6; ++f) faceSlots[f] = active[f % active.size()];
            streamCount = g_streams.size();
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

        Mat4 mvp = cubeMvp((float)((nowNs() - appStartNs) * 1e-9), vk.swapExtent);
        if (!passBound)
            for (int fi = 0; fi < 6; ++fi) faceSlots[fi] = -1;
        pass.recordCube(f.cmd, pool, faceSlots, vk.swapViews[imageIndex], vk.swapExtent,
                        vk.queueFamily, mvp.m);

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
            VKR_LOG("renderer: %llu adoptions / %llu presents in 5s (pool %zu buffers, "
                    "%zu streams)",
                    (unsigned long long)adoptions, (unsigned long long)renders,
                    pool.slots.size(), streamCount);
            adoptions = renders = 0;
            rateWindowStart = nowNs();
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_streamsMutex);
        g_rendererReady = false;
    }
    vkDeviceWaitIdle(vk.device);
    for (auto& f : frames) {
        for (AImage* retired : f.retiring) AImage_delete(retired);
        vkDestroySemaphore(vk.device, f.acquireSem, nullptr);
        vkDestroySemaphore(vk.device, f.renderDoneSem, nullptr);
        vkDestroyFence(vk.device, f.inFlight, nullptr);
    }
    vkDestroyCommandPool(vk.device, cmdPool, nullptr);
    pass.destroy(vk);
    pool.destroy(vk);
    {
        std::lock_guard<std::mutex> lock(g_streamsMutex);
        for (auto& sp : g_streams) {
            if (sp->currentImage) AImage_delete(sp->currentImage);
            AImageReader_delete(sp->reader);
        }
        g_streams.clear();
        g_pendingRelease.clear();
    }
    vk.destroy();
}

#endif // __ANDROID__
