#!/usr/bin/env python3
"""A real short socket write must terminate multipart output and reclaim lease."""
import pathlib, socket, subprocess, sys, tempfile, time, urllib.request, urllib.error
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
base=f'http://127.0.0.1:{port}/api/'
with tempfile.TemporaryFile(mode='w+') as log:
    p=subprocess.Popen([sys.argv[1],str(port),'preview'],stderr=log)
    try:
        for _ in range(100):
            try:
                with urllib.request.urlopen(base+'status',timeout=1) as r:assert r.status==200
                break
            except urllib.error.URLError:time.sleep(.02)
        else:raise AssertionError('server not ready')
        for _ in range(8):
            with urllib.request.urlopen(base+'preview.mjpg',timeout=2) as r:
                body=r.read() # Must reach EOF; continuing after a short write times out.
            head,jpeg=body.split(b'\r\n\r\n',1)
            expected=int(next(line.split(b':')[1] for line in head.split(b'\r\n') if line.startswith(b'Content-Length:')))
            assert len(jpeg)==expected//2 and jpeg.startswith(b'\xff\xd8'), (len(jpeg),expected)
            with urllib.request.urlopen(base+'status',timeout=1) as r:assert r.status==200
    finally:
        p.terminate();p.wait(timeout=5)
    log.seek(0);text=log.read();assert text.count('injected short JPEG write:')==8,text
print('PASS eight injected partial JPEG writes terminate streams and reclaim admission')
