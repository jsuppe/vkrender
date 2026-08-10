# framelink — zero-copy GPU frames between Windows processes

Draft. `framelink` is a working name; the prefix `fl` below changes with it.

A proposal to lift the shared-texture transport out of vkrender into a
standalone library an integrator can build against, so that "receive a rendered
image and do something with it" is a dependency rather than a port.

Nothing here is implemented yet. It exists to make the shape reviewable.

## What it is, and what it deliberately is not

**Is:** a frame channel between Windows processes. One process publishes GPU
frames; others subscribe and read the *same GPU memory*. No copies, no
encode/decode, no network.

**Is not:** a compositor, a scene graph, a layout engine, a codec, or a
cross-machine transport.

That line is the whole design. Today `vkrConnectConsumer` asks the renderer to
*render a 1280x720@30 view for you* — that is a vkrender feature wearing a
transport's clothes, and publishing it would hand every integrator a compositor
they did not ask for. Under this split vkrender becomes an ordinary user of the
library: a process that happens to subscribe to several channels and publish a
composited one. `vkr_vcam` then genuinely is "a consumer of framelink" rather
than "a consumer of vkrender", which is the property we actually want.

## Topology

Today, everything connects *to the renderer*, which owns the only pipe:

```
  miracast ─┐
  airplay  ─┼─► vkrenderer (service, owns pipe "vkrender") ─► vcam consumer view
  whip     ─┤
  rtsp     ─┘
```

Under framelink the ownership inverts: a **producer owns its channel** and
consumers subscribe. vkrender subscribes to whatever is publishing and
republishes a composite:

```
  miracast ─► channel "miracast"  ─┐
  airplay  ─► channel "airplay"   ─┼─► vkrenderer ─► channel "vkrender.scene" ─► vcam
  whip     ─► channel "whip"      ─┤                                          └─► OBS, recorder, ...
  rtsp     ─► channel "rtsp"      ─┘
```

The inversion is what makes the library general — "publish an image" is the
natural unit — but it costs us discovery. Today the renderer's pipe name is
well-known and producers dial in. Under the new model the renderer must find
channels, hence `flEnumerate` below. This is the single biggest behavioural
change and the part most likely to need revising once real.

## Draft API

C ABI, opaque handles, `extern "C"` — so C#, Rust, Python and Go bind without
C++ ABI pain. This keeps the existing shape (it is already this) and adds the
four things a public library needs: versioning, a raw-handle escape hatch,
an access model, and explicit contracts.

