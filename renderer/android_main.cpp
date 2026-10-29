#include "gpu_trace.h"
#include "portable_output.h"
#include "portable_input.h"
#include <sstream>
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
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <volk.h>

#include "framelink.h"
#include "framelink_sync_fd.h"
#include "framelink_ahb.h"

#include "background_pass.h"
#include "external_video_image.h"
#include "log.h"
#include "scene_layout.h"
#include "video_pass.h"
#include "vk_context.h"

using vkr::cubeMvp;
using vkr::Mat4;

namespace {

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Mat4/cubeMvp live in scene_math.h (shared with the desktop compositor).

// ---- Multi-producer stream registry (M11/M12/M22) --------------------------
// One AImageReader per stream, keyed by (Binder calling UID, slot). Slot 0 is
// the classic one-stream-per-app case (M11); M22 lets one client app (the
// WHIP room) hold several slots - one per presenter. Created on demand from
// the Binder thread; images are acquired/imported/released only on the render
// thread. Up to one stream per cube face, two cubes.
constexpr size_t kMaxStreams = 12;

// (uid, slot) packed into the stream key.
inline int64_t streamKey(int uid, int slot) {
    return ((int64_t)uid << 32) | (uint32_t)slot;
}

struct InputStream {
    int64_t key = -1;
    uint64_t sceneId = 0; // unique across release/reacquire of the same UID/slot
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
uint64_t g_nextSceneId = 1;
std::vector<int64_t> g_pendingRelease; // stream keys to tear down on the render thread

bool extractShaders(android_app* app, const std::string& outDir) {
    const char* names[] = {"fullscreen.vert.spv", "cube.vert.spv", "vaporwave.frag.spv",
                           "video_ycbcr.frag.spv", "nv12_planar.frag.spv", "nv12_cube.frag.spv"};
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
    FrameGpuTrace gpuTrace;
    VkSemaphore acquireSem = VK_NULL_HANDLE;
    VkSemaphore renderDoneSem = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    // Images whose last GPU read is fenced by this frame; deleted (returned
    // to the BufferQueue) once the fence is waited.
    std::vector<AImage*> retiring;
    std::vector<std::unique_ptr<InputStream>> retiringStreams;
};

struct RendererState {
    bool windowReady = false;
    bool destroyed = false;
    vkr::SceneLayout layout=vkr::SceneLayout::TwoCubes;
    bool paused=false, backdrop=true;
    float speed=1.0f;
    uint32_t disabled=0;
};

// ---- M40: framelink input slots ---------------------------------------------
//
// The Android twin of the desktop slot channels: vkrender.in.0 .. .in.11,
// pre-created so a native producer has something to attach to, consumed on the
// render thread beside the AIDL streams. The AIDL Surface API STAYS - it is
// the right interface for an ordinary Android app - framelink is the
// native-code path alongside it, and both feed the same scene.
//
// RGBA8 because AHardwareBuffer has no BGRA format; FL_MAP_CPU because a
// native CPU producer is the first integrator this serves.
//
// Per-buffer sampler variants allow RGB and YUV streams in the same scene.
struct FlSlotIn {
    flChannel* channel = nullptr;
    std::string name;
    uint64_t generation = 0;
    uint32_t width = 0, height = 0;
    uint64_t lastReady=0;
    bool departing=false;
    std::vector<std::pair<uint64_t,uint64_t>> retire; // frame fence index, watermark
    bool imported[8] = {}; // ring slot -> pool key valid (fl kMaxPool is 8)
    uint32_t keys[8] = {};
    int64_t currentSlot = -1;
};

constexpr uint32_t kFlSlots = 12;

int32_t onInput(android_app* app,AInputEvent* event) {
    if(AInputEvent_getType(event)!=AINPUT_EVENT_TYPE_KEY || AKeyEvent_getAction(event)!=AKEY_EVENT_ACTION_DOWN)return 0;
    auto& s=*(RendererState*)app->userData;
    int key=AKeyEvent_getKeyCode(event);
    if(key==AKEYCODE_L)s.layout=(vkr::SceneLayout)(((int)s.layout+1)%4);
    else if(key==AKEYCODE_SPACE)s.paused=!s.paused;
    else if(key==AKEYCODE_B)s.backdrop=!s.backdrop;
    else if(key==AKEYCODE_MINUS)s.speed=std::max(0.05f,s.speed/1.25f);
    else if(key==AKEYCODE_EQUALS)s.speed=std::min(8.0f,s.speed*1.25f);
    else if(key==AKEYCODE_0)s.disabled=0;
    else if(key>=AKEYCODE_1 && key<=AKEYCODE_9)s.disabled^=1u<<(key-AKEYCODE_1);
    else return 0;
    return 1;
}

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
// caller's existing stream window for (uid, slot), or creates a fresh
// AImageReader for a new stream (max one per cube face). Re-acquire after a
// legacy client crash returns the same reader. Tracked owners explicitly
// release on death. The returned window reference belongs to the JNI caller.
ANativeWindow* vkrAcquireStreamWindow(int uid, int slot) {
    const int64_t key = streamKey(uid, slot);
    std::unique_lock<std::mutex> lock(g_streamsMutex);
    g_readerCv.wait_for(lock, std::chrono::seconds(5), [] { return g_rendererReady; });
    if (!g_rendererReady) return nullptr;
    // A release for this stream may still be queued (release + immediate
    // re-acquire): wait until the render thread has torn it down, else we
    // would hand back a window that is about to be destroyed.
    g_readerCv.wait_for(lock, std::chrono::seconds(2), [key] {
        return std::find(g_pendingRelease.begin(), g_pendingRelease.end(), key) ==
               g_pendingRelease.end();
    });
    if (std::find(g_pendingRelease.begin(), g_pendingRelease.end(), key) !=
        g_pendingRelease.end())
        return nullptr; // render thread paused; client will retry
    for (auto& s : g_streams)
        if (s->key == key) {
            ANativeWindow_acquire(s->window);
            return s->window;
        }
    if (g_streams.size() >= kMaxStreams) {
        VKR_WARN("renderer: stream request from uid %d slot %d rejected (%zu faces occupied)",
                 uid, slot, g_streams.size());
        return nullptr;
    }
    auto s = std::make_unique<InputStream>();
    s->key = key;
    s->sceneId = g_nextSceneId++;
    media_status_t ms = AImageReader_newWithUsage(1920, 1080, AIMAGE_FORMAT_PRIVATE,
                                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 6,
                                                  &s->reader);
    if (ms != AMEDIA_OK || !s->reader) {
        VKR_ERR("AImageReader_newWithUsage failed for uid %d slot %d: %d", uid, slot, ms);
        return nullptr;
    }
    AImageReader_getWindow(s->reader, &s->window);
    ANativeWindow* window = s->window;
    ANativeWindow_acquire(window);
    g_streams.push_back(std::move(s));
    VKR_LOG("renderer: stream created for uid %d slot %d (%zu total)", uid, slot,
            g_streams.size());
    return window;
}

// Called from the binder thread; actual teardown happens on the render thread
// (it must sequence with in-flight GPU work).
void vkrReleaseStream(int uid, int slot) {
    std::lock_guard<std::mutex> lock(g_streamsMutex);
    g_pendingRelease.push_back(streamKey(uid, slot));
}

void android_main(android_app* app) {
    RendererState state;
    app->userData = &state;
    app->onAppCmd = onAppCmd;
    app->onInputEvent = onInput;
    if(const char* layout=getenv("VKR_LAYOUT")) {
        const char* names[]={"two-cubes","single-cube","wall","presentation"};
        for(int i=0;i<4;i++)if(!strcmp(layout,names[i]))state.layout=(vkr::SceneLayout)i;
    }

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
    vkr::BackgroundPass background;
    if (!background.init(vk, filesDir)) VKR_WARN("renderer: vaporwave backdrop unavailable");

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
    std::vector<std::unique_ptr<InputStream>> departingStreams;
    std::vector<AImage*> newlyRetiring;
    std::vector<std::unique_ptr<InputStream>> newlyRetiringStreams;
    ANativeWindow* vkWindow = app->window;
    uint64_t adoptions = 0, renders = 0;
    int64_t rateWindowStart = nowNs();
    int64_t lastSceneNs=nowNs();
    float sceneTime=0;
    vkr::SceneMotion motion;
    std::vector<std::pair<uint64_t,int64_t>> tileSlots;
    std::vector<int64_t> sceneSlots;
    std::vector<uint64_t> sceneIds;

    auto keyForBuffer = [](AHardwareBuffer* ahb) {
        uint64_t v = (uint64_t)(uintptr_t)ahb;
        return (uint32_t)(v ^ (v >> 32));
    };

    // M40: the framelink input slots, created before the loop so a native
    // producer can attach whenever it likes. Hop instrumentation matches the
    // other platforms: producers stamp CLOCK_MONOTONIC into ptsNs.
    std::vector<FlSlotIn> flSlots(kFlSlots);
    uint64_t hopSamples = 0, hopTotalUs = 0, hopMaxUs = 0;
    for (uint32_t i = 0; i < kFlSlots; ++i) {
        FlSlotIn& s = flSlots[i];
        s.name = std::string("vkrender.in.") + std::to_string(i);
        if (flCreateChannelEx(s.name.c_str(), 640, 360, vkr::inputFormat(i,FL_FORMAT_RGBA8), 4, FL_MAP_CPU|FL_DEFER_RELEASE,
                              &s.channel) != FL_OK) {
            VKR_ERR("renderer: framelink channel '%s' creation failed", s.name.c_str());
            s.channel = nullptr;
        }
    }
    VKR_LOG("renderer: %u framelink input slots ready ('vkrender.in.0' .. 'vkrender.in.%u')",
            kFlSlots, kFlSlots - 1);

    // Poll the slots on the render thread. Import-once per (generation, ring
    // slot); release value-1 on every adoption so latest-wins skips free their
    // slots (the monotonic rule; see the framelink Linux backend's history for
    // why anything less deadlocks on a bursty producer).
    auto flPoll = [&](int64_t* faceActive, size_t* nActive) {
        for (FlSlotIn& s : flSlots) {
            if (!s.channel) continue;
            flChannelInfo info{};
            flQuery(s.channel, &info);
            if (info.generation != s.generation) {
                bool any = false;
                for (uint32_t i = 0; i < 8; ++i) any = any || s.imported[i];
                if (any) {
                    vkDeviceWaitIdle(vk.device);
                    for (uint32_t i = 0; i < 8; ++i) {
                        if (!s.imported[i]) continue;
                        pass.removeSlot(vk, s.keys[i]);
                        pool.removeSlot(vk, s.keys[i]);
                        s.imported[i] = false;
                    }
                }
                s.currentSlot = -1;
                s.generation = info.generation;
                s.retire.clear();s.lastReady=0;s.departing=false;
                s.width = info.width;
                s.height = info.height;
            }
            flFrame fr{};
            const flResult r = flAcquireFrame(s.channel, &fr, 0);
            if (r == FL_DISCONNECTED) {
                if(s.currentSlot>=0)s.departing=true; // retain image through presentation exit
                continue;
            }
            if (r != FL_OK) continue;
            flChannelInfo now{};
            flQuery(s.channel, &now);
            if (now.generation != s.generation || fr.slot >= 8) {
                flRelease(s.channel, &fr);
                continue;
            }
            AHardwareBuffer* ahb = flImageAHardwareBuffer(fr.image);
            if (!ahb) {
                flRelease(s.channel, &fr);
                continue;
            }
            if (!pool.headerValid) {
                vkr::proto::PoolDescMsg header{};
                header.width = s.width & ~1u;
                header.height = s.height & ~1u;
                header.format = vkr::proto::SharedFormat::BGRA8;
                header.matrix = vkr::proto::Matrix::BT709;
                header.range = vkr::proto::Range::Full;
                header.planeCount = 1;
                if (!pool.create(vk, header)) {
                    flRelease(s.channel, &fr);
                    continue;
                }
            }
            if (!s.imported[fr.slot]) {
                const uint32_t key = keyForBuffer(ahb);
                bool ok = pool.addSurfaceAHB(vk, key, ahb);
                if (ok && !passBound) {
                    ok = pass.bindPool(vk, pool);
                    passBound = ok;
                } else if (ok) {
                    ok = pass.addSlot(vk, pool, key);
                }
                if (!ok) {
                    VKR_ERR("renderer: framelink AHB import failed ('%s' slot %u)",
                            s.name.c_str(), fr.slot);
                    flRelease(s.channel, &fr);
                    continue;
                }
                s.imported[fr.slot] = true;
                s.keys[fr.slot] = key;
            }
            // The monotonic release: everything BEFORE this frame is free.
            s.departing=false;
            s.lastReady=fr._readyValue;
            if(fr._readyValue>1)s.retire.push_back({frameIndex+2,fr._readyValue-1});
            flRelease(s.channel,&fr); // bookkeeping; retire after the existing GPU fence
            s.currentSlot = (int64_t)s.keys[fr.slot];
            ++adoptions;
            if (fr.ptsNs > 0) {
                const int64_t d = nowNs() - fr.ptsNs;
                if (d >= 0 && d < 1000000000ll) {
                    const uint64_t us = (uint64_t)(d / 1000);
                    ++hopSamples;
                    hopTotalUs += us;
                    if (us > hopMaxUs) hopMaxUs = us;
                }
            }
        }
        for (FlSlotIn& s : flSlots)
            if (s.currentSlot >= 0 && *nActive < 12) faceActive[(*nActive)++] = s.currentSlot;
    };

    AChoreographer* choreographer = AChoreographer_getInstance();
    bool frameDue = false;

    vkr::PortableOutput portableOutput;
    if(const char* names=std::getenv("VKR_SCENE_OUTPUTS")) {
        std::istringstream list(names); std::string name; std::vector<std::string> outputs;
        while(std::getline(list,name,','))if(!name.empty())outputs.push_back(name);
        portableOutput.configure(outputs);
    }
    const char* traceLimitText=std::getenv("VKR_TRACE_FRAMES");
    const uint64_t traceLimit=traceLimitText?std::strtoull(traceLimitText,nullptr,10):0;
    while (!state.destroyed) {
        vkrtrace::Scope loopTrace("service.iteration");
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

        int64_t faceSlots[12];
        for (int fi = 0; fi < 12; ++fi) faceSlots[fi] = -1;
        size_t streamCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_streamsMutex);

            // Detach immediately so reacquire gets a new generation/reader.
            // Keep the old image/import alive while its scene tile exits.
            for (int64_t skey : g_pendingRelease) {
                for (auto it = g_streams.begin(); it != g_streams.end(); ++it) {
                    if ((*it)->key != skey) continue;
                    VKR_LOG("renderer: stream departing uid %d slot %d generation %llu",
                            (int)(skey >> 32), (int)skey,
                            (unsigned long long)(*it)->sceneId);
                    departingStreams.push_back(std::move(*it));
                    g_streams.erase(it);
                    break;
                }
            }
            g_pendingRelease.clear();
            g_readerCv.notify_all();

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
                            newlyRetiring.push_back(s.currentImage);
                        s.currentImage = image;
                        s.currentSlot = (int64_t)key;
                        ++adoptions;
                        image = nullptr; // ownership moved to the stream
                    }
                }
                if (image) AImage_delete(image); // import failed: return the buffer
            }

            // Face assignment: streams with content cycle across the twelve
            // faces (two cubes) in registry order (1 stream = everywhere,
            // 12 = one per face). M40: framelink slots join the same rotation.
            int64_t active[24];
            size_t nActive = 0;
            for (auto& sp : g_streams)
                if (sp->currentSlot >= 0 && nActive < 12) active[nActive++] = sp->currentSlot;
            flPoll(active, &nActive);
            if (nActive)
                for (int f = 0; f < 12; ++f) faceSlots[f] = active[f % nActive];
            sceneSlots.clear();sceneIds.clear();
            size_t ordinal=0;
            auto addScene=[&](uint64_t id,int64_t slot){
                if(slot<0)return;
                if(!(state.disabled&(1u<<ordinal))){sceneSlots.push_back(slot);sceneIds.push_back(id);}
                ++ordinal;
            };
            for(auto& sp:g_streams)addScene(sp->sceneId,sp->currentSlot);
            for(size_t i=0;i<flSlots.size();i++)if(!flSlots[i].departing)addScene((1ull<<63)|i,flSlots[i].currentSlot);
            streamCount = g_streams.size();
            for (FlSlotIn& fs : flSlots)
                if (fs.currentSlot >= 0) ++streamCount;
        }

        FrameSync& f = frames[frameIndex % kFramesInFlight];
        ++frameIndex;
        vkrtrace::call("scene.wait_inflight_fence",[&]{return vkWaitForFences(vk.device,1,&f.inFlight,VK_TRUE,UINT64_MAX);});
        f.gpuTrace.prepare(vk);
        portableOutput.retire(vk,(frameIndex-1)%2);
        for(auto& slot:flSlots)while(!slot.retire.empty() && slot.retire.front().first<=frameIndex) {
            flSignalConsumedSyncFd(slot.channel,slot.retire.front().second,-1);
            slot.retire.erase(slot.retire.begin());
        }
        // Fence retired: every submission recorded before it is done; images
        // parked on this frame can go back to the BufferQueue.
        for (AImage* retired : f.retiring) AImage_delete(retired);
        f.retiring.clear();
        for (auto& sp : f.retiringStreams) {
            VKR_LOG("renderer: stream generation %llu GPU retirement (%zu imports)",
                    (unsigned long long)sp->sceneId, sp->keys.size());
            for (uint32_t key : sp->keys) {
                pass.removeSlot(vk, key);
                pool.removeSlot(vk, key);
            }
            if (sp->currentImage) AImage_delete(sp->currentImage);
            AImageReader_delete(sp->reader);
        }
        f.retiringStreams.clear();
        uint32_t imageIndex = 0;
        VkResult ar = vkrtrace::call("scene.acquire_swapchain",[&]{return vkAcquireNextImageKHR(vk.device,vk.swapchain,UINT64_MAX,f.acquireSem,VK_NULL_HANDLE,&imageIndex);});
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
            vk.recreateSwapchain();
            continue;
        }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) continue;
        vkResetFences(vk.device, 1, &f.inFlight);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        {
        vkrtrace::Scope recording("scene.record_commands_cpu");
        vkBeginCommandBuffer(f.cmd, &bi);
        f.gpuTrace.begin(f.cmd);

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

        int64_t sceneNow=nowNs();
        double dt=std::min(0.1,(sceneNow-lastSceneNs)*1e-9);lastSceneNs=sceneNow;
        if(!state.paused)sceneTime+=(float)dt*state.speed;
        float tSec=sceneTime;
        std::vector<Mat4> models;
        std::vector<int64_t> faces;
        float aspect=(float)vk.swapExtent.width/vk.swapExtent.height;
        vkr::buildScene(state.layout,tSec,aspect,dt,sceneSlots,sceneIds,pool,motion,tileSlots,models,faces);
        for(size_t i=0;i<flSlots.size();++i) {
            auto& slot=flSlots[i];
            if(slot.departing && !motion.has((1ull<<63)|i)) {
                if(slot.lastReady)slot.retire.push_back({frameIndex+2,slot.lastReady});
                slot.currentSlot=-1;slot.departing=false;
                VKR_LOG("renderer: departure retired '%s'",slot.name.c_str());
            }
        }
        for (auto it = departingStreams.begin(); it != departingStreams.end();) {
            if (!motion.has((*it)->sceneId)) {
                VKR_LOG("renderer: stream generation %llu departure completed",
                        (unsigned long long)(*it)->sceneId);
                newlyRetiringStreams.push_back(std::move(*it));
                it = departingStreams.erase(it);
            } else ++it;
        }
        if(!passBound){models.clear();faces.clear();}
        std::vector<float> mvps(models.size()*16);
        auto projection=vkr::matPerspective(0.7854f,aspect,0.1f,100.0f);
        for(size_t i=0;i<models.size();i++){
            auto mvp=vkr::matMul(projection,models[i]);memcpy(mvps.data()+i*16,mvp.m,sizeof(mvp.m));
        }
        pass.recordScene(f.cmd,pool,faces.data(),(uint32_t)models.size(),mvps.data(),vk.swapViews[imageIndex],
            vk.swapExtent,vk.queueFamily,state.backdrop && background.valid()?&background:nullptr,tSec);

        portableOutput.record(vk,(frameIndex-1)%2,f.cmd,vk.swapImages[imageIndex],vk.swapExtent,
            vk.swapFormat,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,nowNs());
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

        f.gpuTrace.end(f.cmd);
        vkEndCommandBuffer(f.cmd);
        }

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquireSem;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &f.renderDoneSem;
        if (vkrtrace::call("scene.queue_submit_cpu",[&]{return vkQueueSubmit(vk.queue,1,&si,f.inFlight);}) != VK_SUCCESS) break;
        // Old images may still be read by the previous submission. Only arm
        // retirement after a successful submit, never behind an older fence.
        f.retiring.swap(newlyRetiring);
        f.retiringStreams.swap(newlyRetiringStreams);
        f.gpuTrace.submitted(frameIndex-1);
        portableOutput.submitted((frameIndex-1)%2);

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &f.renderDoneSem;
        pi.swapchainCount = 1;
        pi.pSwapchains = &vk.swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkrtrace::call("scene.present_call_cpu",[&]{return vkQueuePresentKHR(vk.queue,&pi);});
        if(traceLimit && frameIndex>=traceLimit)state.destroyed=true;
        if (pr == VK_ERROR_OUT_OF_DATE_KHR) vk.recreateSwapchain();

        ++renders;
        if (nowNs() - rateWindowStart > 5000000000ll) {
            if (hopSamples) {
                VKR_LOG("renderer: %llu adoptions / %llu presents in 5s (pool %zu buffers, "
                        "%zu streams) hop avg %.2f ms max %.2f ms over %llu",
                        (unsigned long long)adoptions, (unsigned long long)renders,
                        pool.slots.size(), streamCount,
                        (double)hopTotalUs / (double)hopSamples / 1000.0,
                        (double)hopMaxUs / 1000.0, (unsigned long long)hopSamples);
            } else {
                VKR_LOG("renderer: %llu adoptions / %llu presents in 5s (pool %zu buffers, "
                        "%zu streams)",
                        (unsigned long long)adoptions, (unsigned long long)renders,
                        pool.slots.size(), streamCount);
            }
            adoptions = renders = 0;
            hopSamples = hopTotalUs = hopMaxUs = 0;
            rateWindowStart = nowNs();
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_streamsMutex);
        g_rendererReady = false;
    }
    vkDeviceWaitIdle(vk.device);
    portableOutput.destroy(vk);
    for (auto& f : frames) {
        f.gpuTrace.destroy(vk);
        for (AImage* retired : f.retiring) AImage_delete(retired);
        vkDestroySemaphore(vk.device, f.acquireSem, nullptr);
        vkDestroySemaphore(vk.device, f.renderDoneSem, nullptr);
        vkDestroyFence(vk.device, f.inFlight, nullptr);
    }
    for (AImage* image : newlyRetiring) AImage_delete(image);
    newlyRetiring.clear();
    vkDestroyCommandPool(vk.device, cmdPool, nullptr);
    for (FlSlotIn& s : flSlots) {
        if (s.channel) flClose(s.channel);
        s.channel = nullptr;
    }
    for (auto& sp : newlyRetiringStreams) departingStreams.push_back(std::move(sp));
    for (auto& f : frames)
        for (auto& sp : f.retiringStreams) departingStreams.push_back(std::move(sp));
    background.destroy(vk);
    pass.destroy(vk);
    pool.destroy(vk);
    {
        std::lock_guard<std::mutex> lock(g_streamsMutex);
        for (auto& sp : g_streams) {
            if (sp->currentImage) AImage_delete(sp->currentImage);
            AImageReader_delete(sp->reader);
        }
        for (auto& sp : departingStreams) {
            if (sp->currentImage) AImage_delete(sp->currentImage);
            AImageReader_delete(sp->reader);
        }
        g_streams.clear();
        g_pendingRelease.clear();
    }
    vk.destroy();
    vkrtrace::flush();
    flTraceFlush();
    if(traceLimit)ANativeActivity_finish(app->activity);
}

#endif // __ANDROID__
