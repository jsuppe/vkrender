import pathlib,csv,json,subprocess,collections
root=pathlib.Path('/tmp/m5-present-causal/fixed-native');summary=[]
for d in sorted(root.glob('1920*')):
 rows=[];files=list(d.glob('*.csv'))
 for f in files:rows.extend(csv.DictReader(x for x in f.open() if not x.startswith('#')))
 channel=next(r['channel_id'] for r in rows if r['operation']=='gst.frame_receive');n=2700
 manifest=d/'expected-source.jsonl';manifest.write_text(''.join(json.dumps(dict(session=channel,generation='2',frame_id=str(i*1000000000//60),pts_ns=i*1000000000//60))+'\n' for i in range(n)))
 subprocess.run(['python3','/tmp/framelink-m5/tools/frame_accounting.py','--csv','--generation','2','--expected-manifest',str(manifest),'--require-complete','--output',str(d/'accounting.json'),*[str(f) for f in files]],check=True,stdout=subprocess.DEVNULL)
 j=json.loads((d/'accounting.json').read_text());t0=min(int(r['start_ns']) for r in rows if r['operation']=='gst.frame_receive');source=sorted(int(r['start_ns']) for r in rows if r['operation']=='gst.frame_receive')
 stages=j['unique_by_stage'];ops={op:max([int(r['duration_ns'])/1e6 for r in rows if r['operation']==op],default=0) for op in ['scene.present_call_cpu','scene.queue_submit_cpu','service.input_cpu_map','d3d11.signal_ready']}
 unique={}
 for r in rows:
  if r['operation']=='av.frame_present_submit' and int(r['result']) in (0,1000001003):unique.setdefault(int(r['pts_ns']),int(r['start_ns']))
 ts=sorted(unique.values());result=dict(case=d.name,unique_by_stage=stages,phases=j['phases'],source_fps=(len(source)-1)*1e9/(source[-1]-source[0]),present_unique_fps=(len(ts)-1)*1e9/(ts[-1]-ts[0]),max_present_unique_gap_ms=max((b-a)/1e6 for a,b in zip(ts,ts[1:])),max_call_ms=ops,power_requests=[dict(relative_source_ms=(int(r['start_ns'])-t0)/1e6,duration_ms=int(r['duration_ns'])/1e6,result=int(r['result'])) for r in rows if r['operation']=='service.power_request']);summary.append(result);print(json.dumps(result))
(root/'verified-summary.json').write_text(json.dumps(summary,indent=2))
