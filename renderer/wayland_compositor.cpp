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

    // shm upload path: double-buffered owned slots
    uint32_t shmKey[2] = {0, 0};
    int shmNext = 0;
    uint32_t shmW = 0, shmH = 0;

    std::vector<wl_resource*> firedFrameCbs; // to signal after next composite
};

constexpr uint32_t kFramesInFlight = 2;
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

    // wl_buffers whose last GPU read is fenced by frame i.
    std::vector<wl_resource*> retiring[kFramesInFlight];
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
    for (auto& list : ctx.retiring)
        list.erase(std::remove(list.begin(), list.end(), buffer), list.end());
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
    // retiring lists (or gets released below with the shm slots removal).
    if (s->currentBuffer) {
        bool queued = false;
        for (auto& list : ctx.retiring)
            if (std::find(list.begin(), list.end(), s->currentBuffer) != list.end())
                queued = true;
        if (!queued) ctx.retiring[0].push_back(s->currentBuffer);
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
    if (ctx.poolCreated) {
        if (nv12 != ctx.poolIsNv12)
            VKR_WARN("compositor: mixed NV12/RGB sessions unsupported; ignoring stream");
        return nv12 == ctx.poolIsNv12;
    }
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
    ctx.passNeedsBind = true;
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
void adoptStreams(Ctx& ctx, uint64_t frameIndex) {
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
        if (s->currentBuffer)
            ctx.retiring[frameIndex % kFramesInFlight].push_back(s->currentBuffer);
        s->currentBuffer = s->waitingBuffer;
        s->currentSlot = (int64_t)buf->slotKey;
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

} // namespace

// ---- the compositor loop ----------------------------------------------------

int runWaylandCompositor(const WaylandOptions& options) {
    const bool headless = options.headlessFrames > 0;

    VkContext vk;
    if (!vk.init(1280, 720, "vkrender compositor", headless)) return 1;

    VideoPass pass;
    if (!pass.init(vk, options.shaderDir, VideoPass::Geometry::Cube)) return 1;
    BackgroundPass background;
    if (!background.init(vk, options.shaderDir))
        VKR_WARN("compositor: vaporwave backdrop unavailable");
    ExternalVideoPool pool;

    Ctx ctx;
    ctx.vk = &vk;
    ctx.pool = &pool;
    ctx.pass = &pass;
    ctx.nv12Mods = queryModifiers(vk.phys, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
    {
        // Per-plane fallback modifiers: importable as R8 + R8G8.
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

    uint64_t frameIndex = 0;
    uint64_t renders = 0;
    int64_t rateWindowStart = nowNs();
    const int64_t appStartNs = nowNs();
    const int64_t headlessDeadline = nowNs() + 60ll * 1000000000ll;
    int exitCode = 0;

    while (true) {
        if (vk.window) {
            glfwPollEvents();
            if (glfwWindowShouldClose(vk.window)) break;
        }
        if (headless) {
            if (ctx.videoFramesSeen >= options.headlessFrames) break;
            if (nowNs() > headlessDeadline) {
                VKR_ERR("compositor headless: no %d frames within 60 s (saw %d)",
                        options.headlessFrames, ctx.videoFramesSeen);
                exitCode = 2;
                break;
            }
            usleep(16000); // ~60 Hz pacing without a swapchain
        }

        wl_event_loop_dispatch(ctx.loop, 0);
        wl_display_flush_clients(ctx.display);

        // Deferred slot teardown (buffer/stream destruction).
        if (!ctx.slotsToRemove.empty()) {
            vkDeviceWaitIdle(vk.device);
            for (auto& list : ctx.retiring) {
                for (wl_resource* b : list) releaseBuffer(b);
                list.clear();
            }
            for (uint32_t key : ctx.slotsToRemove) {
                pass.removeSlot(vk, key);
                pool.removeSlot(vk, key);
            }
            ctx.slotsToRemove.clear();
        }

        adoptStreams(ctx, frameIndex);

        // Face assignment: leaf content streams cycle across the two cubes.
        int64_t faceSlots[12];
        for (int i = 0; i < 12; ++i) faceSlots[i] = -1;
        {
            std::vector<int64_t> active;
            for (Stream* s : ctx.streams)
                if (s->currentSlot >= 0 && !hasContentChild(ctx, s))
                    active.push_back(s->currentSlot);
            if (!active.empty())
                for (int i = 0; i < 12; ++i) faceSlots[i] = active[i % active.size()];
        }

        FrameSync& f = frames[frameIndex % kFramesInFlight];
        ++frameIndex;
        vkWaitForFences(vk.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
        // Fence retired: buffers parked on this frame go back to clients.
        for (wl_resource* b : ctx.retiring[(frameIndex - 1) % kFramesInFlight]) releaseBuffer(b);
        ctx.retiring[(frameIndex - 1) % kFramesInFlight].clear();

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

        float tSec = (float)((nowNs() - appStartNs) * 1e-9);
        float mvps[32];
        // Landscape window: cubes side by side; portrait: stacked.
        bool wide = vk.swapExtent.width >= vk.swapExtent.height;
        Mat4 a = cubeMvp(tSec, vk.swapExtent, wide ? -1.55f : 0.0f, wide ? 0.0f : 1.55f, 1.0f,
                         0.0f);
        Mat4 b = cubeMvp(tSec, vk.swapExtent, wide ? 1.55f : 0.0f, wide ? 0.0f : -1.55f, -1.0f,
                         2.1f);
        memcpy(mvps, a.m, sizeof(a.m));
        memcpy(mvps + 16, b.m, sizeof(b.m));
        if (!ctx.passBound)
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

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = headless ? 0 : 1;
        si.pWaitSemaphores = &f.acquireSem;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        si.signalSemaphoreCount = headless ? 0 : 1;
        si.pSignalSemaphores = &f.renderDoneSem;
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

        ++renders;
        if (nowNs() - rateWindowStart > 5000000000ll) {
            VKR_LOG("compositor: %llu adoptions / %llu frames in 5s (pool %zu slots, "
                    "%zu streams)",
                    (unsigned long long)ctx.adoptions, (unsigned long long)renders,
                    pool.slots.size(), ctx.streams.size());
            ctx.adoptions = renders = 0;
            rateWindowStart = nowNs();
        }
    }

    vkDeviceWaitIdle(vk.device);
    if (headless && exitCode == 0 && ctx.videoFramesSeen >= options.headlessFrames)
        vkrDumpTarget(vk, cmdPool, options.dumpPath.c_str());

    for (auto& list : ctx.retiring) {
        for (wl_resource* b : list) releaseBuffer(b);
        list.clear();
    }
    wl_display_destroy_clients(ctx.display);
    wl_display_destroy(ctx.display);

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

#endif // !_WIN32
