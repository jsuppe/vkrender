#!/usr/bin/env python3
"""Windows native AV resource preparation ordering, fault and cancellation gate.
Needs matching FrameLink sample/plugin binaries and an interactive desktop.
No physical scanout claim. Leaves all failed logs intact.
"""
import argparse,csv,json,os,pathlib,socket,subprocess,time
p=argparse.ArgumentParser();p.add_argument('--build',required=True);p.add_argument('--renderer',required=True);p.add_argument('--output',required=True);p.add_argument('--case');p.add_argument('--headless',action='store_true');a=p.parse_args()
b=pathlib.Path(a.build).resolve();r=pathlib.Path(a.renderer).resolve();root=pathlib.Path(a.output).resolve();root.mkdir(parents=True,exist_ok=True)
results=[]
for name,extra,timeout in [('prepared',{'VKR_TEST_AV_PREPARE_DELAY_MS':'250'},15000),('lazy-negative',{'VKR_TEST_AV_PREPARE_DELAY_MS':'250','VKR_TEST_AV_LAZY_PREPARATION':'1'},15000),('failure',{'VKR_TEST_AV_PREPARE_FAIL':'1'},4000),('timeout',{'VKR_TEST_AV_PREPARE_DELAY_MS':'5000'},4000)]:
 if a.case and a.case!=name:continue
 d=root/name;d.mkdir(exist_ok=True);env=dict(os.environ)
 for key in ['VKR_TEST_AV_PREPARE_DELAY_MS','VKR_TEST_AV_LAZY_PREPARATION','VKR_TEST_AV_PREPARE_FAIL']:env.pop(key,None)
 env.update(extra,VKR_TRACE=str(d/'renderer'),FRAMELINK_TRACE=str(d/'transport'),FRAMELINK_GST_TRACE=str(d/'gst'),VKR_TRACE_EVENTS='262144',FRAMELINK_TRACE_EVENTS='262144',FRAMELINK_GST_TRACE_EVENTS='262144')
 with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
 pipeline=d/'source.pipeline';pipeline.write_text('videotestsrc num-buffers=180 pattern=ball ! @VIDEO_SINK@ audiotestsrc num-buffers=180 samplesperbuffer=800 volume=0.0 ! @AUDIO_SINK@')
 rc=[str(r),'--serve','prepare-'+name,'--control-port',str(port),'--av-exit-on-end','--av-session',str(d/'session'),'--av-plugin',str(b/'gstframelink.dll'),'--av-format','NV12','--av-width','1920','--av-height','1080','--av-fps','60','--av-audio-sink','autoaudiosink','--av-clock','producer','--av-timeout-ms',str(timeout)]
 if a.headless:rc+=['--headless','-1']
 pc=[str(b/'framelink_av_producer.exe'),str(b/'gstframelink.dll'),'--session',str(d/'session'),'--format','NV12','--width','1920','--height','1080','--fps','60','--frames','180','--input-pipeline',str(pipeline),'--timeout-ms',str(timeout)]
 started=time.monotonic();children=[]
 with (d/'producer.log').open('w') as po,(d/'renderer.log').open('w') as ro:
  try:
   consumer=subprocess.Popen(rc,stdout=ro,stderr=subprocess.STDOUT,env=env);children.append(consumer);time.sleep(.3)
   producer=subprocess.Popen(pc,stdout=po,stderr=subprocess.STDOUT,env=env);children.append(producer)
   codes=[producer.wait(timeout=timeout/1000+5),consumer.wait(timeout=5)]
  finally:
   for child in children:
    if child.poll() is None:child.terminate();child.wait(timeout=3)
 elapsed=time.monotonic()-started;rows=[]
 for file in d.glob('*.csv'):
  rows.extend(csv.DictReader(line for line in file.open() if not line.startswith('#')))
 received={int(x['pts_ns']) for x in rows if x['operation']=='gst.frame_receive'}
 presented={int(x['pts_ns']) for x in rows if x['operation']==('av.frame_gpu_complete' if a.headless else 'av.frame_present_submit') and int(x['result']) in (0,1000001003)}
 prepared=[x for x in rows if x['operation']=='service.native_preparation']
 source_start=min([int(x['start_ns']) for x in rows if x['operation']=='gst.frame_receive'],default=0)
 if name=='prepared':
  assert codes==[0,0],codes
  assert presented==received=={i*1000000000//60 for i in range(180)},(len(received),len(presented))
  assert prepared and max(int(x['start_ns'])+int(x['duration_ns']) for x in prepared)<source_start,'media preceded resource preparation'
 elif name=='lazy-negative':
  assert received and len(presented)<len(received),'negative control did not expose first-use loss'
 else:
  assert codes==[1,1],('expected coherent failure, not crash',codes)
  assert not received,'failed preparation released timed video'
  assert 'startup-released' not in (d/'producer.log').read_text(),'failed preparation released GO'
  assert elapsed<timeout/1000+5,elapsed
 result=dict(case=name,terminal_stage='gpu_complete' if a.headless else 'present_submit',codes=codes,elapsed_s=elapsed,source=len(received),accepted_unique=len(presented),commands=[pc,rc]);results.append(result)
 (root/'summary.json').write_text(json.dumps(results,indent=2));print(json.dumps(result),flush=True)
