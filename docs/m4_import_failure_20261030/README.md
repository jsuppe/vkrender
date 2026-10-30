# Failed native adoption regression

Scheduled-frame import failures previously popped the pending descriptor then
performed only bookkeeping release. Without a later successful adoption the
consumed watermark could stay pinned and exhaust the ring. Both Linux and Windows
now forget any retained scene sampling and route failed pool/upload/import/bind
adoption through the same deferred consumed-watermark path as a late-frame drop.
Linux waits its frame-fence retirement indexes; Windows signals the consumed
semaphore through the ordered rendering submission. Neither directly releases
storage from the failure branch while old draws might still sample it.

`VKR_TEST_AV_IMPORT_FAILURES=1` injects 40 consecutive failed adoptions after 19
successful due decisions. Forty exceeds the eight-slot ring; a leak stalls before
the test can observe recovery. It acts only on negotiated A/V input. Production
defaults are unchanged. `av.import_failures` records failures separately from late
frames and scheduling decisions. This hook exercises common cleanup, not every
possible driver allocation error.

Linux BGRA with Khronos validation and `VKR_TEST_GPU_GATE_MS=20` passed: both
processes completed; exactly40 import failures; final sampled263 due decisions;
queue peak6; no invalid PTS or late drops; actual pre-release GPU fences observed
pending. NV12 same 40-failure test also passed with validation enabled. Neither
log contains Validation Error/VUID. NV12 remains CPU-converted, not GPU-only.
HTTP control regression passed. Windows code updated but runtime evidence is
not claimed by this Linux run. Physical audio/display alignment not measured.

Reproducer:
```
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VKR_TEST_GPU_GATE_MS=20 \
python3 test/av_renderer_smoke.py RENDERER PRODUCER PLUGIN --import-failures
```
