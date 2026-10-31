# Windows startup elapsed-time correction: PASS

FrameLink sample b98d42e, retaining the 5ff9c05 startup socket budgets and b9c7602 transport diagnostics. Release build passed. Full m5_startup.py passed on Caspar without enlarging process deadlines or changing 100 ms common latency.

- Original restart fault: requested 1500 ms, actual 1501981 us; 40/40 video, 38400 audio samples, zero late video. Existing 4000 ms process budget unchanged.
- Long preroll fault: requested 2300 ms, actual 2301030 us; 40/40 video, 38400 audio samples, zero late video. Existing 6000 ms test budget unchanged.
- Coordinated 700 ms BGRA/NV12: both 100/100.
- Legacy audio-policy negative control detects initial loss; cancellation terminates as expected.

The prior test counted 1500 individual nominal 1 ms sleeps; Windows rounding stretched this to 2735 ms and consumed the process budget. b98d42e uses an elapsed monotonic deadline with bounded sleep chunks and preserves cancellation/startup limits. This result qualifies startup transport tests, not native 60 fps performance or physical A/V alignment. Prior failing evidence remains under windows-startup-deadline-20261031.
