import pathlib,csv,json,subprocess
root=pathlib.Path('/tmp/m5-present-causal')
for name,relative,headless in [('prepared','fixed-gates/prepared/prepared',False),('late-power-negative','fixed-gates/late-power-negative/prepared',False),('lazy-negative','fixed-gates/lazy-negative/lazy-negative',False),('headless','fixed-headless/prepared',True),('idle-on','idle-on/prepared',False)]:
 d=root/relative;rows=[];files=list(d.glob('*.csv'))
 for f in files:rows.extend(csv.DictReader(x for x in f.open() if not x.startswith('#')))
 channel=next(r['channel_id'] for r in rows if r['operation']=='gst.frame_receive')
 manifest=d/'expected-source.jsonl';manifest.write_text(''.join(json.dumps(dict(session=channel,generation='2',frame_id=str(i*1000000000//60),pts_ns=i*1000000000//60))+'\n' for i in range(180)))
 cmd=['python3','/tmp/framelink-m5/tools/frame_accounting.py','--csv','--generation','2','--expected-manifest',str(manifest),'--require-complete','--output',str(d/'accounting.json')]
 if headless:cmd+=['--required','sink_receive,buffer_acquire,submit,ipc_receive,consumer_acquire,schedule,adopt,render_submit,gpu_complete']
 subprocess.run(cmd+[str(f) for f in files],check=True,stdout=subprocess.DEVNULL)
 j=json.loads((d/'accounting.json').read_text());print(name,j['unique_by_stage'],j['phases'])
