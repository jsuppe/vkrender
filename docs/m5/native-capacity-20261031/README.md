# Windows native capacity matrix — 2026-10-31

Matching FrameLink cbe7352 (wire v6/public max16, startup deadline/fault fixes and probes), renderer cc527ba (code-identical to parent6f90a84; capacity94da351). RTX5090, native window on interactive limited desktop. Same compiled binaries, 100 ms common latency, six scheduled future samples. Only VKR_TEST_AV_POOL_SLOTS=8/12 differs. No Khronos layer during these performance captures; separate prior lifetime qualification remains separate.

## Gate and results

Standalone Windows `expanded_pool` CTest passed1/1, covering all16 slots/high-index recycle, invalid17 and v5 rejection. Renderer superbuild disables FrameLink tests, so its attempted expanded_pool target was absent; a matching standalone test build was used, not a skipped gate.

Eight sequential45-second captures: **18,900/18,900 independently enumerated source identities** passed every required stage through accepted presentation and observed GPU completion. No startup/steady/end losses, missing source identities, unclassified endings or trace overflow. Every repeated render/GPU/present tuple also matched.

- Eight-slot BGRA60 control:2700/2700,59.99915fps.
- Twelve-slot BGRA60 first:2700/2700,59.99918fps; repeat2700/2700,59.99921fps.
- Eight-slot NV1260 control:2700/2700,59.99926fps.
- Twelve-slot NV1260 first:2700/2700,59.99932fps; repeat2700/2700,59.99916fps.
- Twelve-slot BGRA30:1350/1350,29.99971fps.
- Twelve-slot NV1230:1350/1350,29.99959fps.

Rates use first accepted presentation timestamps of distinct source identities within the steady interval[1s,44s), not120Hz repeated scene draws. Twelve-slot steady maximum first-unique presentation gap: BGRA60 first17.01ms/repeat16.99ms; NV1260 first17.41ms/repeat16.92ms; BGRA30 33.65ms; NV1230 33.60ms. NV12 startup still has uneven first presentation intervals up to30.09ms, without identity loss; do not conceal startup jitter behind steady-state results.

## Interpretation and limits

Both eight-slot controls passed in these phases. Therefore this matrix validates the twelve-slot configuration for these captures but **does not demonstrate universal FPS superiority over eight slots**. Prior failing eight-slot ownership evidence remains under ring-ownership-20261031 and native-accounted-20261031; it demonstrates phase-sensitive saturation, not an invariant56fps ceiling.

The clean BGRA60 eight-slot control nevertheless waited in flAcquireBuffer: median4.02455ms/max5.2513ms, versus twelve-slot median0.0003ms/max0.0095ms. Signal/Flush medians stayed approximately0.045ms/0.0025ms. Twelve slots remove observed acquisition wait in this pair, while both configurations meet60fps. Source-to-render phase differs; phase-summary.json records source arrival, end-submit, IPC, adoption and accepted-presentation intervals plus producer-end→next-swapchain-acquire CPU-observation. Small negative end-submit→IPC intervals reflect IPC receipt before the submitting API returns, not negative transport latency.

No physical scanout or speaker alignment is measured. GPU completion timestamps are fence observations; presentation means successful API acceptance. No claim of tolerance for arbitrary unbounded GPU stalls or producer bursts. The previous isolated66ms BGRA30 submission stall remains non-reproduced, not causally explained by these clean captures. A deterministic phase/retirement model, not repeated random captures until failure, is appropriate for further capacity-bound validation.

All raw CSVs, independent manifests, full accounting and concise rate/phase reports are retained. CSV/full reports are compressed losslessly. No original desktop demo was changed. Matching producer/plugin/library versions are mandatory for wire v6.
