# M4 renderer integration checkpoint (not full M4 qualification)

Linux `--wayland` and Windows `--serve` accept one selected A/V session in
slot zero. All other slots retain their existing latest-frame/video-only policy.
Build against the FrameLink revision exposing `framelink_av_playback`.

Example (producer started separately):

```
vkrenderer --wayland m4 --av-session /private/session --av-plugin /path/libgstframelink.so --av-format BGRA
```

Options: `--av-audio-sink` (default autoaudiosink), `--av-width`, `--av-height`,
`--av-fps`, `--av-rate`, `--av-channels`, `--av-latency-ms` (default100),
`--av-exit-on-end` (bounded test lifecycle; otherwise renderer remains running).
Default offer:160x90,50fps,48kHz stereo. Use identical offer at producer.
For headless testing use `--headless -1 --av-audio-sink fakesink`.
This explicitly reports system-clock fallback, NOT a hardware audio master.

## Timeline and ownership

Receiver onSession creates the negotiated native consumer ring on render thread;
no Vulkan call occurs on the audio thread. Presentation remains disabled until
onClock supplies the audio pipeline clock, base and negotiated latency after
preroll. Deadline = base + runningPTS + latency. Arrival time is never substituted.
Only the negotiated session uses relative PTS; legacy absolute timestamps are
not reinterpreted.

At most six pending descriptors are queued against eight native ring images.
`flRelease` at enqueue is bookkeeping under GPU_SYNC/DEFER_RELEASE, not permission
to overwrite storage. The consumed watermark advances after previous GPU reads.
Early frames stay queued; >50ms late or invalid frames are not presented. Dropping
also removes the retained SceneHost image before retiring that watermark, so an
old scene animation cannot still sample recycled storage. Session changes clear
queue and imports with GPU idle at the boundary only. BGRA keeps existing native
GPU import; NV12 currently uses the renderer's existing CPU conversion fallback.

`GET /api/status` includes `av`: active, audio_master, muted, due_decisions,
hold_checks, late_drops, invalid_pts and schedule_error_ns. Due decisions are
scheduler decisions, not measured scanout or guaranteed successful imports.
`POST /api/audio?on=1` mutes selected audio; on=0 unmutes. This never changes clock.
Audio session selection is currently startup-only, one session. There is no mixer.
`POST /api/pause` rejects active A/V with409 rather than freezing only video while
audio continues. Coordinated pause/reset/reconnect remains outstanding.

## Evidence and limits

CPU scheduler tests cover same-epoch segment mapping, latency, early/late policy,
overflow and clock-controlled decisions (not clock correction).
Initial native Linux BGRA300-frame producer and renderer completed0/0,289due
frames at final HTTP poll,0late/invalid. Both audio/video used explicit systemclock
fallback with100ms latency. Native BGRA images imported as DMA-BUFs.
NV12 checkpoint completed0/0 but113due/52late drops due to slow CPUconversion;
not a throughput pass. Mute/unmute status exercised. Files in m4_native_20261030
are pre-staged-clock-fix checkpoints; updated qualification must label revision.

No physical speaker/display synchronization, audio-device clock drift correction,
Windows runtime, Android A/V deployment, multiple audio session switching or
coordinated pause/recovery is qualified by this checkpoint. Do not mark M4 complete.

Staged-clock native BGRA smoke subsequently passed with actual selected PulseAudio
clock backed by PipeWire `auto_null` (virtual, not physical). Producer300frames,
bothprocessesexit0; mute/unmute and pause409 checked; queue never exceeded6,
invalidPTS0. Evidence `audio-master-*`. Preserve `PULSE_SERVER` when isolating
Wayland's XDG_RUNTIME_DIR; otherwise autoaudiosink may fall back, accurately reported
as audio_master=false. Sequence gaps are now reported separately from late drops.
Strict reproducer: `test/av_renderer_smoke.py RENDERER PRODUCER PLUGIN --audio-sink autoaudiosink --audio-master`.

## Resume checkpoint: coordinated suspension

`POST /api/pause?on=1` now suspends the selected live A/V session. Video scheduling
holds immediately; the receiver stops audio and the render thread drains the native
ring. Resume reconnects with a fresh authenticated session/clock, rather than
replaying stale live data or shifting only video timestamps. The producer must be
available for another session (or be restarted). This is live suspension, not a
lossless seekable-media pause. Muting does not reset timing. Status includes
`av.paused` and `av.session_resets` (setup/end boundaries).

Linux validation after this change: scheduler/clock/control tests 4/4 passed;
300-frame BGRA smoke selected the virtual PulseAudio clock; the new
`test/av_renderer_pause.py` passed pause, native drain, producer restart, resume,
new clock/session, unmute and normal completion. No physical playback claim.

Runtime selection: `POST /api/audio/select?session=URL_ENCODED_PRIVATE_DIRECTORY`
selects a different authenticated A/V session using the startup offer/plugin.
Only one selected session's audio plays; ordinary video slots are unaffected.
The old receiver is cancelled, native slot zero is drained on the render thread,
and the new receiver negotiates a fresh shared epoch. Switching does not mix clocks
or reuse old frames. Mute state persists across selection. A configured A/V plugin
is required; a missing peer is a bounded connection failure, not silent fallback.
Linux `test/av_renderer_select.py` passed two live producers, selection, independent
session teardown, mute/unmute and new-session completion. This selects the paired
A/V session in slot zero, not an audio-only mixer for arbitrary legacy video slots.
