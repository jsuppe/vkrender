# Migrating the receivers onto framelink

**Windows is done.** Every Windows receiver is a framelink producer, the
legacy transport is deleted, and the latency did not move. What follows is the
design, the measurements, and what is left (Linux, Android).

## Where the line is now

| direction | transport | components |
|---|---|---|
| in -> scene | **framelink** | `vkr_miracast`, `vkr_webrtc`, `vkr_rtsp`, `vkr_client_demo`, `vkr_airplay` |
| scene -> out | **framelink** | `vkrenderer` (producer), `framecam` (consumer) |

One transport. `vkrender_client` and the M24 consumer protocol are gone, along
with the service's own D3D11 device - framelink allocates the rings now, and
`vkr_consumer_demo`, which had been dead since M35, is deleted rather than
revived.

## The two obstacles this had to solve

Porting was *not* a rename. Two things the old protocol did have no equivalent
in framelink, both consequences of consumer-owned rings.

**1. The producer picks the geometry.** A receiver used to send
`ClientHelloMsg{width, height, format, poolSize}` and the renderer allocates a
ring to match. That is exactly backwards from framelink, where the consumer
chooses and `flQuery` reports. And receivers genuinely differ: a mirroring iPad
sends 752x1080, a MacBook 1920x1080, an RTSP camera whatever it is. The renderer
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

`FL_BUSY` mattered more than it looked: `flOpenProducer` could not distinguish
"no such channel" from "taken", and slot-walking needs exactly that
distinction. Both landed; `flOpenProducerSlot` in framelink now does the walk
itself.

## Phases

**Phase 0 — framelink API. DONE.** `flRequestGeometry` and `FL_BUSY` on both
backends. Five more additions turned up while wiring the real consumer, each
because something specific broke or would have:

| addition | why it exists |
|---|---|
| `FL_GPU_SYNC` + `flSharedConsumedFence` | a Vulkan consumer must not block the CPU on the ready fence, nor release a frame its GPU is still reading |
| `flChannelInfo::generation` | a reallocation replaces every buffer; without this an importer has no idea its handles died |
| `resetSync` on producer attach | a second producer's counter restarts at 1 against a fence sitting at 50 — every wait passes instantly |
| `flOpenProducerSlot` | the walk itself, once, in the library rather than copied into five receivers |
| `flCreateChannelOnAdapter` | a consumer must allocate on the GPU it draws with; "adapter 0" is not that GPU on a hybrid laptop |

Three of those are bug fixes to code that had already passed its tests. The
round trip now covers a detach/reattach (`roundtrip_reattach`) because that is
the case a slot channel sees every time a presenter leaves.

**Phase 1 — Windows receivers. DONE**, in the planned order (cheapest failure
to diagnose first): `vkr_client_demo`, `vkr_rtsp`, `vkr_webrtc`,
`vkr_miracast`, then `vkr_airplay` in its own repository.

`vkr_miracast`'s frame path is the one thing **not** verified: `Start()` hangs
on this machine (docs/MIRACAST.md), which predates the migration. Its slot
attach works; the first real sender is the test.

**Phase 2 — retire the legacy path. DONE.** `client/vkrender_client.{h,cpp}`,
the M24 consumer protocol and `vkr_consumer_demo` (dead since M35) are deleted,
along with the client-ring machinery in `win_service.cpp` — and with it the
service's own D3D11 device, which existed only to allocate those rings.

**Phase 3 — Linux. DONE.** The compositor (`vkrenderer --wayland vkr-0`)
pre-creates `vkr-0.in.0 .. .in.11` beside the Wayland socket, and both Linux
receivers write into them through one shared sink (`client/fl_sink_posix.h`) -
the Wayland socket stays as the front door for arbitrary Wayland clients.
Measured on melchior: two RTSP cameras (1280x720 + 480x640) and three WHIP
presenters simultaneously, five slots, 740 adoptions/5s, zero drops anywhere.
Hop avg ~8.3 ms / max ~16.3 ms - dominated by the headless loop's 16 ms sleep
(adoption granularity), not the transport.

Getting there surfaced two Linux-backend defects that Windows' fences had
masked, both now fixed and both now tested:

- **FL_BUSY did not exist in practice.** The listener closed after one accept,
  so "taken" read as "no such channel". One listener per channel now, refusing
  extras with an explicit Busy message.
- **Per-slot release leaks under latest-wins.** A skipped frame's slot was
  released by nobody; a decoder's startup burst kills the ring in under a
  second (observed: one camera at 100% ring-drop while its sibling ran clean).
  Recycling is monotonic now - the Windows consumed-fence rule, as a message.
  And the consumer must release `value-1` on EVERY adoption including the
  first; releasing only a previously-held frame deadlocks on the same burst.

