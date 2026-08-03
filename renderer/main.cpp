#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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

// vkrenderer [--pipe vkrender] [--pacing latest|pts] [--headless N [--dump out.ppm]]
//
// Process 2 of the zero-copy pipeline: imports the decoder's shared GPU
// surfaces and renders the video. Never copies pixel data - it samples the
// exact memory the decoder wrote. Survives decoder restarts by redialing the
// pipe and reimporting on the next PoolDesc.
//
// --headless renders offscreen without any window system (for CI / SSH-only
// machines), waits for N video frames, writes the last one as a PPM, exits.

namespace {

std::string exeDir() {
#ifdef _WIN32
    char buf[512];
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
#else
    char buf[512];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = 0;
#endif
    std::string path(buf);
    size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? "." : path.substr(0, pos);
}

uint32_t currentPid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct FrameSync {
    VkSemaphore acquireSem = VK_NULL_HANDLE;
    VkSemaphore renderDoneSem = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    // Linux (no shared fences): consumed value to report once this frame's
    // fence proves all older sampling work retired.
    std::optional<uint64_t> pendingConsumed;
};

// Writes the offscreen target as a binary PPM for headless verification.
bool dumpTarget(vkr::VkContext& vk, VkCommandPool cmdPool, const char* path) {
    const uint32_t w = vk.swapExtent.width, h = vk.swapExtent.height;
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
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, vk.swapImages[0], VK_IMAGE_LAYOUT_GENERAL, buf, 1, &region);
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

} // namespace

