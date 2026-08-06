#pragma once

#include <cstdint>

// Wire protocol between vkdecoder (producer) and vkrenderer (consumer).
// Everything here is platform-agnostic: handles travel as opaque uint64 values
// whose meaning is platform-defined (Windows: NT HANDLE value already
// duplicated into the receiving process; Linux future: dma-buf fd passed via
// SCM_RIGHTS, with layout[]/drmModifier describing the planes).

namespace vkr::proto {

constexpr uint32_t kProtocolMagic = 0x31524B56; // "VKR1" little-endian
constexpr uint32_t kMaxPoolSize = 8;   // Windows: fixed pool, handles in PoolDesc
constexpr uint32_t kMaxSurfaces = 256; // dynamic surface announcements (VAAPI pools
                                       // exceed 8; Android M12 composites up to 12
                                       // streams x ~14-19 codec buffers each)

enum class MsgType : uint32_t {
    Hello = 1,         // renderer -> decoder: pid + preferred adapter LUID
    PoolDesc = 2,      // decoder -> renderer: shared texture pool + fences
    FrameReady = 3,    // decoder -> renderer: slot decoded, fence value to wait
    FrameConsumed = 4, // renderer -> decoder: optional (fence is authoritative)
    StreamReset = 5,   // decoder -> renderer: pool obsolete, new PoolDesc follows
    Bye = 6,           // either direction: orderly shutdown
    StreamStatus = 7,  // decoder -> renderer: source connectivity for UI
    SurfaceDesc = 8,   // decoder -> renderer: one shared surface (Linux: dma-buf
                       // fd travels as SCM_RIGHTS ancillary data on this message)
    ClientHello = 9,   // M14 (Windows service mode): producer client -> renderer;
                       // the renderer replies Hello (adapter LUID) + PoolDesc
                       // whose handles are already duplicated into the client.
    ConsumerHello = 10,  // M24: consumer client -> renderer: negotiate a scene
                         // view (resolution/fps/layout/stream subset)
    ConsumerConfig = 11, // renderer -> consumer: the accepted view, followed by
                         // a PoolDesc ring where the RENDERER is the producer
                         // (renderer signals readyFence + sends FrameReady;
                         // the consumer signals consumedFence)
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
enum class SharedFormat : uint32_t {
    NV12 = 1,
    BGRA8 = 2,
    RGB10A2 = 3, // DXGI R10G10B10A2 == VK_FORMAT_A2B10G10R10_UNORM_PACK32; used for 10-bit (P010) sources
};

enum class StreamPhase : uint32_t { Connecting = 0, Streaming = 1, Error = 2 };
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

struct StreamStatusMsg {
    StreamPhase phase;
};

// M14: a producer client asks the Windows renderer service for a buffer ring.
// The service allocates the shared textures + fence pair (renderer-owned pool,
// mirroring Android's AImageReader) and answers with PoolDesc. Slot reuse is
// client-side: track the readyValue signaled per slot and reuse a slot only
// once consumedFence.GetCompletedValue() >= that value.
struct ClientHelloMsg {
    uint32_t clientPid; // for handle duplication into the client process
    uint32_t width;
    uint32_t height;
    SharedFormat format; // BGRA8 supported
    uint32_t poolSize;   // requested ring depth, clamped to kMaxPoolSize
};

// M24: a consumer asks the renderer for a negotiated VIEW of the scene,
// delivered as a shared-texture ring (the mirror image of ClientHello). The
// renderer may clamp resolution/fps; layout/stream selection reuse the M15
// scene vocabulary. After ConsumerConfig comes a PoolDesc whose fences follow
// the M14 contract with the roles reversed: the renderer signals readyFence
// and sends FrameReady; the consumer signals consumedFence when done reading
// (promising never to read frames with readyValue <= the signaled value).
struct ConsumerHelloMsg {
    uint32_t clientPid; // for handle duplication into the consumer process
    uint32_t width;     // requested view size (0 = renderer default)
    uint32_t height;
    uint32_t fps;        // requested delivery cap (0 = every rendered frame)
    uint32_t layout;     // 0 = current on-screen layout; 1 = two-cubes,
                         // 2 = single-cube, 3 = stream wall
    uint32_t streamMask; // bitmask of stream indices to include; 0 = all
    uint32_t poolSize;   // requested ring depth, clamped to kMaxPoolSize
};

struct ConsumerConfigMsg {
    uint32_t width;  // accepted view size
    uint32_t height;
    uint32_t fps;    // accepted delivery cap (0 = every rendered frame)
};

// Linux: announces one decoder surface. The dma-buf fd(s) ride along as
// SCM_RIGHTS ancillary data (IpcMessage::fds), in object order. Sent once per
// distinct surface the first time it appears; afterwards FrameReady refers to
// it by slotKey. Only numObjects == 1 (all planes in one buffer object) is
// currently supported by the renderer.
struct SurfaceDescMsg {
    uint32_t slotKey;       // stable surface identity (VAAPI: VASurfaceID)
    uint32_t numPlanes;     // 2 for NV12
    uint32_t numObjects;    // == count of fds attached to this message
    uint32_t _pad;
    uint64_t drmModifier;   // DRM format modifier of the buffer object(s)
    PlaneLayout plane[4];   // per plane; offset is within its buffer object
    uint32_t planeObject[4]; // which attached fd backs each plane
    uint64_t objectSize[4];
};

#pragma pack(pop)

} // namespace vkr::proto