```c
#pragma once
#include <stdint.h>
#ifdef _WIN32
#include <d3d11.h>

#ifdef FL_EXPORTS
#define FL_API __declspec(dllexport)
#else
#define FL_API __declspec(dllimport)
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

typedef struct flBuffer {
    uint32_t slot;
    ID3D11Texture2D* texture; /* borrowed; valid until flClose (see Lifetimes) */
} flBuffer;

/* `device` may be NULL to let framelink create one on the default adapter.
 * Fails with FL_ACCESS_DENIED if `name` exists and the caller may not own it. */
FL_API flResult flOpenProducer(const char* name, ID3D11Device* device, uint32_t width,
                               uint32_t height, flFormat format, uint32_t poolSize,
                               flChannel** out);

FL_API flResult flAcquireBuffer(flChannel*, flBuffer* out, uint32_t timeoutMs);
FL_API flResult flSubmit(flChannel*, const flBuffer*, int64_t ptsNs); /* -1 = no pts */

/* Republish at a new size/format. Consumers see FL_DISCONNECTED then resubscribe;
 * this is the StreamReset path, made explicit rather than implicit. */
FL_API flResult flReconfigure(flChannel*, uint32_t width, uint32_t height, flFormat);

/* ---- consumer ---------------------------------------------------------- */

typedef struct flFrame {
    uint32_t slot;
    ID3D11Texture2D* texture; /* borrowed; valid until flRelease */
    int64_t  ptsNs;
    uint64_t sequence;        /* monotonic; gaps mean frames were skipped */
} flFrame;

FL_API flResult flOpenConsumer(const char* name, ID3D11Device* device, flChannel** out);
FL_API flResult flAcquireFrame(flChannel*, flFrame* out, uint32_t timeoutMs);
FL_API flResult flRelease(flChannel*, const flFrame*);

/* ---- both -------------------------------------------------------------- */

FL_API flResult flQuery(flChannel*, flChannelInfo* out);
FL_API void     flClose(flChannel*);

/* Discovery. Needed because producers now own channels (see Topology).
 * Returns the count; pass names=NULL to size the buffer first. */
FL_API flResult flEnumerate(const char** names, uint32_t cap, uint32_t* count);

/* ---- interop escape hatch ---------------------------------------------- */
/* The whole point of not being D3D11-only. The handle is a shared NT handle
 * the caller may import into D3D12, Vulkan (VK_KHR_external_memory_win32),
 * CUDA or GL. Caller must NOT close it; it dies with the channel.
 * The fence is a shared ID3D11Fence/ID3D12Fence usable as a Vulkan timeline
 * semaphore (VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT). Wait for
 * `waitValue` before reading the texture. */
FL_API flResult flFrameInterop(flChannel*, const flFrame*, void** sharedHandle,
                               void** fenceHandle, uint64_t* waitValue);

#ifdef __cplusplus
}
#endif
#endif /* _WIN32 */
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

## Migration sketch

The library is essentially today's `client/vkrender_client.cpp` (361 lines) plus
`common/protocol.h` (166) and the Win32 IPC. It links only `d3d11` and `dxgi`.
No Vulkan, no GStreamer, and nothing GPL-adjacent — the UxPlay code lives in the
separate `vkrender-airplay` repo and stays there.

| today | after |
|---|---|
| `vkrConnect(pipe, dev, w, h)` | `flOpenProducer(name, dev, w, h, fmt, pool, &ch)` |
| `vkrDequeueBuffer` / `vkrQueueBuffer` | `flAcquireBuffer` / `flSubmit` |
| `vkrConnectConsumer` (negotiates a *scene view*) | **stays in vkrender** — it is a compositor feature |
| `vkrAcquireFrame` / `vkrReleaseFrame` | `flAcquireFrame` / `flRelease` |
| `StreamReset` message | `flReconfigure` |
| implicit "the renderer owns the pipe" | `flEnumerate` + producer-owned channels |

`vkr_vcam` becomes the reference consumer nearly unchanged: replace
`vkrConnectConsumer` with `flOpenConsumer("vkrender.scene", ...)` and the
acquire/release pair with the `fl` equivalents. It is a real Media Foundation
virtual camera in a couple of hundred lines, which makes it a far more
persuasive sample than a toy.

vkrender's renderer keeps its consumer-view negotiation, because that is
genuinely about compositing: it is the thing deciding what a 1280x720@30 view of
a four-tile scene means.

## Open questions

1. **Does the producer or a broker own the channel?** Producer-owned is simpler
   and matches "publish an image", but discovery gets weaker and a consumer that
   starts first has nothing to attach to. A tiny broker fixes both and adds a
   process to deploy.
2. **What happens to a slow consumer?** Today the fence pair gives
   back-pressure to a single peer. With N consumers, one slow reader must not
   stall the producer — almost certainly latest-frame-wins per consumer, but the
   fence bookkeeping needs revisiting for the fan-out case.
3. **Lifetimes as contract.** "owned by the library; valid until `vkrDisconnect`"
   is currently a comment. Published, it becomes something we cannot quietly
   change, so it wants to be specified and tested before 1.0, not after.
4. **Name and licence.** `vkrender_client` reads as "client of vkrender". MIT or
   Apache-2.0, kept well away from the GPLv3 receiver.
