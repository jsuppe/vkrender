#include <deque>
#include "av_input.h"
#include "control_api.h"
#include "moderation.h"
#include "perf_trace.h"
#include "portable_input.h"
// M13: minimal Wayland compositor front door for the Linux renderer.
// See wayland_compositor.h for the architecture story.
//
// Implemented protocol surface (the subset gstreamer's waylandsink needs):
//   wl_compositor, wl_subcompositor (waylandsink parks the video on a
//   subsurface over a black area surface), wl_shm (libwayland built-in;
//   CPU-upload fallback), xdg_wm_base/xdg_surface/xdg_toplevel (configure
//   dance only), zwp_linux_dmabuf_v1 v3 (format+modifier advertisement from
//   the Vulkan driver's DRM-modifier query, dma-buf import zero-copy).
//
// Threading: single-threaded - the Wayland event loop is dispatched
// non-blocking inside the render loop, so no locking is needed anywhere.

#ifndef _WIN32

#include "wayland_compositor.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/dma-buf.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <volk.h>
#include <wayland-server-core.h>

#include "linux-dmabuf-v1-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include "framelink.h"
#include "framelink_sync_fd.h"
#include "framelink_dmabuf.h"

#include "background_pass.h"
#include "dump_util.h"
#include "external_video_image.h"
#include "log.h"
#include "scene_math.h"
#include "video_pass.h"
#include "vk_context.h"

// DRM fourcc / modifier constants (avoiding a libdrm dependency).
#define VKR_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define VKR_DRM_FORMAT_NV12 VKR_FOURCC('N', 'V', '1', '2')
#define VKR_DRM_FORMAT_ARGB8888 VKR_FOURCC('A', 'R', '2', '4')
#define VKR_DRM_FORMAT_XRGB8888 VKR_FOURCC('X', 'R', '2', '4')
#define VKR_DRM_MOD_LINEAR 0ull

#ifndef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
struct dma_buf_export_sync_file {
    __u32 flags;
    __s32 fd;
};
#define DMA_BUF_SYNC_READ (1 << 0)
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR('b', 2, struct dma_buf_export_sync_file)
#endif

namespace vkr {
namespace {

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Ctx;

// One dmabuf-backed wl_buffer. Owns its fds (kept for implicit-sync export;
// the pool import gets dups). Imported once; the slot lives until the client
// destroys the buffer.
struct DmabufBuffer {
    Ctx* ctx = nullptr;
    wl_resource* resource = nullptr;
    uint32_t width = 0, height = 0, fourcc = 0;
    uint64_t modifier = 0;
    int fds[4] = {-1, -1, -1, -1};
    uint32_t offsets[4] = {}, strides[4] = {};
    int planeCount = 0;
    bool imported = false;
    bool importFailed = false;
    uint32_t slotKey = 0;
};

// Accumulates zwp_linux_buffer_params_v1.add calls.
struct BufferParams {
    Ctx* ctx = nullptr;
    int fds[4] = {-1, -1, -1, -1};
    uint32_t offsets[4] = {}, strides[4] = {};
    uint64_t modifier = 0;
    int planeCount = 0;
    bool used = false;
};

// Every wl_surface gets a Stream record; only surfaces that commit buffers
// (and have no content-bearing subsurface children) land on cube faces.
struct Stream {
    int controlSlot = -1;
    Ctx* ctx = nullptr;
    wl_resource* wlSurface = nullptr;
    Stream* parent = nullptr; // set for subsurfaces
    bool isToplevel = false;
    bool configured = false;

    wl_resource* xdgSurface = nullptr;
    wl_resource* xdgToplevel = nullptr;

    // pending state (attach latched by commit)
    wl_resource* pendingBuffer = nullptr;
    bool hasPendingAttach = false;
    std::vector<wl_resource*> pendingFrameCbs;

    // committed dmabuf waiting for its implicit fence
    wl_resource* waitingBuffer = nullptr;
    int waitingSyncFd = -1;

    // currently composited buffer (released after frame fences retire)
    wl_resource* currentBuffer = nullptr;
    int64_t currentSlot = -1;
    int64_t lastAdoptNs=0, firstAdoptNs=0;
    uint64_t adopted=0;

    // shm upload path: double-buffered owned slots
    uint32_t shmKey[2] = {0, 0};
    int shmNext = 0;
    uint32_t shmW = 0, shmH = 0;

    std::vector<wl_resource*> firedFrameCbs; // to signal after next composite
};

constexpr size_t kMaxStreams = 12;

struct Ctx {
    wl_display* display = nullptr;
    wl_event_loop* loop = nullptr;

    VkContext* vk = nullptr;
    ExternalVideoPool* pool = nullptr;
    VideoPass* pass = nullptr;
    bool poolCreated = false;
    bool poolIsNv12 = false;
    bool passBound = false;
    bool passNeedsBind = false;

    std::vector<Stream*> streams; // creation order = face order
    uint32_t nextSlotKey = 1;
    uint32_t configureSerial = 1;

    // wl_buffers whose last GPU read is fenced by the frame they were retired
    // in; released once the host has waited that frame's fence (two frames
    // in flight -> released when frameCounter advances past retireFrame + 2).
    std::vector<std::pair<uint64_t, wl_resource*>> retiring;
    uint64_t frameCounter = 0;
    // slots whose backing wl_buffer was destroyed -> remove after idle.
    std::vector<uint32_t> slotsToRemove;

    std::vector<uint64_t> nv12Mods, argbMods;

    uint64_t adoptions = 0;
    int videoFramesSeen = 0;
};

Stream* streamFromSurface(wl_resource* surface) {
    return (Stream*)wl_resource_get_user_data(surface);
}

// ---- helpers ----------------------------------------------------------------

void scrubRetiring(Ctx& ctx, wl_resource* buffer) {
    ctx.retiring.erase(std::remove_if(ctx.retiring.begin(), ctx.retiring.end(),
                                      [&](const auto& e) { return e.second == buffer; }),
                       ctx.retiring.end());
}

void releaseBuffer(wl_resource* buffer) {
    if (buffer) wl_buffer_send_release(buffer);
}

// Modifier list for a Vulkan format, filtered to SAMPLED_IMAGE support.
std::vector<uint64_t> queryModifiers(VkPhysicalDevice phys, VkFormat format) {
    VkDrmFormatModifierPropertiesListEXT list{
        VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    props.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(phys, format, &props);
    std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = mods.data();
    vkGetPhysicalDeviceFormatProperties2(phys, format, &props);
    std::vector<uint64_t> out;
    for (const auto& m : mods)
        if (m.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
            out.push_back(m.drmFormatModifier);
    return out;
}

// ---- wl_buffer (dmabuf) -----------------------------------------------------

void bufferHandleDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

const struct wl_buffer_interface dmabufBufferImpl = {bufferHandleDestroy};

void dmabufBufferDestroyed(wl_resource* resource) {
    auto* buf = (DmabufBuffer*)wl_resource_get_user_data(resource);
    if (!buf) return;
    Ctx& ctx = *buf->ctx;
    scrubRetiring(ctx, resource);
    for (Stream* s : ctx.streams) {
        if (s->waitingBuffer == resource) {
            s->waitingBuffer = nullptr;
            if (s->waitingSyncFd >= 0) close(s->waitingSyncFd);
            s->waitingSyncFd = -1;
        }
        if (s->currentBuffer == resource) {
            s->currentBuffer = nullptr;
            s->currentSlot = -1;
        }
        if (s->pendingBuffer == resource) s->pendingBuffer = nullptr;
    }
    if (buf->imported) ctx.slotsToRemove.push_back(buf->slotKey);
    for (int i = 0; i < buf->planeCount; ++i)
        if (buf->fds[i] >= 0) close(buf->fds[i]);
    delete buf;
}

// ---- zwp_linux_buffer_params_v1 --------------------------------------------

void paramsDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void paramsAdd(wl_client*, wl_resource* resource, int32_t fd, uint32_t planeIdx,
               uint32_t offset, uint32_t stride, uint32_t modHi, uint32_t modLo) {
    auto* p = (BufferParams*)wl_resource_get_user_data(resource);
    if (!p || planeIdx >= 4) {
        close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "plane index out of range");
        return;
    }
    if (p->fds[planeIdx] >= 0) {
        close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "plane already set");
        return;
    }
    p->fds[planeIdx] = fd;
    p->offsets[planeIdx] = offset;
    p->strides[planeIdx] = stride;
    p->modifier = ((uint64_t)modHi << 32) | modLo;
    if ((int)planeIdx + 1 > p->planeCount) p->planeCount = planeIdx + 1;
}

wl_resource* makeDmabufBuffer(wl_client* client, BufferParams* p, uint32_t bufferId,
                              int32_t width, int32_t height, uint32_t format) {
    auto* buf = new DmabufBuffer();
    buf->ctx = p->ctx;
    buf->width = (uint32_t)width;
    buf->height = (uint32_t)height;
    buf->fourcc = format;
    buf->modifier = p->modifier;
    buf->planeCount = p->planeCount;
    for (int i = 0; i < p->planeCount; ++i) {
        buf->fds[i] = p->fds[i];
        p->fds[i] = -1; // ownership moved
        buf->offsets[i] = p->offsets[i];
        buf->strides[i] = p->strides[i];
    }
    p->used = true;
    wl_resource* res = wl_resource_create(client, &wl_buffer_interface, 1, bufferId);
    wl_resource_set_implementation(res, &dmabufBufferImpl, buf, dmabufBufferDestroyed);
    buf->resource = res;
    return res;
}

void paramsCreate(wl_client* client, wl_resource* resource, int32_t width, int32_t height,
                  uint32_t format, uint32_t /*flags*/) {
    auto* p = (BufferParams*)wl_resource_get_user_data(resource);
    if (!p || p->used || p->planeCount == 0) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "params already used or empty");
        return;
    }
    wl_resource* buffer = makeDmabufBuffer(client, p, 0, width, height, format);
    zwp_linux_buffer_params_v1_send_created(resource, buffer);
}

