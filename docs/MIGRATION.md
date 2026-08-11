# Migrating the receivers onto framelink

vkrender currently runs **two transports**. This is the plan to finish the job,
and the design problem that has to be solved first.

## Where the line is today

| direction | transport | components |
|---|---|---|
| scene → out | **framelink** | `vkrenderer` (producer), `framecam` (consumer) |
| in → scene | legacy `vkrender_client` | `vkr_miracast`, `vkr_webrtc`, `vkr_rtsp`, `vkr_client_demo` |

Only the consumer path moved (M35). Every receiver still calls
`vkrConnect`/`vkrDequeueBuffer`/`vkrQueueBuffer` over `vkr::proto` to
`win_service.cpp`. Both transports work — they run simultaneously today — but
the duplication is real and the legacy protocol is the one nobody else can use.

## The two obstacles

Porting is *not* a rename. Two things the old protocol does have no equivalent
in framelink, both consequences of consumer-owned rings.

**1. The producer picks the geometry.** Today a receiver sends
`ClientHelloMsg{width, height, format, poolSize}` and the renderer allocates a
ring to match. That is exactly backwards from framelink, where the consumer
chooses and `flQuery` reports. And receivers genuinely differ: a mirroring iPad
sent 752x1080, a MacBook 1920x1080, an RTSP camera whatever it is. The renderer
cannot know before the producer arrives.

**2. There is no "a producer wants in" signal.** A producer cannot create a
channel — deliberately — so something must create one *before* the producer
appears. framelink has no discovery and no broker, and the renderer supports up
to **12** simultaneous producers.

## The design: slot channels + requested geometry

Both obstacles have the same answer, and Android supplies the precedent for
each — which matters, because Android is the platform that constrains this
design and any solution that does not fit it is wrong.

**Slot channels.** The renderer pre-creates K input channels at startup:

```
vkrender.in.0 … vkrender.in.11
```

A receiver attaches to the first one that accepts it, walking the list on
refusal. No discovery, no broker, no producer-created channels. This is not a
workaround: vkrender's own Android service is **already** slot-based —
`Surface acquireInputSurfaceSlot(int slot)` — so the model is one the codebase
has shipped and validated on the most constrained platform.

**Producer-requested geometry.** A producer asks; the consumer decides whether
to honour it by reallocating that slot's ring. Again there is Android
precedent: a BufferQueue's buffers belong to the consumer, yet the producer may
call `ANativeWindow_setBuffersGeometry`. Consumer-owned does not mean the
producer is mute — it means the producer does not *allocate*.

### framelink additions this needs

Two, both small, both fitting the existing model:

```c
// A producer may state what it intends to send. The consumer MAY reallocate
// the ring to match and MAY refuse; either way flQuery afterwards is the
// truth. Mirrors ANativeWindow_setBuffersGeometry.
FL_API flResult flRequestGeometry(flChannel*, uint32_t width, uint32_t height,
                                  flFormat format);

// FL_BUSY: the channel exists but already has a producer. Distinct from
// FL_NOT_FOUND, so a receiver can walk the slots instead of giving up.
```

`FL_BUSY` matters more than it looks: today `flOpenProducer` cannot distinguish
"no such channel" from "taken", and slot-walking needs exactly that
distinction.

## Phases

**Phase 0 — framelink API.** Add `flRequestGeometry` and `FL_BUSY` on both
backends, with round-trip tests covering reallocation mid-life and a second
producer being refused. No vkrender changes. This phase must land first,
because everything after it depends on the shape.

**Phase 1 — Windows receivers, one at a time.** Order chosen by how cheap the
failure is to diagnose:

1. `vkr_client_demo` — synthetic, no hardware, fastest signal.
2. `vkr_rtsp` — a local RTSP source is reproducible on demand.
3. `vkr_webrtc` — needs a sender, but the synthetic WHIP harness exists.
4. `vkr_miracast` — needs a real Windows sender; least reproducible.

`vkr_airplay` lives in its own repo and follows separately, after the four
above have proven the pattern.

**Phase 2 — retire the legacy path.** Delete `vkrender_client`, the M24
consumer protocol, and the client-ring machinery in `win_service.cpp`. Note
`vkr_consumer_demo` is **already dead** — it speaks the consumer protocol the
renderer stopped answering in M35; it builds but cannot connect. It gets
deleted or reborn as a framelink consumer.

**Phase 3 — Linux.** Receivers there do not use `vkrender_client` at all
(sharing happens inside the renderer), so this is a net *simplification*: they
gain a real client library instead of bespoke internal plumbing. Needs
framelink's Linux backend, which now exists.

**Phase 4 — Android.** Keep the AIDL `Surface` API for apps: replacing it with
a C API would be a regression for the ordinary Android integrator. framelink
becomes the native-code path alongside it, sharing the same ring.

## Latency is a constraint, not an outcome

The migration must not add a frame to the on-screen path. It should not, by
construction: today a receiver writes into a shared texture the renderer
samples, and framelink is the same mechanism behind a different handshake -
shared NT handles or dma-buf, plus a fence. Same number of hops, same absence of
copies.

The rule that follows: **ingest must never become a process frames flow
through.** It is a control plane sitting beside the frame path, not in it.

Verification is not optional here. `recv_stats` already reports per-session
`behind_ms` ingress/sink averages and `stage_ms` (M23), so the check is a
before/after comparison on the same machine under the 12-presenter load. A
regression there fails the migration regardless of how much cleaner the code is.

## Risks

**The 12-presenter fan-in is a proven load and must stay proven.** M22
validated 12 simultaneous WHIP presenters; the migration is not done until that
number is re-measured, not assumed. Twelve slot channels also means twelve
rings pre-allocated at a default size — memory that today is allocated on
demand. Sizing the default matters, and `flRequestGeometry` reallocating on
first connect makes the initial size mostly a placeholder.

**Sync on Linux is weaker.** framelink's Linux backend has no shared fence, so
a GPU producer must flush before `flSubmit`. The current Linux receivers hand
over VAAPI/dma-buf surfaces with their own sync assumptions; that needs
checking before Phase 3, not during.

**This trades a working thing for a cleaner thing.** The receivers work today.
The gain is one transport instead of two, and receivers that any framelink
consumer can host — not just vkrender. That is worth doing, but it is not
urgent, and the honest sequencing is to finish the Linux camera first: it
proves framelink's second platform end to end, which this migration then relies
on.

## What this does not change

The renderer stays the thing that composites. Receivers become ordinary
framelink producers, and the scene becomes a framelink consumer of twelve
channels — at which point vkrender is, structurally, an application built on
the two libraries rather than a codebase that happens to contain them.
