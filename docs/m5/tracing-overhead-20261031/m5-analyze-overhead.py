import pathlib,json,re,sys
root=pathlib.Path(sys.argv[1]);out=[]
for case in root.glob('1920*'):
 samples=json.loads((case/'status.json').read_text());mid=[x for x in samples if 5<=x['t']<=40 and x.get('producer_cpu_s') is not None and x.get('renderer_cpu_s') is not None]
 a,b=mid[0],mid[-1];dt=b['t']-a['t'];av0=a['status']['av'];av1=b['status']['av']
 rows=[json.loads(x) for x in (case/'producer.log').read_text().splitlines() if x.startswith('{')]
 transport=next(x for x in rows if x.get('event')=='video-transport-summary')
 logs=(case/'renderer.log').read_text();adoptions=[int(n) for n in re.findall(r'service: (\d+) adoptions in 5s',logs)]
 result=dict(case=case.name,status_sample_window_s=[a['t'],b['t']],producer_cpu_core_percent=(b['producer_cpu_s']-a['producer_cpu_s'])/dt*100,renderer_cpu_core_percent=(b['renderer_cpu_s']-a['renderer_cpu_s'])/dt*100,due_decision_rate=(av1['due_decisions']-av0['due_decisions'])/dt,observed_due_max=max(x['status'].get('av',{}).get('due_decisions',0) for x in samples),transport=transport,adoptions_per_5s=adoptions,late_max=max(x['status'].get('av',{}).get('late_drops',0) for x in samples),import_failures_max=max(x['status'].get('av',{}).get('import_failures',0) for x in samples),presentation_errors_max=max(x['status'].get('presentation_submission',{}).get('errors',0) for x in samples),csv_files=len(list(case.glob('*.csv'))))
 clean=[x for x in mid if x['t']>=10];ca,cb=clean[0],clean[-1];cdt=cb['t']-ca['t']
 result['secondary_post10s_window']={'window_s':[ca['t'],cb['t']],'producer_cpu_core_percent':(cb['producer_cpu_s']-ca['producer_cpu_s'])/cdt*100,'renderer_cpu_core_percent':(cb['renderer_cpu_s']-ca['renderer_cpu_s'])/cdt*100,'due_decision_rate':(cb['status']['av']['due_decisions']-ca['status']['av']['due_decisions'])/cdt}
 (case/'overhead-summary.json').write_text(json.dumps(result,indent=2));out.append(result);print(json.dumps(result))
(root/'overhead-summary.json').write_text(json.dumps(out,indent=2))
