#!/usr/bin/env python3
"""Linux native A/V consumer-stall/reconnect stress with Vulkan validation.

SIGSTOP stalls the process, NOT GPU execution. This checks bounded queues and
recovery; it is not a deterministic slow-GPU semaphore/fence proof.
"""
import argparse
import json
import os
import pathlib
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('renderer'); p.add_argument('producer'); p.add_argument('plugin')
p.add_argument('--output')
a = p.parse_args()
root = pathlib.Path(a.output or tempfile.mkdtemp(prefix='m4-stall-'))
root.mkdir(parents=True, exist_ok=True); root.chmod(0o700)
with socket.socket() as s:
    s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]
env = dict(os.environ, XDG_RUNTIME_DIR=str(root), VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',
           VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT', VK_LOADER_DEBUG='layer')
session = str(root / 'session')
samples = []; procs = []; logs = []; renderer = None
producer_command = [a.producer, a.plugin, '--session', session, '--frames', '250', '--timeout-ms', '15000']
try:
    def launch(cmd, name):
        log = (root / (name + '.log')).open('w'); logs.append(log)
        proc = subprocess.Popen(cmd, stdout=log, stderr=log, env=env, cwd=root)
        procs.append(proc); return proc
    producer = launch(producer_command, 'producer-original')
    renderer = launch([a.renderer, '--wayland', 'm4-stall', '--headless', '-1', '--av-exit-on-end',
                       '--control-port', str(port), '--av-session', session, '--av-plugin', a.plugin,
                       '--av-audio-sink', 'fakesink'], 'renderer')
    deadline = time.monotonic() + 35
    stalls = 0; restarted = False; initial_resets = None; last_stall = 0
    while renderer.poll() is None and time.monotonic() < deadline:
        time.sleep(.05)
        try:
            with urllib.request.urlopen('http://127.0.0.1:' + str(port) + '/api/status', timeout=.5) as r:
                status = json.load(r)
        except (urllib.error.URLError, TimeoutError, ConnectionResetError):
            continue
        samples.append(status); av = status['av']
        if av.get('due_decisions', 0) > 20 and initial_resets is None:
            initial_resets = av['session_resets']
        if initial_resets is not None and stalls < 3 and time.monotonic() - last_stall > .35:
            os.kill(renderer.pid, signal.SIGSTOP)
            try:
                time.sleep(.22)
            finally:
                os.kill(renderer.pid, signal.SIGCONT)
            stalls += 1; last_stall = time.monotonic()
        if stalls == 3 and not restarted and time.monotonic() - last_stall > .3:
            producer.kill(); producer.wait(timeout=5)
            producer = launch(producer_command, 'producer-replacement')
            restarted = True
    renderer.wait(timeout=3); producer.wait(timeout=5)
    assert renderer.returncode == producer.returncode == 0, (renderer.returncode, producer.returncode)
    assert stalls == 3 and restarted
    assert samples and all(s['av']['max_queue_depth'] <= 6 for s in samples)
    assert all(s['av']['invalid_pts'] == 0 for s in samples)
    assert any(s['av']['session_resets'] > initial_resets and s['av']['due_decisions'] > 20 for s in samples)
    assert any(s['av']['late_drops'] > 0 for s in samples), 'stall did not exercise late-drop path'
    for log in logs: log.flush()
    text = (root / 'renderer.log').read_text(errors='replace')
    assert 'Insert instance layer \"VK_LAYER_KHRONOS_validation\"' in text or 'VK_LAYER_KHRONOS_validation' in text and 'libVkLayer_khronos_validation.so' in text, 'validation layer activation not evidenced'
    assert 'Validation Error' not in text and 'VUID-' not in text, 'Vulkan validation findings: see renderer.log'
    result = {'passed': True, 'consumer_stalls': stalls, 'producer_restarts': 1,
              'max_queue': max(s['av']['max_queue_depth'] for s in samples),
              'max_late_drops': max(s['av']['late_drops'] for s in samples),
              'max_session_resets': max(s['av']['session_resets'] for s in samples),
              'scope': 'process stalls, not forced GPU execution delay'}
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
finally:
    if renderer and renderer.poll() is None:
        os.kill(renderer.pid, signal.SIGCONT)
    for proc in procs:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    for log in logs: log.close()
    (root / 'status.json').write_text(json.dumps(samples, indent=2) + '\n')
