#!/usr/bin/env python3
"""Concurrent preview limit, command responsiveness and lease recovery."""
import concurrent.futures, socket, subprocess, sys, threading, time, urllib.request, urllib.error
with socket.socket() as s:
    s.bind(('127.0.0.1',0));port=s.getsockname()[1]
base=f'http://127.0.0.1:{port}/api/'
p=subprocess.Popen([sys.argv[1],str(port),'preview'])
held=[]
def get(path):return urllib.request.urlopen(base+path,timeout=2)
try:
    for _ in range(100):
        try:
            with get('status') as r:assert r.status==200
            break
        except urllib.error.URLError:time.sleep(.02)
    else:raise AssertionError('server not ready')
    for batch in range(8):
        gate=threading.Barrier(8)
        def connect(_):
            gate.wait()
            try:return get('preview.mjpg')
            except urllib.error.HTTPError as e:
                code=e.code;e.close();return code
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results=list(pool.map(connect,range(8)))
        held=[r for r in results if not isinstance(r,int)]
        assert len(held)==3, f'admitted {len(held)} previews'
        assert all(r==503 for r in results if isinstance(r,int)), results
        # Saturated stream leases must leave workers available for commands.
        with get('status') as r:assert r.status==200
        with urllib.request.urlopen(urllib.request.Request(base+'pause?on=1',method='POST'),timeout=2) as r:assert r.status==200
        for r in held:r.close()
        held.clear()
        # Give sockets/handlers time to detect FIN and return their leases.
        time.sleep(.15)
    print('PASS 8 concurrent admission batches: exactly3 streams,5 rejections, responsive commands, leases reclaimed')
finally:
    for r in held:r.close()
    p.terminate();p.wait(timeout=5)
