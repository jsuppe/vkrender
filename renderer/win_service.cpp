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

// M42: the moderation API (control_api.h) and the state it drives.
#include "control_api.h"

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

// M42: what the moderator has decided. Owned and applied by the render loop;
// the control API only queues commands that mutate it.
//
// The interesting part is SELECTION: which of the connected, non-hidden
// streams actually reach the layout each frame. Spotlight wins outright;
// otherwise pinned streams take their seats first and the remainder rotate
// through the leftover seats every rotateSeconds - the carousel that lets a
// room cap the tiles at 4 while every presenter still gets screen time.
struct Moderation {
    bool hidden[kInSlots] = {};
    bool pinned[kInSlots] = {};
    int spotlight = -1;                  // slot index, -1 = none
    uint32_t maxVisible = 4;
    double rotateSeconds = 8.0;
    std::vector<uint32_t> order;         // slot indices, front = first seated

    void ensureOrdered(uint32_t slot) {
        for (uint32_t o : order)
            if (o == slot) return;
        order.push_back(slot);
    }
    void toFront(uint32_t slot) {
        ensureOrdered(slot);
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == slot) {
                order.erase(order.begin() + (ptrdiff_t)i);
                break;
            }
        order.insert(order.begin(), slot);
    }

    // The seated slots for this frame, in seating order.
    std::vector<uint32_t> select(const bool live[kInSlots], int64_t nowNsV) const {
        std::vector<uint32_t> out;
        if (spotlight >= 0 && spotlight < (int)kInSlots && live[spotlight] &&
            !hidden[spotlight]) {
            out.push_back((uint32_t)spotlight);
            return out;
        }
        std::vector<uint32_t> eligible; // ordered, live, not hidden
        for (uint32_t o : order)
            if (o < kInSlots && live[o] && !hidden[o]) eligible.push_back(o);
        // Pinned first, in order.
        std::vector<uint32_t> rest;
        for (uint32_t e : eligible) {
            if (pinned[e] && out.size() < maxVisible) out.push_back(e);
            else if (!pinned[e]) rest.push_back(e);
        }
        const size_t seats = maxVisible > out.size() ? maxVisible - out.size() : 0;
        if (rest.size() <= seats) {
            out.insert(out.end(), rest.begin(), rest.end());
        } else if (seats > 0) {
            // The carousel: the window over `rest` advances one PAGE per
            // period, so every presenter gets equal time and the transition
            // is the presentation layout's own enter/exit animation.
            const uint64_t page =
                (uint64_t)((double)nowNsV * 1e-9 / (rotateSeconds > 0.5 ? rotateSeconds : 0.5));
            const size_t start = (size_t)((page * seats) % rest.size());
            for (size_t i = 0; i < seats; ++i)
                out.push_back(rest[(start + i) % rest.size()]);
        }
        return out;
    }
};

} // namespace

