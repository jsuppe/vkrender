# Parent acceptance review — 2026-10-31

Accepted for the measured Windows native BGRA/NV12 1080p60 workload. This closes
the sustained-performance review task, not M5 or physical-output qualification.

## Reviewed contract and source

Reviewed FrameLink `69d7b07` and renderer `7f58e38`: opt-in mode-3 budget/echo,
bounded arithmetic, unchanged startup and socket idle deadlines, post-GO active
deadline, source progress/EOS probes, cancellation and legacy defaults. No buffer
pool, playback latency, timestamp, fence, or presentation-policy change was made
in this review.

## Independently checked evidence

Decompressed the four retained case folders into a separate review directory and
ran `analyze.py` against the current FrameLink `tools/frame_accounting.py`.
All four cases passed. Traced BGRA and NV12 each account for 4,200/4,200 expected
identities through GPU completion and accepted presentation, including all
startup, steady and drain identities, with no trace footer losses. Steady accepted
presentation was 59.993059 / 59.993001 fps. Both trace-off cases pass their
aggregate counts only; they are not per-identity GPU-completion evidence.

These are fresh analyses of retained Windows captures, not new Windows runs.
The original failures and limits remain preserved. Physical scanout, cold wake,
acoustic synchronization and universal losslessness are not established.

## Fresh affected Linux checks

- Matching Release target builds succeeded in `/tmp/framelink-m5-build` and
  `/tmp/vkrender-m5-build`.
- FrameLink: `av_offer_bounds`, `av_audio_segment`, `m5_finite_budget` and
  `m4_av_recovery`: 4/4 passed. The finite matrix exercises nine real-peer cases,
  including early EOS, silent peers, missing/mismatched peers and cancellation.
- Renderer: `av_schedule`, `av_clock_contract`, `nv12_cpu`: 3/3 passed.

Review found a test-report discrepancy: the old malformed-budget loop listed
seven CLI inputs but printed eight. Added the missing empty-string input and
derive the printed count from the actual list. The updated Linux matrix passed
all eight. Prior Windows evidence covers seven CLI inputs, plus separate unit
parsing coverage; it is not retroactively relabeled as eight CLI cases. The
historical raw logs are preserved and this correction supersedes that count.

## Next milestone task

Review/integrate Android broker producer adoption and then production cross-UID
A/V integration and device qualification, sequentially. Final M5 CI/review/merge
is still required. M5a remains outside active scope.
