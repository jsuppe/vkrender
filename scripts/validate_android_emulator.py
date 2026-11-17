#!/usr/bin/env python3
"""One-shot Android emulator functional validation. No physical-device access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = 'dev.vkrender.renderer'
IMAGE = 'system-images;android-35;google_apis;x86_64'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--apk',type=Path,required=True)
    parser.add_argument('--native-build',type=Path,required=True,
                        help='Previously validated x86_64 native build directory')
    parser.add_argument('--port',type=int,default=5580)
    args=parser.parse_args()
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    apk=args.apk.resolve();sdk=Path.home()/'Android/Sdk'
    env={**os.environ,'ANDROID_SDK_ROOT':str(sdk),'ANDROID_HOME':str(sdk),
         'ANDROID_AVD_HOME':str(out/'avds'),'ANDROID_EMULATOR_HOME':str(out/'emulator-home')}
    for folder in ['avds','emulator-home']:(out/folder).mkdir()
    serial='emulator-'+str(args.port)
    adb=['adb','-s',serial]
    state={'state':'starting','steps':[],'serial':serial,'image':IMAGE,'gpu_mode':'host',
           'apk_sha256':hashlib.sha256(apk.read_bytes()).hexdigest(),
           'framelink_pin':(ROOT/'cmake/framelink-revision.txt').read_text().strip(),
           'scope':'Emulator-only functional checks, not physical-device performance or output qualification'}
    def save():(out/'status.json').write_text(json.dumps(state,indent=2)+'\n')
    def step(label,command,timeout=60,input_text=None,checked=True):
        state.update(state='running',current=label);save();print(label,flush=True)
        (out/(label+'.command.json')).write_text(json.dumps(list(map(str,command)),indent=2)+'\n')
        with (out/(label+'.log')).open('w') as log:
            p=subprocess.run(list(map(str,command)),env=env,input=input_text,text=True,
                             stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
        state['steps'].append({'step':label,'exit':p.returncode});save()
        if checked and p.returncode:raise RuntimeError(label+' failed: '+str(p.returncode))
        return p.returncode
    def shell(*cmd,timeout=30):
        return subprocess.check_output(adb+['shell',*map(str,cmd)],text=True,timeout=timeout,env=env)
    def logs(label):
        result=subprocess.check_output(adb+['logcat','-d','-v','threadtime','-s',
            'vkrender:V','RendererControlTest:V','SurfaceLifecycleTest:V',
            'AndroidRuntime:E','libc:F'],text=True,timeout=30,env=env)
        (out/(label+'.log')).write_text(result);return result
    def fresh(label):
        step(label+'-stop',adb+['shell','am','force-stop',PACKAGE])
        step(label+'-log-reset',adb+['logcat','-c'])
        step(label+'-launch',adb+['shell','am','start','-W','-n',PACKAGE+'/.MainActivity'])
        time.sleep(4)
        if not shell('pidof',PACKAGE).strip():raise RuntimeError(label+': renderer process absent')
    emulator=None;emulator_log=None
    save()
    try:
        with zipfile.ZipFile(apk) as bundle:
            if 'lib/x86_64/libvkrender_renderer.so' not in bundle.namelist():
                raise RuntimeError('APK lacks emulator x86_64 native library')
        # Fail rather than select an existing emulator or displace another job.
        for port in [args.port,args.port+1]:
            with socket.socket() as sock:sock.bind(('127.0.0.1',port))
        step('install-system-image',[sdk/'cmdline-tools/latest/bin/sdkmanager',
             '--sdk_root='+str(sdk),'--install',IMAGE],timeout=2400,input_text='')
        step('create-isolated-avd',[sdk/'cmdline-tools/latest/bin/avdmanager','create','avd',
             '--name','vkrender_validation','--package',IMAGE,'--device','pixel_6',
             '--path',out/'avds/vkrender_validation.avd'],timeout=60,input_text='no\n')
        config=out/'avds/vkrender_validation.avd/config.ini'
        config.write_text(config.read_text()+'\nhw.gpu.enabled=yes\nhw.gpu.mode=host\nhw.ramSize=3072\n')
        command=['/usr/lib/android-sdk/emulator/emulator','-avd','vkrender_validation',
                 '-port',str(args.port),'-no-window','-gpu','host','-feature','Vulkan',
                 '-no-audio','-no-snapshot','-no-boot-anim','-camera-back','none',
                 '-camera-front','none','-cores','4','-memory','3072']
        (out/'emulator.command.json').write_text(json.dumps(command,indent=2)+'\n')
        emulator_log=(out/'emulator.log').open('w')
        emulator=subprocess.Popen(command,stdout=emulator_log,stderr=subprocess.STDOUT,env=env,start_new_session=True)
        state.update(current='booting',emulator_pid=emulator.pid);save()
        step('wait-for-device',adb+['wait-for-device'],timeout=180)
        # One fixed boot allowance, then one check. No boot/recovery retry loop.
        time.sleep(60)
        if emulator.poll() is not None:raise RuntimeError('emulator exited during boot')
        if shell('getprop','sys.boot_completed').strip()!='1':raise RuntimeError('boot incomplete after fixed allowance')
        step('device-build',adb+['shell','getprop','ro.build.fingerprint'])
        step('graphics-features',adb+['shell','pm','list','features'])
        step('wake',adb+['shell','input','keyevent','KEYCODE_WAKEUP'])
        step('dismiss-test-keyguard',adb+['shell','wm','dismiss-keyguard'])
        # This is a fresh, task-owned emulator; never target a real-device serial.
        step('install-renderer',adb+['install','-r',apk],timeout=90)
        for name in ['vkr_gpu_output_caps','vkr_gpu_sync_fd_test','vkr_gpu_output_pixels']:
            remote='/data/local/tmp/'+name
            step('push-'+name,adb+['push',args.native_build.resolve()/name,remote])
            step('chmod-'+name,adb+['shell','chmod','700',remote])
            step('run-'+name,adb+['shell','timeout','35',remote],timeout=45,checked=False)
        fresh('framelink')
        step('framelink-start-producer',adb+['shell','am','startservice','-n',PACKAGE+'/.FlTestService','--ei','frames','150'])
        time.sleep(3)
        with (out/'framelink-rendering.png').open('wb') as image:
            subprocess.run(adb+['exec-out','screencap','-p'],stdout=image,check=True,timeout=30)
        time.sleep(9)
        text=logs('framelink-runtime')
        if not re.search(r'fltest: submitted 150 frames',text):raise RuntimeError('FrameLink producer did not submit 150 frames')
        if not re.search(r'renderer: [1-9]\d* adoptions / [1-9]\d* presents',text):
            raise RuntimeError('no nonzero renderer adoption/presentation evidence')
        if re.search(r'Fatal signal|FATAL EXCEPTION|fltest: flOpenProducer.*->',text):raise RuntimeError('FrameLink runtime error')
        # Existing debug-only real Binder/Surface tests, no production API edits.
        fresh('binder-control')
        step('binder-control-start',adb+['shell','am','startservice','-n',PACKAGE+'/.ControlTestService'])
        time.sleep(24)
        text=logs('binder-control-runtime')
        required=['PASS hidden producer posted120frames without stall',
                  'PASS asynchronous bounded JPEG preview and no idle readback',
                  'PASS control version/capabilities/status, validation, queued actions, hidden progress, pause/resume bounds and kick']
        if any(marker not in text for marker in required) or re.search(r'RendererControlTest.*FAIL|FATAL EXCEPTION|Fatal signal',text):
            raise RuntimeError('Binder control/preview/lifecycle fixture did not pass')
        state['secondary_display']='skipped: no attached secondary display' if 'SKIP no eligible secondary display' in text else 'see raw log'
        with (out/'preview.jpg').open('wb') as image:
            subprocess.run(adb+['exec-out','run-as',PACKAGE,'cat','files/preview.jpg'],stdout=image,check=True,timeout=30)
        for mode,extra,marker,delay in [
            ('tracked',[],'PASS drawing before intentional producer process death',15),
            ('legacy',['--ez','legacy','true'],'PASS legacy release/reacquire',7),
            ('rapid',['--ez','rapid','true'],'PASS 20 rapid release/reacquire generations',10)]:
            fresh('surface-'+mode)
            step('surface-'+mode+'-start',adb+['shell','am','startservice','-n',PACKAGE+'/.SurfaceLifecycleTestService',*extra])
            time.sleep(delay);text=logs('surface-'+mode+'-runtime')
            if marker not in text or re.search(r'SurfaceLifecycleTest.*FAIL|FATAL EXCEPTION|Fatal signal',text):
                raise RuntimeError('Surface '+mode+' fixture did not pass')
            if not shell('pidof',PACKAGE).strip():raise RuntimeError('renderer died after Surface '+mode)
        native_failures=[s for s in state['steps'] if s['step'].startswith('run-vkr_') and s['exit'] not in [0,77]]
        native_skips=[s for s in state['steps'] if s['step'].startswith('run-vkr_') and s['exit']==77]
        state['state']='finished_with_native_failures' if native_failures else 'finished_with_skips' if native_skips or state.get('secondary_display','').startswith('skipped:') else 'finished'
        state['qualification']='Functional emulator fixtures only; native capability failures/skips remain separate in steps. Screenshots saved, not visually reviewed.'
    except Exception as exc:
        state.update(state='failed',error=str(exc))
        if emulator is not None and emulator.poll() is None:
            try:logs('failure-runtime')
            except Exception:pass
        raise
    finally:
        if emulator is not None and emulator.poll() is None:
            subprocess.run(adb+['emu','kill'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=15)
            try:emulator.wait(timeout=20)
            except subprocess.TimeoutExpired:
                os.killpg(emulator.pid,signal.SIGTERM);emulator.wait(timeout=10)
        if emulator_log:emulator_log.close()
        save()


if __name__=='__main__':main()
