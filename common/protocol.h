#pragma once

#include <cstdint>

// Wire protocol between vkdecoder (producer) and vkrenderer (consumer).
// Everything here is platform-agnostic: handles travel as opaque uint64 values
// whose meaning is platform-defined (Windows: NT HANDLE value already
// duplicated into the receiving process; Linux future: dma-buf fd passed via
// SCM_RIGHTS, with layout[]/drmModifier describing the planes).

namespace vkr::proto {

constexpr uint32_t kProtocolMagic = 0x31524B56; // "VKR1" little-endian
constexpr uint32_t kMaxPoolSize = 8;

enum class MsgType : uint32_t {
    Hello = 1,         // renderer -> decoder: pid + preferred adapter LUID
    PoolDesc = 2,      // decoder -> renderer: shared texture pool + fences
    FrameReady = 3,    // decoder -> renderer: slot decoded, fence value to wait
    FrameConsumed = 4, // renderer -> decoder: optional (fence is authoritative)
    StreamReset = 5,   // decoder -> renderer: pool obsolete, new PoolDesc follows
    Bye = 6,           // either direction: orderly shutdown
};

#pragma pack(push, 4)

struct MsgHeader {
    uint32_t magic;
    MsgType  type;
    uint32_t payloadSize;
};

struct HelloMsg {
    uint32_t rendererPid;
    uint32_t luidValid;     // nonzero if deviceLUID identifies the render adapter
    uint8_t  deviceLUID[8]; // VkPhysicalDeviceIDProperties::deviceLUID layout
};

// NV12 sharing works on Linux (dma-buf carries explicit plane layouts) but is
// broken for Windows D3D11->Vulkan import on current NVIDIA drivers (the
// driver's plane-1 offset does not match the D3D11 NV12 allocation), so the
// Windows decoder converts to BGRA8 via the D3D11 VideoProcessor and shares
// single-plane textures.
enum class SharedFormat : uint32_t { NV12 = 1, BGRA8 = 2 };
enum class Matrix : uint32_t { BT601 = 0, BT709 = 1 };
enum class Range  : uint32_t { Limited = 0, Full = 1 };

struct PlaneLayout {
    uint64_t offset;
    uint64_t pitch;
};

struct PoolDescMsg {
    uint32_t width;   // display width (even)
    uint32_t height;  // display height (even)
    SharedFormat format;
    Matrix   matrix;
    Range    range;
    uint32_t poolSize;
    uint32_t planeCount;                  // 2 for NV12
    uint32_t _pad;
    uint64_t textureHandle[kMaxPoolSize]; // valid in the renderer process
    PlaneLayout layout[2];                // unused on Windows (OPTIMAL tiling)
    uint64_t drmModifier;                 // unused on Windows
    uint64_t readyFenceHandle;            // timeline fence, decoder signals
    uint64_t consumedFenceHandle;         // timeline fence, renderer signals
};

// Contract: before sampling slot, wait readyFence >= readyValue.
// The renderer signals consumedFence = V to promise it will never again read
// any frame whose readyValue is <= V. The decoder may reuse slot i once
// consumedFence completed value >= the last readyValue it signaled for slot i.
struct FrameReadyMsg {
    uint32_t slot;
    uint32_t _pad;
    uint64_t readyValue;
    int64_t  ptsNs; // presentation timestamp in ns, -1 if unknown
};

struct FrameConsumedMsg {
    uint32_t slot;
    uint32_t _pad;
    uint64_t readyValue;
};

#pragma pack(pop)

} // namespace vkr::proto
