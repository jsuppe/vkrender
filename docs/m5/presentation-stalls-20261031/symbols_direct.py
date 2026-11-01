import ctypes as c, os, json, pathlib
from ctypes import wintypes as w
kit='C:/Program Files (x86)/Windows Kits/10/Debuggers/x64'
dll_directory=os.add_dll_directory(kit)
symbol_server=c.WinDLL(kit+"/symsrv.dll",use_last_error=True)
k=c.WinDLL('kernel32',use_last_error=True);a=c.WinDLL('advapi32',use_last_error=True);p=c.WinDLL('psapi',use_last_error=True)
k.GetCurrentProcess.restype=w.HANDLE;h=k.GetCurrentProcess()
class LUID(c.Structure):_fields_=[('low',w.DWORD),('high',w.LONG)]
class PRIV(c.Structure):_fields_=[('count',w.DWORD),('luid',LUID),('attrs',w.DWORD)]
t=w.HANDLE();a.OpenProcessToken.argtypes=[w.HANDLE,w.DWORD,c.POINTER(w.HANDLE)];a.OpenProcessToken(h,0x20|8,c.byref(t));priv=PRIV();priv.count=1;priv.attrs=2;a.LookupPrivilegeValueW(None,'SeDebugPrivilege',c.byref(priv.luid));a.AdjustTokenPrivileges(t,False,c.byref(priv),0,None,None)
mods=(c.c_void_p*2048)();needed=w.DWORD();p.EnumDeviceDrivers(c.byref(mods),c.sizeof(mods),c.byref(needed));bases={}
p.GetDeviceDriverBaseNameW.argtypes=[c.c_void_p,w.LPWSTR,w.DWORD]
for base in mods[:needed.value//c.sizeof(c.c_void_p)]:
 b=c.create_unicode_buffer(260);p.GetDeviceDriverBaseNameW(base,b,260)
 if b.value.lower() in ['win32kbase.sys','win32kfull.sys','dxgkrnl.sys','dxgmms2.sys','ntoskrnl.exe']:bases[b.value.lower()]=base
print('bases',bases,flush=True)
d=c.WinDLL(kit+'/dbghelp.dll',use_last_error=True)
d.SymSetOptions(0x00000004|0x00000002|0x00000200|0x00080000)
d.SymInitializeW.argtypes=[w.HANDLE,w.LPCWSTR,w.BOOL];d.SymInitializeW.restype=w.BOOL
if not d.SymInitializeW(h,'C:/Users/jonsu/dev/m5-symbol-cache',False):raise c.WinError(c.get_last_error())
d.SymLoadModuleExW.argtypes=[w.HANDLE,w.HANDLE,w.LPCWSTR,w.LPCWSTR,c.c_ulonglong,w.DWORD,c.c_void_p,w.DWORD];d.SymLoadModuleExW.restype=c.c_ulonglong
class SYMBOL(c.Structure):_fields_=[('SizeOfStruct',w.ULONG),('TypeIndex',w.ULONG),('Reserved',c.c_ulonglong*2),('Index',w.ULONG),('Size',w.ULONG),('ModBase',c.c_ulonglong),('Flags',w.ULONG),('Value',c.c_ulonglong),('Address',c.c_ulonglong),('Register',w.ULONG),('Scope',w.ULONG),('Tag',w.ULONG),('NameLen',w.ULONG),('MaxNameLen',w.ULONG),('Name',c.c_char*1024)]
d.SymFromAddr.argtypes=[w.HANDLE,c.c_ulonglong,c.POINTER(c.c_ulonglong),c.POINTER(SYMBOL)];d.SymFromAddr.restype=w.BOOL
rows=json.load(open('C:/Users/jonsu/dev/m5-present-readier.json'));out=[]
for name in ['win32kfull.sys','win32kbase.sys','dxgkrnl.sys','dxgmms2.sys']:
 base=bases[name];path=str(next(f for f in pathlib.Path('C:/Users/jonsu/dev/m5-symbol-cache').rglob(name.replace('.sys','.pdb')) if f.is_file()))
 print('load',name,d.SymLoadModuleExW(h,None,path,None,base,0x1000000,None,0),flush=True)
 for addr in dict.fromkeys(int(r[4],16) for r in rows if r[5].startswith(name+'!')):
  sym=SYMBOL();sym.SizeOfStruct=88;sym.MaxNameLen=1024;disp=c.c_ulonglong()
  ok=d.SymFromAddr(h,addr,c.byref(disp),c.byref(sym));v=dict(module=name,address=hex(addr),symbol=sym.Name.decode(errors='replace') if ok else None,displacement=disp.value,error=0 if ok else c.get_last_error());out.append(v);print(v,flush=True)
open('C:/Users/jonsu/dev/m5-present-readier-symbols.json','w').write(json.dumps(out,indent=2))
