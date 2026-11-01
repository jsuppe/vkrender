import pathlib,csv,json,subprocess
root=pathlib.Path('/tmp/m5-preparation-all-evidence')
for sub in ['m5-native-preparation-tests/prepared','m5-native-preparation-negatives/lazy-negative','m5-native-preparation-headless/prepared','m5-native-preparation-headless/lazy-negative']:
 c=root/sub;files=list(c.glob('*.csv'));rows=[]
 for p in files:rows.extend(csv.DictReader(x for x in p.open() if not x.startswith('#')))
 channel=next(r['channel_id'] for r in rows if r['operation']=='gst.frame_receive');manifest=c/'expected-source.jsonl'
 manifest.write_text(''.join(json.dumps(dict(session=channel,generation='2',frame_id=str(i*1000000000//60),pts_ns=i*1000000000//60))+'\n' for i in range(180)))
 cmd=['python3','/tmp/framelink-m5-frame-accounting/tools/frame_accounting.py','--csv','--generation','2','--expected-manifest',str(manifest),'--require-complete','--output',str(c/'accounting-with-manifest.json')]
 if 'headless' in sub:cmd+=['--required','sink_receive,buffer_acquire,submit,ipc_receive,consumer_acquire,schedule,adopt,render_submit,gpu_complete']
 subprocess.run(cmd+[str(p) for p in files],check=True,stdout=subprocess.DEVNULL)
 j=json.loads((c/'accounting-with-manifest.json').read_text());print(sub,j['unique_by_stage'],j['phases'])
