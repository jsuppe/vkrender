# Decomposing vkrender into components

A proposal: vkrender stops being one program and becomes four, joined by
framelink. Written to be argued with before anything moves, and before the
repository is renamed and published.

## The four components

```
  Miracast ─┐
  AirPlay  ─┤   ┌──────────┐  N channels  ┌────────────┐  1 channel  ┌──────────┐
  WHIP     ─┼──►│  INGEST  │─────────────►│ COMPOSITOR │────────────►│  CAMERA  │
  RTSP     ─┘   │ + admit  │              │  → texture │             └──────────┘
                │ + PIN    │              └────────────┘             ┌──────────┐
                └──────────┘                                    └───►│ DISPLAY  │
                                                                     └──────────┘
```

| component | owns | state today |
|---|---|---|
| **ingest** | every way of receiving a stream, plus who is allowed to and on what terms | exists as four separate exes, each with its own auth |
| **compositor** | which streams are on screen, arrangement, animation → one texture | `scene_host`, **fused with display** |
| **camera sink** | texture → an OS camera | **done** — `framecam` |
| **display sink** | texture → a window, surface, or set of surfaces | inside the renderer, fused with the compositor |

Each boundary is a framelink channel, so each component is independently
useful: a compositor anybody can feed, a camera anybody can drive, an ingest
that can front something other than this compositor.

`framecam` already proves the pattern — it is the camera sink, and it does not
know vkrender exists.

## Three things that are not obvious

### 1. The compositor and display sink are genuinely entangled

`scene_host` computes a virtual canvas, per-window off-centre sub-frustums, and
bezel insets. That is display knowledge living inside the compositor, and the
split has to decide who owns it.

**Recommended: the compositor emits ONE canvas texture; the display sink slices
it.** The display sink knows its windows, their positions and their bezels, and
reports the resulting canvas aspect back over a control channel; the compositor
renders one texture at that aspect. Slicing a flat canvas is equivalent to
per-window frustums *for flat tiles*, which is what the product actually shows.

The cost is real and worth stating: per-window sub-frustums are strictly better
for 3D content (the cube scenes), because each window gets a correct off-centre
projection rather than a crop of one image. Choosing one canvas trades the cube
demos' correctness for a clean component boundary. Given presentations are the
product and the cubes are a demo, that is the right trade — but it is a trade,
not a free win.

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

### 3. Every boundary costs a frame of latency

Today a receiver's frame reaches the screen in one hop. Under this split it is
ingest → compositor → display: two hops, each adding up to a frame of ring
latency. At 60 Hz that is ~33 ms of added end-to-end delay in the worst case.

For presentation content that is invisible. For anything interactive it would
not be. Worth measuring once the compositor split exists rather than assuming
either way.

## Naming

`vkrender` describes an implementation detail (Vulkan) rather than what the
thing does, and it stops making sense once the parts are separable. A family
that reads consistently:

| component | proposed | notes |
|---|---|---|
| transport | **framelink** | published, MIT |
| camera sink | **framecam** | published, MIT |
| ingest | **framecast** | it receives casts - AirPlay, Miracast, WHIP, RTSP |
| compositor | **framestage** | arranges what is on the stage |
| display sink | **framewall** | windows, surfaces, and multi-display walls; the Android module is already called `wall` |
| the appliance | the product name | what the installer installs and the room runs |

The GPLv3 problem also resolves itself under this split: the vendored uxplay and
libplist trees belong to **framecast** alone. Everything else can then be MIT,
with one GPLv3 component clearly fenced - which is already how
`vkrender-airplay` is treated.

## Sequencing

Repository-per-component from day one would multiply overhead for four things
that change together. Suggested order, each step leaving a working system:

1. **Split display from compositor inside the current repo.** The hard part, and
   the one that proves or kills the one-canvas decision. No new repos.
2. **Extract the control plane** into the ingest component: one place that owns
   PINs, admission and source lists, with the receivers becoming dumb.
3. **Then** split repositories, once the boundaries have stopped moving, and
   rename as they go.

Doing (3) first would freeze boundaries that are still guesses.

## What this does not change

framelink and framecam stay exactly as they are; this proposal is about what
sits between them. The receiver migration
([MIGRATION.md](MIGRATION.md)) is a prerequisite for step 1, since the ingest
component only exists once receivers are framelink producers.
