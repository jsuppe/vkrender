#!/usr/bin/env python3
"""Renderer rate cell on Linux: the twin of test/windows_rate_cell.ps1.

framelink_producer -> vkrenderer --wayland (headless) -> scene outputs ->
vkr_portable_output_check, at 1920x1080@60 or 3840x2160@30. The gates are the
same as on Windows: no output arrivals over 17/34 ms and no output sequence gaps
over the measured window; every steady-state 5 s window adopts every input frame
with the producer-to-adoption hop inside the budget; sampled pixels within the
BT.709 limit; and <= 64 KB/min private memory growth per process.
Qualification uses --seconds 1800; shorter runs are smoke checks only.
"""
import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time

CELLS = {"1080p60": (1920, 1080, 60, 17), "4k30": (3840, 2160, 30, 34)}


def private_bytes(pid):
    try:
        text = pathlib.Path(f"/proc/{pid}/smaps_rollup").read_text()
    except OSError:
        return None
    kb = sum(int(m.group(1)) for m in re.finditer(r"^Private_(?:Clean|Dirty):\s+(\d+) kB", text, re.M))
    return kb * 1024


def slope_kb_per_min(points):
    if len(points) < 3:
        return None
    n = len(points)
    mx = sum(t for t, _ in points) / n
    my = sum(v for _, v in points) / n
    den = sum((t - mx) ** 2 for t, _ in points)
    return round(sum((t - mx) * (v - my) for t, v in points) / den * 60 / 1024, 2) if den else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bindir", type=pathlib.Path, help="directory holding vkrenderer and the checker")
    ap.add_argument("--cell", choices=CELLS, default="1080p60")
    ap.add_argument("--format", choices=["bgra", "nv12"], default="bgra")
    ap.add_argument("--output-mode", choices=["cpu", "gpu"], default="gpu")
    ap.add_argument("--seconds", type=int, default=1800)
    ap.add_argument("--warmup", type=int, default=10)
    ap.add_argument("--verify-every", type=int, default=-1, help="0 = timing only; default: fps for cpu, 0 for gpu")
    ap.add_argument("--producer", type=pathlib.Path, help="framelink_producer (default: beside vkrenderer)")
    ap.add_argument("--out", type=pathlib.Path)
    a = ap.parse_args()
    w, h, fps, budget = CELLS[a.cell]
    # GPU-rendered outputs are not CPU-readable in a meaningful layout everywhere
    # (NVIDIA Linux), and mapping them stalls acquisition, so GPU cells time
    # arrivals only; gpu_output_pixels_* ctests cover their pixels.
    every = a.verify_every if a.verify_every >= 0 else (fps if a.output_mode == "cpu" else 0)
    b = a.bindir.resolve()
    producer_exe = (a.producer or b / "framelink_producer").resolve()
    name = f"rate-{a.cell}-{a.format}-{a.output_mode}-{os.getpid()}"
    out = (a.out or b / name).resolve()
    out.mkdir(parents=True, exist_ok=True)
    frames = (a.seconds + a.warmup + 60) * fps
    config = dict(cell=a.cell, width=w, height=h, fps=fps, budget_ms=budget, input_format=a.format,
                  output_mode=a.output_mode, seconds=a.seconds, warmup=a.warmup, verify_every=every,
                  producer_frames=frames)
    result = {"config": config}
    with tempfile.TemporaryDirectory(prefix="vkr-rate-") as runtime:
        env = dict(os.environ, XDG_RUNTIME_DIR=runtime, WAYLAND_DISPLAY=name, VKR_INPUT_FORMATS=a.format,
                   VKR_OUTPUT_MODE="gpu" if a.output_mode == "gpu" else "", VKR_TRACE="", FRAMELINK_TRACE="",
                   VKR_CHECK_WIDTH=str(w), VKR_CHECK_HEIGHT=str(h), VKR_CHECK_SECONDS=str(a.seconds),
                   VKR_CHECK_WARMUP=str(a.warmup), VKR_CHECK_BUDGET_MS=str(budget),
                   VKR_CHECK_VERIFY_EVERY=str(every), VKR_CHECK_FRAMES=str(max(1, a.seconds // 2)),
                   VKR_CHECK_TIMEOUT=str(a.seconds + a.warmup + 120))
        procs = {}

        def start(key, argv):
            procs[key] = subprocess.Popen([str(x) for x in argv], cwd=out, env=env,
                                          stdout=(out / f"{key}.out").open("w"),
                                          stderr=(out / f"{key}.err").open("w"))
            return procs[key]

        memory = []
        try:
            check = start("check", [b / "vkr_portable_output_check", name])
            time.sleep(0.3)
            start("render", [b / "vkrenderer", "--wayland", name, "--headless", "-1",
                             "--scene-output", f"{name}.rgba", "--scene-output", f"{name}.nv12",
                             "--window", f"{w}x{h}+0+0", "--control-port", "0"])
            time.sleep(0.8)
            start("producer", [producer_exe, "--channel", f"{name}.in.0", "--format", a.format,
                               "--size", f"{w}x{h}", "--fps", fps, "--frames", frames, "--deadline-ms", 20000])
            t0 = time.monotonic()
            limit = a.seconds + a.warmup + 150
            while check.poll() is None:
                if time.monotonic() - t0 > limit:
                    raise RuntimeError("checker timeout")
                row = {"t": round(time.monotonic() - t0, 1)}
                for k, p in procs.items():
                    if p.poll() is None:
                        row[f"{k}_private"] = private_bytes(p.pid)
                memory.append(row)
                try:
                    check.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass
            result["check_exit"] = check.returncode
        except Exception as e:  # recorded as a failure, never retried
            result["error"] = str(e)
        finally:
            for k in ("producer", "render", "check"):
                p = procs.get(k)
                if p and p.poll() is None:
                    p.terminate()
                    try:
                        p.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        p.kill()
                        p.wait()
    (out / "memory.json").write_text(json.dumps(memory, indent=1) + "\n")
    check_out = (out / "check.out").read_text().splitlines() if (out / "check.out").exists() else []
    result["check"] = check_out
    slopes = {}
    for k in ("render", "producer", "check"):
        pts = [(r["t"], r[f"{k}_private"]) for r in memory
               if r["t"] >= a.warmup + 30 and r.get(f"{k}_private")]
        slopes[k] = slope_kb_per_min(pts)
    result["memory_kb_per_min"] = slopes
    pattern = re.compile(r"compositor: (\d+) adoptions in 5s \(pool[^)]*\)(?: hop avg ([\d.]+) ms max ([\d.]+) ms)?")
    windows = [(int(m.group(1)), float(m.group(3)) if m.group(3) else None)
               for m in pattern.finditer((out / "render.err").read_text(errors="replace"))]
    steady = windows[2:-1] if len(windows) > 3 else []
    min_adopt = min((n for n, _ in steady), default=0)
    hops = [x for _, x in steady if x is not None]
    hop_max = max(hops) if hops else None
    result["input"] = dict(windows=len(steady), min_adoptions_per_5s=min_adopt, expected_per_5s=fps * 5,
                           hop_max_ms=hop_max)
    perr = (out / "producer.err").read_text(errors="replace").splitlines() if (out / "producer.err").exists() else []
    result["producer"] = next((l for l in reversed(perr) if l.startswith("producer ")), None)
    fail = []
    if result.get("error"):
        fail.append(result["error"])
    if result.get("check_exit") != 0:
        fail.append(f"checker exit {result.get('check_exit')}")
    if not steady:
        fail.append("no steady-state renderer windows")
    elif min_adopt < fps * 5 - 1:
        fail.append(f"input drops: min {min_adopt} adoptions per 5 s, expected {fps * 5}")
    if hop_max is not None and hop_max > budget:
        fail.append(f"input hop max {hop_max} ms over {budget} ms")
    fail += [f"{k} memory +{v} KB/min" for k, v in slopes.items() if v is not None and v > 64]
    result["pass"] = not fail
    result["failures"] = fail
    (out / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    for line in check_out:
        print("  " + line)
    print(f"  input: min {min_adopt}/{fps * 5} adoptions per 5 s over {len(steady)} windows, "
          f"hop max {hop_max} ms; memory KB/min: {slopes}")
    if fail:
        print(f"FAIL {a.cell} {a.format} {a.output_mode}: " + "; ".join(fail))
        return 1
    print(f"PASS: {a.cell} {a.format} {a.output_mode}, {a.seconds} s, summary {out / 'summary.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
