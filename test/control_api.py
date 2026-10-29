#!/usr/bin/env python3
import json, subprocess, sys, socket, time, urllib.request, urllib.error
with socket.socket() as sock:
    sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
p=subprocess.Popen([sys.argv[1],str(port)])
def request(path, post=False):
    try:
        with urllib.request.urlopen(urllib.request.Request(f'http://127.0.0.1:{port}/api/'+path,method='POST' if post else 'GET'),timeout=2) as r:return r.status,r.read()
    except urllib.error.HTTPError as e:return e.code,e.read()
try:
    for _ in range(100):
        try:
            if request('status')[0]==200:break
        except urllib.error.URLError:pass
        time.sleep(.02)
    else:raise RuntimeError('server unavailable')
    caps=json.loads(request('capabilities')[1]);assert caps==dict(camera=False,preview=False,displays=False,maxSlots=12),caps
    for path in ['layout?name=bogus','pause?on=maybe','config?max_visible=abc','config?max_visible=13','config?rotate_seconds=nan','config?rotate_seconds=inf','config','stream?slot=garbage&action=hide','stream?slot=12&action=hide','stream?slot=1&action=hide','stream?slot=0&action=bogus']:
        assert request(path,True)[0]==400,path
    for path in ['camera?on=1','display?id=0&on=1']:
        assert request(path,True)[0]==501,path
    assert request('preview.jpg')[0]==501
    assert request('pause?on=1')[0]==405
    for path in ['layout?name=presentation','pause?on=1','config?max_visible=2&rotate_seconds=3','stream?slot=0&action=hide','stream?action=unspotlight']:
        assert request(path,True)[0]==200,path
    time.sleep(.05)
    s=json.loads(request('status')[1]);assert s['layout']=='presentation' and s['paused'] and s['maxVisible']==2 and s['rotateSeconds']==3,s
    print('control HTTP validation and shared moderation passed')
finally:
    p.terminate();p.wait(timeout=5)
