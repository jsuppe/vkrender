#!/usr/bin/env python3
"""Bounded native renderer A/V integration smoke; not physical lip-sync measurement."""
import argparse,json,os,socket,subprocess,tempfile,time,urllib.request,urllib.error,urllib.parse
p=argparse.ArgumentParser();p.add_argument('renderer');p.add_argument('producer');p.add_argument('plugin')
p.add_argument('--format',choices=['BGRA','NV12'],default='BGRA');p.add_argument('--audio-sink',default='fakesink');p.add_argument('--audio-master',action='store_true')
a=p.parse_args();root=tempfile.mkdtemp(prefix='vkr-av-render-');os.chmod(root,0o700)
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
env=dict(os.environ,XDG_RUNTIME_DIR=root)
original_runtime=os.environ.get('XDG_RUNTIME_DIR','')
if original_runtime and os.path.exists(original_runtime+'/pulse/native'):
 env.setdefault('PULSE_SERVER','unix:'+original_runtime+'/pulse/native')
session=root+'/session';samples=[];procs=[]
url='http://127.0.0.1:'+str(port)
def post(path):
 return urllib.request.urlopen(urllib.request.Request(url+path,method='POST'),timeout=1).status
try:
 with open(root+'/producer.log','w') as pl,open(root+'/renderer.log','w') as rl:
  procs.append(subprocess.Popen([a.producer,a.plugin,'--session',session,'--format',a.format,'--frames','300','--timeout-ms','15000'],stdout=pl,stderr=pl,env=env))
  mode='--serve' if os.name=='nt' else '--wayland'
  procs.append(subprocess.Popen([a.renderer,mode,'av-smoke','--headless','-1','--av-exit-on-end','--control-port',str(port),'--av-session',session,'--av-plugin',a.plugin,'--av-format',a.format,'--av-audio-sink',a.audio_sink],stdout=rl,stderr=rl,env=env,cwd=root))
  deadline=time.monotonic()+25;muted=False;unmuted=False;selected=False;old=None
  while time.monotonic()<deadline and procs[1].poll() is None:
   time.sleep(.1)
   try:
    with urllib.request.urlopen(url+'/api/status',timeout=1) as r:sample=json.load(r)
   except (urllib.error.URLError,TimeoutError,ConnectionResetError):continue
   samples.append(sample);av=sample.get('av',{})
   if av.get('due_decisions',0)>20 and not muted:
    assert post('/api/audio?on=1')==200;muted=True
    old=procs[0]
    procs[0]=subprocess.Popen([a.producer,a.plugin,'--session',session+'-second','--format',a.format,'--frames','300','--timeout-ms','15000'],stdout=pl,stderr=pl,env=env)
    assert post('/api/audio/select?'+urllib.parse.urlencode({'session':session+'-second'}))==200;selected=True
   if muted and av.get('muted') and not unmuted:
    assert post('/api/audio?on=0')==200;unmuted=True
  for proc in procs:proc.wait(timeout=5)
  assert all(proc.returncode==0 for proc in procs),[proc.returncode for proc in procs]
  assert samples and any(s['av']['due_decisions']>20 for s in samples)
  assert all(s['av']['max_queue_depth']<=6 for s in samples)
  assert all(s['av']['invalid_pts']==0 for s in samples)
  assert muted and unmuted and selected
  assert any(s['av'].get('session_resets',0)>=3 and s['av']['active'] for s in samples)
  assert any(s['av']['audio_master']==a.audio_master and s['av']['due_decisions']>20 for s in samples)
  print('PASS native renderer selected-session switch, format='+a.format+', evidence='+root)
finally:
 if old is not None:procs.append(old)
 for proc in procs:
  if proc.poll() is None:proc.terminate();proc.wait(timeout=5)
 with open(root+'/status.json','w') as f:json.dump(samples,f,indent=2)
