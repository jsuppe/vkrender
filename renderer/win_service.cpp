// M14: Windows renderer service - see win_service.h for the architecture.
//
// The service allocates each client's ring with D3D11 (shared NT-handle BGRA
// textures + a shared ready/consumed fence pair - the exact M2 machinery,
// ownership inverted), imports everything into Vulkan through the proven M3
// paths, and composites client streams on the two-cube vaporwave scene.
//
// Sync contract (per client, identical to the M2/M3 timeline model):
//  - client signals readyFence = ++V after rendering slot S, sends
//    FrameReady{S, V, pts};
//  - the service waits readySem >= V before sampling and signals
//    consumedSem = V-1, promising to never again read frames <= V-1;
//  - the client reuses slot S only once consumedFence >= the last value it
//    signaled for S (that logic lives in the client library).

#ifdef _WIN32

#include "win_service.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <volk.h>

#include "background_pass.h"
#include "dump_util.h"
#include "external_video_image.h"
#include "ipc.h"
#include "log.h"
#include "scene_math.h"
#include "video_pass.h"
#include "vk_context.h"

using Microsoft::WRL::ComPtr;

namespace vkr {
namespace {

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Client {
    IpcChannel channel;
    uint32_t id = 0;
    uint32_t width = 0, height = 0, poolSize = 0;
    std::vector<uint32_t> keys;
    ComPtr<ID3D11Texture2D> textures[proto::kMaxPoolSize];
    HANDLE localTexHandle[proto::kMaxPoolSize] = {};
    ComPtr<ID3D11Fence> readyFence, consumedFence;
    HANDLE localFenceHandle[2] = {};
    VkSemaphore readySem = VK_NULL_HANDLE, consumedSem = VK_NULL_HANDLE;
    int64_t currentSlot = -1; // pool key
    uint64_t currentReady = 0;
    uint64_t lastConsumedSignal = 0;
    uint64_t pendingConsumed = 0; // 0 = none this frame
};

struct PendingConnection {
    IpcChannel channel;
    proto::ClientHelloMsg hello;
};

std::mutex g_acceptMutex;
std::vector<std::unique_ptr<PendingConnection>> g_pending;

// Accept thread: each accepted pipe instance reads the ClientHello, then the
// connection is handed to the render thread for allocation + import.
void acceptLoop(std::string pipeName) {
    for (;;) {
        IpcChannel ch = IpcChannel::listen(pipeName);
        if (!ch.connected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        auto msg = ch.recv();
        if (!msg || msg->type != proto::MsgType::ClientHello) continue;
        const auto* hello = msg->as<proto::ClientHelloMsg>();
        if (!hello) continue;
        auto pending = std::make_unique<PendingConnection>();
        pending->channel = std::move(ch);
        pending->hello = *hello;
        std::lock_guard<std::mutex> lock(g_acceptMutex);
        g_pending.push_back(std::move(pending));
    }
}

bool duplicateForClient(HANDLE local, HANDLE clientProcess, uint64_t* out) {
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), local, clientProcess, &dup, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return false;
    *out = (uint64_t)(uintptr_t)dup;
    return true;
}

} // namespace

int runWinService(const WinServeOptions& options) {
    const bool headless = options.headlessFrames > 0;

    VkContext vk;
    if (!vk.init(1280, 720, "vkrender service", headless)) return 1;

    VideoPass pass;
    if (!pass.init(vk, options.shaderDir, VideoPass::Geometry::Cube)) return 1;
    BackgroundPass background;
    if (!background.init(vk, options.shaderDir))
        VKR_WARN("service: vaporwave backdrop unavailable");
    ExternalVideoPool pool;

    // D3D11 allocator device on the same adapter as the Vulkan device.
    ComPtr<ID3D11Device5> d3d;
    ComPtr<ID3D11DeviceContext4> d3dCtx;
    {
        ComPtr<IDXGIFactory1> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter1> adapter, pick;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (!vk.luidValid || memcmp(&desc.AdapterLuid, vk.deviceLUID, 8) == 0) {
                pick = adapter;
                break;
            }
        }
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
        if (FAILED(D3D11CreateDevice(pick.Get(), pick ? D3D_DRIVER_TYPE_UNKNOWN
                                                      : D3D_DRIVER_TYPE_HARDWARE,
                                     nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                                     &ctx))) {
            VKR_ERR("service: D3D11 allocator device creation failed");
            return 1;
        }
        dev.As(&d3d);
        ctx.As(&d3dCtx);
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
        if (!d3d || !d3dCtx) {
            VKR_ERR("service: D3D11.4 interfaces unavailable");
            return 1;
        }
    }