void paramsCreateImmed(wl_client* client, wl_resource* resource, uint32_t bufferId,
                       int32_t width, int32_t height, uint32_t format, uint32_t /*flags*/) {
    auto* p = (BufferParams*)wl_resource_get_user_data(resource);
    if (!p || p->used || p->planeCount == 0) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "params already used or empty");
        return;
    }
    makeDmabufBuffer(client, p, bufferId, width, height, format);
}

const struct zwp_linux_buffer_params_v1_interface paramsImpl = {
    paramsDestroy, paramsAdd, paramsCreate, paramsCreateImmed};

void paramsDestroyed(wl_resource* resource) {
    auto* p = (BufferParams*)wl_resource_get_user_data(resource);
    if (!p) return;
    for (int i = 0; i < 4; ++i)
        if (p->fds[i] >= 0) close(p->fds[i]);
    delete p;
}

// ---- zwp_linux_dmabuf_v1 ----------------------------------------------------

void dmabufDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void dmabufCreateParams(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* p = new BufferParams();
    p->ctx = (Ctx*)wl_resource_get_user_data(resource);
    wl_resource* res = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                                          wl_resource_get_version(resource), id);
    wl_resource_set_implementation(res, &paramsImpl, p, paramsDestroyed);
}

void dmabufGetDefaultFeedback(wl_client*, wl_resource* resource, uint32_t) {
    // We advertise v3; feedback is a v4 request and should never arrive.
    wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD, "feedback unsupported");
}

void dmabufGetSurfaceFeedback(wl_client*, wl_resource* resource, uint32_t, wl_resource*) {
    wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD, "feedback unsupported");
}

const struct zwp_linux_dmabuf_v1_interface dmabufImpl = {
    dmabufDestroy, dmabufCreateParams, dmabufGetDefaultFeedback, dmabufGetSurfaceFeedback};

void dmabufBind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    Ctx& ctx = *(Ctx*)data;
    wl_resource* res =
        wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, (int)version, id);
    wl_resource_set_implementation(res, &dmabufImpl, &ctx, nullptr);

    auto advertise = [&](uint32_t fourcc, const std::vector<uint64_t>& mods) {
        zwp_linux_dmabuf_v1_send_format(res, fourcc);
        if (version >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
            for (uint64_t m : mods)
                zwp_linux_dmabuf_v1_send_modifier(res, fourcc, (uint32_t)(m >> 32), (uint32_t)m);
        }
    };
    advertise(VKR_DRM_FORMAT_NV12, ctx.nv12Mods);
    advertise(VKR_DRM_FORMAT_ARGB8888, ctx.argbMods);
    advertise(VKR_DRM_FORMAT_XRGB8888, ctx.argbMods);
}

// ---- wl_surface -------------------------------------------------------------

void surfaceDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void surfaceAttach(wl_client*, wl_resource* resource, wl_resource* buffer, int32_t, int32_t) {
    Stream* s = streamFromSurface(resource);
    s->pendingBuffer = buffer;
    s->hasPendingAttach = true;
}

void surfaceDamage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}

void surfaceFrame(wl_client* client, wl_resource* resource, uint32_t callback) {
    Stream* s = streamFromSurface(resource);
    wl_resource* cb = wl_resource_create(client, &wl_callback_interface, 1, callback);
    wl_resource_set_implementation(cb, nullptr, s, [](wl_resource* res) {
        // Client died before we fired the callback: forget it.
        auto* stream = (Stream*)wl_resource_get_user_data(res);
        if (!stream) return;
        auto scrub = [&](std::vector<wl_resource*>& v) {
            v.erase(std::remove(v.begin(), v.end(), res), v.end());
        };
        scrub(stream->pendingFrameCbs);
        scrub(stream->firedFrameCbs);
    });
    s->pendingFrameCbs.push_back(cb);
}

void surfaceSetOpaqueRegion(wl_client*, wl_resource*, wl_resource*) {}
void surfaceSetInputRegion(wl_client*, wl_resource*, wl_resource*) {}

void handleShmCommit(Stream* s, wl_shm_buffer* shm);
void handleDmabufCommit(Stream* s, wl_resource* buffer);

void surfaceCommit(wl_client*, wl_resource* resource) {
    Stream* s = streamFromSurface(resource);
    Ctx& ctx = *s->ctx;

    // xdg configure dance: first commit (typically bufferless) triggers it.
    if (s->xdgToplevel && !s->configured) {
        wl_array states;
        wl_array_init(&states);
        xdg_toplevel_send_configure(s->xdgToplevel, 0, 0, &states);
        wl_array_release(&states);
        xdg_surface_send_configure(s->xdgSurface, ctx.configureSerial++);
        s->configured = true;
    }

    if (!s->hasPendingAttach) return;
    wl_resource* buffer = s->pendingBuffer;
    s->pendingBuffer = nullptr;
    s->hasPendingAttach = false;
    if (!buffer) { // attach(null) = surface has no content
        if (s->waitingBuffer) {
            releaseBuffer(s->waitingBuffer);
            if (s->waitingSyncFd >= 0) close(s->waitingSyncFd);
            s->waitingBuffer = nullptr;
            s->waitingSyncFd = -1;
        }
        s->currentSlot = -1;
        return;
    }

    if (wl_shm_buffer* shm = wl_shm_buffer_get(buffer)) {
        handleShmCommit(s, shm);
        releaseBuffer(buffer); // contents copied; client may reuse immediately
    } else {
        handleDmabufCommit(s, buffer);
    }
    // Frame callbacks requested before this commit fire on the next composite.
    for (wl_resource* cb : s->pendingFrameCbs) s->firedFrameCbs.push_back(cb);
    s->pendingFrameCbs.clear();
}

