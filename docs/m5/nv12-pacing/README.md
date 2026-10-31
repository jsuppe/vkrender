# Windows NV12 offscreen render-thread performance

Caspar RTX5090, Windows, Release, GStreamer1.28.0, ProducerMapped clock.
Measurements are native acquisition/event timings, not actual display scanout.
Unchanged FrameLink producer/plugin: `vkrender-m5-policy-build/framelink/Release`.
Renderer baseline f3a856e; candidate b8a1489. No browser/JPEG requests in runs.

## Mechanism and controls

The old headless loop slept16ms **in addition to** frame work. NV121080p60
baseline measured median16.485ms sleeping and10.848ms CPU readback/conversion/
upload; fresh10-second acquisition35.30fps matches that budget.

Replace additive sleep with an absolute deadline. Retain original nominal16ms
cadence; processing consumes that budget. Missed ticks discard schedule debt;
there is no spin loop, extra queue, or expanded timeout. Display/FIFO-paced
mode is unchanged and resets the offscreen epoch when leaving that mode.

An exact60Hz-deadline negative experiment is retained (`pacing-after`): whole
loop60Hz but acquisition43BGRA/35NV12. Latest-wins acquisition at the same rate
as producer cadence can miss frames through polling phase; this is NOT the
shipped cadence. Original nominal16ms deadline gives58.22BGRA/56.19NV12 in
matched10-second runs (`pacing-16ms`).

Further scoped measurements isolate NV12 CPU cost: map/readback0.675ms,
scalarpixels8.397ms, upload0.307ms (medians), total10.273ms. SSE2 conversion
retains exact BT.709 limited integer coefficients, rounding, clipping, chroma
pairing and alpha. Non-x86 retains scalar fallback. It avoids changing GPU
allocation, plane interpretation, synchronization, or memory-negotiation scope.

## Tests and limitations

- Linux and Windows CPU tests compare SIMD result byte-for-byte against scalar
  reference across padded rows, widths2/4/6/14/1920, heights2/10 and random input,
  including untouched output padding. Linux ASan/UBSan passed.
- Deterministic pacing test proves processing time consumes budget, missed-tick
  debt discarded, and display-mode reset. Linux and Windows passed.
- `VKR_VALIDATION=1` explicitly requests Khronos validation in optimized builds;
  unavailable layer is a clear initialization failure, not a silent unvalidated
  success. Default release behavior remains unchanged.
- Caspar currently has no registered Khronos validation layer. No Windows GPU
  lifetime validation claim is made from the default Release performance runs.
- Candidate SIMD performance matrix pending at this checkpoint.

Startup losses and latest-wins polling gaps remain visible in raw counts.
These fixes do not promise lossless acquisition or physical smooth playback.
