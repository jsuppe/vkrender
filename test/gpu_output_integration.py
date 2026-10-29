#!/usr/bin/env python3
"""Strict GPU scene-output RGB/NV12 comparison; requires a native GPU/GBM."""
import os, pathlib, subprocess, sys, tempfile
build = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='vkr-gpu-') as runtime:
    env = dict(os.environ, VKR_OUTPUT_MODE='gpu', XDG_RUNTIME_DIR=runtime)
    logpath = build / 'gpu-output-integration.log'
    with logpath.open('w') as log:
        consumer = subprocess.Popen([str(build/'vkr_portable_output_check'), 'gpu.output'], env=env, stdout=log, stderr=log)
        renderer = subprocess.Popen([str(build/'vkrenderer'), '--wayland', 'gpu-output-wayland', '--headless', '2000', '--scene-output', 'gpu.output.rgba', '--scene-output', 'gpu.output.nv12'], cwd=build, env=env, stdout=log, stderr=log)
        try:
            code = consumer.wait(timeout=50)
        finally:
            for process in (renderer, consumer):
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait()
    print(logpath.read_text())
    sys.exit(code)
