# M4 Windows renderer runtime gate — 2026-10-30

Renderer source `5543540`; FrameLink playback source `a007e56`.
Host Caspar / NVIDIA RTX 5090 / MSVC 19.44 / GStreamer 1.28.0.
Isolated source/build `C:/Users/jonsu/dev/vkrender-m4-20261030` and
`framelink-m4-20261030`. Existing interactive demo was not replaced.

Passed:
- 300-frame BGRA native input normal completion, producer/renderer exit0.
- 300-frame NV12 input normal completion, producer/renderer exit0.
- Pause, native session drain, fresh producer restart, resume, new epoch, mute and
  unmute, both processes normal completion.
- Two separate producers, runtime selected-session switch, old receiver teardown,
  new native ring/epoch, normal completion of selected producer and renderer.
- Four targeted tests: HTTP, preview admission, scheduler, real GstTestClock/segment
  contract. The latter is now enabled on Windows, not merely compiled on Linux.

All four native test status captures had queue depth <=6 and invalid PTS0. Normal
BGRA/NV12 last sampled due decisions247/246 and sequence gaps29/28 respectively.
Thus these are functional, scheduling and lifecycle checks, NOT a claim that all
300 frames were presented or lossless50fps/physical scanout qualification. BGRA
uses shared GPU imports. NV12 retains the existing CPU conversion fallback.

Release builds do not enable Vulkan validation layers. This evidence does not
establish arbitrary stalled-GPU fault safety, sustained performance or physical
speaker/display lip sync. Independent source review and fault tests supplement it.

Earlier pause attempts using an intermediate source/build did not drain within
0.5s then5s; those failed runs are preserved outside this committed passing set in
`/tmp/m4-win-evidence`. Latest rebuilt source passed. No precise causal attribution
is made to the build refresh versus intervening playback cleanup changes.

Additional latest-source `autoaudiosink`300-frame BGRA run passed with selected
sink clock reported `audio_master=true` (see `vkr-av-render-avkrt5sm`). This verifies
clock selection/use, not acoustic output timing. Other four runs deliberately use
fakesink and explicitly report `audio_master=false`.
