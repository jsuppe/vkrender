# M4 repeated native generation validation

Linux RTX3090; renderer12196b9, FrameLink687eb73, fakesink explicit system-clock
fallback. Five-switch baseline passed. Ten-switch run with enabled Khronos
validation layer passed in the SAME renderer process. Final producer/renderer
exit0;22 setup/end boundaries;497 due decisions;queue highwater6;zero invalidPTS,
zero late drops.13 sequence gaps are recorded, not hidden or labeled lossless.
Renderer log explicitly confirms validation enabled; no VUID/validation error.

Command (use corresponding build paths):

```
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation python3 test/av_renderer_generations.py RENDERER PRODUCER PLUGIN --switches 10
```

This stresses authenticated selection, native ring drain/re-import and deferred
retirement on the normal GPU path. It does NOT artificially delay GPU completion,
verify sampled pixels, qualify sustained drift, or measure physical outputs.
Full raw poll history was retained at /tmp/vkr-av-render-r7th72dc/status.json.

## Remaining gates found by independent audit

- Sustained producer-vs-device drift is unqualified. Producer paces using its
  GstSystemClock and exact sample-count timestamps. Receiver selects audio clock;
  configureAudioSlaving sets RESAMPLE but explicitly needs no slaving when that
  device's clock is itself the master. This setting alone does not demonstrate
  adaptation between independent producer cadence and device clock. Add bounded
  +/-ppm SOURCE PACING injection without corrupting sample PTS, measure receiver
  queue/underruns over time, and demonstrate correction or documented bounded
  session recovery. Changing PTS slope would currently trigger the strict
  AudioTimeline continuity check, not correctly emulate source clock drift.
- Deliberately delayed GPU completion is still unqualified. Repeated ordinary
  switching is useful but not equivalent to an event-gated GPU fence test.
- HTTP status exposes schedule/queue/generation counts, but no measured audio
  underrun count, drift estimate or calibrated A/V-offset metric. Do not describe
  all required diagnostic fields as implemented.
- Actual device removal/reopen differs from injected CLOCK_LOST; platform output
  qualification remains separately labeled, not a transport-preservation blocker.
