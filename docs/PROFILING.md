# Receive-pipeline profiling: latency and dropped frames (M23)

Every cast receiver is a pipeline of queues: network → jitterbuffer →
depay/decode → convert/upload → producer handoff → renderer adoption →
present. This document defines how each stage is measured, what the measured
budgets are on all three platforms, and — because "dropped frames" conflates
several very different phenomena — a taxonomy of where frames can disappear
and which of those losses are actually unhealthy.

## Measurement model

One model on every platform, because the receive pipelines are all GStreamer
(`client/recv_stats.h`):

```
behind-live(probe) = pipeline_clock_now − (base_time + buffer_PTS)
```

For a live source the buffer PTS is the stream's running time, so
behind-live at a probe point is how far that frame is behind real time at
that stage — **no sender clock sync required**. Probes sit at:

- **ingress** — the source element's src pad (for WebRTC, this is
  post-jitterbuffer, so it includes jitterbuffer residence). Counts RTP
  **packets** (content-dependent: a noisy 640×360 frame is 20+ packets, a
  flat one 1–2).
- **sink** — the terminal sink's sink pad: decoded (+converted on
  Windows/Linux) **frames**. `stage_ms` = sink avg − ingress avg = the
  depay + decode (+ convert) residence.

Each stream logs one line per 5 s window:

```
recv-stats: [session N] 5.0s pkts=… frames=… (fps) behind_ms ingress=avg/max
            sink=avg/max stage_ms=… pulled=… qdrop=… ringdrop=… gap=…
```

Renderer-side consumption is read from the existing per-5s logs:
`service: N adoptions (pool, clients)` (Windows), `compositor: N adoptions
(pool, streams)` (Linux), `renderer: N adoptions / M presents` (Android).

For a per-element breakdown beyond the two probes, run any receiver with
GStreamer's built-in latency tracer — no code changes:

```bash
GST_TRACERS="latency(flags=pipeline+element)" GST_DEBUG="GST_TRACER:7" vkr_webrtc …
```

## Drop taxonomy

| Counter | Where | Meaning | Healthy? |
|---|---|---|---|
| `qdrop` | appsink (`drop=true, max-buffers=4`) | consumer slower than producer; frames overwritten in the appsink queue | **No** under steady state |
| `ringdrop` | producer handoff (`vkrDequeueBuffer` 5 ms timeout on Windows; codec input stall on Android) | the renderer side isn't consuming — ring/codec starved | **No** |
| `gap` | ingress PTS discontinuity > 1.5× observed cadence | jitterbuffer gave up on packets (network loss) or the sender paused | Network-side |
| adoptions < frames·streams | renderer latest-wins | deliberate freshness coalescing when a producer outpaces vsync | **Yes, by design** |

The renderer's latest-wins coalescing is *not* loss: when a stream produces
faster than the renderer samples it, older frames are deliberately skipped
so the newest is shown. Compare `recv-stats` frame rates against the
renderer's adoption rate to attribute any difference to design vs defect.

## Measured budgets (2026-08-05)

Test signal: `videotestsrc` 640×360@30, H.264, loopback WHIP into the M22
room. Sink column includes decode+convert. All numbers avg/max ms behind
live at the probe.

### Windows (RTX 5090 box, `vkr_webrtc` → service ring → SceneHost)

12 concurrent presenters, `VKR_WEBRTC_LATENCY_MS=50`:

| Metric | Typical content | Max-entropy (snow) |
|---|---|---|
| ingress behind-live | 3.0–3.8 avg / ~8 max | 7.7 avg / 37 max |
| sink behind-live | 4.0–4.8 avg / ~9 max | 31.8 avg / 38 max |
| stage (depay+decode+convert) | ~1.0 | ~24 |
| fps per stream | exactly 30.0 | exactly 30.0 |
| qdrop / ringdrop / gap | 0 / 0 / 0 | 0 / 0 / 0 |
| renderer | **1800 adoptions/5s = 12×30 exactly**, pool 48 slots | |

4-presenter run at the default 200 ms jitterbuffer latency: same figures
(ingress ~3.4 avg) — see "Key findings".

### Linux (melchior, RTX 3090, `vkr_webrtc` → waylandsink → M13 compositor)

4 concurrent presenters:

| Metric | Typical content | Max-entropy (snow) |
|---|---|---|
| ingress behind-live | 0.2–0.6 avg / ≤4.4 max | 0.5 avg / 2.6 max |
| sink behind-live | 1.2–1.6 avg / ≤6.5 max | 1.7–1.8 avg / 3.7 max |
| stage | ~1.0 | ~1.2 |
| fps per stream | exactly 30.0 | exactly 30.0 |
| qdrop / ringdrop / gap | 0 / 0 / 0 | 0 / 0 / 0 |
| compositor | **600 adoptions/5s = 4×30 exactly**, pool 12 slots | |

