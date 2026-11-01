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
  including untouched output padding. Linux ASan/UBSan and forced scalar
  fallback builds passed.
- Deterministic pacing test proves processing time consumes budget, missed-tick
  debt discarded, and display-mode reset. Linux and Windows passed.
- `VKR_VALIDATION=1` explicitly requests Khronos validation in optimized builds;
  unavailable layer is a clear initialization failure, not a silent unvalidated
  success. Default release behavior remains unchanged.
- Caspar explicit `VKR_VALIDATION=1` initialization was exercised and correctly
  exited1 with `requires VK_LAYER_KHRONOS_validation`. Caspar currently has no
  registered Khronos validation layer. No Windows GPU
  lifetime validation claim is made from the default Release performance runs.
- SIMD final10-second matrix: 160x90@50 BGRA49.58/NV1249.57;
  1080p30 both30.18; 1080p60 BGRA59.25/NV1256.09 acquisitions/s.
  CPU pixel median1.475ms and full readback/conversion/upload3.364ms at1080p30.
  SIMD removes the conversion bottleneck but remaining acquisition loss is NOT
  closed. Readiness-gated metadata acquisition is being isolated next.

Startup losses and latest-wins polling gaps remain visible in raw counts.
These fixes do not promise lossless acquisition or physical smooth playback.

## Readiness-gated acquisition follow-up (ce86138)

NV12 used FL_DEFER_RELEASE, so zero-timeout acquisition rejected metadata while
producer GPU copy was unfinished; BGRA's FL_GPU_SYNC did not. On latest-wins
transport this creates extra polling losses, even though actual CPU access is
scheduled roughly100ms later. Use GPU_SYNC for scheduled input metadata and
explicitly `vkWaitSemaphores(timeout=0)` on the imported ready timeline before
CPU mapping. Not-ready frames stay in the unchanged bounded queue. This is
not permission to read before producer completion; it moves readiness checking
to the adoption boundary without introducing a blocking wait.

Single-variable10-second A/B: NV12 acquisition56.09→59.04fps, submissions581→594,
acquisitions552→581 from600 source frames. BGRA path unchanged by this follow-up.
GPU retirement/consumed-fence ownership remains unchanged. Actual Windows
validation layer is absent, so delayed-producer-fence/GPU stress qualification
must remain explicit rather than inferred from throughput.

Acquisition interval distribution,10-second NV12 controls: baseline median27.54ms,
p99 55.57ms,max104.30ms; final readiness correction median16.02ms,p99 32.58ms,
max47.12ms. These include startup/intervening stalls between first and last
observed events; they are not actual presentation intervals. Final control
reported maxqueue6, zero invalidPTS/import failures, and all600audio blocks/
480000samples completed. There are still13 sequence gaps and2 late drops in
that short run, so this is not a zero-loss claim.


Final ce86138 sustained45-second NV121080p60 run:2700inputs,2679successful
submissions,2650acquisitions at59.11fps. Producer/renderer exits0. Two ordinary
native NV12 session-generation switches passed with queue<=6, no invalidPTS.
This is ordinary recovery qualification, NOT delayed-GPU validation-layer proof.
Remaining loss is visible; no silent queue enlargement or timeout relaxation.
