import csv,pathlib,json
c=pathlib.Path('/tmp/m5-trace-on/1920x1080-60-NV12-av');j=json.loads((c/'accounting-with-manifest.json').read_text());base=next(f for f in j['frames'] if f['pts_ns']==0)['timeline'][0]['time_ns']
rows=[]
for p in c.glob('*.csv'):
 for r in csv.DictReader(x for x in p.open() if not x.startswith('#')):
  r['t']=int(r['start_ns']);r['d']=int(r['duration_ns']);rows.append(r)
for f in j['frames']:
 if f['outcome']=='complete':continue
 print('\nDROP',f['pts_ns'],f['timeline'])
 ipc=next(x['time_ns'] for x in f['timeline'] if x['stage']=='ipc_receive')
 for r in sorted(rows,key=lambda x:x['t']):
  if r['t']<=ipc<=r['t']+r['d'] and not r['operation'].startswith('av.frame') and r['d']>1000000:
   print('SPANS',r['operation'],round((r['t']-base)/1e6,3),round(r['d']/1e6,3))
print('\nLONG STARTUP SCOPES')
for r in sorted(rows,key=lambda x:x['t']):
 if 0<=r['t']-base<400000000 and r['d']>10000000 and not r['operation'].startswith('av.frame'):
  print(r['operation'],round((r['t']-base)/1e6,3),round(r['d']/1e6,3))
