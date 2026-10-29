#!/usr/bin/env python3
"""Strict GPU scene-output RGB/NV12 comparison; requires a native GPU/GBM."""
import os, pathlib, subprocess, sys, tempfile, time, csv
build = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='vkr-gpu-') as runtime:
    env = dict(os.environ, VKR_OUTPUT_MODE='gpu', XDG_RUNTIME_DIR=runtime)
    logpath = build / 'gpu-output-integration.log'
    with logpath.open('w') as log:
        consumer = renderer = None
        try:
            consumer = subprocess.Popen([str(build/'vkr_portable_output_check'), 'gpu.output'], env=env, stdout=log, stderr=log)
            renderer = subprocess.Popen([str(build/'vkrenderer'), '--wayland', 'gpu-output-wayland', '--headless', '-1', '--scene-output', 'gpu.output.rgba', '--scene-output', 'gpu.output.nv12'], cwd=build, env=env, stdout=log, stderr=log)
            started = time.monotonic()
            limit = int(env.get('VKR_CHECK_TIMEOUT', '45')) + 5
            with (build / 'gpu-output-resources.csv').open('w') as samples:
                writer = csv.writer(samples)
                writer.writerow(['elapsed_s', 'role', 'pid', 'rss_kib', 'fds'])
                while consumer.poll() is None:
                    if time.monotonic() - started > limit:
                        raise TimeoutError('GPU comparison exceeded deadline')
                    for role, process in [('renderer', renderer), ('consumer', consumer)]:
                        proc = pathlib.Path('/proc') / str(process.pid)
                        try:
                            status = (proc / 'status').read_text()
                            rss = next(int(line.split()[1]) for line in status.splitlines() if line.startswith('VmRSS:'))
                            writer.writerow([round(time.monotonic()-started, 3), role, process.pid, rss, len(list((proc/'fd').iterdir()))])
                            samples.flush()
                        except (FileNotFoundError, ProcessLookupError, StopIteration):
                            pass
                    time.sleep(1)
                code = consumer.returncode
                if renderer.poll() is not None and code == 0:
                    raise RuntimeError('renderer exited before test completion')
        finally:
            for process in (renderer, consumer):
                if process is not None and process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait()
    print(logpath.read_text())
    sys.exit(code)
