// M14/M15: Windows renderer service - see win_service.h for the architecture.
//
// The service allocates each client's ring with D3D11 (shared NT-handle BGRA
// textures + a shared ready/consumed fence pair - the exact M2 machinery,
// ownership inverted), imports everything into Vulkan through the proven M3
// paths, and hands the streams to the SceneHost (M15: multi-window spanning,
// layouts, controls). Per-client fence waits/signals ride the host's frame
// submit as extras.

#ifdef _WIN32

#include "win_service.h"

// M35: the scene view now goes out over framelink (docs/FRAMELINK.md)
// instead of the M24 consumer protocol - the CONSUMER owns the ring.
#include "framelink.h"
#include "framelink_d3d11.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <volk.h>

#include "dump_util.h"
#include "external_video_image.h"
#include "ipc.h"
#include "log.h"
#include "scene_host.h"
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
    // M31: the pipe has closed but the scene is still drawing this stream's
    // last frame while it animates off. The ring stays imported until the
    // scene says it is done (SceneHost::retainsStream), so the exit animation
    // has something to sample instead of popping.
    bool departing = false;
    int64_t departedNs = 0;
};

// M35: the scene view, published over framelink. Was `Consumer`: the renderer
// allocated the ring and handed it out, which is backwards from every other
// platform - Android's Surface belongs to whoever consumes it. Now the camera
// (or any consumer) creates the channel and we attach as a producer.
//
// We composite with VULKAN, so we cannot use flSubmit: it signals from
// framelink's D3D11 context, which orders nothing against our writes. We import
// the channel's ready fence as a timeline semaphore and signal it inside our own
// frame submit - flBeginSubmit/flEndSubmit exist for exactly this.
struct SceneOut {
    flChannel* channel = nullptr;
    uint32_t width = 0, height = 0, poolSize = 0;
    VkImage images[proto::kMaxPoolSize] = {};        // imported per slot, cached
    VkDeviceMemory memories[proto::kMaxPoolSize] = {};
    VkSemaphore readySem = VK_NULL_HANDLE;           // we signal, consumer waits
    int pendingSlot = -1;                            // blit recorded this frame
    uint64_t pendingValue = 0;
    int64_t nextRetryNs = 0;                         // the consumer may not exist yet
};

struct PendingConnection {
    IpcChannel channel;
    proto::ClientHelloMsg hello;
};

std::mutex g_acceptMutex;
std::vector<std::unique_ptr<PendingConnection>> g_pending;

