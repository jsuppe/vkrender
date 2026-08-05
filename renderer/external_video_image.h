#pragma once

#include <cstdint>
#include <unordered_map>

#include <volk.h>

#include "protocol.h"
#include "vk_context.h"

namespace vkr {

// The renderer-side view of the decoder's shared surfaces: VkImages bound to
// imported external memory (the decoder's own GPU allocations - zero copy).
//
// Two population models share this class:
//  - Windows (fixed pool): PoolDesc carries N duplicated NT handles and two
//    shared D3D11 fences; create() imports everything up front. Slots are
//    keyed 0..N-1. Sync is GPU-side via the imported timeline semaphores.
//  - Linux (dynamic surfaces): PoolDesc is only the stream header; each
//    distinct decoder surface arrives later as SurfaceDesc + dma-buf fd and
//    addSurface() imports it with explicit DRM-modifier plane layouts. Slots
//    are keyed by the decoder's surface id. readySem/consumedSem stay null;
//    the producer syncs before announcing (vaSyncSurface) and the consumer
//    releases surfaces with FrameConsumed messages.
//
// NV12 sampling uses VkSamplerYcbcrConversion (the driver-validated path for
// imported video surfaces; on Windows, NV12 D3D11 import itself is broken on
// NVIDIA - see protocol.h - so Windows pools are BGRA/RGB10A2 instead).
class ExternalVideoPool {
public:
    struct Slot {
        // Combined mode: image[0]/view[0] is the whole (multi-planar or RGB)
        // image. Planar mode (Linux, when the driver cannot sample the
        // exported modifier as multi-planar NV12 - e.g. NVIDIA block-linear):
        // image[0]=luma as R8, image[1]=chroma as R8G8.
        VkImage image[2] = {};
        VkImageView view[2] = {};
        VkDeviceMemory memory[4] = {};
        // false for renderer-owned slots (M13 wl_shm upload path): no external
        // queue-family ownership transfer applies to those.
        bool external = true;
        // Owned slots only: persistently mapped host-visible memory + layout.
        void* mapped = nullptr;
        uint64_t rowPitch = 0;
        uint32_t width = 0, height = 0;
    };

    // Windows: full import of a PoolDesc (textures + fences).
    // Linux: records the stream header + creates the YCbCr conversion; slots
    // arrive via addSurface().
    bool create(VkContext& vk, const proto::PoolDescMsg& desc);

    // Linux only: import one dma-buf surface (fds from the IpcMessage; takes
    // ownership - closes them after import, or on failure).
    bool addSurface(VkContext& vk, const proto::SurfaceDescMsg& desc, const int* fds,
                    size_t fdCount);

#ifdef __ANDROID__
    // Android: import one AHardwareBuffer (received out-of-band on the
    // socket). Acquires its own reference; the caller keeps theirs.
    bool addSurfaceAHB(VkContext& vk, uint32_t key, struct AHardwareBuffer* buffer);
#endif

#ifdef _WIN32
    // M14 service mode: import one shared D3D11 texture dynamically (handle
    // valid in this process). Uses the pool's format; create() must have run
    // with poolSize == 0 (header-only).
    bool addSurfaceWin32(VkContext& vk, uint32_t key, uint64_t handleValue, uint32_t w,
                         uint32_t h);
#endif

    // Destroys one slot's Vulkan resources (caller must ensure no GPU work
    // references it - e.g. after vkDeviceWaitIdle). Needed when a producer
    // stream goes away: its AHardwareBuffers are freed and their addresses
    // (our slot keys) can be reused by a later allocation.
    void removeSlot(VkContext& vk, uint32_t key);

    // M13 wl_shm fallback: a renderer-owned LINEAR host-visible BGRA image the
    // caller fills via updateOwnedSurface (CPU upload - the non-zero-copy
    // path for clients without dma-buf support).
    bool addOwnedSurface(VkContext& vk, uint32_t key, uint32_t width, uint32_t height);
    bool updateOwnedSurface(VkContext& vk, uint32_t key, const void* pixels, uint64_t srcStride,
                            uint32_t width, uint32_t height);

    void destroy(VkContext& vk);

    bool valid() const { return headerValid; }
    const Slot* find(uint32_t key) const {
        auto it = slots.find(key);
        return it == slots.end() ? nullptr : &it->second;
    }

    std::unordered_map<uint32_t, Slot> slots;
    bool headerValid = false;
    // Decided on the first imported surface (needs the actual modifier):
    // false = combined NV12/RGB image with YCbCr conversion sampling,
    // true = per-plane R8+R8G8 images, YUV->RGB in the fragment shader.
    bool planar = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    VkSemaphore readySem = VK_NULL_HANDLE;    // Windows: decoder signals
    VkSemaphore consumedSem = VK_NULL_HANDLE; // Windows: we signal
    uint32_t width = 0, height = 0;
    proto::Matrix matrix = proto::Matrix::BT709;
    proto::Range range = proto::Range::Limited;
};

#ifdef _WIN32
// Imports a shared D3D11/D3D12 fence handle as a Vulkan timeline semaphore
// (M14 service mode has one ready/consumed pair per client, outside the pool).
bool importTimelineSemaphoreWin32(VkContext& vk, uint64_t handleValue, VkSemaphore* out);
#endif

} // namespace vkr
