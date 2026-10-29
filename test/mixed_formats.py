#!/usr/bin/env python3
"""Real Linux DMA-BUF RGB + NV12 fan-in; assert both rendered pixel fixtures."""
import os
import pathlib
import subprocess
import sys
import tempfile
import time

root = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="vkr-mixed-") as runtime:
    env = dict(os.environ, XDG_RUNTIME_DIR=runtime, WAYLAND_DISPLAY="mixed", VKR_LAYOUT="wall")
    output = root / "mixed-formats.ppm"
    children = []
    with (root / "mixed-formats.log").open("w") as log:
        renderer = subprocess.Popen([str(root / "vkrenderer"), "--wayland", "mixed", "--headless", "100",
            "--window", "640x360+0+0", "--dump", str(output)], env=env, stdout=log, stderr=subprocess.STDOUT)
        children.append(renderer)
        try:
            deadline = time.monotonic() + 5
            while not pathlib.Path(runtime, "mixed").exists():
                if renderer.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("renderer did not create its Wayland socket")
                time.sleep(.02)
            for fmt in ["bgra", "nv12"]:
                children.append(subprocess.Popen([str(root / "vkr_wl_dmabuf_client"), "8", fmt],
                    env=env, stdout=log, stderr=subprocess.STDOUT))
                # Stable ordering gives separate left/right fixtures.
                time.sleep(.15)
            assert renderer.wait(timeout=20) == 0, "renderer failed"
        finally:
            for child in children:
                if child.poll() is None:
                    child.terminate()
                child.wait(timeout=5)
    with output.open("rb") as f:
        assert f.readline() == b"P6\n"
        assert f.readline() == b"640 360\n"
        assert f.readline() == b"255\n"
        pixels = f.read()
    def region(x0, x1):
        return [tuple(pixels[(y*640+x)*3:(y*640+x)*3+3])
                for y in range(110,250) for x in range(x0,x1)]
    left, right = region(25,285), region(355,610)
    red = sum(r>240 and g<12 and b<12 for r,g,b in right) / len(right)
    assert red > .98, f"NV12 red fixture fraction {red:.3f}"
    assert len(set(left)) > 500, "RGB gradient missing"
    print(f"PASS mixed RGB/NV12 native input: NV12 red={red:.3f}, RGB colors={len(set(left))}")
