import argparse,collections,csv,json,pathlib,re,subprocess,sys
p=argparse.ArgumentParser();p.add_argument('root',type=pathlib.Path);p.add_argument('--seconds',type=int,default=70);p.add_argument('--accounting-tool',type=pathlib.Path,default=pathlib.Path('/tmp/framelink-m5/tools/frame_accounting.py'));a=p.parse_args();n=a.seconds*60
summary=json.loads((a.root/'summary.json').read_text());assert len(summary)==1 and summary[0]['exit_codes']==[0,0],summary
case=summary[0];d=a.root/case['case'];plog=(d/'producer.log').read_text();rlog=(d/'renderer.log').read_text()
jsonrows=lambda text:[json.loads(line) for line in text.splitlines() if line.startswith('{')]
pj=jsonrows(plog);rj=jsonrows(rlog)
def event(rows,name):return next(x for x in rows if x.get('event')==name)
transport=event(pj,'video-transport-summary');assert transport==dict(event='video-transport-summary',received=n,submitted=n,dropped=0,ring_full_drops=0),transport
complete=event(rj,'playback-complete');timing=event(rj,'playback-timing');metrics=event(rj,'playback-metrics')
assert complete['audio_blocks']==n and complete['audio_samples']==a.seconds*48000 and complete['latency_ns']==100000000,complete
assert timing['audio_first_running_pts_ns']==0 and timing['audio_end_ns']==timing['video_end_ns']==a.seconds*10**9,timing
assert metrics['late_audio_blocks']==0,metrics
assert 'AV_RING_CONFIG ring_slots=12 pending_slots=6 common_latency_ns=100000000' in rlog
assert re.search(rf'AV_RENDER due_decisions={n} .*late_drops=0 invalid_pts=0 status=ended',rlog),rlog[-2500:]
status=json.loads((d/'status.json').read_text());av=[x['status']['av'] for x in status if 'av' in x['status']]
for key in ('late_drops','sequence_gaps','import_failures','invalid_pts','late_audio_blocks'):
 assert max([x.get(key,0) for x in av],default=0)==0,(key,max(x.get(key,0) for x in av))
assert max(x['status']['presentation_submission']['errors'] for x in status)==0
assert max(x.get('max_queue_depth',0) for x in av)<=6
assert max(x.get('audio_peak_queue_buffers',0) for x in av)<=4
# Full activity interval, trimmed by fixed first/last one-second startup/drain,
# never selected for a favorable subwindow. Off cannot prove unique present IDs.
active=[x for x in status if x.get('status',{}).get('av',{}).get('due_decisions',0)>0 and x['status']['av'].get('active')]
assert active[-1]['t']-active[0]['t']-2>=60,('short aggregate interval',active[-1]['t']-active[0]['t'])
cpu={}
for key in ('producer_cpu_s','renderer_cpu_s'):
 samples=[x for x in status if x.get(key) is not None and active[0]['t']+1<=x['t']<=active[-1]['t']-1]
 cpu[key]=(samples[-1][key]-samples[0][key])/(samples[-1]['t']-samples[0]['t'])*100
out=dict(present_errors=0,peak_pending_frames=max(x.get('max_queue_depth',0) for x in av),aggregate_steady_interval_s=active[-1]['t']-active[0]['t']-2,case=case,aggregate_transport=transport,playback_complete=complete,playback_timing=timing,audio_metrics=metrics,steady_cpu_percent_one_core=cpu,aggregate_active_interval_s=active[-1]['t']-active[0]['t'],sampled_schedule_error_ms=dict(min=min(x.get('schedule_error_ns',0) for x in av)/1e6,max=max(x.get('schedule_error_ns',0) for x in av)/1e6))
files=sorted(d.glob('*.csv'))
if case['trace']=='off':assert not files;out['identity_proof']=False
else:
 rows=[];footers=[]
 for f in files:
  lines=f.read_text().splitlines();footer=lines[-1]
  assert footer.startswith('# dropped_events=0,rejected_threads=0,'),(f,footer)
  file_rows=list(csv.DictReader(line for line in lines if not line.startswith('#')))
  counts=collections.Counter(r['thread'] for r in file_rows)
  footers.append(dict(file=f.name,footer=footer,max_events_per_thread=max(counts.values(),default=0)))
  rows.extend(file_rows)
 channel=next(r['channel_id'] for r in rows if r['operation']=='gst.frame_receive')
 manifest=d/'expected-source.jsonl';manifest.write_text(''.join(json.dumps(dict(session=channel,generation='2',frame_id=str(i*10**9//60),pts_ns=i*10**9//60))+'\n' for i in range(n)))
 run=subprocess.run([sys.executable,str(a.accounting_tool),'--csv','--generation','2','--expected-manifest',str(manifest),'--require-complete','--output',str(d/'accounting.json'),*map(str,files)],capture_output=True,text=True)
 (d/'accounting-command.log').write_text(run.stdout+run.stderr)
 accounting=json.loads((d/'accounting.json').read_text());out['accounting']=accounting
 assert run.returncode==0,('identity gate',run.stdout[-2000:],run.stderr)
 maps={}
 for name,op in [('source','gst.frame_receive'),('present','av.frame_present_submit'),('gpu','av.frame_gpu_complete')]:
  times={}
  for r in rows:
   if r['operation']==op and (name!='present' or int(r['result']) in (0,1000001003)):
    pts=int(r['pts_ns']);t=int(r['start_ns']);times[pts]=min(t,times.get(pts,t))
  assert set(times)=={i*10**9//60 for i in range(n)},(name,len(times))
  maps[name]=times
 def stats(values):
  values=sorted(values);return dict(min=values[0],median=values[len(values)//2],p99=values[int((len(values)-1)*.99)],max=values[-1])
 out['cadence']={}
 for name,times in maps.items():
  out['cadence'][name]={}
  for phase,lo,hi in [('all',0,a.seconds*10**9),('startup',0,10**9),('steady',10**9,(a.seconds-1)*10**9),('drain',(a.seconds-1)*10**9,a.seconds*10**9)]:
   values=sorted(t for pts,t in times.items() if lo<=pts<hi);span=(values[-1]-values[0])/1e9
   out['cadence'][name][phase]=dict(count=len(values),measured_interval_s=span,fps=(len(values)-1)/span,gap_ms=stats([(y-x)/1e6 for x,y in zip(values,values[1:])]))
   if phase=='steady':
    assert span>=60,(name,span)
    assert 59.4<=(len(values)-1)/span<=60.6,(name,span,'not requested rate')
 out['source_to_stage_ms']={name:stats([(t-maps['source'][pts])/1e6 for pts,t in maps[name].items()]) for name in ('present','gpu')}
 out['trace_footers']=footers;out['identity_proof']=True
 out['call_max_ms']={op:max((int(r['duration_ns'])/1e6 for r in rows if r['operation']==op),default=0) for op in ('scene.present_call_cpu','scene.queue_submit_cpu','service.input_cpu_map','d3d11.signal_ready')}
(a.root/'verified-summary.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps({k:v for k,v in out.items() if k!='accounting'},indent=2))
