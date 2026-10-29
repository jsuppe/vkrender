# Windows stage 3 shared-change regression

Renderer source snapshot: `81bcd770128b2890b75c8082f99d709d5b43b6cd` from the
integration branch. Its successor `b7add7b` changes CI selection only; runtime
sources are identical. FrameLink source was synchronized to `a722a5b` (sample
reconnect fix); GPU backend is unchanged from the earlier validated `5e80342`.

Caspar RTX 5090. Isolated `vkrender-control-20261029/build-control/Release`;
pre-existing live demo and its build output were not modified or stopped.

- Build: Windows renderer, shared control tests, scene layout test, GPU output
  conversion test and portable comparison checker all succeeded.
- **7/7 CTest cases passed:** three GPU conversion tests, shared scene layouts,
  shared moderation, HTTP validation and concurrent preview admission.
- **300 matched, 300 distinct RGB/NV12 pairs**, worst YUV byte error **1**, zero
  sequence gaps. Elapsed6.802s includes startup and CPU pixel readback/checking.
- Interarrival RGB p50/p95/p99/max:16.710/18.262/18.292/27.996ms;
  NV12:16.531/18.291/18.322/18.712ms. Not physical presentation latency.
- Actual headless renderer GPU JPEG preview: **8 snapshots, 8 distinct JPEG
  hashes**, valid JPEG start/end markers. This exercises the scene readback and
  HTTP path, not just a fabricated control fixture.

Raw logs and exact invocation scripts are included. The bounded rerun verifies
shared control/readback changes without repeating the earlier five-minute GPU
stress. No sustained performance claim is based on this short regression.