void acceptLoop(std::string pipeName) {
    for (;;) {
        IpcChannel ch = IpcChannel::listen(pipeName);
        if (!ch.connected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        auto msg = ch.recv();
        if (!msg) continue;
        if (msg->type == proto::MsgType::ClientHello) {
            const auto* hello = msg->as<proto::ClientHelloMsg>();
            if (!hello) continue;
            auto pending = std::make_unique<PendingConnection>();
            pending->channel = std::move(ch);
            pending->hello = *hello;
            std::lock_guard<std::mutex> lock(g_acceptMutex);
            g_pending.push_back(std::move(pending));
        }
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
    uint32_t initW = options.windows.empty() ? 1280 : (uint32_t)options.windows[0].width;
    uint32_t initH = options.windows.empty() ? 720 : (uint32_t)options.windows[0].height;
    if (!vk.init(initW, initH, "vkrender service", headless)) return 1;

    VideoPass pass;
    if (!pass.init(vk, options.shaderDir, VideoPass::Geometry::Cube)) return 1;
    ExternalVideoPool pool;

    SceneHost host;
    if (!host.init(vk, options.shaderDir, options.windows)) return 1;

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

    std::vector<std::unique_ptr<Client>> clients;
    uint32_t nextClientId = 1;
    bool passBound = false;
    bool passNeedsBind = false;
    uint64_t adoptions = 0;
    int videoFramesSeen = 0;
    int64_t rateWindowStart = nowNs();
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

    // M35: attach to a framelink channel that a CONSUMER created. Retried,
    // because the consumer owns the channel and may start after us - the
    // inverse of the old model, where we allocated the ring and the consumer
    // dialled in to collect it.
    SceneOut sceneOut;
    const std::string sceneChannel = options.pipeName + ".scene";

    auto teardownSceneOut = [&](SceneOut& so) {
        if (!so.channel) return;
        vkDeviceWaitIdle(vk.device);
        for (uint32_t i = 0; i < proto::kMaxPoolSize; ++i) {
            if (so.images[i]) vkDestroyImage(vk.device, so.images[i], nullptr);
            if (so.memories[i]) vkFreeMemory(vk.device, so.memories[i], nullptr);
            so.images[i] = VK_NULL_HANDLE;
            so.memories[i] = VK_NULL_HANDLE;
        }
        if (so.readySem) vkDestroySemaphore(vk.device, so.readySem, nullptr);
        so.readySem = VK_NULL_HANDLE;
        flClose(so.channel);
        so.channel = nullptr;
        so.pendingSlot = -1;
        VKR_LOG("service: scene channel '%s' released", sceneChannel.c_str());
    };

    auto attachSceneOut = [&](SceneOut& so) {
        if (so.channel || nowNs() < so.nextRetryNs) return;
        so.nextRetryNs = nowNs() + 1000000000ll; // 1 s between attempts
        flChannel* ch = nullptr;
        if (flOpenProducer(sceneChannel.c_str(), &ch) != FL_OK)
            return; // FL_NOT_FOUND until a consumer creates it
        flChannelInfo info{};
        flQuery(ch, &info);
        // We composite with Vulkan, so we must signal the channel's ready fence
        // from our own submit; flSubmit's D3D11 signal would order nothing.
        void* fenceHandle = nullptr;
        if (flSharedReadyFence(ch, &fenceHandle) != FL_OK ||
            !importTimelineSemaphoreWin32(vk, (uint64_t)(uintptr_t)fenceHandle,
                                          &so.readySem)) {
            VKR_ERR("service: scene channel ready-fence import failed");
            flClose(ch);
            return;
        }
        so.channel = ch;
        so.width = info.width;   // the consumer's geometry, not ours
        so.height = info.height;
        so.poolSize = info.poolSize;
        VKR_LOG("service: publishing scene to '%s' (%ux%u, ring of %u)", sceneChannel.c_str(),
                info.width, info.height, info.poolSize);
    };

    bool quit = false;
    while (!quit) {
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
        attachSceneOut(sceneOut);

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
            // M31: do not free on disconnect. Mark departing and hold the ring
            // so the exit animation still has a texture; the scene reports
            // when it has finished with the stream. The timeout is a backstop
            // - a bug in the motion layer must not pin a ring forever.
            if (!c.channel.connected() && !c.departing) {
                c.departing = true;
                c.departedNs = nowNs();
            }
            const bool expired =
                c.departing && (nowNs() - c.departedNs) > 3000000000ll; // 3 s
            if (c.departing && (expired || !host.retainsStream((uint64_t)c.id))) {
                if (expired)
                    VKR_WARN("service: client %u ring held 3 s after disconnect, forcing",
                             c.id);
                teardownClient(c);
                it = clients.erase(it);
            } else {
                ++it;
            }
        }

        std::vector<SceneHost::StreamRef> streams;
        SceneHost::Extras extras;
        for (auto& cp : clients) {
            Client& c = *cp;
            // A departing client is deliberately NOT offered as a stream: that
            // absence is exactly how the scene's motion layer learns it is
            // leaving and starts animating it out. Its ring is still alive
            // (teardown is deferred above), so the tile keeps its texture.
            // No fence waits either - the producer is gone, and its last frame
            // was already waited on when it was delivered.
            if (c.departing) continue;
            streams.push_back({(uint64_t)c.id, c.currentSlot});
            if (c.currentSlot >= 0 && c.readySem) {
                extras.waitSems.push_back(c.readySem);
                extras.waitValues.push_back(c.currentReady);
            }
            if (c.pendingConsumed && c.consumedSem) {
                extras.signalSems.push_back(c.consumedSem);
                extras.signalValues.push_back(c.pendingConsumed);
                c.lastConsumedSignal = c.pendingConsumed;
                c.pendingConsumed = 0;
            }
        }

        // M24: deliver consumer views. Pick a reusable ring slot (the consumer
        // has finished reading it: consumedFence >= the value we signaled for
        // it), record the blit + this frame's readyFence signal.
        // M25: only while a presentation is ACTIVE. A connected client is not
        // enough: receivers (Miracast/AirPlay) dial in and hold an idle ring
        // long before anyone casts, so gate on a client actually having
        // delivered a frame (currentSlot >= 0) - otherwise the virtual camera
        // would broadcast an empty scene, which is what the first appliance
        // smoke test showed.
        bool presenting = false;
        for (auto& cp : clients)
            if (cp->currentSlot >= 0) {
                presenting = true;
                break;
            }

        // M28: keep the display awake while a presentation is live.
        //
        // Not cosmetic. A sleeping display stops the compositor consuming
        // swapchain images, so FIFO vkQueuePresentKHR blocks ~134 ms and the
        // ENTIRE scene collapses to ~7 fps - measured on this box after its
        // monitor had been asleep for three hours, with the GPU otherwise
        // finishing frames instantly (136k fps under IMMEDIATE). In a meeting
        // room that is a live failure: the display sleeps after five idle
        // minutes, someone casts, and every producer's ring starves against a
        // 7 fps consumer.
        //
        // Held only while presenting, so an idle room display is still allowed
        // to sleep.
        static bool displayHeld = false;
        if (presenting != displayHeld) {
            SetThreadExecutionState(presenting ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
                                                  ES_DISPLAY_REQUIRED)
                                               : ES_CONTINUOUS);
            // ES_DISPLAY_REQUIRED only PREVENTS sleep - it will not wake a
            // display that is already off, which is exactly the state a room
            // appliance is in when someone starts casting. Waking needs input,
            // so synthesise a zero-delta mouse move: it resets the idle timer
            // and powers the panel without moving the user's cursor.
            // VKR_NO_DISPLAY_WAKE=1 opts out (useful on a dev box, where a
            // receiver stealing the display is just rude).
            static const bool wakeAllowed = getenv("VKR_NO_DISPLAY_WAKE") == nullptr;
            if (presenting && wakeAllowed) {
                INPUT in{};
                in.type = INPUT_MOUSE;
                in.mi.dwFlags = MOUSEEVENTF_MOVE;
                in.mi.dx = 0;
                in.mi.dy = 0;
                SendInput(1, &in, sizeof(in));
            }
            displayHeld = presenting;
            VKR_LOG("service: display wake %s (presentation %s)",
                    presenting ? "held" : "released", presenting ? "active" : "idle");
        }
        sceneOut.pendingSlot = -1;
        if (sceneOut.channel && presenting) {
            flBuffer buf{};
            // Non-blocking: if the consumer is behind, skip this frame rather
            // than stall the scene. framelink's ring is the back-pressure.
            if (flAcquireBuffer(sceneOut.channel, &buf, 0) == FL_OK) {
                const uint32_t slot = buf.slot;
                // Import once per slot; flImage is stable for the channel's
                // lifetime, so the VkImage can be cached against the slot.
                if (!sceneOut.images[slot]) {
                    void* h = flImageSharedHandle(buf.image);
                    if (!importWin32BlitTarget(vk, (uint64_t)(uintptr_t)h, sceneOut.width,
                                               sceneOut.height, &sceneOut.images[slot],
                                               &sceneOut.memories[slot])) {
                        VKR_ERR("service: scene blit-target import failed (slot %u)", slot);
                        sceneOut.images[slot] = VK_NULL_HANDLE;
                    }
                }
                uint64_t value = 0;
                if (sceneOut.images[slot] &&
                    flBeginSubmit(sceneOut.channel, &buf, &value) == FL_OK) {
                    sceneOut.pendingSlot = (int)slot;
                    sceneOut.pendingValue = value;
                    // The signal rides OUR Vulkan submit, which is the whole
                    // reason flSubmit is unusable here.
                    extras.blits.push_back({sceneOut.images[slot], sceneOut.width,
                                            sceneOut.height});
                    extras.signalSems.push_back(sceneOut.readySem);
                    extras.signalValues.push_back(value);
                }
            }
        }

        if (!host.frame(vk, pool, pass, passBound, streams, extras, &quit)) {
            exitCode = 1;
            break;
        }

        // The submit carrying the blit is queued and will signal the value we
        // reserved, so the frame can be published now.
        if (sceneOut.pendingSlot >= 0) {
            flBuffer buf{};
            buf.slot = (uint32_t)sceneOut.pendingSlot;
            buf.image = nullptr; // only the slot matters to flEndSubmit
            if (flEndSubmit(sceneOut.channel, &buf, sceneOut.pendingValue, nowNs()) !=
                FL_OK)
                teardownSceneOut(sceneOut);
        }

        if (nowNs() - rateWindowStart > 5000000000ll) {
            VKR_LOG("service: %llu adoptions in 5s (pool %zu slots, %zu clients)",
                    (unsigned long long)adoptions, pool.slots.size(), clients.size());
            adoptions = 0;
            rateWindowStart = nowNs();
        }
    }

    vkDeviceWaitIdle(vk.device);
    if (headless && exitCode == 0 && videoFramesSeen >= options.headlessFrames)
        vkrDumpTarget(vk, host.commandPool(), options.dumpPath.c_str());

    for (auto& c : clients) teardownClient(*c);
    clients.clear();
    teardownSceneOut(sceneOut);
    host.destroy(vk);
    pass.destroy(vk);
    pool.destroy(vk);
    vk.destroy();
    return exitCode;
}

} // namespace vkr

#endif // _WIN32
