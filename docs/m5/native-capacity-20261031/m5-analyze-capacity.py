import pathlib,csv,json,subprocess,sys,statistics
root=pathlib.Path(sys.argv[1]);results=[]
for case in sorted(root.glob('1920*')):
 if not case.is_dir():continue
 files=list(case.glob('*.csv'));fps=int(case.name.split('-')[1]);count=fps*45
 sink=[];present={};generation=None;channel=None
 for p in files:
  for r in csv.DictReader(x for x in p.open() if not x.startswith('#')):
   if r['operation']=='gst.frame_receive':channel=r['channel_id']
   if r['operation']=='av.frame_present_submit' and int(r['result']) in (0,1000001003):
    generation=r['slot'];pts=int(r['pts_ns']);present[pts]=min(present.get(pts,2**64),int(r['start_ns']))
 if not channel or generation is None:print('MISSINGIDENTITIES',case);continue
 manifest=case/'expected-source.jsonl';manifest.write_text(''.join(json.dumps(dict(session=channel,generation=generation,frame_id=str(i*1000000000//fps),pts_ns=i*1000000000//fps))+'\n' for i in range(count)))
 subprocess.run(['python3','/tmp/framelink-m5-frame-accounting/tools/frame_accounting.py','--csv','--generation',generation,'--expected-manifest',str(manifest),'--require-complete','--output',str(case/'accounting-with-manifest.json'),*[str(p) for p in files]],check=True,stdout=subprocess.DEVNULL)
 report=json.loads((case/'accounting-with-manifest.json').read_text());times=sorted(present.values());gaps=[(b-a)/1e6 for a,b in zip(times,times[1:])]
 steady=sorted(t for pts,t in present.items() if 1000000000<=pts<44000000000);rate=(len(steady)-1)*1e9/(steady[-1]-steady[0]) if len(steady)>1 else 0
 steady_gaps=[(b-a)/1e6 for a,b in zip(steady,steady[1:])]
 result=dict(steady_present_gap_ms_max=max(steady_gaps),case=case.name,expected=count,accepted_unique=len(present),steady_rate_fps=rate,first_present_gap_ms_max=max(gaps),first_present_gap_ms_p99=sorted(gaps)[int(.99*(len(gaps)-1))],accounting_complete=report['accounting_complete'],stages=report['unique_by_stage'],phases=report['phases'])
 (case/'rate-accounting-summary.json').write_text(json.dumps(result,indent=2));results.append(result);print(json.dumps(result))
(root/'rate-accounting-summary.json').write_text(json.dumps(results,indent=2))