int runWinService(const WinServeOptions& options) {
    // N > 0: finite verification (wait for N frames, dump, exit; 60 s
    // deadline). N < 0: ENDLESS headless service - the appliance's vcam-only
    // mode, where the scene exists solely as the composite the virtual camera
    // broadcasts and no window is ever created.
    const bool headless = options.headlessFrames != 0;
    const bool finite = options.headlessFrames > 0;

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

    // M42: the moderation API + the state it drives. Per-slot adoption
    // bookkeeping feeds the roster (fps, last-frame age).
    ControlApi control;
    const bool controlUp = options.controlPort > 0 && control.start(options.controlPort);
    Moderation mod;
    // M43: the moderator's camera gate. Off = the scene stops publishing to
    // the vcam channel; framecam times out and shows its idle card, so the
    // meeting sees "room camera idle", not a frozen frame.
    bool cameraOn = true;
    int64_t slotLastFrameNs[kInSlots] = {};
    uint32_t slotFrameCount[kInSlots] = {};
    float slotFps[kInSlots] = {};
    int64_t fpsWindowStart = nowNs();

    // Kick: transport-level and uniform across every receiver type - close the
    // slot's channel and recreate it. The producer sees FL_DISCONNECTED on its
    // next call; the fresh channel is empty and waiting.
    auto kickSlot = [&](uint32_t i) {
        if (i >= kInSlots || !inSlots[i].channel) return;
        SlotIn& s = inSlots[i];
        VKR_LOG("service: moderator kicked '%s'", s.name.c_str());
        dropRing(s);
        flClose(s.channel);
        s.channel = nullptr;
        s.departing = false;
        s.generation = 0;
        s.currentSlot = -1;
        if (flCreateChannelOnAdapter(s.name.c_str(), kInDefaultW, kInDefaultH, FL_FORMAT_BGRA8,
                                     kInPool, FL_GPU_SYNC,
                                     vk.luidValid ? vk.deviceLUID : nullptr,
                                     &s.channel) != FL_OK) {
            VKR_ERR("service: could not recreate '%s' after kick", s.name.c_str());
            s.channel = nullptr;
        }
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
            if (finite && videoFramesSeen >= options.headlessFrames) break;
            if (finite && nowNs() > headlessDeadline) {
                VKR_ERR("service headless: no %d frames within 60 s (saw %d)",
                        options.headlessFrames, videoFramesSeen);
                exitCode = 2;
                break;
            }
            // No vsync without a swapchain; this sleep IS the frame pacing.
            // M43: unless a display output is attached - then its FIFO
            // present paces the loop at the panel's refresh instead.
            if (host.activeDisplayCount() == 0)
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
            slotLastFrameNs[&s - inSlots.data()] = nowNs();
            ++slotFrameCount[&s - inSlots.data()];
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

        // M42: apply the moderator's commands before deciding what is shown.
        for (const ControlCommand& c : control.drainCommands()) {
            using T = ControlCommand::Type;
            const uint32_t sl = c.slot >= 0 && c.slot < (int)kInSlots ? (uint32_t)c.slot : 0;
            switch (c.type) {
                case T::Layout:
                    if (c.str == "presentation") host.setLayout(SceneLayout::Presentation);
                    else if (c.str == "two-cubes") host.setLayout(SceneLayout::TwoCubes);
                    else if (c.str == "single-cube") host.setLayout(SceneLayout::SingleCube);
                    else if (c.str == "wall") host.setLayout(SceneLayout::Wall);
                    break;
                case T::Pause: host.setPaused(c.i != 0); break;
                case T::Config:
                    if (c.i >= 1 && c.i <= (int)kInSlots) mod.maxVisible = (uint32_t)c.i;
                    if (c.d >= 1.0) mod.rotateSeconds = c.d;
                    break;
                case T::Hide: mod.hidden[sl] = true; break;
                case T::Show: mod.hidden[sl] = false; break;
                case T::Kick: kickSlot(sl); break;
                case T::Pin: mod.pinned[sl] = true; mod.toFront(sl); break;
                case T::Unpin: mod.pinned[sl] = false; break;
                case T::Spotlight: mod.spotlight = (int)sl; break;
                case T::Unspotlight: mod.spotlight = -1; break;
                case T::Front: mod.toFront(sl); break;
                case T::Camera:
                    cameraOn = c.i != 0;
                    VKR_LOG("service: camera %s by moderator", cameraOn ? "on" : "off");
                    break;
                case T::Display:
                    // c.slot is the monitor id here. Runs on the render thread
                    // - the one place swapchains may be built and torn down.
                    host.setDisplayActive(vk, c.slot, c.i != 0);
                    break;
            }
        }

        std::vector<SceneHost::StreamRef> streams;
        SceneHost::Extras extras;
        // Reap departures and record liveness. Consumed-fence signals are
        // issued for EVERY live slot - hidden or rotated-out producers must
        // keep flowing, or their rings fill and they stall behind a moderator
        // decision they cannot see.
        bool live[kInSlots] = {};
        for (SlotIn& s : inSlots) {
            const size_t idx = (size_t)(&s - inSlots.data());
            if (s.currentSlot < 0) continue;
            const bool expired =
                s.departing && (nowNs() - s.departedNs) > 3000000000ll; // 3 s
            if (s.departing && (expired || !host.retainsStream((uint64_t)s.uid))) {
                dropRing(s);
                s.departing = false;
                s.generation = 0; // force a re-import when someone attaches
                continue;
            }
            if (s.departing) continue;
            live[idx] = true;
            mod.ensureOrdered((uint32_t)idx);
            if (s.pendingConsumed && s.consumedSem) {
                extras.signalSems.push_back(s.consumedSem);
                extras.signalValues.push_back(s.pendingConsumed);
                s.lastConsumedSignal = s.pendingConsumed;
                s.pendingConsumed = 0;
            }
        }
        // Selection: spotlight, then pinned, then the carousel over the rest
        // (see Moderation::select). Only the SELECTED streams reach the layout
        // - and only their ready fences are waited on, because only their
        // textures are sampled this frame.
        const std::vector<uint32_t> seated = mod.select(live, nowNs());
        for (uint32_t idx : seated) {
            SlotIn& s = inSlots[idx];
            streams.push_back({(uint64_t)s.uid, s.currentSlot});
            if (s.readySem) {
                extras.waitSems.push_back(s.readySem);
                extras.waitValues.push_back(s.currentReady);
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
        // Headless (vcam-only) needs the SYSTEM held awake while presenting -
        // the camera must keep pumping into the meeting - but has no display
        // to wake and no swapchain for a sleeping panel to starve, so the
        // panel is left alone entirely.
        // M43: a headless appliance WITH attached display outputs is back in
        // the "sleeping panel starves the swapchain" regime above, so the
        // display hold (and the wake nudge) applies whenever displays are out.
        const bool displaysOut = host.activeDisplayCount() > 0;
        static int powerHeld = 0; // 0 = none, 1 = system, 2 = system+display
        const int powerWant = !presenting ? 0 : (headless && !displaysOut) ? 1 : 2;
        if (powerWant != powerHeld) {
            SetThreadExecutionState(powerWant == 0
                                        ? ES_CONTINUOUS
                                        : powerWant == 1
                                              ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
                                              : (ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
                                                 ES_DISPLAY_REQUIRED));
            // ES_DISPLAY_REQUIRED only PREVENTS sleep - it will not wake a
            // display that is already off, which is exactly the state a room
            // appliance is in when someone starts casting. Waking needs input,
            // so synthesise a zero-delta mouse move: it resets the idle timer
            // and powers the panel without moving the user's cursor.
            // VKR_NO_DISPLAY_WAKE=1 opts out (useful on a dev box, where a
            // receiver stealing the display is just rude).
            static const bool wakeAllowed = getenv("VKR_NO_DISPLAY_WAKE") == nullptr;
            if (powerWant == 2 && wakeAllowed) {
                INPUT in{};
                in.type = INPUT_MOUSE;
                in.mi.dwFlags = MOUSEEVENTF_MOVE;
                in.mi.dx = 0;
                in.mi.dy = 0;
                SendInput(1, &in, sizeof(in));
            }
            powerHeld = powerWant;
            VKR_LOG("service: %s %s (presentation %s)",
                    powerWant == 1 ? "system wake" : "display wake",
                    powerWant ? "held" : "released", presenting ? "active" : "idle");
        }
        sceneOut.pendingSlot = -1;
        if (sceneOut.channel && presenting && cameraOn) {
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

        // M42: publish the moderation snapshot + refresh the live preview when
        // the console is watching. The readback happens HERE, between frames
        // on the render thread - the one place it is legal - and only when a
        // preview was requested in the last few seconds.
        if (controlUp) {
            if (control.previewDue()) {
                std::vector<uint8_t> bgra;
                if (vkrReadImage(vk, host.commandPool(),
                                 headless ? vk.swapImages[0] : vk.swapImages[0], vk.swapExtent,
                                 headless ? VK_IMAGE_LAYOUT_GENERAL
                                          : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                 bgra))
                    control.publishPreview(bgra.data(), vk.swapExtent.width,
                                           vk.swapExtent.height);
            }
            ControlSnapshot snap;
            switch (host.layout()) {
                case SceneLayout::Presentation: snap.layout = "presentation"; break;
                case SceneLayout::TwoCubes: snap.layout = "two-cubes"; break;
                case SceneLayout::SingleCube: snap.layout = "single-cube"; break;
                case SceneLayout::Wall: snap.layout = "wall"; break;
                default: snap.layout = "?"; break;
            }
            snap.paused = host.paused();
            snap.maxVisible = mod.maxVisible;
            snap.rotateSeconds = mod.rotateSeconds;
            snap.spotlightSlot = mod.spotlight;
            snap.cameraOn = cameraOn;
            snap.displaysAvailable = vk.canDisplay;
            for (const auto& d : host.listDisplays()) {
                ControlDisplayInfo cd;
                cd.id = d.id;
                cd.name = d.name;
                cd.x = d.x;
                cd.y = d.y;
                cd.width = d.width;
                cd.height = d.height;
                cd.primary = d.primary;
                cd.active = d.active;
                cd.u0 = d.u0;
                cd.v0 = d.v0;
                cd.u1 = d.u1;
                cd.v1 = d.v1;
                snap.displays.push_back(std::move(cd));
            }
            for (uint32_t i = 0; i < kInSlots; ++i) {
                SlotIn& sl = inSlots[i];
                if (sl.currentSlot < 0 && !sl.channel) continue;
                ControlStreamInfo st;
                st.slot = i;
                st.width = sl.width;
                st.height = sl.height;
                st.fps = slotFps[i];
                st.ageMs = slotLastFrameNs[i]
                               ? (nowNs() - slotLastFrameNs[i]) / 1000000ll
                               : -1;
                st.connected = sl.currentSlot >= 0 && !sl.departing;
                st.hidden = mod.hidden[i];
                st.pinned = mod.pinned[i];
                st.spotlit = mod.spotlight == (int)i;
                for (uint32_t v : seated)
                    if (v == i) st.visible = true;
                if (st.connected || st.hidden || st.pinned) snap.streams.push_back(st);
                snap.presenting = snap.presenting || st.connected;
            }
            control.publish(snap);
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

        if (nowNs() - fpsWindowStart > 2000000000ll) {
            const double secs = (double)(nowNs() - fpsWindowStart) * 1e-9;
            for (uint32_t i = 0; i < kInSlots; ++i) {
                slotFps[i] = (float)((double)slotFrameCount[i] / secs);
                slotFrameCount[i] = 0;
            }
            fpsWindowStart = nowNs();
        }
        if (nowNs() - rateWindowStart > 5000000000ll) {
            size_t liveCount = 0;
            for (SlotIn& s : inSlots)
                if (s.currentSlot >= 0) ++liveCount;
            if (hopSamples) {
                VKR_LOG("service: %llu adoptions in 5s (pool %zu slots, %zu producers) "
                        "hop avg %.2f ms max %.2f ms over %llu",
                        (unsigned long long)adoptions, pool.slots.size(), liveCount,
                        (double)hopTotalUs / (double)hopSamples / 1000.0,
                        (double)hopMaxUs / 1000.0, (unsigned long long)hopSamples);
            } else {
                VKR_LOG("service: %llu adoptions in 5s (pool %zu slots, %zu producers)",
                        (unsigned long long)adoptions, pool.slots.size(), liveCount);
            }
            adoptions = 0;
            hopSamples = hopTotalUs = hopMaxUs = 0;
            rateWindowStart = nowNs();
        }
    }

    vkDeviceWaitIdle(vk.device);
    if (finite && exitCode == 0 && videoFramesSeen >= options.headlessFrames)
        vkrDumpTarget(vk, host.commandPool(), options.dumpPath.c_str());

    if (controlUp) control.stop();
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
