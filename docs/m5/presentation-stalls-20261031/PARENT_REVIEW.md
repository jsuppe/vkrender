# Parent review — 2026-10-31

Reviewed source `78b13ec`, evidence `78abf8a` and final harness records
`76b9977`. No additional source changes were required by this review.

## Accepted correction

- The session callback requests power on the render thread before acknowledging
  session setup. ProducerMapped PRIMED/final epoch/GO follow that acknowledgment.
- The normal loop keeps the request for the timed session, independent of whether
  a current image is temporarily absent after a scheduled drop.
- Teardown drops the ending session's image ownership before recalculating power
  for any other active slots. The scope guard clears the thread's request on
  service exit, including failure/cancellation. Headless-only output requests
  SYSTEM, not DISPLAY; existing unscheduled slots preserve their prior policy.
- No media timestamps, buffer capacity, pending queue, presentation delay or GPU
  fences changed. Diagnostics report actual selected mode and window state.
- The test-only old-order control is not the default. Its rejection is an ordering
  test, not evidence that a warm monitor reproduced the original hardware stall.

## Evidence and limits

The exact-symbol ETW readier stack establishes a monitor-power transition in the
reproduced failure. Wait-state timing rules out ordinary runnable-thread starvation
as the main cost of that capture. It does not retroactively identify the historical
untraced BGRA excursion. The retained warm controls do not qualify cold power-on.

Reviewed executor results: Windows builds; validation-enabled short positive,
negative and failure gates; normal native BGRA/NV12 **5,400/5,400** complete frame
identities; tracing-off BGRA clean aggregate counters; Linux affected **3/3**;
terminal power-request cleanup. These are the saved executor runs, not fresh
parent reruns. No unchanged test was repeated solely to inflate test counts.

The request completes before source media; asynchronous physical monitor readiness
is not guaranteed by that API call. Cold physical wake and scanout remain explicitly
unqualified. No claim of complete M5 performance qualification is made.

**Disposition:** approve this bounded power-ordering task. Next single task is
the sustained-performance gate, including a reviewed finite-run watchdog budget
that permits a real >=60-second steady interval plus setup and drain.
