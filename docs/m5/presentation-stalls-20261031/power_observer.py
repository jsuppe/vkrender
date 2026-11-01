"""Read-only interactive-session display power notification recorder (Windows)."""
import ctypes as c,json,time,sys,pathlib
from ctypes import wintypes as w
u=c.WinDLL('user32',use_last_error=True); k=c.WinDLL('kernel32',use_last_error=True)
LRESULT=c.c_ssize_t; WNDPROC=c.WINFUNCTYPE(LRESULT,w.HWND,w.UINT,w.WPARAM,w.LPARAM)
class GUID(c.Structure):_fields_=[('a',w.DWORD),('b',w.WORD),('c',w.WORD),('d',c.c_ubyte*8)]
class WNDCLASS(c.Structure):_fields_=[('style',w.UINT),('proc',WNDPROC),('clsExtra',c.c_int),('wndExtra',c.c_int),('instance',w.HINSTANCE),('icon',w.HICON),('cursor',w.HANDLE),('brush',w.HBRUSH),('menu',w.LPCWSTR),('name',w.LPCWSTR)]
class MSG(c.Structure):_fields_=[('hwnd',w.HWND),('message',w.UINT),('wParam',w.WPARAM),('lParam',w.LPARAM),('time',w.DWORD),('pt',w.POINT),('lPrivate',w.DWORD)]
class Setting(c.Structure):_fields_=[('guid',GUID),('length',w.DWORD),('value',w.DWORD)]
class LastInput(c.Structure):_fields_=[('size',w.UINT),('tick',w.DWORD)]
u.DefWindowProcW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM];u.DefWindowProcW.restype=LRESULT
u.RegisterClassW.argtypes=[c.POINTER(WNDCLASS)];u.RegisterClassW.restype=w.ATOM
u.CreateWindowExW.argtypes=[w.DWORD,w.LPCWSTR,w.LPCWSTR,w.DWORD,c.c_int,c.c_int,c.c_int,c.c_int,w.HWND,w.HMENU,w.HINSTANCE,c.c_void_p];u.CreateWindowExW.restype=w.HWND
u.RegisterPowerSettingNotification.argtypes=[w.HANDLE,c.POINTER(GUID),w.DWORD];u.RegisterPowerSettingNotification.restype=w.HANDLE
u.PeekMessageW.argtypes=[c.POINTER(MSG),w.HWND,w.UINT,w.UINT,w.UINT]
u.DispatchMessageW.argtypes=[c.POINTER(MSG)];u.DispatchMessageW.restype=LRESULT
u.DestroyWindow.argtypes=[w.HWND];u.UnregisterPowerSettingNotification.argtypes=[w.HANDLE]
k.GetModuleHandleW.restype=w.HMODULE
out=pathlib.Path(sys.argv[1]).open('w',buffering=1)
def emit(event,**values):out.write(json.dumps(dict(event=event,steady_ns=time.perf_counter_ns(),wall_ns=time.time_ns(),**values))+'\n')
@WNDPROC
def callback(hwnd,msg,wp,lp):
 if msg==0x218 and wp==0x8013:
  s=c.cast(lp,c.POINTER(Setting)).contents;emit('display_state',guid=f'{s.guid.a:08x}',value=s.value,length=s.length)
 if msg==0x7e:emit('display_change',bits=int(wp),size=int(lp))
 return u.DefWindowProcW(hwnd,msg,wp,lp)
wc=WNDCLASS();wc.proc=callback;wc.instance=k.GetModuleHandleW(None);wc.name='FrameLinkM5PowerObserver'
if not u.RegisterClassW(c.byref(wc)):raise c.WinError(c.get_last_error())
hwnd=u.CreateWindowExW(0,wc.name,'FrameLink M5 read-only display state',0,0,0,0,0,None,None,wc.instance,None)
if not hwnd:raise c.WinError(c.get_last_error())
# GUID_CONSOLE_DISPLAY_STATE (0=off,1=on,2=dimmed); no power mutation.
guid=GUID(0x6fe69556,0x704a,0x47a0,(c.c_ubyte*8)(0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47))
h=u.RegisterPowerSettingNotification(hwnd,c.byref(guid),0)
if not h:raise c.WinError(c.get_last_error())
li=LastInput(c.sizeof(LastInput),0);u.GetLastInputInfo(c.byref(li));emit('observer_start',idle_ms=(k.GetTickCount()-li.tick)&0xffffffff)
end=time.monotonic()+float(sys.argv[2]);msg=MSG()
try:
 while time.monotonic()<end and not (len(sys.argv)>3 and pathlib.Path(sys.argv[3]).exists()):
  while u.PeekMessageW(c.byref(msg),None,0,0,1):u.TranslateMessage(c.byref(msg));u.DispatchMessageW(c.byref(msg))
  time.sleep(.005)
finally:u.UnregisterPowerSettingNotification(h);u.DestroyWindow(hwnd);emit('observer_end')
