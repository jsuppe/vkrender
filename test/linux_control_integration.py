#!/usr/bin/env python3
"""GPU-backed Linux control API; explicit isolated HTTP port/runtime."""
import json, os, pathlib, socket, subprocess, sys, tempfile, time, urllib.request, urllib.error
build=pathlib.Path(sys.argv[1]).resolve();producer=pathlib.Path(sys.argv[2]).resolve();client=pathlib.Path(sys.argv[3]).resolve()
with socket.socket() as sock:
    sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
def request(path,post=False):
    try:
        with urllib.request.urlopen(urllib.request.Request(f'http://127.0.0.1:{port}/api/'+path,method='POST' if post else 'GET'),timeout=4) as r:return r.status,r.read()
    except urllib.error.HTTPError as e:return e.code,e.read()
def until(fn):
    stop=time.monotonic()+8
    while time.monotonic()<stop:
        try:
            result=fn()
            if result:return result
        except urllib.error.URLError:pass
        time.sleep(.04)
    raise AssertionError('deadline')
def roster():return json.loads(request('streams')[1])
with tempfile.TemporaryDirectory(prefix='vkr-control-') as runtime, (build/'control-integration.log').open('w') as log:
    env=dict(os.environ,XDG_RUNTIME_DIR=runtime,WAYLAND_DISPLAY='control-test',VKR_LAYOUT='wall')
    children=[]
    try:
        render=subprocess.Popen([str(build/'vkrenderer'),'--wayland','control-test','--headless','-1','--window','640x360+0+0','--control-port',str(port)],env=env,stdout=log,stderr=log);children.append(render)
        until(lambda:request('status')[0]==200)
        source=subprocess.Popen([str(producer),'--channel','control-test.in.0','--frames','240','--fps','60','--size','128x72'],env=env,stdout=log,stderr=log);children.append(source)
        wayland=subprocess.Popen([str(client),'12','bgra'],env=env,stdout=log,stderr=log);children.append(wayland)
        until(lambda:len([s for s in roster() if s['connected']])>=2)
        assert request('layout?name=presentation',True)[0]==200
        for action in ['hide','pin','show','spotlight','unspotlight','front']:
            assert request('stream?slot=0&action='+action,True)[0]==200
        assert request('stream?slot=0&action=hide',True)[0]==200
        until(lambda:any(s['slot']==0 and s['hidden'] and not s['visible'] for s in roster()))
        assert source.wait(timeout=10)==0,'hidden producer stalled'
        assert request('pause?on=1',True)[0]==200
        until(lambda:json.loads(request('status')[1])['paused'])
        until(lambda:request('preview.jpg')[0]==200)
        assert request('preview.jpg')[1][:2]==b'\xff\xd8'
        with urllib.request.urlopen(f'http://127.0.0.1:{port}/api/preview.mjpg',timeout=4) as stream:
            assert b'Content-Type: image/jpeg' in stream.read(100)
        assert request('camera?on=1',True)[0]==501
        wl=next(s['slot'] for s in roster() if s['slot']>=12 and s['connected'])
        assert request(f'stream?slot={wl}&action=kick',True)[0]==200
        until(lambda:not any(s['slot']==wl and s['connected'] for s in roster()))
        assert render.poll() is None
        print('PASS Linux control: dual transports, hidden producer progress, actions, preview, kick and capabilities')
    finally:
        for p in reversed(children):
            if p.poll() is None:p.terminate()
        for p in children:
            try:p.wait(timeout=5)
            except subprocess.TimeoutExpired:p.kill();p.wait()
