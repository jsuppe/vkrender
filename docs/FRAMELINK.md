# framelink — zero-copy GPU frames between processes

Windows, Linux and Android from one C API; macOS later (see the caveat).

Draft. `framelink` is a working name; the prefix `fl` below changes with it.

A proposal to lift the shared-texture transport out of vkrender into a
standalone library an integrator can build against, so that "receive a rendered
image and do something with it" is a dependency rather than a port.

Nothing here is implemented yet. It exists to make the shape reviewable.

## What it is, and what it deliberately is not

**Is:** a frame channel between processes on one machine. One process publishes
GPU frames; others read the *same GPU memory*. No copies, no encode/decode, no
network. The same calls on Windows, Linux and Android.

**Is not:** a compositor, a scene graph, a layout engine, a codec, or a
cross-machine transport.

That line is the whole design. Today `vkrConnectConsumer` asks the renderer to
*render a 1280x720@30 view for you* — that is a vkrender feature wearing a
transport's clothes, and publishing it would hand every integrator a compositor
they did not ask for. Under this split vkrender becomes an ordinary user of the
library: a process that happens to subscribe to several channels and publish a
composited one. `vkr_vcam` then genuinely is "a consumer of framelink" rather
than "a consumer of vkrender", which is the property we actually want.

## Topology: the ring stays consumer-owned

An earlier draft of this document proposed inverting ownership so that
producers own channels and consumers subscribe. **That was wrong, and
cross-platform is what proves it.**

The existing Windows API is already BufferQueue-shaped, and says so:

