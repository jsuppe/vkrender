import hashlib, os, pathlib, socket, subprocess, sys, time, urllib.request, urllib.error
build=pathlib.Path(sys.argv[1]);log=build/'stage3-preview.log'
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
with log.open('w') as output:
 p=subprocess.Popen([str(build/'Release/vkrenderer.exe'),'--serve','stage3-preview','--headless','-1','--window','640x360+0+0','--control-port',str(port)],cwd=build,stdout=output,stderr=output)
 hashes=set();count=0
 try:
  until=time.monotonic()+20
  while count<8 and time.monotonic()<until:
   try:
    with urllib.request.urlopen(f'http://127.0.0.1:{port}/api/preview.jpg',timeout=3) as r:
     frame=r.read();assert frame[:2]==b'\xff\xd8' and frame[-2:]==b'\xff\xd9';hashes.add(hashlib.sha256(frame).digest());count+=1
   except urllib.error.URLError:pass
   assert p.poll() is None,'renderer stopped'
   time.sleep(.45)
  assert count==8 and len(hashes)>1,(count,len(hashes))
  print(f'PASS Windows headless GPU JPEG preview: {count} snapshots, {len(hashes)} distinct JPEG hashes')
 finally:
  p.terminate();p.wait(timeout=5)