void surfaceSetBufferTransform(wl_client*, wl_resource*, int32_t) {}
void surfaceSetBufferScale(wl_client*, wl_resource*, int32_t) {}
void surfaceDamageBuffer(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void surfaceOffset(wl_client*, wl_resource*, int32_t, int32_t) {}

const struct wl_surface_interface surfaceImpl = {
    surfaceDestroy,        surfaceAttach,          surfaceDamage,
    surfaceFrame,          surfaceSetOpaqueRegion, surfaceSetInputRegion,
    surfaceCommit,         surfaceSetBufferTransform, surfaceSetBufferScale,
    surfaceDamageBuffer,   surfaceOffset};

void surfaceDestroyed(wl_resource* resource) {
    Stream* s = streamFromSurface(resource);
    if (!s) return;
    Ctx& ctx = *s->ctx;
    if (s->waitingBuffer) {
        releaseBuffer(s->waitingBuffer);
        if (s->waitingSyncFd >= 0) close(s->waitingSyncFd);
    }
    // currentBuffer may still be sampled by in-flight frames; it stays on the
    // retiring list (or gets released below with the shm slots removal).
    if (s->currentBuffer) {
        bool queued = false;
        for (auto& e : ctx.retiring)
            if (e.second == s->currentBuffer) queued = true;
        if (!queued) ctx.retiring.push_back({ctx.frameCounter, s->currentBuffer});
    }
    for (uint32_t key : s->shmKey)
        if (key) ctx.slotsToRemove.push_back(key);
    ctx.streams.erase(std::remove(ctx.streams.begin(), ctx.streams.end(), s),
                      ctx.streams.end());
    for (Stream* other : ctx.streams)
        if (other->parent == s) other->parent = nullptr;
    VKR_LOG("compositor: stream gone (%zu remaining)", ctx.streams.size());
    delete s;
}

// ---- wl_compositor / wl_region / wl_subcompositor ---------------------------

void regionDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
void regionAdd(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void regionSubtract(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
const struct wl_region_interface regionImpl = {regionDestroy, regionAdd, regionSubtract};

void compositorCreateSurface(wl_client* client, wl_resource* resource, uint32_t id) {
    Ctx& ctx = *(Ctx*)wl_resource_get_user_data(resource);
    auto* s = new Stream();
    s->ctx = &ctx;
    wl_resource* res = wl_resource_create(client, &wl_surface_interface,
                                          wl_resource_get_version(resource), id);
    wl_resource_set_implementation(res, &surfaceImpl, s, surfaceDestroyed);
    s->wlSurface = res;
    ctx.streams.push_back(s);
}

void compositorCreateRegion(wl_client* client, wl_resource* resource, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_region_interface,
                                          wl_resource_get_version(resource), id);
    wl_resource_set_implementation(res, &regionImpl, nullptr, nullptr);
}

const struct wl_compositor_interface compositorImpl = {compositorCreateSurface,
                                                       compositorCreateRegion};

void compositorBind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_compositor_interface, (int)version, id);
    wl_resource_set_implementation(res, &compositorImpl, data, nullptr);
}

void subsurfaceDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
void subsurfaceSetPosition(wl_client*, wl_resource*, int32_t, int32_t) {}
void subsurfacePlaceAbove(wl_client*, wl_resource*, wl_resource*) {}
void subsurfacePlaceBelow(wl_client*, wl_resource*, wl_resource*) {}
void subsurfaceSetSync(wl_client*, wl_resource*) {}
void subsurfaceSetDesync(wl_client*, wl_resource*) {}
const struct wl_subsurface_interface subsurfaceImpl = {
    subsurfaceDestroy,   subsurfaceSetPosition, subsurfacePlaceAbove,
    subsurfacePlaceBelow, subsurfaceSetSync,     subsurfaceSetDesync};

void subcompositorDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void subcompositorGetSubsurface(wl_client* client, wl_resource* resource, uint32_t id,
                                wl_resource* surface, wl_resource* parent) {
    Stream* s = streamFromSurface(surface);
    s->parent = streamFromSurface(parent);
    wl_resource* res = wl_resource_create(client, &wl_subsurface_interface,
                                          wl_resource_get_version(resource), id);
    wl_resource_set_implementation(res, &subsurfaceImpl, s, nullptr);
}

const struct wl_subcompositor_interface subcompositorImpl = {subcompositorDestroy,
                                                             subcompositorGetSubsurface};

void subcompositorBind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &wl_subcompositor_interface, (int)version, id);
    wl_resource_set_implementation(res, &subcompositorImpl, data, nullptr);
}

// ---- xdg_wm_base ------------------------------------------------------------

void toplevelDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
void toplevelSetParent(wl_client*, wl_resource*, wl_resource*) {}
void toplevelSetTitle(wl_client*, wl_resource*, const char*) {}
void toplevelSetAppId(wl_client*, wl_resource*, const char*) {}
void toplevelShowWindowMenu(wl_client*, wl_resource*, wl_resource*, uint32_t, int32_t, int32_t) {}
void toplevelMove(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
void toplevelResize(wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {}
void toplevelSetMaxSize(wl_client*, wl_resource*, int32_t, int32_t) {}
void toplevelSetMinSize(wl_client*, wl_resource*, int32_t, int32_t) {}
void toplevelSetMaximized(wl_client*, wl_resource*) {}
void toplevelUnsetMaximized(wl_client*, wl_resource*) {}
void toplevelSetFullscreen(wl_client*, wl_resource*, wl_resource*) {}
void toplevelUnsetFullscreen(wl_client*, wl_resource*) {}
void toplevelSetMinimized(wl_client*, wl_resource*) {}
const struct xdg_toplevel_interface toplevelImpl = {
    toplevelDestroy,        toplevelSetParent,     toplevelSetTitle,
    toplevelSetAppId,       toplevelShowWindowMenu, toplevelMove,
    toplevelResize,         toplevelSetMaxSize,    toplevelSetMinSize,
    toplevelSetMaximized,   toplevelUnsetMaximized, toplevelSetFullscreen,
    toplevelUnsetFullscreen, toplevelSetMinimized};

void xdgSurfaceDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void xdgSurfaceGetToplevel(wl_client* client, wl_resource* resource, uint32_t id) {
    Stream* s = (Stream*)wl_resource_get_user_data(resource);
    wl_resource* res = wl_resource_create(client, &xdg_toplevel_interface,
                                          wl_resource_get_version(resource), id);
    wl_resource_set_implementation(res, &toplevelImpl, s, [](wl_resource* r) {
        if (auto* stream = (Stream*)wl_resource_get_user_data(r)) stream->xdgToplevel = nullptr;
    });
    s->xdgToplevel = res;
    s->isToplevel = true;
}

void xdgSurfaceGetPopup(wl_client*, wl_resource* resource, uint32_t, wl_resource*,
                        wl_resource*) {
    wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD, "popups unsupported");
}

void xdgSurfaceSetWindowGeometry(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void xdgSurfaceAckConfigure(wl_client*, wl_resource*, uint32_t) {}
const struct xdg_surface_interface xdgSurfaceImpl = {
    xdgSurfaceDestroy, xdgSurfaceGetToplevel, xdgSurfaceGetPopup,
    xdgSurfaceSetWindowGeometry, xdgSurfaceAckConfigure};

void wmBaseDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void wmBaseCreatePositioner(wl_client*, wl_resource* resource, uint32_t) {
    wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_METHOD, "positioners unsupported");
}

void wmBaseGetXdgSurface(wl_client* client, wl_resource* resource, uint32_t id,
                         wl_resource* surface) {
    Stream* s = streamFromSurface(surface);
    wl_resource* res = wl_resource_create(client, &xdg_surface_interface,
                                          wl_resource_get_version(resource), id);
    wl_resource_set_implementation(res, &xdgSurfaceImpl, s, [](wl_resource* r) {
        if (auto* stream = (Stream*)wl_resource_get_user_data(r)) stream->xdgSurface = nullptr;
    });
    s->xdgSurface = res;
}

void wmBasePong(wl_client*, wl_resource*, uint32_t) {}
const struct xdg_wm_base_interface wmBaseImpl = {wmBaseDestroy, wmBaseCreatePositioner,
                                                 wmBaseGetXdgSurface, wmBasePong};

void wmBaseBind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &xdg_wm_base_interface, (int)version, id);
    wl_resource_set_implementation(res, &wmBaseImpl, data, nullptr);
}

