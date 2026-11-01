import pathlib,csv,json,collections,re
p=pathlib.Path('C:/Users/jonsu/dev/m5-present-etw-baseline.csv');tids=set();out=[];stack=[];headers=[];stacks=collections.defaultdict(list);switches=[];ready=[];lastout={}
for row in csv.reader(p.open(errors='replace'),skipinitialspace=True):
 if not row:continue
 row=[x.strip() for x in row];op=row[0]
 try:
  if int(row[1])>7000000:break
 except:pass
 if op in ['T-Start','T-End','ThreadName'] and len(row)>3 and any(t in row[2] for t in ['vkrenderer.exe','framelink_av_producer.exe']):
  try:tids.add(int(row[3]))
  except:continue
  if op=='ThreadName':out.append(row)
 if op=='CSwitch' and len(row)>10 and any(t in row[2]+row[8] for t in ['vkrenderer.exe','framelink_av_producer.exe']):switches.append(row)
 if op=='ReadyThread' and len(row)>5 and any(t in row[4] for t in ['vkrenderer.exe','framelink_av_producer.exe']):ready.append(row)
 if op not in ['Stack','CSwitch','ReadyThread','I-Start','I-End','I-DCStart','I-DCEnd','T-Start','T-End','T-DCStart','T-DCEnd'] and 'Header' in op:headers.append(row)
for row in csv.reader(p.open(errors='replace'),skipinitialspace=True):
 if not row or row[0].strip()!='Stack':continue
 row=[x.strip() for x in row]
 try:tid=int(row[2]);tm=int(row[1])
 except:continue
 if tm>7000000:break
 try:tid=int(row[2])
 except:continue
 if tid in tids:stacks[(tm,tid)].append(row[5])
result={'headers':headers,'names':out,'switches':switches,'ready':ready,'stacks':[[tm,tid,s] for (tm,tid),s in stacks.items()]}
pathlib.Path('C:/Users/jonsu/dev/m5-present-etw-baseline-target.json').write_text(json.dumps(result))
print('names',out);print('tids',sorted(tids));print('headers',headers[:6]);print('switches',len(switches),'stacks',len(stacks))
for r in switches:
 tm=int(r[1]);new=int(r[3]);old=int(r[9]);reason=r[13];state=r[12]
 if new in lastout:
  prev=lastout[new]
  if tm-int(prev[1])>30000:
   print('OFF',r[2],new,'end',tm,'duration',tm-int(prev[1]),'state',prev[12],'reason',prev[13],'stack',stacks.get((int(prev[1]),new),[]))
 lastout[old]=r
