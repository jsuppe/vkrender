#pragma once

#include <cstdint>
#include <vector>

#include <volk.h>

#include "protocol.h"
#include "vk_context.h"

namespace vkr {

// The renderer-side view of the decoder's shared texture pool: VkImages bound
// to imported D3D11 texture memory (same GPU allocations, zero copy), plus the
// two shared fences imported as timeline semaphores.
//
// Sampling uses VkSamplerYcbcrConversion (the driver-validated path for
// imported video surfaces; manual R8/R8G8 plane views of imported D3D11 NV12
// textures lose the device on NVIDIA). The conversion object carries the
// stream's colorimetry and is rebuilt whenever the decoder sends a new pool.
class ExternalVideoPool {
public:
    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE; // NV12 view with YCbCr conversion
    };

    bool create(VkContext& vk, const proto::PoolDescMsg& desc);
    void destroy(VkContext& vk);

    bool valid() const { return !slots.empty(); }

    std::vector<Slot> slots;
    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    VkSemaphore readySem = VK_NULL_HANDLE;    // decoder signals (wait before sampling)
    VkSemaphore consumedSem = VK_NULL_HANDLE; // we signal (frees slots for reuse)
    uint32_t width = 0, height = 0;
    proto::Matrix matrix = proto::Matrix::BT709;
    proto::Range range = proto::Range::Limited;
};

} // namespace vkr