// ---- commit paths -----------------------------------------------------------

bool ensurePool(Ctx& ctx, uint32_t width, uint32_t height, bool nv12) {
    if(ctx.poolCreated && nv12==ctx.poolIsNv12 && ctx.pool->width==(width&~1u) && ctx.pool->height==(height&~1u))return true;
    bool first=!ctx.poolCreated;
    proto::PoolDescMsg header{};
    header.width = width & ~1u;
    header.height = height & ~1u;
    header.format = nv12 ? proto::SharedFormat::NV12 : proto::SharedFormat::BGRA8;
    header.matrix = height >= 720 ? proto::Matrix::BT709 : proto::Matrix::BT601;
    header.range = proto::Range::Limited;
    header.planeCount = nv12 ? 2 : 1;
    if (!ctx.pool->create(*ctx.vk, header)) return false;
    ctx.poolCreated = true;
    ctx.poolIsNv12 = nv12;
    ctx.passNeedsBind = first;
    return true;
}

bool bindSlot(Ctx& ctx, uint32_t key) {
    bool ok = ctx.passNeedsBind ? ctx.pass->bindPool(*ctx.vk, *ctx.pool)
                                : ctx.pass->addSlot(*ctx.vk, *ctx.pool, key);
    if (ctx.passNeedsBind && ok) ctx.passBound = true;
    ctx.passNeedsBind = false;
    return ok;
}

void handleShmCommit(Stream* s, wl_shm_buffer* shm) {
    Ctx& ctx = *s->ctx;
    uint32_t fmt = wl_shm_buffer_get_format(shm);
    if (fmt != WL_SHM_FORMAT_ARGB8888 && fmt != WL_SHM_FORMAT_XRGB8888) return;
    uint32_t w = (uint32_t)wl_shm_buffer_get_width(shm);
    uint32_t h = (uint32_t)wl_shm_buffer_get_height(shm);
    if (!ensurePool(ctx, w, h, /*nv12=*/false)) return;

    if (s->shmW != w || s->shmH != h) { // (re)create the double buffer
        for (uint32_t key : s->shmKey)
            if (key) ctx.slotsToRemove.push_back(key);
        s->shmKey[0] = s->shmKey[1] = 0;
        s->shmW = w;
        s->shmH = h;
    }
    int idx = s->shmNext;
    if (!s->shmKey[idx]) {
        uint32_t key = ctx.nextSlotKey++;
        if (!ctx.pool->addOwnedSurface(*ctx.vk, key, w, h) || !bindSlot(ctx, key)) return;
        s->shmKey[idx] = key;
    }
    wl_shm_buffer_begin_access(shm);
    ctx.pool->updateOwnedSurface(*ctx.vk, s->shmKey[idx], wl_shm_buffer_get_data(shm),
                                 (uint64_t)wl_shm_buffer_get_stride(shm), w, h);
    wl_shm_buffer_end_access(shm);
    s->currentSlot = (int64_t)s->shmKey[idx];
        s->lastAdoptNs=nowNs();if(!s->firstAdoptNs)s->firstAdoptNs=s->lastAdoptNs;++s->adopted;
    s->shmNext = 1 - idx;
    ++ctx.adoptions;
    ++ctx.videoFramesSeen;
}

