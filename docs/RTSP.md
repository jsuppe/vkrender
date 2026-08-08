# RTSP sources (M27)

Pull streams from RTSP servers — IP cameras, hardware encoders, mediamtx,
anything that speaks RTSP — into the scene as ordinary presentation streams,
alongside AirPlay / Miracast / WHIP presenters.

## Why this is a new receiver and not `vkdecoder`

The repo has done RTSP since M1: `vkdecoder` pulls RTSP, decodes on the GPU
and hands frames to the renderer. It **cannot** participate in the scene,
because it predates the multi-producer architecture and has the IPC inverted:

| | `vkdecoder` (M1–M5) | `vkr_rtsp` (M27) |
|---|---|---|
| IPC direction | *decoder* listens, renderer dials in | connects **to** the renderer service |
| Streams | one, fills the window | N, one presentation slot each |
| Reconnect | RTSP only | RTSP **and** renderer |
| Sources | one URL, fixed at launch | added/removed at runtime |

`vkdecoder` is still the right tool for the original single-stream zero-copy
pipeline (it keeps decoded frames in GPU memory end to end). `vkr_rtsp` is the
appliance path.

## The one thing that makes RTSP different

Every other receiver is **push**: a sender discovers us and initiates. RTSP is
**pull** — we dial out and keep the connection up. Consequences:

- **No PIN.** We are the client; the *server* authenticates *us*. Credentials
  ride in the URL: `rtsp://user:pass@host/path`.
- **No mDNS.** There is nothing to advertise.
- **Sources are configured, not discovered**, so there is a control API.

## Running it

```bash
vkr_rtsp --pipe vkrender --port 8890 --url rtsp://cam1.local/stream
```

Or via the appliance — `vkrender.ini`:

```ini
rtsp=1
rtsp_port=8890
rtsp_urls=rtsp://cam1.local/stream;rtsp://user:pw@cam2.local/h264
```

The component installs by default but `rtsp=0` ships as the default, because
an RTSP receiver with no URLs has nothing to do.

## Control API

```bash
curl http://localhost:8890/sources                              # list
curl -d rtsp://cam1.local/stream http://localhost:8890/sources  # add -> 201 {"id":1}
curl -X DELETE http://localhost:8890/sources/1                  # remove -> 204
```

`POST` accepts a bare URL or `{"url":"rtsp://..."}`. `GET` reports live state:

```json
[{"id":1,"url":"rtsp://cam1.local/stream","state":"live","failures":0,"lastError":""}]
```

States: `pending` → `connecting` → `live`, or `retrying` after a failure.

## Liveness: why frame arrival, not connection state

**A dead RTSP source usually does not report an error.** Measured against
mediamtx: kill the publisher and the server holds the reader session open, so
`rtspsrc` sees a perfectly healthy connection that simply stops delivering —
no `ERROR`, no `EOS`. Without a watchdog the source sits in `live` forever
behind a frozen tile. Real cameras do the same on a PoE flap or an encoder
crash.

So `rtsp_room` judges liveness by **frames arriving**, via a pad probe:

| Watchdog | Default | Env | Fires when |
|---|---|---|---|
| Stall | 8 s | `VKR_RTSP_STALL_MS` | `live` but no frame for this long |
| Connect | 15 s | `VKR_RTSP_CONNECT_MS` | `connecting` with no video pad yet |

Either one tears the source's pipeline down and reconnects with exponential
backoff (1 s → 2 s → … → 30 s cap, reset by a good connection). Verified:
killing a publisher produces `stalled (no frames) - retrying in 1000 ms`, then
backoff, then automatic recovery when the publisher returns.

## Tuning

| Env | Default | Notes |
|---|---|---|
| `VKR_RTSP_LATENCY_MS` | 200 | `rtspsrc` jitter buffer. Its own default is **2000 ms** — far too much for a sub-100 ms compositor. |
| `VKR_RTSP_PROTOCOLS` | auto | `tcp` forces TCP interleaved; useful where UDP is firewalled. |
| `VKR_RTSP_STALL_MS` | 8000 | Lower for faster failover, higher for sources with long GOPs or sparse keyframes. |
| `VKR_RTSP_CONNECT_MS` | 15000 | |

Measured on CASPAR against mediamtx (h264, 1280x720@30): **~44 ms behind live**,
stable across intervals, `qdrop=0`. See [PROFILING.md](PROFILING.md) for what
the `recv-stats:` fields mean.

> If you see high `ringdrop`, that is the *renderer* not consuming, not RTSP.
> It is the designed response to a slow consumer: drop the frame rather than
> queue latency. The tell that it is working is `behind_ms` staying flat
> instead of climbing.

## Android

`android/rtsp/` is a separate APK, mirroring `android/webrtc/`. It binds the
renderer over Binder and takes one slot Surface per camera, so each source
lands on its own cube face.

```bash
gradle -p android :rtsp:assembleDebug

# manage the source list without a UI (persisted in SharedPreferences)
adb shell am start -n dev.vkrender.rtsp/.MainActivity -e url rtsp://cam.local/stream
adb shell am start -n dev.vkrender.rtsp/.MainActivity -e clear true
# or at runtime, same control API as desktop
curl -d rtsp://cam.local/stream http://<device-ip>:8890/sources
```

**H.264 only**, like the Android WHIP receiver: the sink is MediaCodec
decoding straight into the renderer's slot Surface, which is the whole point
of the zero-copy path. A non-H.264 source is discarded and the connect
watchdog retries it.

The renderer and receiver APKs must be **signed with the same key** (the
Binder service sits behind a signature permission) — build both from one CI
run.

## Architecture

`client/rtsp_room.{h,cpp}` is the shared core across all three platforms —
same split as `whip_room`:

```
rtsp_room  (shared)          per-platform sink
  N sources, one             rtsp_win.cpp    -> vkrender_client ring
  GstPipeline each     ->    rtsp_linux.cpp  -> waylandsink (M13 compositor)
  + HTTP control API         android/rtsp/   -> MediaCodec -> renderer Surface
  + backoff/watchdogs
```

`onStream` receives **rtspsrc's encoded RTP pad** — deliberately the same
contract as `whip_room`'s `onTrack`, so a platform's sink code is nearly
identical between the two ingest types.

> That contract was not the first design. `rtsp_room` originally decoded
> centrally with `uridecodebin` and handed over a raw pad, which was simpler
> for Windows and Linux and **completely broke Android**: MediaCodec-into-
> Surface needs encoded H.264 access units, so a centrally-decoded pad would
> have thrown the zero-copy path away. Matching `whip_room` fixed it and was
> better anyway — `recv-stats` now separates RTP arrival from decoded output,
> exposing a ~35 ms decode stage the old shape hid entirely.

The one real difference from `whip_room` is **one pipeline per source rather
than one shared pipeline**. RTSP endpoints fail independently and often; a
private pipeline means one dead camera cannot error out the others, and
reconnect is just `NULL` → `PLAYING` on that pipeline alone.

Threading matches `whip_room`: the HTTP listener only ever queues intent, and
every pipeline mutation happens in `rtsp_room_poll()` on the main thread.

## Verification status

| Platform | Status |
|---|---|
| Windows | **Verified end to end** — two sources at different resolutions, runtime add/remove, stall detection, backoff, recovery |
| Android | **Builds** (arm64-v8a + x86_64, APK links clean) — not yet device-verified |
| Linux | **Written, not yet compiled** — no Linux host available when it was added |
