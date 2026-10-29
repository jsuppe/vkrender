#!/usr/bin/env python3
"""Headless preview integration under Vulkan synchronization validation.
Requires the Khronos validation layer; explicitly reports unsupported hosts.
"""
import ctypes, os, pathlib, socket, subprocess, sys, tempfile, time, urllib.request, urllib.error
class Layer(ctypes.Structure):
    _fields_=[('name',ctypes.c_char*256),('spec',ctypes.c_uint32),('impl',ctypes.c_uint32),('description',ctypes.c_char*256)]
try:
    vk=ctypes.CDLL('libvulkan.so.1');count=ctypes.c_uint32()
    assert vk.vkEnumerateInstanceLayerProperties(ctypes.byref(count),None)==0
    layers=(Layer*count.value)()
    assert vk.vkEnumerateInstanceLayerProperties(ctypes.byref(count),layers)==0
    if not any(x.name==b'VK_LAYER_KHRONOS_validation' for x in layers):raise RuntimeError('Khronos validation unavailable')
except (OSError,RuntimeError) as e:
    print('SKIP:',e);sys.exit(77)
build=pathlib.Path(sys.argv[1]).resolve()
log_path=pathlib.Path(sys.argv[2]).resolve() if len(sys.argv)>2 else build/'preview-sync-validation.log'
with socket.socket() as sock:
    sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
with tempfile.TemporaryDirectory(prefix='vkr-preview-sync-') as runtime, log_path.open('w') as log:
    env=dict(os.environ,XDG_RUNTIME_DIR=runtime,VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',
        VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT',
        VK_LAYER_KHRONOS_VALIDATION_VALIDATE_SYNC='true')
    p=subprocess.Popen([str(build/'vkrenderer'),'--wayland','preview-sync','--headless','-1',
        '--window','640x360+0+0','--control-port',str(port)],env=env,stdout=log,stderr=log)
    count=0
    try:
        deadline=time.monotonic()+15
        while time.monotonic()<deadline and count<12:
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{port}/api/preview.jpg',timeout=2) as response:
                    data=response.read();assert data.startswith(b'\xff\xd8') and data.endswith(b'\xff\xd9')
                    count+=1
            except (urllib.error.URLError,ConnectionError):
                pass
            assert p.poll() is None, 'renderer exited'
            time.sleep(.45)
        assert count==12,f'only {count} previews'
    finally:
        p.terminate()
        try:p.wait(timeout=5)
        except subprocess.TimeoutExpired:p.kill();p.wait()
text=log_path.read_text(errors='replace')
assert 'SYNC-HAZARD' not in text and 'Validation Error' not in text,text[-10000:]
print(f'PASS {count} headless previews under requested synchronization validation; log={log_path}')
