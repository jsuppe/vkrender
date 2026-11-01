import pathlib,uuid,struct,urllib.request,json
out=[]
for name in ['win32kfull.sys','win32kbase.sys','dxgkrnl.sys','dxgmms2.sys']:
 file=pathlib.Path('C:/Windows/System32')/('drivers' if name.startswith('dxg') else '')/name
 b=file.read_bytes();i=b.index(b'RSDS');guid=uuid.UUID(bytes_le=b[i+4:i+20]).hex.upper();age=struct.unpack_from('<I',b,i+20)[0];pdb=b[i+24:].split(b'\0',1)[0].decode();pdb=pathlib.PureWindowsPath(pdb).name
 url=f'https://msdl.microsoft.com/download/symbols/{pdb}/{guid}{age:X}/{pdb}';p=pathlib.Path('C:/Users/jonsu/dev/m5-symbol-cache')/pdb/f'{guid}{age:X}'/pdb;p.parent.mkdir(parents=True,exist_ok=True)
 try:
  if not p.exists():urllib.request.urlretrieve(url,p)
  value=dict(module=name,pdb=pdb,url=url,bytes=p.stat().st_size);out.append(value);print(value,flush=True)
 except Exception as e: print(name,str(e),flush=True)
pathlib.Path('C:/Users/jonsu/dev/m5-symbol-provenance.json').write_text(json.dumps(out,indent=2))
