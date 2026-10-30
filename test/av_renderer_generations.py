#!/usr/bin/env python3
"""Repeated authenticated native ring generation stress in one renderer process."""
import argparse,json,os,re,socket,subprocess,tempfile,time,urllib.request,urllib.error,urllib.parse
p=argparse.ArgumentParser();p.add_argument('renderer');p.add_argument('producer');p.add_argument('plugin')
p.add_argument('--format',choices=['BGRA','NV12'],default='BGRA');p.add_argument('--audio-sink',default='fakesink');p.add_argument('--audio-master',action='store_true')
p.add_argument('--switches',type=int,default=5);p.add_argument('--gpu-gate-ms',type=int,default=0)
a=p.parse_args();assert 1<=a.switches<=20;root=tempfile.mkdtemp(prefix='vkr-av-render-');os.chmod(root,0o700)
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
env=dict(os.environ,XDG_RUNTIME_DIR=root)
if a.gpu_gate_ms:
 assert 1<=a.gpu_gate_ms<=1000
 env['VKR_TEST_GPU_GATE_MS']=str(a.gpu_gate_ms)
original_runtime=os.environ.get('XDG_RUNTIME_DIR','')
if original_runtime and os.path.exists(original_runtime+'/pulse/native'):
 env.setdefault('PULSE_SERVER','unix:'+original_runtime+'/pulse/native')
session=root+'/session';samples=[];procs=[];retired=[];old=None
url='http://127.0.0.1:'+str(port)
def post(path):
 return urllib.request.urlopen(urllib.request.Request(url+path,method='POST'),timeout=1).status
try:
 with open(root+'/producer.log','w') as pl,open(root+'/renderer.log','w') as rl:
  procs.append(subprocess.Popen([a.producer,a.plugin,'--session',session,'--format',a.format,'--frames','300','--timeout-ms','15000'],stdout=pl,stderr=pl,env=env))
  mode='--serve' if os.name=='nt' else '--wayland'
  procs.append(subprocess.Popen([a.renderer,mode,'av-smoke','--headless','-1','--av-exit-on-end','--control-port',str(port),'--av-session',session,'--av-plugin',a.plugin,'--av-format',a.format,'--av-audio-sink',a.audio_sink],stdout=rl,stderr=rl,env=env,cwd=root))
  deadline=time.monotonic()+15*(a.switches+1);muted=False;unmuted=False;selected=False;switches=0;last_due=0
  while time.monotonic()<deadline and procs[1].poll() is None:
   time.sleep(.1)
   try:
    with urllib.request.urlopen(url+'/api/status',timeout=1) as r:sample=json.load(r)
   except (urllib.error.URLError,TimeoutError,ConnectionResetError):continue
   samples.append(sample);av=sample.get('av',{})
   if av.get('due_decisions',0)>last_due+20 and switches<a.switches and av.get('active'):
    assert post('/api/audio?on=1')==200;muted=True
    old=procs[0];retired.append(old);switches+=1;last_due=av['due_decisions'];unmuted=False
    procs[0]=subprocess.Popen([a.producer,a.plugin,'--session',session+'-generation-'+str(switches),'--format',a.format,'--frames','300','--timeout-ms','15000'],stdout=pl,stderr=pl,env=env)
    assert post('/api/audio/select?'+urllib.parse.urlencode({'session':session+'-generation-'+str(switches)}))==200;selected=True
   if muted and av.get('muted') and not unmuted:
    assert post('/api/audio?on=0')==200;unmuted=True
  for proc in procs:proc.wait(timeout=5)
  assert all(proc.returncode==0 for proc in procs),[proc.returncode for proc in procs]
  assert samples and any(s['av']['due_decisions']>20 for s in samples)
  assert all(s['av']['max_queue_depth']<=6 for s in samples)
  assert all(s['av']['invalid_pts']==0 for s in samples)
  assert muted and unmuted and selected and switches==a.switches, switches
  assert any(s['av'].get('session_resets',0)>=2*a.switches+1 and s['av']['active'] for s in samples)
  assert any(s['av']['audio_master']==a.audio_master and s['av']['due_decisions']>20 for s in samples)
  if a.gpu_gate_ms:
   log=open(root+'/renderer.log').read()
   assert 'validation layer enabled' in log, 'GPU gate qualification requires VK_LAYER_KHRONOS_validation'
   gates=re.findall(r'gpu-test-gate: pre-release fence=(\d+) counter=(\d+) target=(\d+)',log)
   assert len(gates)>=a.switches, gates
   assert all(f=='1' and int(c)<int(t) for f,c,t in gates), gates
   assert 'Validation Error' not in log and 'VUID-' not in log, log
  print('PASS native renderer '+str(switches)+' generation switches, format='+a.format+', evidence='+root)
finally:
 procs.extend(retired)
 for proc in procs:
  if proc.poll() is None:proc.terminate();proc.wait(timeout=5)
 with open(root+'/status.json','w') as f:json.dump(samples,f,indent=2)
