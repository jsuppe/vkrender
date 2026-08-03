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
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    // Windows: full import of a PoolDesc (textures + fences).
    // Linux: records the stream header + creates the YCbCr conversion; slots
    // arrive via addSurface().
    bool create(VkContext& vk, const proto::PoolDescMsg& desc);

    // Linux only: import one dma-buf surface (fds from the IpcMessage; takes
    // ownership - closes them after import, or on failure).
    bool addSurface(VkContext& vk, const proto::SurfaceDescMsg& desc, const int* fds,
                    size_t fdCount);

    void destroy(VkContext& vk);

    bool valid() const { return headerValid; }
    const Slot* find(uint32_t key) const {
        auto it = slots.find(key);
        return it == slots.end() ? nullptr : &it->second;
    }

    std::unordered_map<uint32_t, Slot> slots;
    bool headerValid = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    VkSemaphore readySem = VK_NULL_HANDLE;    // Windows: decoder signals
    VkSemaphore consumedSem = VK_NULL_HANDLE; // Windows: we signal
    uint32_t width = 0, height = 0;
    proto::Matrix matrix = proto::Matrix::BT709;
    proto::Range range = proto::Range::Limited;
};

} // namespace vkr
