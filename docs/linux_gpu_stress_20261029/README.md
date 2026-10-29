# Linux GPU stress validation — 2026-10-29

RTX3090, Release build, strict GPU-only scene outputs. CPU verification reads outputs; producer pixels stay on GPU. Measurements are consumer arrival timing, **not scanout or end-to-end glass latency**.

## Sustained differential pixel check

`VKR_CHECK_FRAMES=18000 VKR_CHECK_TIMEOUT=420 python3 test/gpu_output_integration.py /tmp/vkrender-linux-parity`

- 18,000 timestamp-matched RGB/NV12 pairs in 292.236 seconds, at 160x90 consumer dimensions from 1280x720 scene.
- 13,041 unique RGB image hashes; the background scene can revisit quantized pixel patterns, so this is not 18,000 distinct frame identities.
- Maximum BT.709 YUV discrepancy 1 byte (tolerance3).
- RGB received18,016 / sequence gaps14; NV12 received18,018 / sequence gaps17. **Not drop-free.**
- RGB arrival p50/p95/p99/max:15.653/17.696/18.061/28.555ms.
- NV12 arrival p50/p95/p99/max:15.599/17.837/19.154/28.210ms.
- Renderer RSS after15s:148,488→148,500KiB; FDs136 throughout. This is bounded process-memory evidence for this run, not a leak-free guarantee or VRAM accounting.
- Consumer growth includes intentionally retained per-frame timing/hash samples.

`resources.csv` and `sustained.log` preserve observations. Initial finite-headless harness hit the renderer's documented60s verification deadline; its failure is retained in `initial-finite-timeout.log`. Long run uses existing endless headless mode; production renderer behavior was not changed.

## Failure/recovery

`python3 test/linux_gpu_stress.py BUILD --output OUTPUT`

12 cycles passed: each kills an active consumer, recreates channels at different dimensions and verifies120 matched pairs. Three cycles also SIGKILL/restart the renderer while the consumer remains alive. No timeout or pixel mismatch; logs in `crash-cycles/`. Arbitrary process-kill timing does not prove an outstanding held GPU frame at the exact kill; deterministic ownership regressions are separate.

## Native pending-GPU recreation

`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation BUILD/vkr_gpu_output_pixels 2 stress`

100 consumer recreations/resizes while output recording is pending; 2,400 attempts with100 intentional discards, **2,300 verified frames, zero byte error**. Khronos validation enabled with no errors reported. Includes recorded-but-never-submitted shutdown rejection. This fixture has deterministic consumer disappearance before queue submission, not arbitrary producer-process death.

Final renderer regression suite9/9 passed after harness and Android import-guard changes.
