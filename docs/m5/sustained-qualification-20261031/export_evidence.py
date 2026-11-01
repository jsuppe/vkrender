import pathlib,sys,tarfile
root=pathlib.Path(sys.argv[1]);target=pathlib.Path(sys.argv[2])
with tarfile.open(target,'w:gz') as archive:
 for f in sorted(root.rglob('*')):
  relative=f.relative_to(root)
  if f.is_file() and 'session' not in relative.parts and f.suffix in ('.csv','.json','.log','.pipeline'):
   archive.add(f,arcname=str(relative))
print(target)