FL_VERSION is 3: the release message's meaning changed.

**Phase 4 — Android.** Keep the AIDL `Surface` API for apps: replacing it with
a C API would be a regression for the ordinary Android integrator. framelink
becomes the native-code path alongside it, sharing the same ring.

## Latency is a constraint, not an outcome

The migration must not add a frame to the on-screen path, and it did not. That
was the expectation by construction: a receiver writes into a shared texture the
renderer samples, and framelink is the same mechanism behind a different
handshake - shared NT handles or dma-buf, plus a fence. Same number of hops,
same absence of copies. Expecting it and measuring it are different things,
which is why the instrument came first.

The rule that follows: **ingest must never become a process frames flow
through.** It is a control plane sitting beside the frame path, not in it.

Verification is not optional here, so the instrument came first. The service now
measures **producer-submit -> renderer-adopt** directly: producers stamp
`steady_clock` ns into `ptsNs` and the service subtracts its own clock (MSVC's
`steady_clock` is QPC-based and shares an epoch across processes, so the
subtraction is meaningful). Values outside a sane window are rejected rather
than averaged in, so a producer stamping real presentation time is simply not
counted.

### Baseline: 12 producers, legacy transport, CASPAR

Measured before any porting, four consecutive 5 s windows:

```
service: 1773 adoptions in 5s (pool 48 slots, 12 clients) hop avg 4.22 ms max 8.43 ms
service: 1773 adoptions in 5s (pool 48 slots, 12 clients) hop avg 4.22 ms max 8.49 ms
service: 1776 adoptions in 5s (pool 48 slots, 12 clients) hop avg 4.18 ms max 8.49 ms
service: 1774 adoptions in 5s (pool 48 slots, 12 clients) hop avg 4.19 ms max 8.59 ms
```

**avg 4.2 ms, max 8.6 ms, ~355 adoptions/s across 12 producers.** Reproduce with
12 `vkr_client_demo` instances against `vkrenderer --serve`.

The shape is what one would expect rather than a surprise, which is mild
evidence the measurement is real: ~4.2 ms is about a quarter of a 60 Hz frame
and the max sits near half a frame, consistent with a renderer sampling
submissions once per frame.

### The result: same load, same latency

Twelve `vkr_client_demo` instances against `vkrenderer --serve`, on CASPAR,
after the port and again after the legacy path was deleted:

```
legacy      1773-1776 adoptions/5s   hop avg 4.18-4.22 ms   max 8.43-8.59 ms
framelink   1772-1776 adoptions/5s   hop avg 4.18-4.29 ms   max 8.40-8.53 ms
```

**No regression.** The max is marginally lower and the imported surface count
HALVED - 24 pool slots against 48 - because holding one frame and releasing the
previous only ever touches two slots of each ring, where the old protocol
imported all four up front.

Also verified, because a latency number alone would not have caught them:

- **Producer-requested geometry.** A 752x1080 producer (an iPad's mirroring
  size) and a 1920x1080 one took different slots at their own sizes, and a
  third reused slot 0 at 1280x800 after the first left - re-imported at the new
  size, which is the generation check doing its job.
- **Real ingest.** Two RTSP cameras at 480x640 and 1280x720 landed on separate
  slots at their own resolutions, both textured live in the headless dump.
  Portrait matters specifically: it was the M33 stretch bug.
- **Fan-in and churn.** Ten WHIP presenters on ten slots at ten resolutions; a
  second wave took in.5..in.9 while the first still held in.0..in.4, and the
  pool shrank back as the first wave's sessions closed. (Twelve were requested;
  two failed on the SENDER side in whipclientsink's signaller before the
  receiver saw them.)

## Risks, as they stand now

**The 12-presenter fan-in stayed proven.** Re-measured, not assumed.

**Twelve rings are pre-allocated.** They start at a 640x360 placeholder and the
first producer replaces it with `flRequestGeometry`, so the idle cost is small
and the initial size barely matters.

**Sync on Linux is weaker, and that is now a measured fact rather than a
worry.** The backend has no shared fence; a frame is trusted to be finished
when `flSubmit` said so. That is correct for every current receiver (they are
CPU writers through `flImageMap`), and a GPU producer must flush first. The
explicit-sync piece (sync_file fds) remains future work.

## What this does not change

The renderer stays the thing that composites. Receivers become ordinary
framelink producers, and the scene becomes a framelink consumer of twelve
channels — at which point vkrender is, structurally, an application built on
the two libraries rather than a codebase that happens to contain them.