int main(int argc, char** argv) {
    std::string pipe = "vkrender";
    std::string dumpPath = "headless.ppm";
    bool ptsPacing = false;
    int headlessFrames = 0; // 0 = windowed mode
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--pipe") && i + 1 < argc) pipe = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpPath = argv[++i];
        else if (!strcmp(argv[i], "--headless") && i + 1 < argc) headlessFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pacing") && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "pts") ptsPacing = true;
            else if (mode != "latest") { fprintf(stderr, "bad --pacing\n"); return 1; }
        } else {
            fprintf(stderr,
                    "usage: vkrenderer [--pipe name] [--pacing latest|pts] "
                    "[--headless N [--dump out.ppm]]\n");
            return 1;
        }
    }
    const bool headless = headlessFrames > 0;
    const bool dbgNoTimelineWait = getenv("VKR_NO_TIMELINE_WAIT") != nullptr;
    const bool dbgNoVideoDraw = getenv("VKR_NO_VIDEO_DRAW") != nullptr;
    if (dbgNoTimelineWait) VKR_WARN("debug: timeline wait disabled");
    if (dbgNoVideoDraw) VKR_WARN("debug: video draw disabled");
    if (ptsPacing) VKR_LOG("renderer: PTS pacing enabled (smooth playback mode)");
    if (headless) VKR_LOG("renderer: headless, waiting for %d video frames", headlessFrames);

    vkr::VkContext vk;
    if (!vk.init(1280, 720, "vkrender - waiting for decoder", headless)) return 1;

    vkr::VideoPass pass;
    if (!pass.init(vk, exeDir() + "/shaders")) return 1;

    auto sendHello = [&](vkr::IpcChannel& c) {
        vkr::proto::HelloMsg hello{};
        hello.rendererPid = currentPid();
        hello.luidValid = vk.luidValid ? 1 : 0;
        memcpy(hello.deviceLUID, vk.deviceLUID, sizeof(hello.deviceLUID));
        return c.send(vkr::proto::MsgType::Hello, hello);
    };
    auto setTitle = [&](const char* title) {
        if (vk.window) glfwSetWindowTitle(vk.window, title);
    };

    vkr::IpcChannel ch = vkr::IpcChannel::connect(pipe, 30000);
    if (!ch.connected()) {
        VKR_ERR("could not reach vkdecoder on pipe '%s' (start it first)", pipe.c_str());
        return 1;
    }
    if (!sendHello(ch)) return 1;

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
    // Linux: the pipeline can only be built once the first surface reveals
    // whether the modifier needs planar-mode sampling; Windows binds at once.
    bool passNeedsBind = false;
    int64_t currentSlot = -1; // slot key, -1 = none
    uint64_t currentReady = 0;
    uint64_t lastConsumedSignal = 0;
    std::optional<uint64_t> pendingConsumedSignal;
    uint64_t frameIndex = 0;
    int videoFramesSeen = 0;
    bool decoderGone = false;
    int64_t lastDialNs = 0;
    const int64_t headlessDeadline = nowNs() + 60ll * 1000000000ll;

    std::deque<vkr::proto::FrameReadyMsg> ptsQueue;
    bool anchored = false;
    int64_t anchorPts = 0, anchorLocal = 0;

    auto resetStreamState = [&]() {
        ptsQueue.clear();
        anchored = false;
        currentSlot = -1;
        currentReady = 0;
        lastConsumedSignal = 0;
        pendingConsumedSignal.reset();
    };

    auto adoptFrame = [&](const vkr::proto::FrameReadyMsg& fr) {
        currentSlot = (int64_t)fr.slot;
        currentReady = fr.readyValue;
        // Consumed = V-1 promises we never again read any frame with
        // readyValue <= V-1, releasing those surfaces to the decoder.
        if (currentReady > 0 && currentReady - 1 > lastConsumedSignal)
            pendingConsumedSignal = currentReady - 1;
        ++videoFramesSeen;
    };

    bool running = true;
    while (running) {
        if (vk.window) {
            glfwPollEvents();
            if (glfwWindowShouldClose(vk.window)) break;
        }
        if (headless) {
            if (videoFramesSeen >= headlessFrames) break;
            if (nowNs() > headlessDeadline) {
                VKR_ERR("headless: no %d video frames within 60 s (saw %d)", headlessFrames,
                        videoFramesSeen);
                break;
            }
        }

        // Redial a restarted decoder once a second.
        if (!ch.connected()) {
            if (!decoderGone) {
                decoderGone = true;
                setTitle("vkrender - decoder disconnected (showing last frame)");
            }
            if (nowNs() - lastDialNs > 1000000000ll) {
                lastDialNs = nowNs();
                vkr::IpcChannel next = vkr::IpcChannel::connect(pipe, 1, /*quiet=*/true);
                if (next.connected() && sendHello(next)) {
                    ch = std::move(next);
                    decoderGone = false;
                    VKR_LOG("renderer: reconnected to decoder");
                    setTitle("vkrender - reconnected, waiting for stream");
                }
            }
        }

        // Drain IPC. Latest-wins mode coalesces frames; PTS mode queues them.
        std::optional<vkr::proto::FrameReadyMsg> newest;
        while (auto msg = ch.tryRecv()) {
            switch (msg->type) {
                case vkr::proto::MsgType::PoolDesc: {
                    if (const auto* d = msg->as<vkr::proto::PoolDescMsg>()) {
                        vkDeviceWaitIdle(vk.device);
                        pass.unbindPool(vk);
                        pool.destroy(vk);
                        resetStreamState();
                        newest.reset();
                        if (!pool.create(vk, *d)) {
                            VKR_ERR("failed to import shared pool");
                            return 1;
                        }
                        if (!pool.slots.empty()) { // Windows: slots came with PoolDesc
                            if (!pass.bindPool(vk, pool)) return 1;
                            passNeedsBind = false;
                        } else {
                            passNeedsBind = true;
                        }
                        setTitle("vkrender - zero-copy stream");
                    }
                    break;
                }
                case vkr::proto::MsgType::SurfaceDesc: {
                    if (const auto* d = msg->as<vkr::proto::SurfaceDescMsg>()) {
                        if (!pool.addSurface(vk, *d, msg->fds.data(), msg->fds.size())) {
                            VKR_ERR("failed to import surface %u", d->slotKey);
                            return 1;
                        }
                        // First surface decided combined-vs-planar sampling;
                        // build the pipeline now, then per-slot descriptors.
                        bool ok = passNeedsBind ? pass.bindPool(vk, pool)
                                                : pass.addSlot(vk, pool, d->slotKey);
                        passNeedsBind = false;
                        if (!ok) {
                            VKR_ERR("failed to bind surface %u", d->slotKey);
                            return 1;
                        }
                    }
                    break;
                }
                case vkr::proto::MsgType::FrameReady:
                    if (const auto* fr = msg->as<vkr::proto::FrameReadyMsg>()) {
                        if (ptsPacing) ptsQueue.push_back(*fr);
                        else newest = *fr;
                    }
                    break;
                case vkr::proto::MsgType::StreamReset:
                    vkDeviceWaitIdle(vk.device);
                    pass.unbindPool(vk);
                    pool.destroy(vk);
                    resetStreamState();
                    newest.reset();
                    break;
                case vkr::proto::MsgType::StreamStatus:
                    if (const auto* st = msg->as<vkr::proto::StreamStatusMsg>()) {
                        switch (st->phase) {
                            case vkr::proto::StreamPhase::Connecting:
                                setTitle(pool.valid()
                                             ? "vkrender - reconnecting to stream (showing last frame)"
                                             : "vkrender - connecting to stream");
                                break;
                            case vkr::proto::StreamPhase::Streaming:
                                setTitle("vkrender - stream connected");
                                break;
                            case vkr::proto::StreamPhase::Error:
                                setTitle("vkrender - stream lost, decoder retrying");
                                break;
                        }
                    }
                    break;
                case vkr::proto::MsgType::Bye:
                    VKR_LOG("renderer: decoder said goodbye");
                    break;
                default:
                    break;
            }
        }

        // Frame adoption
        if (pool.valid()) {
            if (!ptsPacing && newest) {
                adoptFrame(*newest);
            } else if (ptsPacing) {
                // Never let the queue starve the decoder's surface ring.
                while (ptsQueue.size() > 4) {
                    adoptFrame(ptsQueue.front());
                    ptsQueue.pop_front();
                }
                const int64_t now = nowNs();
                while (!ptsQueue.empty()) {
                    const auto& f = ptsQueue.front();
                    int64_t due = now;
                    if (f.ptsNs >= 0) {
                        if (!anchored) {
                            anchored = true;
                            anchorPts = f.ptsNs;
                            anchorLocal = now;
                        }
                        due = anchorLocal + (f.ptsNs - anchorPts);
                        if (due - now > 1000000000ll || now - due > 1000000000ll) {
                            anchorPts = f.ptsNs; // discontinuity: re-anchor
                            anchorLocal = now;
                            due = now;
                        }
                    }
                    if (due <= now) {
                        adoptFrame(f);
                        ptsQueue.pop_front();
                    } else {
                        break;
                    }
                }
            }
        }

        FrameSync& f = frames[frameIndex % kFramesInFlight];
        ++frameIndex;
        vkWaitForFences(vk.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
        // This fence retiring proves every submission recorded before it has
        // retired too - safe point to release older surfaces via IPC (the
        // no-shared-fence sync path).
        if (f.pendingConsumed) {
            vkr::proto::FrameConsumedMsg fc{};
            fc.readyValue = *f.pendingConsumed;
            if (ch.connected()) ch.send(vkr::proto::MsgType::FrameConsumed, fc);
            f.pendingConsumed.reset();
        }

        uint32_t imageIndex = 0;
        if (!headless) {
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
        }
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

        pass.record(f.cmd, pool, (pool.valid() && !dbgNoVideoDraw) ? currentSlot : -1,
                    vk.swapViews[imageIndex], vk.swapExtent, vk.queueFamily);

        // Windowed: -> PRESENT for the swapchain. Headless: -> GENERAL so the
        // dump can copy from the offscreen target.
        VkImageMemoryBarrier2 fin{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        fin.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        fin.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        fin.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        fin.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        fin.newLayout = headless ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        fin.image = vk.swapImages[imageIndex];
        fin.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dep.pImageMemoryBarriers = &fin;
        vkCmdPipelineBarrier2(f.cmd, &dep);

        vkEndCommandBuffer(f.cmd);

        VkSemaphore waitSems[2] = {f.acquireSem, VK_NULL_HANDLE};
        VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
        uint64_t waitValues[2] = {0, 0};
        uint32_t waitCount = headless ? 0 : 1;
        if (!headless) waitStages[0] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (pool.readySem && currentSlot >= 0 && !dbgNoTimelineWait) {
            waitSems[waitCount] = pool.readySem;
            waitStages[waitCount] = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            waitValues[waitCount] = currentReady;
            ++waitCount;
        }
        VkSemaphore signalSems[2] = {f.renderDoneSem, VK_NULL_HANDLE};
        uint64_t signalValues[2] = {0, 0};
        uint32_t signalCount = headless ? 0 : 1;
        if (pendingConsumedSignal) {
            if (pool.consumedSem) {
                // Windows: GPU-side timeline signal on the shared fence.
                signalSems[signalCount] = pool.consumedSem;
                signalValues[signalCount] = *pendingConsumedSignal;
                ++signalCount;
                lastConsumedSignal = *pendingConsumedSignal;
                pendingConsumedSignal.reset();
            } else {
                // Linux: report over IPC once this submission's fence retires.
                f.pendingConsumed = *pendingConsumedSignal;
                lastConsumedSignal = *pendingConsumedSignal;
                pendingConsumedSignal.reset();
            }
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
            VKR_ERR("vkQueueSubmit failed: %d (slot=%lld ready=%llu)", sr,
                    (long long)currentSlot, (unsigned long long)currentReady);
            break;
        }

        if (!headless) {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &f.renderDoneSem;
            pi.swapchainCount = 1;
            pi.pSwapchains = &vk.swapchain;
            pi.pImageIndices = &imageIndex;
            VkResult pr = vkQueuePresentKHR(vk.queue, &pi);
            if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) vk.recreateSwapchain();
        }
    }

    int exitCode = 0;
    if (headless) {
        vkQueueWaitIdle(vk.queue);
        if (videoFramesSeen >= headlessFrames) {
            dumpTarget(vk, cmdPool, dumpPath.c_str());
            VKR_LOG("renderer: headless OK, %d video frames", videoFramesSeen);
        } else {
            exitCode = 2;
        }
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
    return exitCode;
}
