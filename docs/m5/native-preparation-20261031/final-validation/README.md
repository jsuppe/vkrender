# Final validation-layer follow-up

Matching implementation: renderer e101348, FrameLink f69dc83 (v6 base cbe7352).
Khronos layer explicitly enabled in all four logs; no VUID reported. This is ownership/error-path checking, not physical scanout qualification.

- Prepared 250 ms: strict delivery test FAILED, 179/180 GPU-completed. Independent expected-source accounting classified sole PTS 1,116,666,666 ns as unread mailbox replacement. Startup 60/60, steady 59/60, end 60/60. Preparation finished before source. No rerun to select a pass.
- Lazy negative 250 ms: expected failure observed, 158/180. All 22 losses in startup: 10 ring-full, five unread replacements, seven late. Steady/end complete.
- Injected preparation failure: both processes exit 1, no source media/GO, bounded exit; negative test passed.
- Preparation deadline: both processes exit 1, no source media/GO, bounded exit; negative test passed.

The additional steady mailbox replacement means the full strict suite is NOT passed. It does not erase the separate normal native 45-second 2700/2700 result or earlier 180/180 headless preparation result. The first-use gate addresses preparation only, not arbitrary runtime scheduling jitter. Remaining native present-call stalls and earlier trace-off BGRA excursion are still unresolved.

Raw failed positive, all negative logs, exact manifests and full accounting are retained here. No rendezvous authentication endpoint is included. PowerShell wrapper logs retain their original UTF-16 encoding.
