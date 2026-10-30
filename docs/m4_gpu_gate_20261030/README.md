# Deliberately delayed GPU completion qualification

Linux RTX3090, native BGRA, FrameLink687eb73, renderer based on12196b9 plus
this commit. Enabled Khronos validation. Five selected-session switches in ONE
renderer,12 setup/end boundaries,91 gated GPU submissions. Each gate holds the
submission on an unsatisfied Vulkan timeline semaphore for100ms, every seventh
scene frame. A host thread verifies VkFence is NOT_READY and timeline counter
below target, then signals the semaphore. Fence reuse and destruction join the
thread; normal native fences and deferred consumed-watermark paths are unchanged.

Result: all91 observations verified pending GPU completion;zero validationerrors;
215due decisions,128late drops,59sequence gaps,queue highwater6,invalidPTS0.
Producer/renderer exit0. Late drops under intentional overload are expected and
recorded; this is NOT a throughput or pixel-integrity qualification.

```
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation python3 test/av_renderer_generations.py RENDERER PRODUCER PLUGIN --switches 5 --gpu-gate-ms 100
```

`VKR_TEST_GPU_GATE_MS` is opt-in,headless-only,capped1000ms;default0 allocates no
extra Vulkan resources and starts no threads. The test requires validation enabled.

Negative control: remove only the three submit wait-vector additions, retaining
CPU timer,host signal and assertions. Test exits1 because ALL inspected fences
are already signaled(status0), despite counter still below target. This proves
CPU sleeping alone cannot satisfy the test. Mutation restored and normal renderer
rebuilt after test. See negative-no-gpu-wait.log.

Initial experimental HOST-event design was rejected by validation
(VUID-vkSetEvent-event-09543) and discarded. Its raw failed run remains
/tmp/vkr-av-render-ujzmau58;no event-based gate is shipped.

Raw successful sample history: /tmp/vkr-av-render-99xkpmg3/status.json.
Remaining separate gates: sustained source/device drift,underrun/drift/A-V offset
metrics,physical output qualification. This closes the previously untested
**delayed native GPU retirement plus repeated generation** scenario on Linux,
not Windows runtime,not pixel comparisons,not broad long-soak qualification.
