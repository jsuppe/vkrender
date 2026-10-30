# Independent native A/V stall / producer-reconnect stress

Linux RTX3090, 2026-10-30. Renderer checkpoint 80d71c1 (build also includes
5543540, a Windows-only CMake test registration); FrameLink playback b594dfe.

Run:

```
python3 test/av_stall_reconnect.py /tmp/vkrender-m4-build/vkrenderer \
 /tmp/framelink-m4-playback-build/framelink_av_producer \
 /tmp/framelink-m4-playback-build/libgstframelink.so
```

- Real native BGRA input; no CPU image bridge in the renderer.
- Three 220 ms consumer process stalls using SIGSTOP/SIGCONT.
- Abrupt producer termination, then new authenticated producer/session.
- Receiver completes replacement session; replacement producer and renderer
  exit successfully. Original producer is deliberately killed.
- Queue depth remains bounded at six; 19 late drops demonstrate drop path was
  exercised; four session reset transitions; zero invalid PTS observed.
- Khronos validation explicitly enabled, loader activation evidenced in log;
  no Validation Error/VUID diagnostics. Synchronization validation requested.
- Fake audio output: no audio-device or physical presentation qualification.

**This is consumer-stall stress, not forced slow GPU execution.** SIGSTOP stops
CPU submissions while the GPU may finish outstanding work. A deterministic GPU
queue semaphore gate is still required for a stronger assertion that reuse is
prevented while old GPU reads remain deliberately in flight. Source review
alone does not establish that stronger runtime property.

Raw renderer/producer logs, result JSON, and per-poll status JSONL are included.
Session private directory/key files are intentionally excluded.
