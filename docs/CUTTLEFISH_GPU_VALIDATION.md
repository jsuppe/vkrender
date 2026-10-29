# Cuttlefish validation — 2026-10-29

Resumed retained Android 13 release-83 Cuttlefish image with gfxstream, sandbox=false; no image replacement or userdata reset. Native x86_64 tests built with NDK 28.2, API 29, Release. Emulator uses host RTX 3090 through gfxstream, not the host native Vulkan driver contract.

## Passed: 11 FrameLink checks

- NV12 geometry/padding/overflow.
- NV12, RGBA, BGRA and RGB10A2 CPU-plane mapping: 8 patterned frames per format, all bytes verified.
- Ready/consumed FD ownership and timeout contracts.
- Reconnect while storage is held, then new generation.
- Tracing disabled, bounded concurrent tracing, thread limit.
- Separate producer/consumer roundtrip: 20 verified, zero mismatches/skips; producer-requested resize from 256x128 to 320x240.

These exercise Android AHardwareBuffer transport and CPU mappings, not Vulkan GPU output.

## GPU output: BLOCKED, not passed

`vkr_gpu_output_caps` exits 1. Logcat: `no Vulkan 1.3 device with the required interop extensions`. Android `cmd gpu vkjson` reports device API 4198400 (Vulkan 1.1.0), although the loader reports 1.3. The renderer requires Vulkan 1.3 dynamic rendering/synchronization2. `VK_ANDROID_external_format_resolve` is absent, blocking the current Android native-YUV output path as well.

GPU pixel/lifecycle/4K and full Activity comparison were NOT run: renderer initialization cannot succeed. No requirement bypass, CPU fallback, or emulator result substituted for GPU qualification. A newer Vulkan-1.3-capable Cuttlefish/gfxstream guest/host stack is needed for RGB; NV12 additionally requires external-format resolve support. Physical-device results remain separately documented in GPU_OUTPUT.md.

Raw evidence: workspace `plans/cuttlefish-gpu-20261029/` (results.json, individual test logs, vkjson.json, logcat.txt, identity.txt, launch/stop logs). Existing host Cuttlefish pointers backed up before launch and restored after shutdown; retained VM userdata preserved.
