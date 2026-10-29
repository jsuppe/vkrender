#!/usr/bin/env python3
"""Isolated GPU output crash/reconnect/resize checks; no scanout timing claim."""
import argparse, json, os, pathlib, subprocess, tempfile, time
p = argparse.ArgumentParser()
p.add_argument('build', type=pathlib.Path)
p.add_argument('--cycles', type=int, default=12)
p.add_argument('--output', type=pathlib.Path, required=True)
a = p.parse_args()
if a.cycles < 1: p.error('cycles must be positive')
build = a.build.resolve()
a.output.mkdir(parents=True, exist_ok=True)
results=[]

def stop(child):
    if child and child.poll() is None:
        child.terminate()
        try: child.wait(timeout=5)
        except subprocess.TimeoutExpired: child.kill(); child.wait()

def resources(child):
    root=pathlib.Path('/proc')/str(child.pid)
    status=(root/'status').read_text()
    return dict(rss_kib=int(next(x for x in status.splitlines() if x.startswith('VmRSS:')).split()[1]),fds=len(list((root/'fd').iterdir())))

with tempfile.TemporaryDirectory(prefix='gpu-stress-') as runtime:
    env=dict(os.environ, VKR_OUTPUT_MODE='gpu', XDG_RUNTIME_DIR=runtime,
             VKR_CHECK_FRAMES='120', VKR_CHECK_TIMEOUT='30')
    renderer=checker=None
    logs=[]
    try:
        for cycle in range(a.cycles):
            if cycle % 4 == 0:
                if renderer:
                    renderer.kill(); renderer.wait()
                log=(a.output/f'renderer-{cycle}.log').open('w'); logs.append(log)
                renderer=subprocess.Popen([str(build/'vkrenderer'),'--wayland','stress-wayland','--headless','-1','--scene-output','stress.rgba','--scene-output','stress.nv12'],cwd=build,env=env,stdout=log,stderr=log)
            e=dict(env,VKR_CHECK_WIDTH=str(160+(cycle%2)*14),VKR_CHECK_HEIGHT=str(90+(cycle%2)*8))
            # Crash a consumer without flClose while renderer may own pending writes.
            log=(a.output/f'crashed-consumer-{cycle}.log').open('w');logs.append(log)
            checker=subprocess.Popen([str(build/'vkr_portable_output_check'),'stress'],env=dict(e,VKR_CHECK_FRAMES='1000000'),stdout=log,stderr=log)
            time.sleep(1.5)
            if checker.poll() is not None: raise RuntimeError('consumer exited before injected kill')
            checker.kill();checker.wait()
            # New owner changes geometry; verify both outputs after recovery.
            e['VKR_CHECK_WIDTH']=str(192+(cycle%2)*14)
            e['VKR_CHECK_HEIGHT']=str(108+(cycle%2)*8)
            log=(a.output/f'verify-{cycle}.log').open('w');logs.append(log)
            checker=subprocess.Popen([str(build/'vkr_portable_output_check'),'stress'],env=e,stdout=log,stderr=log)
            if cycle % 4 == 3:
                # Keep the consumer/channel alive across abrupt producer death.
                time.sleep(1)
                if checker.poll() is not None: raise RuntimeError('checker ended before producer crash')
                renderer.kill();renderer.wait()
                log=(a.output/f'renderer-restart-{cycle}.log').open('w');logs.append(log)
                renderer=subprocess.Popen([str(build/'vkrenderer'),'--wayland','stress-wayland','--headless','-1','--scene-output','stress.rgba','--scene-output','stress.nv12'],cwd=build,env=env,stdout=log,stderr=log)
            code=checker.wait(timeout=35)
            if code: raise RuntimeError(f'cycle {cycle}: checker exit {code}')
            if renderer.poll() is not None: raise RuntimeError('renderer died')
            result=dict(cycle=cycle,exit=code,**resources(renderer))
            results.append(result);print(json.dumps(result),flush=True)
        (a.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    finally:
        stop(checker);stop(renderer)
        for log in logs:log.close()