### Android (TCL 8483A tablet, `whip_room` → MediaCodec → renderer Surface)

The sink probe on Android sits on parsed H.264 AUs **before** MediaCodec
(hardware decode latency is not in `stage_ms`; it is absorbed between
`recv-stats` and the renderer's adoption rate, both in logcat:
`adb logcat -s vkrender | grep -E 'recv-stats|adoptions'`).

3 concurrent presenters over `adb forward` (USB):

| Metric | Flat content (ball) | Packet-heavy (smpte/snow) |
|---|---|---|
| ingress behind-live | 5.2–5.8 avg / ≤13 max | 9.1–19.4 avg / ≤45 max |
| sink behind-live (pre-codec) | 5.4–6.0 avg / ≤13 max | 14.2–37.3 avg / ≤45 max |
| stage (depay+parse) | 0.2 | 5–18 |
| fps per stream | 30.0 | 30.0 |
| qdrop / ringdrop / gap | 0 / 0 / 0 | 0 / 0 / 0 |
| renderer | **450 adoptions/5s = 3×30 exactly**, 301 presents/5s = 60 Hz locked, pool 53 buffers | |

Higher ingress figures than desktop reflect the adb-forward USB transport
plus the tablet-class CPU doing depay on packet-heavy content; still under
1.5 frame periods worst-case, with the vendor hardware codec keeping the
renderer at full rate.

## Key findings

1. **webrtcbin's 200 ms default `latency` is a reordering deadline, not a
   constant delay.** Folklore says the jitterbuffer "adds 200 ms"; measured
   ingress behind-live on a clean link is **~3.4 ms** at the default
   setting, identical to a 50 ms setting. The property only matters under
   real network jitter/reordering, where it bounds how long the
   jitterbuffer waits before declaring loss (then visible as `gap`).
   Tune per deployment with `VKR_WEBRTC_LATENCY_MS`.
2. **Decode cost is content-dependent and platform-dependent.** Max-entropy
   video costs ~24 ms/frame on the Windows box's decoder path but ~1 ms on
   melchior — the only stage that varied by an order of magnitude between
   platforms. Both remain under one 30 fps frame period, so no drops
   result; at higher resolutions this is the stage to watch (`stage_ms`).
3. **No unhealthy drops at target load on any platform.** 12 concurrent
   presenters on Windows: every category zero, renderer adopting 12×30
   exactly. The queues (appsink 4-deep, ring 4-deep, jitterbuffer) exist to
   absorb transients; under steady state they stay empty.
4. **The end-to-end receiver-side budget on a LAN is ~5–35 ms** (ingress →
   uploaded), i.e., ~1 frame period, dominated by decode. Everything after
   the sink (ring upload, adoption, vsync wait) is bounded by the
   renderer's present cadence — worst case one vsync (16.7 ms at 60 Hz).

## Tuning surface

| Knob | Where | Default | Effect |
|---|---|---|---|
| `VKR_WEBRTC_LATENCY_MS` | whip_room (webrtcbin `latency`) | 200 | Jitterbuffer loss deadline. Lower = faster loss recovery declaration on bad networks, no effect on clean links. |
| appsink `max-buffers` | receivers | 4 (drop=true) | Depth before `qdrop`. Raising hides a slow consumer; prefer fixing the consumer. |
| `vkrDequeueBuffer` timeout | Windows receivers | 5 ms | How long a decode thread waits for a ring slot before `ringdrop`. |
| Ring depth | M14 service | 4/client | Producer-side headroom before starvation. |
| Renderer pacing | scene host / Android | vsync latest-wins | Coalescing policy; PTS pacing (`--pacing pts`) trades latency for smoothness. |

## Extending to the other receive paths

- **AirPlay (Windows/Linux/Android)** and the **RTSP decoders** are also
  GStreamer pipelines — `recv_stats.h` probes attach the same way
  (ingress = depay/rtspsrc pad, sink = terminal appsink). Not yet wired.
- **Miracast (Windows)** is WinRT (`MediaPlayer` frame server), not
  GStreamer: equivalent measurements need `QueryPerformanceCounter` stamps
  in `VideoFrameAvailable` vs the frame's presentation timestamp, plus the
  existing ring counters. Not yet wired.
- The renderer side already logs adoption/present rates on every platform;
  Android additionally has `VK_GOOGLE_display_timing` available for
  present-latency measurement if deeper analysis is needed.
