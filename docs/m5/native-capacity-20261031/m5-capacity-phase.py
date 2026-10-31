import csv,pathlib,json,sys,bisect,statistics
for arg in sys.argv[1:]:
 case=pathlib.Path(arg);stage={};ticks=[]
 for p in case.glob('*.csv'):
  for r in csv.DictReader(x for x in p.open() if not x.startswith('#')):
   op=r['operation'];t=int(r['start_ns']);pts=int(r['pts_ns'])
   if op=='scene.acquire_swapchain':ticks.append(t+int(r['duration_ns']))
   if op in ('gst.frame_receive','flEndSubmit','ipc.frame_received','av.frame_adopt','av.frame_present_submit') and pts>=0:
    if op=='av.frame_present_submit' and int(r['result']) not in (0,1000001003):continue
    if op=='flEndSubmit':t+=int(r['duration_ns'])
    stage.setdefault(op,{}).setdefault(pts,t)
 ticks.sort();out={}
 def stats(a):
  a=sorted(x/1e6 for x in a);return dict(n=len(a),min=min(a),p50=statistics.median(a),p99=a[int(.99*(len(a)-1))],max=max(a)) if a else {}
 for a,b in [('gst.frame_receive','flEndSubmit'),('flEndSubmit','ipc.frame_received'),('ipc.frame_received','av.frame_adopt'),('gst.frame_receive','av.frame_present_submit')]:
  out[a+'->'+b+'_ms']=stats([stage[b][pts]-t for pts,t in stage.get(a,{}).items() if 1000000000<=pts<44000000000 and pts in stage.get(b,{})])
 delays=[]
 for pts,t in stage.get('flEndSubmit',{}).items():
  if not 1000000000<=pts<44000000000:continue
  i=bisect.bisect_left(ticks,t)
  if i<len(ticks):delays.append(ticks[i]-t)
 out['producer_end_to_next_swapchain_acquire_observation_ms']=stats(delays)
 (case/'phase-summary.json').write_text(json.dumps(out,indent=2));print(case.name,json.dumps(out))
