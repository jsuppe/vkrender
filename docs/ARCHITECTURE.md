# Decomposing vkrender into components

A proposal: vkrender stops being one program and becomes three, joined by
framelink. Written to be argued with before anything moves, and before the
repository is renamed and published.

## The three components

```
  Miracast ─┐                              ┌──────────────────┐   surfaces
  AirPlay  ─┤   ┌──────────┐  N stream     │     RENDERER     │──► window 0
  WHIP     ─┼──►│  INGEST  │──channels────►│ per-surface draw │──► window 1
  RTSP     ─┘   │ + admit  │               │ bezels, frustums │──► window 2
                │ + PIN    │               └────────┬─────────┘
                └──────────┘                        │ published view
                                                    ▼
                                        ┌───────────────────────┐
                                        │ SINKS: camera (done), │
                                        │ recorder, encoder     │
                                        └───────────────────────┘
```

| component | owns | state today |
|---|---|---|
| **ingest** | every way of receiving a stream, plus who is allowed to and on what terms | exists as four separate exes, each with its own auth |
| **renderer** | the individual stream textures, its own surfaces, and what to draw on each | `scene_host` — already exactly this |
| **sinks** | a published view → an OS camera, a recorder, an encoder | **done** — `framecam` |

Each boundary is a framelink channel, so each component is independently
useful: a renderer anybody can feed, a camera anybody can drive, an ingest that
can front something other than this renderer.

Note the renderer's on-screen output does NOT go through a channel - it draws
straight to its own surfaces. The published view is an EXTRA output for sinks.

`framecam` already proves the pattern — it is a sink, and it does not know
vkrender exists.

## Three things that are not obvious

### 1. The renderer stays surface-aware. Do NOT split it.

An earlier draft of this document proposed separating a compositor (streams →
one canvas texture) from a display sink that slices that texture across windows.
**That was wrong**, and the same draft contained the reason: per-window
off-centre sub-frustums are strictly better than slicing, and slicing throws
that away for a boundary nobody needed.

The renderer takes the **individual stream textures** and decides what to draw
on **each surface** it owns. It therefore has to know its windows, their
positions on the desktop, and their bezels — which is exactly how `scene_host`
works today (`matFrustumSub(canvasAspect, u0, v0, u1, v1)` per window, with
bezel insets, M29).

That flexibility is the point, and a slicing layer cannot express any of it:

- **Per-surface projection.** Each window gets a correct off-centre frustum, not
  a crop. This is what makes 3D content correct across a wall.
- **Content passing behind bezels** rather than being squashed between them.
- **Per-display timing.** The multi-display roadmap's `presentOffsetNs` skew
  compensation targets a specific vsync *per swapchain*. A sliced canvas has one
  present; there is nothing to offset.
- **Different content per surface.** Nothing forces every window to show the
  same scene. A slicing layer forces exactly that.

So the boundary is **ingest → renderer**, not compositor → display. The renderer
consumes N stream channels and owns everything downstream of them.

**Sinks are additional outputs, not the display path.** The renderer also
publishes a view as a framelink channel for the camera, recorder or encoder to
consume - which is precisely what it does today (M24's negotiated view, moved
onto framelink in M35). The on-screen path never goes through that channel, so
it costs no extra hop.

### 2. The control plane is the new component, and framelink has none

framelink deliberately carries frames and nothing else. But "which sources are
connected, which are admitted, what the PIN is, which keys and certificates
apply, which streams should be on screen" is exactly what the ingest component
is *for*, and none of it fits in a frame channel.

Today this is scattered: `VKR_PIN` env vars read independently by
`miracast_receiver`, `whip_room` and `pin_overlay`, with the appliance passing
them down. Centralising it is the single biggest improvement in this proposal —
and the single largest piece of new design, because it needs an actual API
(admit/deny a sender, set or rotate a PIN, list active sources, choose the
layout) and a transport that is not framelink.

That transport should be boring and separate: a local HTTP or IPC control
socket, versioned, with the frame path untouched. `rtsp_room` already has an
HTTP control API for adding and removing sources - that is the shape to
generalise.

### 3. Latency: one hop on screen, and that is the point

Keeping the renderer whole means the on-screen path stays **one hop**:
ingest → renderer → its own swapchains, exactly as today. Only sink outputs
(camera, recorder) pay a second hop, and they are the paths that can afford it.

The two-hop cost was an argument against the compositor/display split, and it
disappears along with it.

## Naming

`vkrender` describes an implementation detail (Vulkan) rather than what the
thing does, and it stops making sense once the parts are separable. A family
that reads consistently:

| component | proposed | notes |
|---|---|---|
| transport | **framelink** | published, MIT |
| camera sink | **framecam** | published, MIT |
| ingest | **framecast** | it receives casts - AirPlay, Miracast, WHIP, RTSP |
| renderer | **framestage** | consumes stream textures, owns its surfaces, draws per surface |
| the appliance | the product name | what the installer installs and the room runs |

The GPLv3 problem also resolves itself under this split: the vendored uxplay and
libplist trees belong to **framecast** alone. Everything else can then be MIT,
with one GPLv3 component clearly fenced - which is already how
`vkrender-airplay` is treated.

## Sequencing

Repository-per-component from day one would multiply overhead for parts that
still change together. Suggested order, each step leaving a working system:

1. **Finish the receiver migration** ([MIGRATION.md](MIGRATION.md)) so receivers
   are framelink producers. Nothing else can start until they are.
2. **Extract the control plane** into the ingest component: one place that owns
   PINs, admission and source lists, with the receivers becoming dumb. This is
   the only genuinely new component, and the largest piece of new design.
3. **Then** split repositories, once the boundaries have stopped moving, and
   rename as they go.

The renderer needs no structural change at all - it becomes a framelink consumer
of N stream channels instead of the owner of N client rings, and keeps every
surface-aware behaviour it has.

Doing (3) first would freeze boundaries that are still guesses.

## What this does not change

framelink and framecam stay exactly as they are, and so does the renderer's
surface handling - multi-window spanning, bezel compensation and the per-display
skew work in [MULTIDISPLAY.md](MULTIDISPLAY.md) all remain renderer concerns and
all survive intact. This proposal is only about what feeds it and what it feeds.