void handleDmabufCommit(Stream* s, wl_resource* buffer) {
    Ctx& ctx = *s->ctx;
    auto* buf = (DmabufBuffer*)wl_resource_get_user_data(buffer);
    if (!buf || buf->importFailed) {
        releaseBuffer(buffer);
        return;
    }
    // Latest-wins at commit granularity: a still-waiting previous buffer was
    // never sampled - hand it straight back.
    if (s->waitingBuffer) {
        releaseBuffer(s->waitingBuffer);
        if (s->waitingSyncFd >= 0) close(s->waitingSyncFd);
        s->waitingSyncFd = -1;
    }
    s->waitingBuffer = buffer;
    // Implicit-sync bridge: export the buffer's write fences as a sync_file;
    // the render loop polls it before sampling (kernel 5.18+; absence means
    // we just trust the producer finished - same as M6's vaSyncSurface model).
    s->waitingSyncFd = -1;
    if (buf->fds[0] >= 0) {
        struct dma_buf_export_sync_file req{};
        req.flags = DMA_BUF_SYNC_READ;
        req.fd = -1;
        if (ioctl(buf->fds[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &req) == 0) {
            s->waitingSyncFd = req.fd;
        }
    }
}

// Adopt ready buffers: import on first sight, then swap the stream's current.
void adoptStreams(Ctx& ctx) {
    for (Stream* s : ctx.streams) {
        if (!s->waitingBuffer) continue;
        if (s->waitingSyncFd >= 0) {
            struct pollfd pfd{s->waitingSyncFd, POLLIN, 0};
            if (poll(&pfd, 1, 0) <= 0) continue; // not ready yet - keep waiting
            close(s->waitingSyncFd);
            s->waitingSyncFd = -1;
        }
        auto* buf = (DmabufBuffer*)wl_resource_get_user_data(s->waitingBuffer);
        if (!buf) {
            s->waitingBuffer = nullptr;
            continue;
        }
        if (!buf->imported) {
            bool nv12 = buf->fourcc == VKR_DRM_FORMAT_NV12;
            bool rgb = buf->fourcc == VKR_DRM_FORMAT_ARGB8888 ||
                       buf->fourcc == VKR_DRM_FORMAT_XRGB8888;
            if ((!nv12 && !rgb) || !ensurePool(ctx, buf->width, buf->height, nv12)) {
                buf->importFailed = true;
                releaseBuffer(s->waitingBuffer);
                s->waitingBuffer = nullptr;
                continue;
            }
            proto::SurfaceDescMsg desc{};
            desc.slotKey = ctx.nextSlotKey++;
            desc.numPlanes = nv12 ? 2u : 1u;
            desc.numObjects = (uint32_t)buf->planeCount;
            desc.drmModifier = buf->modifier;
            int dupFds[4] = {-1, -1, -1, -1};
            for (int i = 0; i < buf->planeCount; ++i) {
                desc.plane[i].offset = buf->offsets[i];
                desc.plane[i].pitch = buf->strides[i];
                desc.planeObject[i] = (uint32_t)i;
                off_t size = lseek(buf->fds[i], 0, SEEK_END);
                desc.objectSize[i] = size > 0 ? (uint64_t)size : 0;
                dupFds[i] = fcntl(buf->fds[i], F_DUPFD_CLOEXEC, 0);
            }
            // NV12 with both planes in one object: collapse to one fd.
            if (nv12 && buf->planeCount == 1) {
                desc.numPlanes = 2;
                desc.numObjects = 1;
                desc.plane[1].offset = buf->offsets[1];
                desc.plane[1].pitch = buf->strides[1];
                desc.planeObject[1] = 0;
            }
            bool ok = ctx.pool->addSurface(*ctx.vk, desc, dupFds,
                                           (size_t)(desc.numObjects)) &&
                      bindSlot(ctx, desc.slotKey);
            if (!ok) {
                VKR_ERR("compositor: dmabuf import failed (fourcc %.4s mod 0x%llx)",
                        (const char*)&buf->fourcc, (unsigned long long)buf->modifier);
                buf->importFailed = true;
                releaseBuffer(s->waitingBuffer);
                s->waitingBuffer = nullptr;
                continue;
            }
            buf->imported = true;
            buf->slotKey = desc.slotKey;
        }
        // Previous buffer retires behind this frame's fence.
        if (s->currentBuffer) ctx.retiring.push_back({ctx.frameCounter, s->currentBuffer});
        s->currentBuffer = s->waitingBuffer;
        s->currentSlot = (int64_t)buf->slotKey;
        s->lastAdoptNs=nowNs();if(!s->firstAdoptNs)s->firstAdoptNs=s->lastAdoptNs;++s->adopted;
        s->waitingBuffer = nullptr;
        ++ctx.adoptions;
        ++ctx.videoFramesSeen;
    }
}

// A surface with content-bearing children is a waylandsink "area" surface -
// composite the children instead.
bool hasContentChild(Ctx& ctx, Stream* s) {
    for (Stream* other : ctx.streams)
        if (other->parent == s && other->currentSlot >= 0) return true;
    return false;
}

// ---- M39: framelink input slots ---------------------------------------------
//
// The Linux twin of the Windows service's slot channels: <socket>.in.0 .. .11,
// pre-created so receivers have something to attach to, consumed alongside the
// Wayland socket. A framelink frame arrives as a dma-buf, which is the exact
// import path the compositor already runs for Wayland dmabuf clients - the
// only new plumbing is the walk from flImageDmabuf to a SurfaceDescMsg.
//
// Sync is the platform's weak spot, stated rather than hidden: this backend
// has no shared fence, so a frame is trusted to be finished when flSubmit
// said so. True for CPU producers (flImageMap writers - all of today's
// receivers), true for a GPU producer only if it flushed.
struct FlSlotIn {
    std::deque<flFrame> pending;
    flChannel* channel = nullptr;
    std::string name;
    uint64_t uid = 0;        // scene stream id
    uint64_t generation = 0; // which ring we imported - see flChannelInfo
    uint32_t width = 0, height = 0;
    uint64_t lastReady=0;
    std::vector<std::pair<uint64_t,uint64_t>> retire; // frame fence index, watermark
    bool imported[8] = {}; // ring slot -> pool key valid (fl kMaxPool is 8)
    uint32_t keys[8] = {};
    int64_t currentSlot = -1;
    int64_t lastAdoptNs=0, firstAdoptNs=0;
    uint64_t adopted=0;
    bool departing = false;
    int64_t departedNs = 0;
};

constexpr uint32_t kFlSlots = 12;

void flDropRing(Ctx& ctx, FlSlotIn& s) {
    s.pending.clear();
    for (uint32_t i = 0; i < 8; ++i) {
        if (!s.imported[i]) continue;
        ctx.slotsToRemove.push_back(s.keys[i]);
        s.imported[i] = false;
    }
    s.currentSlot = -1;
}

void flPollSlots(Ctx& ctx, std::vector<FlSlotIn>& slots, AvInput& av, SceneHost& host, uint64_t* hopSamples,
                 uint64_t* hopTotalUs, uint64_t* hopMaxUs) {
    for (FlSlotIn& s : slots) {
        if (!s.channel) continue;

        flChannelInfo info{};
        flQuery(s.channel, &info);
        if (info.generation != s.generation) {
            // The ring was reallocated (flRequestGeometry): every buffer we
            // imported is a different allocation now. The dup'd fds kept the
            // old memory alive for any in-flight GPU reads; the deferred
            // teardown drops them.
            flDropRing(ctx, s);
            s.generation = info.generation;
            s.retire.clear();s.lastReady=0;
            s.adopted=0;s.firstAdoptNs=s.lastAdoptNs=0;
            s.width = info.width;
            s.height = info.height;
        }

        flFrame f{};
        const bool scheduled=(&s==&slots.front() && av.active());
            flResult r=FL_TIMEOUT;
            AvAcquireBudget acquireBudget;
            auto acquirePending=[&]{
                acquireBudget.poll(unsigned(s.pending.size()),6,[&]{
                    flFrame next{};r=flAcquireFrame(s.channel,&next,0);
                    if(r==FL_OK){s.pending.push_back(next);av.schedule.enqueue(next.sequence,s.pending.size());flRelease(s.channel,&next);}
                });
            };
            if(scheduled){
                // Bookkeeping release does NOT recycle deferred/GPU-sync storage.
                // This permits a bounded timestamp queue without blocking acquire.
                acquirePending();
                if(!s.pending.empty()){f=s.pending.front();r=FL_OK;}
            }else r=flAcquireFrame(s.channel,&f,0);
            auto releaseFrame=[&]{if(!scheduled)flRelease(s.channel,&f);};
        if (r == FL_DISCONNECTED) {
            if (!s.departing && s.currentSlot >= 0) {
                s.departing = true;
                s.departedNs = nowNs();
            }
            continue;
        }
        if (r != FL_OK) continue;

        // A reallocation between the flQuery above and here would hand us a
        // frame from a ring we have not imported. Drop it; the producer
        // resends and the next iteration re-imports.
        flChannelInfo now{};
        flQuery(s.channel, &now);
        if (now.generation != s.generation || f.slot >= 8) {
            releaseFrame();
            continue;
        }

        if(&s==&slots.front() && av.active()) {
            auto decision=av.decide(f.ptsNs);
            if(decision==AvSchedule::Decision::Hold){continue;}
            s.pending.pop_front();av.schedule.queued=s.pending.size();
                // A full queue skipped the pre-decision acquire. Refill the newly
                // freed slot now, not one render iteration later: otherwise a
                // steady 60 Hz source loses one latest-wins frame every queue cycle.
                // Bookkeeping release retains deferred/GPU-sync ownership; neither
                // capacity nor consumed-fence retirement is changed here.
                acquirePending();
            if(decision!=AvSchedule::Decision::Present){
                // Stop sampling before retiring ALL values through the dropped
                // frame. Existing frame fences still protect previous draws.
                host.forgetStream(s.uid);s.currentSlot=-1;
                s.retire.push_back({ctx.frameCounter+2,f._readyValue});
                releaseFrame();continue;
            }
        }
            auto failedAdoption=[&]{
                if(scheduled){
                    ++av.schedule.importFailures;
                    // Forget retained sampling before releasing through this
                    // frame; existing GPU reads retire through the same fence
                    // path as a deliberately dropped scheduled frame.
                    host.forgetStream(s.uid);s.currentSlot=-1;
                    s.retire.push_back({ctx.frameCounter+2,f._readyValue});
                }
                releaseFrame();
            };
            // Diagnostic-only failure of 40 consecutive adoption attempts,
            // longer than the eight-slot ring, after initial valid sampling.
            static const bool failAdoption=getenv("VKR_TEST_AV_IMPORT_FAILURES")!=nullptr;
            if(scheduled && failAdoption && av.schedule.presented>=20 && av.schedule.presented<60){
                failedAdoption();continue;
            }
        if (!ensurePool(ctx, s.width, s.height, /*nv12=*/false)) {
            failedAdoption();
            continue;
        }
        if(now.format!=FL_FORMAT_BGRA8) {
            if(!s.imported[f.slot])s.keys[f.slot]=ctx.nextSlotKey++;
            if(!uploadFrame(*ctx.vk,*ctx.pool,*ctx.pass,ctx.passBound,f.image,s.keys[f.slot],now)) {
                failedAdoption();continue;
            }
            ctx.passNeedsBind=false;s.imported[f.slot]=true;
        }
        if (!s.imported[f.slot]) {
            flDmabuf db{};
            if (flImageDmabuf(f.image, &db) != FL_OK || db.planes < 1) {
                failedAdoption();
                continue;
            }
            proto::SurfaceDescMsg desc{};
            desc.slotKey = ctx.nextSlotKey++;
            desc.numPlanes = 1;
            desc.numObjects = 1;
            desc.drmModifier = db.modifier;
            desc.plane[0].offset = db.offset[0];
            desc.plane[0].pitch = (uint32_t)db.stride[0];
            desc.planeObject[0] = 0;
            const off_t size = lseek(db.fd[0], 0, SEEK_END);
            desc.objectSize[0] = size > 0 ? (uint64_t)size : 0;
            // Dup: framelink's fds die with the channel, and Vulkan needs the
            // buffer to outlive any reallocation that races an in-flight frame.
            int dupFd = fcntl(db.fd[0], F_DUPFD_CLOEXEC, 0);
            const bool ok = dupFd >= 0 &&
                            ctx.pool->addSurface(*ctx.vk, desc, &dupFd, 1) &&
                            bindSlot(ctx, desc.slotKey);
            if (!ok) {
                VKR_ERR("compositor: framelink dmabuf import failed ('%s' slot %u, "
                        "mod 0x%llx)",
                        s.name.c_str(), f.slot, (unsigned long long)db.modifier);
                if (dupFd >= 0) close(dupFd);
                failedAdoption();
                continue;
            }
            s.imported[f.slot] = true;
            s.keys[f.slot] = desc.slotKey;
        }

        // Release everything BEFORE this frame - the Windows consumed-fence
        // rule, on every adoption including the FIRST. Recycling is monotonic
        // (value V frees all submits <= V), so this also frees the frames
        // latest-wins skipped. Releasing only a previously-held frame - the
        // first cut of this code - deadlocks when a decoder's startup burst
        // fills the ring before the second adoption: the consumer holds one
        // frame, has released nothing, and the producer has no free slot to
        // ever announce a newer one.
        s.lastReady=f._readyValue;
        if(f._readyValue>1)s.retire.push_back({ctx.frameCounter+2,f._readyValue-1});
        releaseFrame(); // bookkeeping only; storage stays pinned
        s.departing = false;
        s.currentSlot = (int64_t)s.keys[f.slot];
        s.lastAdoptNs=nowNs();if(!s.firstAdoptNs)s.firstAdoptNs=s.lastAdoptNs;++s.adopted;
        ++ctx.adoptions;
        ++ctx.videoFramesSeen;
        if (f.ptsNs > 0) {
            const int64_t d = nowNs() - f.ptsNs;
            // steady_clock is CLOCK_MONOTONIC on Linux - one epoch for every
            // process, so producer-submit -> adopt is measurable exactly as on
            // Windows. Reject nonsense rather than poison the average.
            if (d >= 0 && d < 1000000000ll) {
                const uint64_t us = (uint64_t)(d / 1000);
                ++*hopSamples;
                *hopTotalUs += us;
                if (us > *hopMaxUs) *hopMaxUs = us;
            }
        }
    }
}

} // namespace

