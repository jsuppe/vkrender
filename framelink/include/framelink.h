// framelink - zero-copy GPU frames between processes. Portable core.
//
// SPIKE. This is the Windows backend only, built to test the API shape from
// docs/FRAMELINK.md against a real consumer before committing to it. Scope is
// deliberately narrow: one producer per channel, BGRA8 only, no broker, no
// access control. Those are design questions, not oversights - see the doc.
//
// The ring belongs to the CONSUMER. A consumer calls flCreateChannel and owns
// the buffers; producers flOpenProducer onto a channel that already exists and
// never allocate. That is BufferQueue/AImageReader ownership, which is what
// Android forces and therefore what all platforms use.
//
//   consumer:  flCreateChannel -> flAcquireFrame / flRelease  -> flClose
//   producer:  flOpenProducer  -> flAcquireBuffer / flSubmit  -> flClose
//
// No platform type appears in this header. Turn an flImage into something you
// can draw with via the interop header for your platform (framelink_d3d11.h).
#pragma once
#include <stdint.h>

#if defined(_WIN32)
#  ifdef FL_EXPORTS
#    define FL_API __declspec(dllexport)
#  else
#    define FL_API __declspec(dllimport)
#  endif
#else
#  define FL_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Bumped on any wire-format change. Peers refuse each other on mismatch rather
// than misbehaving: shipping one side while an integrator builds the other is
// exactly how a silent version skew becomes someone else's afternoon.
#define FL_VERSION 1

typedef struct flChannel flChannel;
typedef struct flImage flImage;

typedef enum flResult {
    FL_OK = 0,
    FL_TIMEOUT = 1,          // no frame/buffer within timeoutMs; not an error
    FL_DISCONNECTED = 2,     // peer went away; handle valid, must still flClose
    FL_VERSION_MISMATCH = 3,
    FL_FORMAT_UNSUPPORTED = 4,
    FL_ADAPTER_MISMATCH = 5, // producer and consumer are on different GPUs
    FL_ACCESS_DENIED = 6,
    FL_NOT_FOUND = 7,        // flOpenProducer: no such channel. It never creates.
    FL_INVALID = 8,
    FL_OOM = 9,
    FL_INTERNAL = 10,
} flResult;

FL_API const char* flResultString(flResult r); // never NULL, never allocates

typedef enum flFormat {
    FL_FORMAT_BGRA8 = 1,   // the always-works path
    FL_FORMAT_NV12 = 2,    // NOT in the spike: NV12 D3D11->Vulkan import is
                           // broken on NVIDIA (chroma plane offset -> device
                           // lost), so it must be validated, not assumed
    FL_FORMAT_RGB10A2 = 3, // not in the spike
} flFormat;

typedef struct flChannelInfo {
    uint32_t version;
    uint32_t width, height;
    flFormat format;
    uint32_t poolSize;
    uint8_t  adapterLUID[8]; // the GPU the frames live on
} flChannelInfo;

// ---- consumer: owns the ring ------------------------------------------------

typedef struct flFrame {
    uint32_t slot;
    flImage* image;    // borrowed; valid until flRelease
    int64_t  ptsNs;    // -1 if the producer did not timestamp
    uint64_t sequence; // monotonic; a gap means frames were skipped
    // Internal release bookkeeping - pass the frame back to flRelease
    // unmodified. It identifies WHICH frame is being released, so the producer
    // is told this slot is free and not some later one that is still being read.
    uint64_t _readyValue;
} flFrame;

FL_API flResult flCreateChannel(const char* name, uint32_t width, uint32_t height,
                                flFormat format, uint32_t poolSize, flChannel** out);
FL_API flResult flAcquireFrame(flChannel*, flFrame* out, uint32_t timeoutMs);
FL_API flResult flRelease(flChannel*, const flFrame*);

// ---- producer: attaches ------------------------------------------------------

typedef struct flBuffer {
    uint32_t slot;
    flImage* image; // borrowed; valid until flClose
} flBuffer;

// Attaches to an EXISTING channel; FL_NOT_FOUND otherwise. Geometry is the
// consumer's, so read it from flQuery rather than asking for it here - the same
// reason an Android producer does not choose a Surface's size.
FL_API flResult flOpenProducer(const char* name, flChannel** out);
FL_API flResult flAcquireBuffer(flChannel*, flBuffer* out, uint32_t timeoutMs);

// Submit, having drawn with the channel's own D3D11 device. framelink signals
// readiness for you.
FL_API flResult flSubmit(flChannel*, const flBuffer*, int64_t ptsNs); // -1 = no pts

// Submit, having drawn with your OWN GPU API - Vulkan, D3D12, Metal.
//
// flSubmit signals the ready fence from framelink's D3D11 context, which orders
// nothing if your writes came from a different device: a Vulkan producer that
// used it would hand consumers half-drawn frames. Instead:
//
//   1. flSharedReadyFence() once, and import the handle as a timeline
//      semaphore (Vulkan: VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT).
//   2. flBeginSubmit() -> the value to signal for this frame.
//   3. queue your own work, signalling that value as part of it.
//   4. flEndSubmit() to publish the frame.
//
// The vkrender renderer is exactly this case: it composites with Vulkan and
// signals the imported semaphore inside its frame submit.
FL_API flResult flSharedReadyFence(flChannel*, void** handle); // borrowed
FL_API flResult flBeginSubmit(flChannel*, const flBuffer*, uint64_t* signalValue);
FL_API flResult flEndSubmit(flChannel*, const flBuffer*, uint64_t signalValue, int64_t ptsNs);

// ---- both --------------------------------------------------------------------

FL_API flResult flQuery(flChannel*, flChannelInfo* out);
FL_API void     flClose(flChannel*);

// No enumeration API. A consumer knows the name it created; a producer is
// configured with the name to attach to. Adding discovery later is easy,
// removing it is not.

#ifdef __cplusplus
}
#endif