> the renderer owns the buffer ring (like Android's AImageReader); the client
> dequeues a shared D3D11 texture, renders into it with its own device, and
> queues it back

Android's service interface is the same model in Java:

```java
Surface acquireInputSurfaceSlot(int slot);   // producer renders into it
void    releaseInputSurfaceSlot(int slot);
```

A Surface *is* a BufferQueue whose buffers belong to the consumer. On Android
that ownership is not a design choice we get to make — it is how the platform
works. Producer-owned channels would have ported to Windows and Linux and then
hit a wall on Android, which is exactly the outcome a cross-platform API exists
to prevent.

So the topology stays as it is, and the library simply names it:

```
  miracast ─┐
  airplay  ─┼─► ring owned by the CONSUMER (vkrenderer) ─► composite ring ─► vcam
  whip     ─┤                                                            └─► OBS, recorder, ...
  rtsp     ─┘
```

A consumer (or a broker acting for one) creates a channel and owns its buffers;
producers attach, `flAcquireBuffer` (dequeue), render, `flSubmit` (queue). This
is `dequeueBuffer`/`queueBuffer` under a different spelling, which is precisely
why it maps onto every platform's native mechanism without argument.

It also means a producer that starts before any consumer has nothing to attach
to — correct behaviour, not a gap: there are no buffers until someone allocates
them.

## Draft API

C ABI, opaque handles, `extern "C"` — so C#, Rust, Python and Go bind without
C++ ABI pain. This keeps the existing shape (it is already this) and adds the
four things a public library needs: versioning, a raw-handle escape hatch,
an access model, and explicit contracts.

**The core header names no platform type.** `ID3D11Texture2D*` cannot appear in
an API that also has to compile for Linux, Android and eventually macOS, so the
image is opaque and each platform gets a small interop header
(`framelink_d3d11.h`, `framelink_dmabuf.h`, `framelink_ahb.h`,
`framelink_vulkan.h`). Call sequences are then byte-identical everywhere; only
the line that turns a handle into a texture differs.

```c
/* framelink.h - portable core. No platform types appear here. */
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

#define FL_VERSION 1   /* bumped on any wire-format change; see flOpenProducer */

typedef struct flChannel flChannel;

typedef enum flResult {
    FL_OK = 0,
    FL_TIMEOUT = 1,          /* no frame within timeoutMs; not an error */
    FL_DISCONNECTED = 2,     /* peer went away; handle still valid, must close */
    FL_VERSION_MISMATCH = 3, /* peer speaks a different FL_VERSION - REFUSED */
    FL_FORMAT_UNSUPPORTED = 4,
    FL_ADAPTER_MISMATCH = 5, /* producer and consumer are on different GPUs */
    FL_ACCESS_DENIED = 6,
    FL_NOT_FOUND = 7,
    FL_INVALID = 8,
    FL_OOM = 9,
    FL_INTERNAL = 10,
} flResult;

FL_API const char* flResultString(flResult r);   /* never NULL, never allocates */

typedef enum flFormat {
    FL_FORMAT_BGRA8 = 1,     /* DXGI_FORMAT_B8G8R8A8_UNORM   - always supported */
    FL_FORMAT_NV12 = 2,      /* DXGI_FORMAT_NV12             - see caveat below */
    FL_FORMAT_RGB10A2 = 3,   /* DXGI_FORMAT_R10G10B10A2_UNORM */
} flFormat;

typedef struct flChannelInfo {
    uint32_t version;        /* peer's FL_VERSION */
    uint32_t width, height;
    flFormat format;
    uint32_t fpsHint;        /* 0 if the producer does not pace */
    uint8_t  adapterLUID[8]; /* the GPU the frames live on */
    uint32_t poolSize;
} flChannelInfo;

/* ---- producer ---------------------------------------------------------- */

/* An opaque image. Turn it into something you can render to with the interop
 * header for your platform - see "Cross-platform" below. */
typedef struct flImage flImage;

typedef struct flBuffer {
    uint32_t slot;
    flImage* image;  /* borrowed; valid until flClose (see Lifetimes) */
} flBuffer;

/* Attach as a producer to a channel a consumer already created. The ring
 * belongs to the consumer, so width/height/format are REPORTED by flQuery,
 * not requested here - the same reason an Android producer does not get to
 * pick a Surface's geometry. */
FL_API flResult flOpenProducer(const char* name, flChannel** out);

FL_API flResult flAcquireBuffer(flChannel*, flBuffer* out, uint32_t timeoutMs);
FL_API flResult flSubmit(flChannel*, const flBuffer*, int64_t ptsNs); /* -1 = no pts */

/* Republish at a new size/format. Consumers see FL_DISCONNECTED then resubscribe;
 * this is the StreamReset path, made explicit rather than implicit. */
FL_API flResult flReconfigure(flChannel*, uint32_t width, uint32_t height, flFormat);

/* ---- consumer ---------------------------------------------------------- */

typedef struct flFrame {
    uint32_t slot;
    flImage* image;    /* borrowed; valid until flRelease */
    int64_t  ptsNs;
    uint64_t sequence; /* monotonic; gaps mean frames were skipped */
} flFrame;

/* Creates the channel and OWNS the ring. */
FL_API flResult flCreateChannel(const char* name, uint32_t width, uint32_t height,
                                flFormat format, uint32_t poolSize, flChannel** out);
FL_API flResult flAcquireFrame(flChannel*, flFrame* out, uint32_t timeoutMs);
FL_API flResult flRelease(flChannel*, const flFrame*);

/* ---- both -------------------------------------------------------------- */

FL_API flResult flQuery(flChannel*, flChannelInfo* out);
FL_API void     flClose(flChannel*);

/* No enumeration API. An earlier draft needed one because producers owned
 * channels and had to be discovered; with consumer-owned rings the consumer
 * already knows the name it created, and a producer is configured with the name
 * it should attach to. Adding discovery later is easy; removing it is not. */

/* Sync, expressed portably. Wait for `waitValue` on the channel's timeline
 * before reading an acquired image; signal after writing one. Platform headers
 * expose the underlying object (D3D fence / sync fd / VkSemaphore) for callers
 * who schedule their own GPU work. */
FL_API flResult flFrameTimeline(flChannel*, const flFrame*, uint64_t* waitValue);

#ifdef __cplusplus
}
#endif
```

Per-platform interop, one line each — the only code that differs:

```c
/* framelink_d3d11.h  (Windows) */
FL_API ID3D11Texture2D* flImageD3D11(flImage*);
FL_API void*            flImageSharedHandle(flImage*);   /* NT handle, for D3D12/CUDA */

/* framelink_dmabuf.h (Linux) */
typedef struct flDmabuf { int fd[4]; uint64_t offset[4], pitch[4];
                          uint32_t planes, fourcc; uint64_t modifier; } flDmabuf;
FL_API flResult flImageDmabuf(flImage*, flDmabuf* out);   /* fds borrowed, do not close */

/* framelink_ahb.h    (Android) */
FL_API struct AHardwareBuffer* flImageAHardwareBuffer(flImage*);

/* framelink_vulkan.h (Windows, Linux, Android - and macOS via MoltenVK) */
FL_API flResult flImageVulkan(flImage*, VkDevice, VkImage* out, VkDeviceMemory* out2);
```

## The four additions, and why each earns its place

**`FL_VERSION` refused at handshake.** The existing wire has a magic
(`kProtocolMagic`, "VKR1") but no version, so a mismatch is undefined rather
than refused. When we ship one side and an integrator builds the other, that is
the difference between a clear error and a silent misbehaviour. This session
alone lost an hour to a stale `vkr_airplay.exe` that ignored `VKR_PIN` because
it predated the feature, and came close to the same thing with the vcam DLLs.
Cheap now, impossible to retrofit once third parties have shipped.

**`flFrameInterop`.** `ID3D11Texture2D*` in the ABI locks integrators into
D3D11. The object underneath is already a shared NT handle, so exposing it costs
nothing and is what lets a D3D12 or Vulkan or CUDA consumer participate. The
D3D11 helpers stay as the convenience tier. This is the single change that most
affects how seamless this feels to someone who is not us.

**An access model.** Today any process in the session that knows the pipe name
can publish frames or read them. For our own appliance that is fine. For a
published library whose flagship consumer is *a virtual camera*, "any process
can inject video into your webcam" has to be either fixed or documented in
capitals. `FL_ACCESS_DENIED` is a placeholder for a decision not yet made —
default-open with an opt-in ACL is probably right, but it is a decision, not an
oversight.

**`FL_ADAPTER_MISMATCH` and a narrow format list.** Producer and consumer may
sit on different GPUs; vkrender sidesteps this by LUID-matching internally. A
library must detect and refuse rather than hand someone a device-lost. Formats
are sharp here too: NV12 D3D11 -> Vulkan import is broken on NVIDIA (chroma
plane offset mismatch, device lost), which is why vkrender shares BGRA via
VideoProcessor. BGRA8 is the always-works path; anything else must be validated
at open, not discovered at first frame.

## Cross-platform

One API, four backends. Everything below already exists somewhere in vkrender
except the macOS column, which is the only genuinely new work.

| | Windows | Linux | Android | macOS (later) |
|---|---|---|---|---|
| buffer | `ID3D11Texture2D` shared NT handle | dma-buf fd + DRM modifier + plane layouts | `AHardwareBuffer` / Surface | `IOSurface` |
| handle transport | `DuplicateHandle`, value over named pipe | `SCM_RIGHTS` over unix socket | Binder (AIDL) or socket | mach port / IOSurface ID |
| sync | shared D3D11/D3D12 timeline fence | sync_file fd, or VK timeline semaphore fd | acquire fence fd | `MTLSharedEvent` |
| IPC | named pipe | unix domain socket | Binder | unix socket / XPC |
| status in vkrender | shipped (`vkrender_client`) | shipped inside the renderer | shipped as AIDL + Surface | none |

**Vulkan is the portable tier.** It exists on Windows, Linux and Android, and on
macOS through MoltenVK, so an integrator who imports via `flImageVulkan` writes
one code path for every platform. The native accessors stay for people already
committed to D3D11, EGL or Metal. Making Vulkan the recommended path — rather
than a fallback — is what actually delivers "the programming interface remains
the same".

**What is genuinely missing today.** `vkrender_client` is Windows-only: there is
no Linux or Android build of this C API. Linux does its sharing inside the
renderer, and Android exposes Java `Surface` over Binder. So this is not "port
the Windows library" — it is implementing the same C API twice more on top of
mechanisms that already work. The protocol is ready for it: `SurfaceDescMsg`
already carries plane layouts, DRM modifiers and per-plane object indices
precisely so the wire format spans Windows and dma-buf.

**Android's Java surface stays.** An Android app that wants a `Surface` should
keep getting one; `flOpenProducer` sits alongside as the native-code path and
both feed the same ring. Replacing the AIDL with a C API would be a regression
for the ordinary Android integrator.

**macOS is the risk, and it is not small.** IOSurface is a good analogue of the
other three, but MoltenVK's external-memory support is narrower than native
Vulkan, and Metal's sync primitives do not map onto a timeline semaphore as
neatly as D3D12 fences do. Treat the macOS column as a design intention, not a
plan, until someone has imported an IOSurface into MoltenVK and waited on a
shared event. Nothing else in this document should be allowed to depend on it.

## Migration sketch

The library is essentially today's `client/vkrender_client.cpp` (361 lines) plus
`common/protocol.h` (166) and the Win32 IPC. It links only `d3d11` and `dxgi`.
No Vulkan, no GStreamer, and nothing GPL-adjacent — the UxPlay code lives in the
separate `vkrender-airplay` repo and stays there.

| today | after |
|---|---|
| `vkrConnect(pipe, dev, w, h)` | `flOpenProducer(name, &ch)` + `flQuery` for geometry |
| `vkrDequeueBuffer` / `vkrQueueBuffer` | `flAcquireBuffer` / `flSubmit` |
| `vkrGetDevice` | `flImageD3D11` / `flImageVulkan` / … (platform header) |
| `vkrConnectConsumer` (negotiates a *scene view*) | **stays in vkrender** — it is a compositor feature |
| `vkrAcquireFrame` / `vkrReleaseFrame` | `flAcquireFrame` / `flRelease` |
| `StreamReset` message | `flReconfigure` |
| Android `acquireInputSurfaceSlot` | kept, plus `flOpenProducer` for native code |

`vkr_vcam` becomes the reference consumer nearly unchanged: replace
`vkrConnectConsumer` with `flOpenConsumer("vkrender.scene", ...)` and the
acquire/release pair with the `fl` equivalents. It is a real Media Foundation
virtual camera in a couple of hundred lines, which makes it a far more
persuasive sample than a toy.

vkrender's renderer keeps its consumer-view negotiation, because that is
genuinely about compositing: it is the thing deciding what a 1280x720@30 view of
a four-tile scene means.

## Decided: consumer-owned, with no create-if-absent

Only a consumer creates a channel. `flOpenProducer` attaches to one that exists
and fails `FL_NOT_FOUND` otherwise — it never allocates. A standalone publisher
with no consumer running uses the optional broker below.

The alternative — letting a producer create a ring and hand it over to the first
consumer — was rejected. It is friendlier for exactly one scenario and costs:

- **Android cannot do it idiomatically.** Buffer ownership cannot be handed to a
  `Surface` consumer that appears later, so it needs a second, non-Surface path
  over raw `AHardwareBuffer`. That forks the thing this API exists to unify, and
  it is a structural crack rather than a cost.
- **Creation becomes a race** needing atomic create-or-attach, with a different
  primitive on each platform.
- **The same call means two things depending on start order** — sometimes
  choosing geometry, sometimes accepting it.
- **Creator exit is unanswerable.** Either an unrelated process quitting tears
  down a live consumer, or something else owns the allocation, which is a broker
  that has not admitted what it is.
- **Negotiation turns bidirectional**, so `flReconfigure` runs during ordinary
  startup instead of on a rare stream reset.
- **Name squatting.** If anyone may create a channel by name, a hostile process
  can pre-create the one the compositor expects and own it. The flagship
  consumer is a virtual camera, so this is not theoretical.

**The broker (`framelinkd`, optional).** A small daemon that creates channels on
request, for publishers with no consumer. One process to deploy, in exchange for
one ownership rule, one code path per platform, no creation race, a well-defined
owner for access control, and Android staying on the Surface path. It is not
required for the vkrender case, where the renderer is already the consumer.

## Open questions

1. **Fan-out is N channels, not one channel with N readers.** ~~What happens to
   a slow consumer?~~ Measured in the spike, and the single-consumer half is
   settled. A consumer sleeping 50 ms per frame against a flat-out producer:
   20 frames read, **57 skipped**, 0 mismatched, and the producer managed
   27.6 fps with 21 stalls. So the behaviour is *bounded* back-pressure plus
   drops, not one or the other: latest-frame-wins discards stale frames at the
   message level, and because the consumed fence is monotonic, releasing one
   frame frees every older slot at once — so the producer runs in bursts of
   `poolSize` rather than being pinned to the reader's rate. Latency is bounded
   by the ring, there is no unbounded queue, and the reader always gets the
   newest frame. That is the right trade for a compositor.

   What this does *not* give is zero-copy fan-out. A channel has one ring and
   one consumer that owns it, so two consumers of the same content means two
   channels and the producer submitting into each — one GPU write per consumer,
   not one write shared by all. Fine for vkrender (the virtual camera is
   normally the only consumer of the composite) and honest for a v1, but a real
   broadcast case would want one ring with per-consumer release tracking, which
   changes the fence bookkeeping. **Not** attempted in the spike; it is the
   thing most likely to force a wire-format change later, so it should be
   settled before 1.0 rather than after.
2. **Lifetimes as contract.** "owned by the library; valid until `vkrDisconnect`"
   is currently a comment. Published, it becomes something we cannot quietly
   change, so it wants to be specified and tested before 1.0, not after.
3. **Name and licence.** `vkrender_client` reads as "client of vkrender". MIT or
   Apache-2.0, kept well away from the GPLv3 receiver.
