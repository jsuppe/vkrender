# Native A/V ring capacity and retirement

The native consumer uses eight transport slots: at most six pending timed frames,
one currently sampled frame, and one producer writer. Its nominal future-frame
budget is therefore `6 * fpsDen / fpsNum` seconds (100 ms at 60 fps, 200 ms at
30 fps). This is not a guarantee against unbounded GPU stalls, source bursts,
startup gaps, or frames retained by other consumers. Negotiated latency above
that envelope emits `AV_RING_BUDGET`; the shared A/V latency is **not** silently
reduced. Storage remains bounded and producer backpressure/timeouts or explicit
late drops remain possible. Larger guaranteed latency requires a separate
negotiated storage-capacity change, not an arbitrary local queue increase.

The renderer submits consumed timeline signals separately, before the next
scene submission, on the same Vulkan queue. These signals are ordered after
previous submitted GPU reads, but do not unnecessarily wait for a new swapchain
image, producer-ready wait, or the next scene's rendering. A consumed signal is
not physical presentation. The current sampled input is not released merely
because its CPU NV12 conversion finished; its mirror can still be sampled by
repeated draws. Device/slot destruction still drains GPU work.

Single-variable Windows native 1080p60 NV12 experiment (10 s, same 100 ms shared
latency, eight slots, six pending, validation disabled in both):

- Old bundled signal: 560/600 submitted, 554 acquired, 56.28 acquisition fps.
- Separate ordered retirement: 599/600 submitted, 597 acquired, 60.61 acquisition fps.

These first-to-last acquisition rates do not imply complete end-to-end delivery;
startup/end losses remain to be accounted for. A lower-latency 80 ms control
also recovered throughput, but is not the fix. An 8.33 ms early-adoption control
only partly recovered throughput and was removed. Sustained identity-traced
validation and delayed-GPU negative controls must qualify the final build.