// ---- the compositor loop ----------------------------------------------------

int runWaylandCompositor(const WaylandOptions& options) {
    // N > 0: verification mode (wait for N frames, dump, exit; 60 s deadline).
    // N < 0: endless headless service (SSH-only receiver boxes); periodic
    // dumps via VKR_SNAPSHOT_SECONDS land in scene-headless.ppm.
    const bool headless = options.headlessFrames != 0;
    const bool finite = options.headlessFrames > 0;

    VkContext vk;
    uint32_t initW = options.windows.empty() ? 1280 : (uint32_t)options.windows[0].width;
    uint32_t initH = options.windows.empty() ? 720 : (uint32_t)options.windows[0].height;
    if (!vk.init(initW, initH, "vkrender compositor", headless)) return 1;

    VideoPass pass;
    if (!pass.init(vk, options.shaderDir, VideoPass::Geometry::Cube)) return 1;
    ExternalVideoPool pool;

    SceneHost host;
    host.setOutputs(options.sceneOutputs);
    if (!host.init(vk, options.shaderDir, options.windows)) return 1;

    Ctx ctx;
    ctx.vk = &vk;
    ctx.pool = &pool;
    ctx.pass = &pass;
    ctx.nv12Mods = queryModifiers(vk.phys, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
    {
        auto r8 = queryModifiers(vk.phys, VK_FORMAT_R8_UNORM);
        auto rg8 = queryModifiers(vk.phys, VK_FORMAT_R8G8_UNORM);
        for (uint64_t m : r8)
            if (std::find(rg8.begin(), rg8.end(), m) != rg8.end() &&
                std::find(ctx.nv12Mods.begin(), ctx.nv12Mods.end(), m) == ctx.nv12Mods.end())
                ctx.nv12Mods.push_back(m);
    }
    ctx.argbMods = queryModifiers(vk.phys, VK_FORMAT_B8G8R8A8_UNORM);
    VKR_LOG("compositor: advertising %zu NV12 modifiers, %zu ARGB modifiers",
            ctx.nv12Mods.size(), ctx.argbMods.size());

    // M39: framelink input slots beside the Wayland socket. FL_MAP_CPU because
    // every current receiver writes decoded BGRA with the CPU (flImageMap) -
    // the exact analogue of the Windows receivers' UpdateSubresource - and a
    // buffer that cannot be mapped would shut them all out. On NVIDIA that
    // forces LINEAR (RENDERING|LINEAR together is refused, measured), which
    // Vulkan imports fine.
    std::vector<FlSlotIn> flSlots(kFlSlots);
    uint64_t hopSamples = 0, hopTotalUs = 0, hopMaxUs = 0;
    for (uint32_t i = 0; i < kFlSlots; ++i) {
        FlSlotIn& s = flSlots[i];
        s.name = options.socketName + ".in." + std::to_string(i);
        s.uid = 0x46500000ull + i; // distinct from Stream* ids ('FL' + index)
        if (flCreateChannelEx(s.name.c_str(), 640, 360, inputFormat(i,FL_FORMAT_BGRA8), 4, FL_MAP_CPU|FL_DEFER_RELEASE,
                              &s.channel) != FL_OK) {
            VKR_ERR("compositor: input channel '%s' creation failed", s.name.c_str());
            s.channel = nullptr;
        }
    }
    VKR_LOG("compositor: %u framelink input slots ready ('%s.in.0' .. '%s.in.%u')", kFlSlots,
            options.socketName.c_str(), options.socketName.c_str(), kFlSlots - 1);

    ctx.display = wl_display_create();
    ctx.loop = wl_display_get_event_loop(ctx.display);
    if (wl_display_add_socket(ctx.display, options.socketName.c_str()) != 0) {
        VKR_ERR("compositor: cannot create socket '%s' (already running?)",
                options.socketName.c_str());
        return 1;
    }
    wl_display_init_shm(ctx.display);
    wl_global_create(ctx.display, &wl_compositor_interface, 4, &ctx, compositorBind);
    wl_global_create(ctx.display, &wl_subcompositor_interface, 1, &ctx, subcompositorBind);
    wl_global_create(ctx.display, &xdg_wm_base_interface, 1, &ctx, wmBaseBind);
    wl_global_create(ctx.display, &zwp_linux_dmabuf_v1_interface, 3, &ctx, dmabufBind);
    VKR_LOG("compositor: listening on WAYLAND_DISPLAY=%s (up to %zu streams)",
            options.socketName.c_str(), kMaxStreams);

    const int64_t appStartNs = nowNs();
    const int64_t headlessDeadline = nowNs() + 60ll * 1000000000ll;
    int64_t rateWindowStart = nowNs();
    int exitCode = 0;
    AvInput avInput(options.avOptions);
    bool quit = false;
    constexpr size_t controlSlots=kFlSlots+kMaxStreams;
    ModerationPolicy<controlSlots> moderation;
    Stream* owners[controlSlots]{};
    ControlApi control;
    ControlSnapshot initial;initial.cameraAvailable=false;initial.cameraOn=false;
    initial.maxSlots=controlSlots;initial.previewAvailable=true;
    initial.displaysAvailable=headless && vk.canDisplay;
    control.publish(initial);
    const bool controlUp=options.controlPort>0 && control.start(options.controlPort);
    if(options.controlPort>0 && !controlUp){exitCode=1;quit=true;}

    while (!quit) {
        if(options.avExitOnEnd && avInput.done()){exitCode=avInput.succeeded?0:1;break;}
        vkrtrace::Scope loopTrace("service.iteration");
        if (headless) {
            if (finite && ctx.videoFramesSeen >= options.headlessFrames) break;
            if (finite && nowNs() > headlessDeadline) {
                VKR_ERR("compositor headless: no %d frames within 60 s (saw %d)",
                        options.headlessFrames, ctx.videoFramesSeen);
                exitCode = 2;
                break;
            }
            usleep(16000);
            static double snapshotEvery =
                getenv("VKR_SNAPSHOT_SECONDS") ? atof(getenv("VKR_SNAPSHOT_SECONDS")) : 0.0;
            static int64_t lastSnap = nowNs();
            if (!finite && snapshotEvery > 0.0 &&
                (double)(nowNs() - lastSnap) * 1e-9 >= snapshotEvery) {
                lastSnap = nowNs();
                vkQueueWaitIdle(vk.queue);
                vkrDumpTarget(vk, host.commandPool(), "scene-headless.ppm");
            }
        }

        wl_event_loop_dispatch(ctx.loop, 0);
        wl_display_flush_clients(ctx.display);

        // Deferred slot teardown (buffer/stream destruction).
        if (!ctx.slotsToRemove.empty()) {
            vkDeviceWaitIdle(vk.device);
            for (auto& e : ctx.retiring) releaseBuffer(e.second);
            ctx.retiring.clear();
            for (uint32_t key : ctx.slotsToRemove) {
                pass.removeSlot(vk, key);
                pool.removeSlot(vk, key);
            }
            ctx.slotsToRemove.clear();
        }

        adoptStreams(ctx);
        avInput.service([&](const AvInput::Request& request){
            auto& s=flSlots.front();
            vkDeviceWaitIdle(vk.device); // session boundary only, never per frame
            host.forgetStream(s.uid);flDropRing(ctx,s);
            if(s.channel)flClose(s.channel);s.channel=nullptr;
            s.generation=0;s.retire.clear();s.lastReady=0;
            if(!request.end){s.name=request.name;
                if(flCreateChannelEx(s.name.c_str(),request.config.video.width,request.config.video.height,
                    (flFormat)request.config.video.format,8,FL_MAP_CPU|FL_DEFER_RELEASE,&s.channel)!=FL_OK)
                    throw std::runtime_error("A/V video channel creation failed");
            }
        });
        flPollSlots(ctx, flSlots, avInput, host, &hopSamples, &hopTotalUs, &hopMaxUs);
        // Stable slots while each Wayland surface lives; FrameLink owns 0..11.
        for(size_t i=kFlSlots;i<controlSlots;i++)if(owners[i]) {
            auto it=std::find(ctx.streams.begin(),ctx.streams.end(),owners[i]);
            if(it==ctx.streams.end() || (*it)->controlSlot!=(int)i) {
                owners[i]=nullptr;moderation.hidden[i]=moderation.pinned[i]=false;
                if(moderation.spotlight==(int)i)moderation.spotlight=-1;
            }
        }
        for(auto* stream:ctx.streams)if(stream->controlSlot<0 && stream->currentSlot>=0 && !hasContentChild(ctx,stream)) {
            for(size_t i=kFlSlots;i<controlSlots;i++)if(!owners[i]){owners[i]=stream;stream->controlSlot=(int)i;break;}
        }
        for(const auto& c:control.drainCommands()) {
            using T=ControlCommand::Type;
            if(c.type==T::Layout) {
                if(c.str=="wall")host.setLayout(SceneLayout::Wall);
                else if(c.str=="presentation")host.setLayout(SceneLayout::Presentation);
                else if(c.str=="single-cube")host.setLayout(SceneLayout::SingleCube);
                else host.setLayout(SceneLayout::TwoCubes);
            } else if(c.type==T::AudioSelect)avInput.selectSession(c.str);
            else if(c.type==T::AudioMute)avInput.muted=c.i!=0;
            else if(c.type==T::Pause){avInput.paused=c.i!=0;host.setPaused(c.i!=0);}
            else if(c.type==T::Config){if(c.i>0)moderation.maxVisible=c.i;if(c.d>=1)moderation.rotateSeconds=c.d;}
            else if(c.type==T::Unspotlight)moderation.spotlight=-1;
            else if(c.type==T::Display)host.setDisplayActive(vk,c.slot,c.i!=0);
            else if(c.slot>=0 && (size_t)c.slot<controlSlots) {
                unsigned i=c.slot;
                switch(c.type) {
                case T::Hide:moderation.hidden[i]=true;break;
                case T::Show:moderation.hidden[i]=false;break;
                case T::Pin:moderation.pinned[i]=true;moderation.toFront(i);break;
                case T::Unpin:moderation.pinned[i]=false;break;
                case T::Spotlight:moderation.spotlight=i;break;
                case T::Front:moderation.toFront(i);break;
                case T::Kick:
                    if(i<kFlSlots) {
                        // Explicit kick is rare. Finish sampling before closing ring ownership.
                        vkDeviceWaitIdle(vk.device);
                        auto& slot=flSlots[i];host.forgetStream(slot.uid);flDropRing(ctx,slot);
                        if(slot.channel)flClose(slot.channel);
                        slot.channel=nullptr;slot.lastReady=0;slot.generation=0;
                        slot.retire.clear();slot.departing=false;slot.adopted=0;slot.firstAdoptNs=slot.lastAdoptNs=0;
                        flCreateChannelEx(slot.name.c_str(),640,360,inputFormat(i,FL_FORMAT_BGRA8),4,FL_MAP_CPU|FL_DEFER_RELEASE,&slot.channel);
                    } else if(owners[i]) {
                        auto* client=wl_resource_get_client(owners[i]->wlSurface);
                        for(auto* stream:ctx.streams)if(wl_resource_get_client(stream->wlSurface)==client)host.forgetStream((uint64_t)(uintptr_t)stream);
                        wl_client_destroy(client);
                        // One Wayland client may own multiple surfaces, all now destroyed.
                        for(size_t j=kFlSlots;j<controlSlots;j++)if(std::find(ctx.streams.begin(),ctx.streams.end(),owners[j])==ctx.streams.end()){owners[j]=nullptr;moderation.hidden[j]=moderation.pinned[j]=false;if(moderation.spotlight==(int)j)moderation.spotlight=-1;}
                    }
                    break;
                default:break;
                }
            }
        }


        // Leaf content streams -> SceneHost (it applies layout + enable set).
        std::vector<SceneHost::StreamRef> streams;
        for (Stream* s : ctx.streams)
            if (s->currentSlot >= 0 && !hasContentChild(ctx, s))
                streams.push_back({(uint64_t)(uintptr_t)s, s->currentSlot});
        for (FlSlotIn& s : flSlots) {
            if (s.currentSlot < 0) continue;
            // Withheld while departing, so the motion layer animates the tile
            // out; the ring is ours (consumer-owned) and stays sampleable.
            const bool expired =
                s.departing && (nowNs() - s.departedNs) > 3000000000ll; // 3 s
            if (s.departing && (expired || !host.retainsStream(s.uid))) {
                if(s.lastReady)s.retire.push_back({ctx.frameCounter+2,s.lastReady});
                s.currentSlot = -1;
                s.departing = false;
                continue;
            }
            if (s.departing) continue;
            streams.push_back({s.uid, s.currentSlot});
        }

        bool live[controlSlots]{};
        for(size_t i=0;i<kFlSlots;i++)live[i]=flSlots[i].currentSlot>=0 && !flSlots[i].departing;
        for(size_t i=kFlSlots;i<controlSlots;i++)live[i]=owners[i] && owners[i]->currentSlot>=0 && !hasContentChild(ctx,owners[i]);
        for(size_t i=0;i<controlSlots;i++)if(live[i])moderation.ensureOrdered(i);
        auto selected=moderation.select(live,nowNs());
        std::vector<SceneHost::StreamRef> filtered;
        for(auto i:selected){uint64_t uid=i<kFlSlots?flSlots[i].uid:(uint64_t)(uintptr_t)owners[i];for(auto stream:streams)if(stream.id==uid)filtered.push_back(stream);}
        streams=std::move(filtered);
        if(controlUp && control.previewDue())host.requestPreview();
        SceneHost::Extras extras; // wayland sync is CPU-side (sync-file polls)
        if (!host.frame(vk, pool, pass, ctx.passBound, streams, extras, &quit)) {
            exitCode = 1;
            break;
        }
        if(controlUp) {
            ControlSnapshot snap=initial;
            const char* layouts[]={"two-cubes","single-cube","wall","presentation"};
            switch(host.layout()){case SceneLayout::TwoCubes:snap.layout=layouts[0];break;case SceneLayout::SingleCube:snap.layout=layouts[1];break;case SceneLayout::Wall:snap.layout=layouts[2];break;default:snap.layout=layouts[3];}
            snap.avConfigured=avInput.configured();snap.avActive=avInput.active();snap.avAudioMaster=avInput.audioMaster();snap.avMuted=avInput.muted.load();snap.avPaused=avInput.paused.load();
            snap.avResets=avInput.schedule.resets;snap.avImportFailures=avInput.schedule.importFailures;
            snap.avGeneration=avInput.schedule.generation;
            const auto audioMetrics=avInput.metrics();
            snap.avAudioMetricsValid=audioMetrics.valid;
            const auto& am=audioMetrics.audio;
            snap.avAudioBlocks=am.audioBlocks;snap.avAudioSamples=am.audioSamples;
            snap.avAudioQueueNs=am.queueTimeNs;snap.avAudioPeakQueueNs=am.peakQueueTimeNs;
            snap.avAudioQueueBuffers=am.queueBuffers;snap.avAudioPeakQueueBuffers=am.peakQueueBuffers;
            snap.avAudioQueueEmpty=am.queueEmptyEvents;snap.avAudioQueueFull=am.queueFullEvents;
            snap.avAudioLate=am.lateAudioBlocks;snap.avAudioArrivalDriftNs=am.arrivalDriftNs;
            snap.avPresented=avInput.schedule.presented;snap.avHeld=avInput.schedule.held;
            snap.avSkipped=avInput.schedule.skipped;snap.avQueued=avInput.schedule.queued;snap.avMaxQueued=avInput.schedule.maxQueued;
            snap.avLate=avInput.schedule.late;snap.avInvalid=avInput.schedule.invalid;snap.avErrorNs=avInput.schedule.lastErrorNs;
            snap.paused=host.paused();snap.maxVisible=moderation.maxVisible;
            snap.rotateSeconds=moderation.rotateSeconds;snap.spotlightSlot=moderation.spotlight;
            for(size_t i=0;i<controlSlots;i++)if(live[i] || moderation.hidden[i] || moderation.pinned[i]) {
                ControlStreamInfo st;st.slot=i;st.connected=live[i];st.hidden=moderation.hidden[i];st.pinned=moderation.pinned[i];st.spotlit=moderation.spotlight==(int)i;
                st.visible=std::find(selected.begin(),selected.end(),i)!=selected.end();
                if(live[i]){auto key=i<kFlSlots?flSlots[i].currentSlot:owners[i]->currentSlot;auto* image=pool.find((uint32_t)key);if(image){st.width=image->width;st.height=image->height;}
                    auto last=i<kFlSlots?flSlots[i].lastAdoptNs:owners[i]->lastAdoptNs;
                    auto first=i<kFlSlots?flSlots[i].firstAdoptNs:owners[i]->firstAdoptNs;
                    auto count=i<kFlSlots?flSlots[i].adopted:owners[i]->adopted;
                    if(last){st.ageMs=(nowNs()-last)/1000000ll;if(last>first)st.fps=(count-1)*1e9/(last-first);}
                }
                snap.presenting|=live[i];snap.streams.push_back(st);
            }
            for(const auto& d:host.listDisplays())snap.displays.push_back({d.id,d.name,d.x,d.y,d.width,d.height,d.primary,d.active,d.u0,d.v0,d.u1,d.v1});
            control.publish(snap);
            std::vector<uint8_t> bgra;
            if(host.takePreview(bgra))control.publishPreview(bgra.data(),vk.swapExtent.width,vk.swapExtent.height);
        }
        ++ctx.frameCounter;
        for(auto& slot:flSlots)while(!slot.retire.empty() && slot.retire.front().first<=ctx.frameCounter) {
            flSignalConsumedSyncFd(slot.channel,slot.retire.front().second,-1);
            slot.retire.erase(slot.retire.begin());
        }
        if(const char* limit=getenv("VKR_TRACE_FRAMES"))
            if(vkrtrace::enabled() && std::strtoull(limit,nullptr,10)>0 &&
               ctx.frameCounter>=std::strtoull(limit,nullptr,10))quit=true;

        // Buffers retired >= 2 frames ago: the host has waited that frame
        // fence before recording this one, so their GPU reads are done.
        for (auto it = ctx.retiring.begin(); it != ctx.retiring.end();) {
            if (it->first + 2 <= ctx.frameCounter) {
                releaseBuffer(it->second);
                it = ctx.retiring.erase(it);
            } else {
                ++it;
            }
        }

        // Frame callbacks: composited - let clients produce the next frame.
        uint32_t tMs = (uint32_t)((nowNs() - appStartNs) / 1000000ll);
        for (Stream* s : ctx.streams) {
            for (wl_resource* cb : s->firedFrameCbs) {
                wl_callback_send_done(cb, tMs);
                wl_resource_destroy(cb);
            }
            s->firedFrameCbs.clear();
        }
        wl_display_flush_clients(ctx.display);

        if (nowNs() - rateWindowStart > 5000000000ll) {
            size_t flLive = 0;
            for (FlSlotIn& s : flSlots)
                if (s.currentSlot >= 0) ++flLive;
            if (hopSamples) {
                VKR_LOG("compositor: %llu adoptions in 5s (pool %zu slots, %zu wayland + "
                        "%zu framelink) hop avg %.2f ms max %.2f ms over %llu",
                        (unsigned long long)ctx.adoptions, pool.slots.size(),
                        ctx.streams.size(), flLive,
                        (double)hopTotalUs / (double)hopSamples / 1000.0,
                        (double)hopMaxUs / 1000.0, (unsigned long long)hopSamples);
            } else {
                VKR_LOG("compositor: %llu adoptions in 5s (pool %zu slots, %zu wayland + "
                        "%zu framelink)",
                        (unsigned long long)ctx.adoptions, pool.slots.size(),
                        ctx.streams.size(), flLive);
            }
            ctx.adoptions = 0;
            hopSamples = hopTotalUs = hopMaxUs = 0;
            rateWindowStart = nowNs();
        }
    }

    if(controlUp)control.stop();
    vkDeviceWaitIdle(vk.device);
    if (finite && exitCode == 0 && ctx.videoFramesSeen >= options.headlessFrames)
        vkrDumpTarget(vk, host.commandPool(), options.dumpPath.c_str());

    avInput.shutdown();
    for (FlSlotIn& s : flSlots) {
        if (s.channel) flClose(s.channel);
        s.channel = nullptr;
    }
    for (auto& e : ctx.retiring) releaseBuffer(e.second);
    ctx.retiring.clear();
    wl_display_destroy_clients(ctx.display);
    wl_display_destroy(ctx.display);

    host.destroy(vk);
    pass.destroy(vk);
    pool.destroy(vk);
    vk.destroy();
    return exitCode;
}

} // namespace vkr

#endif // !_WIN32