    std::thread(acceptLoop, options.pipeName).detach();
    VKR_LOG("service: listening on pipe '%s' (producer clients welcome)",
            options.pipeName.c_str());

    constexpr uint32_t kFramesInFlight = 2;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = vk.queueFamily;
    vkCreateCommandPool(vk.device, &cpci, nullptr, &cmdPool);

    struct FrameSync {
        VkSemaphore acquireSem = VK_NULL_HANDLE;
        VkSemaphore renderDoneSem = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
    } frames[kFramesInFlight];
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

    std::vector<std::unique_ptr<Client>> clients;
    uint32_t nextClientId = 1;
    bool passBound = false;
    bool passNeedsBind = false;
    uint64_t frameIndex = 0;
    uint64_t adoptions = 0, renders = 0;
    int videoFramesSeen = 0;
    int64_t rateWindowStart = nowNs();
    const int64_t appStartNs = nowNs();
    const int64_t headlessDeadline = nowNs() + 60ll * 1000000000ll;
    int exitCode = 0;

    auto teardownClient = [&](Client& c) {
        vkDeviceWaitIdle(vk.device);
        for (uint32_t key : c.keys) {
            pass.removeSlot(vk, key);
            pool.removeSlot(vk, key);
        }
        if (c.readySem) vkDestroySemaphore(vk.device, c.readySem, nullptr);
        if (c.consumedSem) vkDestroySemaphore(vk.device, c.consumedSem, nullptr);
        for (HANDLE h : c.localTexHandle)
            if (h) CloseHandle(h);
        for (HANDLE h : c.localFenceHandle)
            if (h) CloseHandle(h);
        VKR_LOG("service: client %u gone (%zu slots freed)", c.id, c.keys.size());
    };

    // Allocate a ring for a newly accepted client and duplicate the handles
    // into its process. Runs on the render thread.
    auto integrateClient = [&](PendingConnection& pc) -> std::unique_ptr<Client> {
        auto c = std::make_unique<Client>();
        c->channel = std::move(pc.channel);
        c->id = nextClientId++;
        c->width = pc.hello.width ? pc.hello.width : 640;
        c->height = pc.hello.height ? pc.hello.height : 360;
        c->poolSize = pc.hello.poolSize ? pc.hello.poolSize : 4;
        if (c->poolSize > proto::kMaxPoolSize) c->poolSize = proto::kMaxPoolSize;
        if (pc.hello.format != proto::SharedFormat::BGRA8) {
            VKR_ERR("service: client %u requested unsupported format", c->id);
            return nullptr;
        }

        if (!pool.valid()) {
            proto::PoolDescMsg header{};
            header.width = c->width;
            header.height = c->height;
            header.format = proto::SharedFormat::BGRA8;
            header.matrix = proto::Matrix::BT709;
            header.range = proto::Range::Full;
            header.poolSize = 0; // dynamic slots
            header.planeCount = 1;
            if (!pool.create(vk, header)) return nullptr;
            passNeedsBind = true;
        }

        HANDLE clientProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pc.hello.clientPid);
        if (!clientProcess) {
            VKR_ERR("service: OpenProcess(%u) failed", pc.hello.clientPid);
            return nullptr;
        }

        proto::PoolDescMsg desc{};
        desc.width = c->width;
        desc.height = c->height;
        desc.format = proto::SharedFormat::BGRA8;
        desc.matrix = proto::Matrix::BT709;
        desc.range = proto::Range::Full;
        desc.poolSize = c->poolSize;
        desc.planeCount = 1;

