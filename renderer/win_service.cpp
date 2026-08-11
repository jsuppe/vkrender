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

// M38: an INPUT channel - the receiver side of the migration
// (docs/MIGRATION.md). The renderer pre-creates kInSlots of these and
// receivers attach as framelink producers, walking the list on FL_BUSY. A
// producer cannot create a channel, deliberately, so somebody has to have
// created them first; slot channels are how, and it is the model the Android
// service already ships (acquireInputSurfaceSlot).
//
// FL_GPU_SYNC because we composite with Vulkan: blocking the render thread on
// a ready fence twelve times a frame is precisely the latency this migration
// is not allowed to add. We wait and signal both fences inside our own submit
// instead, exactly as the client rings do.
struct SlotIn {
    flChannel* channel = nullptr;
    std::string name;
    uint32_t uid = 0;        // scene stream id; stable for the slot's lifetime
    uint64_t generation = 0; // which ring we imported - see flChannelInfo
    uint32_t width = 0, height = 0;
    VkSemaphore readySem = VK_NULL_HANDLE, consumedSem = VK_NULL_HANDLE;
    bool imported[proto::kMaxPoolSize] = {}; // ring slot -> already in the pool
    int64_t currentSlot = -1;                // pool key, or -1
    uint64_t currentReady = 0;
    uint64_t lastConsumedSignal = 0;
    uint64_t pendingConsumed = 0; // 0 = nothing to signal this frame
    bool departing = false;
    int64_t departedNs = 0;
};

