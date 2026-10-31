# Windows startup-phase deadline regression, 2026-10-31

Built FrameLink 5ff9c05 selected sample/plugin/test changes on the isolated Windows validation checkout, retaining b9c7602 transport diagnostics. Renderer unchanged, native demo untouched. Build succeeded. Original failure evidence remains separately preserved.

- m5_preconnect.py: passed.
- m5_startup.py coordinated 700 ms cases: BGRA and NV12 each 100/100, original PTS preserved.
- Audio-policy negative control detected initial video loss.
- Cancellation failed promptly as expected.
- Original 1500 ms restart: **FAILED**, both processes exited 1. New socket-startup budget permits GO, but whole-process watchdog expires. Actual logged preroll was 2735341 us. Producer progressed to at least 25 media frames before `FAIL process deadline`.
- The subsequent >2-second long-preroll case was not reached.

This is not a startup qualification pass. Evidence distinguishes the earlier socket-read failure from the now-exposed whole-process deadline. No timeout or latency was loosened to obtain a pass. Startup worker is investigating the configured deadline contract; performance capacity work is separate.