        bool ok = true;
        for (uint32_t i = 0; i < c->poolSize && ok; ++i) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = c->width;
            td.Height = c->height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            ok = SUCCEEDED(d3d->CreateTexture2D(&td, nullptr, &c->textures[i]));
            ComPtr<IDXGIResource1> res;
            if (ok) ok = SUCCEEDED(c->textures[i].As(&res));
            if (ok)
                ok = SUCCEEDED(res->CreateSharedHandle(
                    nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                    &c->localTexHandle[i]));
            if (ok) {
                uint32_t key = (c->id << 4) | i;
                ok = pool.addSurfaceWin32(vk, key, (uint64_t)(uintptr_t)c->localTexHandle[i],
                                          c->width, c->height);
                if (ok) c->keys.push_back(key);
            }
            if (ok) ok = duplicateForClient(c->localTexHandle[i], clientProcess,
                                            &desc.textureHandle[i]);
        }
        if (ok) ok = SUCCEEDED(d3d->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                                IID_PPV_ARGS(&c->readyFence)));
        if (ok) ok = SUCCEEDED(d3d->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                                IID_PPV_ARGS(&c->consumedFence)));
        if (ok)
            ok = SUCCEEDED(c->readyFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                                             &c->localFenceHandle[0])) &&
                 SUCCEEDED(c->consumedFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                                                &c->localFenceHandle[1]));
        if (ok)
            ok = importTimelineSemaphoreWin32(vk, (uint64_t)(uintptr_t)c->localFenceHandle[0],
                                              &c->readySem) &&
                 importTimelineSemaphoreWin32(vk, (uint64_t)(uintptr_t)c->localFenceHandle[1],
                                              &c->consumedSem);
        if (ok)
            ok = duplicateForClient(c->localFenceHandle[0], clientProcess,
                                    &desc.readyFenceHandle) &&
                 duplicateForClient(c->localFenceHandle[1], clientProcess,
                                    &desc.consumedFenceHandle);

        if (ok) {
            proto::HelloMsg hello{};
            hello.rendererPid = GetCurrentProcessId();
            hello.luidValid = vk.luidValid ? 1 : 0;
            memcpy(hello.deviceLUID, vk.deviceLUID, sizeof(hello.deviceLUID));
            ok = c->channel.send(proto::MsgType::Hello, hello) &&
                 c->channel.send(proto::MsgType::PoolDesc, desc);
        }
        CloseHandle(clientProcess);
        if (!ok) {
            VKR_ERR("service: client %u setup failed", c->id);
            teardownClient(*c);
            return nullptr;
        }
        if (passNeedsBind) {
            passBound = pass.bindPool(vk, pool);
            passNeedsBind = false;
            if (!passBound) return nullptr;
        } else {
            for (uint32_t key : c->keys)
                if (!pass.addSlot(vk, pool, key)) return nullptr;
        }
        VKR_LOG("service: client %u connected (%ux%u ring of %u)", c->id, c->width, c->height,
                c->poolSize);
        return c;
    };

    while (true) {
        if (vk.window) {
            glfwPollEvents();
            if (glfwWindowShouldClose(vk.window)) break;
        }
        if (headless) {
            if (videoFramesSeen >= options.headlessFrames) break;
            if (nowNs() > headlessDeadline) {
                VKR_ERR("service headless: no %d frames within 60 s (saw %d)",
                        options.headlessFrames, videoFramesSeen);
                exitCode = 2;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        // New connections (allocation happens here, on the render thread).
        {
            std::vector<std::unique_ptr<PendingConnection>> pending;
            {
                std::lock_guard<std::mutex> lock(g_acceptMutex);
                pending.swap(g_pending);
            }
            for (auto& pc : pending) {
                if (clients.size() >= 12) {
                    VKR_WARN("service: rejecting client (12 streams occupied)");
                    continue;
                }
                if (auto c = integrateClient(*pc)) clients.push_back(std::move(c));
            }
        }

        // Drain clients: latest FrameReady wins per client.
        for (auto it = clients.begin(); it != clients.end();) {
            Client& c = **it;
            while (auto msg = c.channel.tryRecv()) {
                if (msg->type == proto::MsgType::FrameReady) {
                    if (const auto* fr = msg->as<proto::FrameReadyMsg>()) {
                        c.currentSlot = (int64_t)((c.id << 4) | fr->slot);
                        c.currentReady = fr->readyValue;
                        if (fr->readyValue > 0 && fr->readyValue - 1 > c.lastConsumedSignal)
                            c.pendingConsumed = fr->readyValue - 1;
                        ++adoptions;
                        ++videoFramesSeen;
                    }
                } else if (msg->type == proto::MsgType::Bye) {
                    c.channel = IpcChannel();
                }
            }
            if (!c.channel.connected()) {
                teardownClient(c);
                it = clients.erase(it);
            } else {
                ++it;
            }
        }

        int64_t faceSlots[12];
        for (int i = 0; i < 12; ++i) faceSlots[i] = -1;
        {
            std::vector<int64_t> active;
            for (auto& c : clients)
                if (c->currentSlot >= 0) active.push_back(c->currentSlot);
            if (!active.empty())
                for (int i = 0; i < 12; ++i) faceSlots[i] = active[i % active.size()];
        }

        FrameSync& f = frames[frameIndex % kFramesInFlight];
        ++frameIndex;
        vkWaitForFences(vk.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        if (!headless) {
            VkResult ar = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX,
                                                f.acquireSem, VK_NULL_HANDLE, &imageIndex);
            if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
                if (!vk.recreateSwapchain()) {
                    glfwWaitEventsTimeout(0.1);
                    continue;
                }
                if (ar == VK_ERROR_OUT_OF_DATE_KHR) continue;
            } else if (ar != VK_SUCCESS) {
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

        float tSec = (float)((nowNs() - appStartNs) * 1e-9);
        float mvps[32];
        bool wide = vk.swapExtent.width >= vk.swapExtent.height;
        Mat4 a = cubeMvp(tSec, vk.swapExtent, wide ? -1.55f : 0.0f, wide ? 0.0f : 1.55f, 1.0f,
                         0.0f);
        Mat4 b = cubeMvp(tSec, vk.swapExtent, wide ? 1.55f : 0.0f, wide ? 0.0f : -1.55f, -1.0f,
                         2.1f);
        memcpy(mvps, a.m, sizeof(a.m));
        memcpy(mvps + 16, b.m, sizeof(b.m));
        if (!passBound)
            for (int i = 0; i < 12; ++i) faceSlots[i] = -1;
        pass.recordScene(f.cmd, pool, faceSlots, 2, mvps, vk.swapViews[imageIndex],
                         vk.swapExtent, vk.queueFamily,
                         background.valid() ? &background : nullptr, tSec);

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

        // Waits: swapchain acquire + every visible client's ready value.
        std::vector<VkSemaphore> waitSems;
        std::vector<VkPipelineStageFlags> waitStages;
        std::vector<uint64_t> waitValues;
        if (!headless) {
            waitSems.push_back(f.acquireSem);
            waitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            waitValues.push_back(0);
        }
        std::vector<VkSemaphore> signalSems;
        std::vector<uint64_t> signalValues;
        if (!headless) {
            signalSems.push_back(f.renderDoneSem);
            signalValues.push_back(0);
        }
        for (auto& cp : clients) {
            Client& c = *cp;
            if (c.currentSlot >= 0 && c.readySem) {
                waitSems.push_back(c.readySem);
                waitStages.push_back(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                waitValues.push_back(c.currentReady);
            }
            if (c.pendingConsumed && c.consumedSem) {
                signalSems.push_back(c.consumedSem);
                signalValues.push_back(c.pendingConsumed);
                c.lastConsumedSignal = c.pendingConsumed;
                c.pendingConsumed = 0;
            }
        }

        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = (uint32_t)waitValues.size();
        timeline.pWaitSemaphoreValues = waitValues.data();
        timeline.signalSemaphoreValueCount = (uint32_t)signalValues.size();
        timeline.pSignalSemaphoreValues = signalValues.data();

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.pNext = &timeline;
        si.waitSemaphoreCount = (uint32_t)waitSems.size();
        si.pWaitSemaphores = waitSems.data();
        si.pWaitDstStageMask = waitStages.data();
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        si.signalSemaphoreCount = (uint32_t)signalSems.size();
        si.pSignalSemaphores = signalSems.data();
        if (vkQueueSubmit(vk.queue, 1, &si, f.inFlight) != VK_SUCCESS) break;

        if (!headless) {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &f.renderDoneSem;
            pi.swapchainCount = 1;
            pi.pSwapchains = &vk.swapchain;
            pi.pImageIndices = &imageIndex;
            VkResult pr = vkQueuePresentKHR(vk.queue, &pi);
            if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
                vk.recreateSwapchain();
        }

        ++renders;
        if (nowNs() - rateWindowStart > 5000000000ll) {
            VKR_LOG("service: %llu adoptions / %llu frames in 5s (pool %zu slots, %zu clients)",
                    (unsigned long long)adoptions, (unsigned long long)renders,
                    pool.slots.size(), clients.size());
            adoptions = renders = 0;
            rateWindowStart = nowNs();
        }
    }

    vkDeviceWaitIdle(vk.device);
    if (headless && exitCode == 0 && videoFramesSeen >= options.headlessFrames)
        vkrDumpTarget(vk, cmdPool, options.dumpPath.c_str());

    for (auto& c : clients) teardownClient(*c);
    clients.clear();
    for (auto& f : frames) {
        vkDestroySemaphore(vk.device, f.acquireSem, nullptr);
        vkDestroySemaphore(vk.device, f.renderDoneSem, nullptr);
        vkDestroyFence(vk.device, f.inFlight, nullptr);
    }
    vkDestroyCommandPool(vk.device, cmdPool, nullptr);
    background.destroy(vk);
    pass.destroy(vk);
    pool.destroy(vk);
    vk.destroy();
    return exitCode;
}

} // namespace vkr

#endif // _WIN32