// Twelve, because that is the fan-in M22 proved and the scene supports.
constexpr uint32_t kInSlots = 12;

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

    // M38: no D3D11 device here any more. The service used to keep one to
    // allocate every client's ring; framelink owns that now, and it allocates
    // on the adapter we name below.

    uint32_t nextStreamId = 1;
    bool passBound = false;
    bool passNeedsBind = false;
    uint64_t adoptions = 0;
    // M37 baseline: producer-submit -> renderer-adopt latency, the ONE number
    // the framelink migration must not regress (docs/MIGRATION.md). Producers
    // stamp steady_clock ns into ptsNs; MSVC's steady_clock is QPC-based and
    // shares an epoch across processes, so the subtraction is meaningful.
    // Ignored unless the producer opts in by stamping a plausible value.
    uint64_t hopSamples = 0, hopTotalUs = 0, hopMaxUs = 0;
    int videoFramesSeen = 0;
    int64_t rateWindowStart = nowNs();
    const int64_t headlessDeadline = nowNs() + 60ll * 1000000000ll;
    int exitCode = 0;

    // ---- M38: framelink input slots -------------------------------------
    //
    // Created up front so a receiver has something to attach to. They are
    // cheap while idle: the ring is allocated at a placeholder size and the
    // first producer replaces it with flRequestGeometry, which is the only
    // reason the placeholder can be small.
    std::vector<SlotIn> inSlots(kInSlots);
    const uint32_t kInDefaultW = 640, kInDefaultH = 360, kInPool = 4;

    auto ensurePool = [&]() -> bool {
        if (pool.valid()) return true;
        proto::PoolDescMsg header{};
        header.width = kInDefaultW;
        header.height = kInDefaultH;
        header.format = proto::SharedFormat::BGRA8;
        header.matrix = proto::Matrix::BT709;
        header.range = proto::Range::Full;
        header.poolSize = 0; // dynamic slots
        header.planeCount = 1;
        if (!pool.create(vk, header)) return false;
        passNeedsBind = true;
        return true;
    };

    // Drop everything imported from this slot's current ring. Called when the
    // generation moves, which on Windows means either a reallocation or a new
    // producer - both hand out new shared handles AND a new fence pair.
    auto dropRing = [&](SlotIn& s) {
        if (!s.readySem && s.currentSlot < 0 && s.generation == 0) return;
        vkDeviceWaitIdle(vk.device);
        for (uint32_t i = 0; i < proto::kMaxPoolSize; ++i) {
            if (!s.imported[i]) continue;
            const uint32_t key = (s.uid << 4) | i;
            pass.removeSlot(vk, key);
            pool.removeSlot(vk, key);
            s.imported[i] = false;
        }
        if (s.readySem) vkDestroySemaphore(vk.device, s.readySem, nullptr);
        if (s.consumedSem) vkDestroySemaphore(vk.device, s.consumedSem, nullptr);
        s.readySem = s.consumedSem = VK_NULL_HANDLE;
        s.currentSlot = -1;
        s.currentReady = 0;
        s.lastConsumedSignal = 0;
        s.pendingConsumed = 0;
    };

    // Import the ring's fence pair as timeline semaphores. Both of them: under
    // FL_GPU_SYNC framelink signals nothing itself, so the consumed fence is
    // ours to advance once our reads have actually completed.
    auto importSync = [&](SlotIn& s) -> bool {
        void* ready = nullptr;
        void* consumed = nullptr;
        if (flSharedReadyFence(s.channel, &ready) != FL_OK ||
            flSharedConsumedFence(s.channel, &consumed) != FL_OK)
            return false;
        return importTimelineSemaphoreWin32(vk, (uint64_t)(uintptr_t)ready, &s.readySem) &&
               importTimelineSemaphoreWin32(vk, (uint64_t)(uintptr_t)consumed, &s.consumedSem);
    };

    for (uint32_t i = 0; i < kInSlots; ++i) {
        SlotIn& s = inSlots[i];
        s.name = options.pipeName + ".in." + std::to_string(i);
        s.uid = nextStreamId++;
        // On OUR adapter, explicitly. framelink would otherwise take the first
        // one DXGI enumerates, which on a laptop with an integrated and a
        // discrete GPU is not the one our Vulkan device is on - and then every
        // import of these buffers fails.
        if (flCreateChannelOnAdapter(s.name.c_str(), kInDefaultW, kInDefaultH,
                                     FL_FORMAT_BGRA8, kInPool, FL_GPU_SYNC,
                                     vk.luidValid ? vk.deviceLUID : nullptr,
                                     &s.channel) != FL_OK) {
            VKR_ERR("service: input channel '%s' creation failed", s.name.c_str());
            s.channel = nullptr;
        }
    }
    VKR_LOG("service: %u framelink input slots ready ('%s.in.0' .. '%s.in.%u')", kInSlots,
            options.pipeName.c_str(), options.pipeName.c_str(), kInSlots - 1);

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

        attachSceneOut(sceneOut);

        // M38: poll the framelink input slots. Non-blocking (timeout 0): a
        // slot with nothing new must cost the render thread nothing, and the
        // ready fence is waited on by the GPU, not here.
        for (SlotIn& s : inSlots) {
            if (!s.channel) continue;

            flChannelInfo info{};
            flQuery(s.channel, &info);
            if (info.generation != s.generation) {
                // Everything we imported from the old ring is gone - textures
                // and the fence pair both. Re-import lazily as frames arrive.
                dropRing(s);
                s.generation = info.generation;
                s.width = info.width;
                s.height = info.height;
                if (!importSync(s)) {
                    VKR_ERR("service: '%s' fence import failed", s.name.c_str());
                    s.generation = 0; // try again next frame
                    continue;
                }
            }

            flFrame f{};
            const flResult r = flAcquireFrame(s.channel, &f, 0);
            if (r == FL_DISCONNECTED) {
                // M31 again: hold the ring so the scene's exit animation has
                // something to sample. The slot itself stays; the next
                // producer bumps the generation and we re-import then.
                if (!s.departing && s.currentSlot >= 0) {
                    s.departing = true;
                    s.departedNs = nowNs();
                }
                continue;
            }
            if (r != FL_OK) continue;

            // A reallocation between the flQuery above and here would leave us
            // holding a frame from a ring we have not imported. Drop it; the
            // next iteration re-imports and the producer resends.
            flChannelInfo now{};
            flQuery(s.channel, &now);
            if (now.generation != s.generation) {
                flRelease(s.channel, &f);
                continue;
            }

            if (!ensurePool()) {
                flRelease(s.channel, &f);
                continue;
            }
            const uint32_t key = (s.uid << 4) | f.slot;
            if (!s.imported[f.slot]) {
                void* h = flImageSharedHandle(f.image);
                if (!pool.addSurfaceWin32(vk, key, (uint64_t)(uintptr_t)h, s.width, s.height)) {
                    VKR_ERR("service: '%s' slot %u import failed", s.name.c_str(), f.slot);
                    flRelease(s.channel, &f);
                    continue;
                }
                if (passNeedsBind) {
                    passBound = pass.bindPool(vk, pool);
                    passNeedsBind = false;
                } else if (!pass.addSlot(vk, pool, key)) {
                    VKR_ERR("service: '%s' slot %u bind failed", s.name.c_str(), f.slot);
                    flRelease(s.channel, &f);
                    continue;
                }
                s.imported[f.slot] = true;
            }

            s.departing = false;
            s.currentSlot = (int64_t)key;
            s.currentReady = f._readyValue;
            // Release the PREVIOUS frame, not this one: the scene keeps
            // sampling this texture until a newer frame replaces it, so
            // telling the producer it is free would let it be overwritten
            // mid-draw. The client rings do exactly the same.
            if (f._readyValue > 1 && f._readyValue - 1 > s.lastConsumedSignal)
                s.pendingConsumed = f._readyValue - 1;
            ++adoptions;
            ++videoFramesSeen;
            if (f.ptsNs > 0) {
                const int64_t d = nowNs() - f.ptsNs;
                if (d >= 0 && d < 1000000000ll) {
                    const uint64_t us = (uint64_t)(d / 1000);
                    ++hopSamples;
                    hopTotalUs += us;
                    if (us > hopMaxUs) hopMaxUs = us;
                }
            }
            // Bookkeeping only under FL_GPU_SYNC - it signals nothing. The
            // real release is the consumed-fence signal in our submit below.
            flRelease(s.channel, &f);
        }

        std::vector<SceneHost::StreamRef> streams;
        SceneHost::Extras extras;
        for (SlotIn& s : inSlots) {
            if (s.currentSlot < 0) continue;
            // Same rule as a departing client: withhold the stream so the
            // motion layer animates it out, but keep the ring alive for it.
            const bool expired =
                s.departing && (nowNs() - s.departedNs) > 3000000000ll; // 3 s
            if (s.departing && (expired || !host.retainsStream((uint64_t)s.uid))) {
                dropRing(s);
                s.departing = false;
                s.generation = 0; // force a re-import when someone attaches
                continue;
            }
            if (s.departing) continue;
            streams.push_back({(uint64_t)s.uid, s.currentSlot});
            if (s.readySem) {
                extras.waitSems.push_back(s.readySem);
                extras.waitValues.push_back(s.currentReady);
            }
            if (s.pendingConsumed && s.consumedSem) {
                extras.signalSems.push_back(s.consumedSem);
                extras.signalValues.push_back(s.pendingConsumed);
                s.lastConsumedSignal = s.pendingConsumed;
                s.pendingConsumed = 0;
            }
        }
        // M24/M35: publish the scene view for sinks (the camera). Pick a
        // reusable ring slot and record the blit + this frame's ready signal.
        // M25: only while a presentation is ACTIVE. An ATTACHED producer is not
        // enough: receivers (Miracast/AirPlay) take a slot at startup and hold
        // it idle long before anyone casts, so gate on one having actually
        // delivered a frame - otherwise the virtual camera broadcasts an empty
        // scene, which is what the first appliance smoke test showed.
        bool presenting = false;
        for (SlotIn& s : inSlots)
            if (s.currentSlot >= 0 && !s.departing) {
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
            size_t live = 0;
            for (SlotIn& s : inSlots)
                if (s.currentSlot >= 0) ++live;
            if (hopSamples) {
                VKR_LOG("service: %llu adoptions in 5s (pool %zu slots, %zu producers) "
                        "hop avg %.2f ms max %.2f ms over %llu",
                        (unsigned long long)adoptions, pool.slots.size(), live,
                        (double)hopTotalUs / (double)hopSamples / 1000.0,
                        (double)hopMaxUs / 1000.0, (unsigned long long)hopSamples);
            } else {
                VKR_LOG("service: %llu adoptions in 5s (pool %zu slots, %zu producers)",
                        (unsigned long long)adoptions, pool.slots.size(), live);
            }
            adoptions = 0;
            hopSamples = hopTotalUs = hopMaxUs = 0;
            rateWindowStart = nowNs();
        }
    }

    vkDeviceWaitIdle(vk.device);
    if (headless && exitCode == 0 && videoFramesSeen >= options.headlessFrames)
        vkrDumpTarget(vk, host.commandPool(), options.dumpPath.c_str());

    for (SlotIn& s : inSlots) {
        dropRing(s);
        if (s.channel) flClose(s.channel);
        s.channel = nullptr;
    }
    teardownSceneOut(sceneOut);
    host.destroy(vk);
    pass.destroy(vk);
    pool.destroy(vk);
    vk.destroy();
    return exitCode;
}

} // namespace vkr

#endif // _WIN32
