#!/usr/bin/env python3
"""One-shot physical-device renderer checks; requires an unlocked device."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
PACKAGE='dev.vkrender.renderer'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial',required=True)
    parser.add_argument('--apk',type=Path,required=True)
    parser.add_argument('--diagnostic',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--suite',choices=['all','ui','lifecycle'],default='all')
    parser.add_argument('--skip-native',action='store_true',help='Reuse separately recorded native checks; do not rerun them')
    args=parser.parse_args()
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    adb=['adb','-s',args.serial]
    state={'state':'starting','serial':args.serial,'steps':[],'suite':args.suite,'native_checks_reused':args.skip_native,
           'apk_sha256':hashlib.sha256(args.apk.read_bytes()).hexdigest(),
           'framelink_pin':(ROOT/'cmake/framelink-revision.txt').read_text().strip(),
           'scope':'Physical-device functional rendering/Binder/lifecycle and fence diagnostics; not endurance or optical/acoustic qualification'}
    def save():(out/'status.json').write_text(json.dumps(state,indent=2)+'\n')
    def shell(*cmd):return subprocess.check_output(adb+['shell',*map(str,cmd)],text=True,timeout=30)
    def step(label,cmd,checked=True,timeout=45):
        state.update(state='running',current=label);save();print(label,flush=True)
        (out/(label+'.command.json')).write_text(json.dumps(list(map(str,cmd)),indent=2)+'\n')
        p=subprocess.run(list(map(str,cmd)),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=timeout)
        (out/(label+'.log')).write_text(p.stdout)
        state['steps'].append({'step':label,'exit':p.returncode});save()
        if checked and p.returncode:raise RuntimeError(label+' failed: '+str(p.returncode))
        return p
    def logs(label,since):
        text=subprocess.check_output(adb+['logcat','-d','-T',since,'-v','threadtime','-s',
             'vkrender:V','RendererControlTest:V','SurfaceLifecycleTest:V','AndroidRuntime:E','libc:F'],text=True,timeout=30)
        (out/(label+'.log')).write_text(text);return text
    def fresh(label):
        # Force-stop only the renderer being tested, never other device apps.
        step(label+'-stop',adb+['shell','am','force-stop',PACKAGE])
        since=shell("date '+%m-%d %H:%M:%S.000'").strip()
        step(label+'-wake',adb+['shell','input','keyevent','KEYCODE_WAKEUP'])
        step(label+'-launch',adb+['shell','am','start','-W','-n',PACKAGE+'/.MainActivity'])
        time.sleep(4)
        if not shell('pidof',PACKAGE).strip():raise RuntimeError(label+': renderer process absent')
        return since
    def screen(name):
        # On this Android build the 'windows' subsection omits global focus.
        # Query the full dump but retain only the app-relevant focus line.
        focus=shell('dumpsys','window')
        focused=[x for x in focus.splitlines() if 'mCurrentFocus=' in x]
        if not any(PACKAGE in x for x in focused):raise RuntimeError('renderer not focused; no screen capture')
        (out/(name+'-focus.txt')).write_text('\n'.join(focused)+'\n')
        with (out/(name+'.png')).open('wb') as image:
            subprocess.run(adb+['exec-out','screencap','-p'],stdout=image,check=True,timeout=30)
    timeout_before=None
    save()
    try:
        policy=shell('dumpsys','window','policy')
        if re.search(r'\bshowing=true\b',policy):raise RuntimeError('secure keyguard is showing; user unlock required')
        installed=shell('pm','path',PACKAGE).strip().splitlines()[0].removeprefix('package:')
        step('pull-installed-apk',adb+['pull',installed,out/'installed.apk'])
        if hashlib.sha256((out/'installed.apk').read_bytes()).hexdigest()!=state['apk_sha256']:
            raise RuntimeError('installed APK differs from validated artifact; not replacing it silently')
        step('device-build',adb+['shell','getprop','ro.build.fingerprint'])
        timeout_before=shell('settings','get','system','screen_off_timeout').strip()
        step('temporary-screen-timeout',adb+['shell','settings','put','system','screen_off_timeout','300000'])
        if not args.skip_native:
            binary=args.diagnostic.resolve();state['diagnostic_sha256']=hashlib.sha256(binary.read_bytes()).hexdigest();save()
            remote='/data/local/tmp/vkr_gpu_sync_fd_diagnostic'
            step('push-diagnostic',adb+['push',binary,remote]);step('chmod-diagnostic',adb+['shell','chmod','700',remote])
            for label,mode in [('raw-vulkan-fence',['--raw-fence']),('eventfd-control',['--eventfd-control']),('original-sync-contract',[])]:
                step(label,adb+['shell','timeout','20',remote,*mode],checked=False,timeout=30)
        if args.suite in ['all','ui']:
            since=fresh('framelink')
            step('framelink-producer',adb+['shell','am','startservice','-n',PACKAGE+'/.FlTestService','--ei','frames','150'])
            time.sleep(2);screen('framelink-frame-a');time.sleep(2);screen('framelink-frame-b');time.sleep(8)
            text=logs('framelink-runtime',since)
            if 'fltest: submitted 150 frames' not in text:raise RuntimeError('FrameLink producer incomplete')
            if not re.search(r'renderer: [1-9]\d* adoptions / [1-9]\d* presents',text):raise RuntimeError('missing adoption/presentation evidence')
            if re.search(r'FATAL EXCEPTION|Fatal signal|fltest: flOpenProducer.*->',text):raise RuntimeError('FrameLink runtime error')
            since=fresh('binder-control')
            step('binder-control-start',adb+['shell','am','startservice','-n',PACKAGE+'/.ControlTestService'])
            time.sleep(24);text=logs('binder-control-runtime',since)
            required=['PASS hidden producer posted120frames without stall',
                      'PASS asynchronous bounded JPEG preview and no idle readback',
                      'PASS control version/capabilities/status, validation, queued actions, hidden progress, pause/resume bounds and kick']
            if any(x not in text for x in required) or re.search(r'RendererControlTest.*FAIL|FATAL EXCEPTION|Fatal signal',text):
                raise RuntimeError('Binder control/preview/lifecycle test failed')
            state['secondary_display']='skipped: no eligible secondary display' if 'SKIP no eligible secondary display' in text else 'see raw log'
            with (out/'preview.jpg').open('wb') as image:
                subprocess.run(adb+['exec-out','run-as',PACKAGE,'cat','files/preview.jpg'],stdout=image,check=True,timeout=30)
        for mode,extra,marker,delay in ([] if args.suite=='ui' else [
            ('tracked',[],'PASS drawing before intentional producer process death',15),
            ('legacy',['--ez','legacy','true'],'PASS legacy release/reacquire',7),
            ('rapid',['--ez','rapid','true'],'PASS 20 rapid release/reacquire generations',10)]):
            since=fresh('surface-'+mode)
            step('surface-'+mode+'-start',adb+['shell','am','startservice','-n',PACKAGE+'/.SurfaceLifecycleTestService',*extra])
            time.sleep(delay);text=logs('surface-'+mode+'-runtime',since)
            if marker not in text or re.search(r'SurfaceLifecycleTest.*FAIL|FATAL EXCEPTION|Fatal signal',text):
                raise RuntimeError('Surface '+mode+' test failed')
            if not shell('pidof',PACKAGE).strip():raise RuntimeError('renderer died in '+mode)
        failures=[s for s in state['steps'] if s['step'] in ['raw-vulkan-fence','eventfd-control','original-sync-contract'] and s['exit'] not in [0,77]]
        skips=[s for s in state['steps'] if s['exit']==77]
        state['state']='finished_with_native_failures' if failures else 'finished_with_skips' if skips or state.get('secondary_display','').startswith('skipped:') else 'finished'
        state['screenshots']='Saved; human/model visual inspection still required.'
    except Exception as exc:
        state.update(state='failed',error=str(exc));raise
    finally:
        if timeout_before is not None:
            try:
                command=['settings','delete','system','screen_off_timeout'] if timeout_before=='null' else ['settings','put','system','screen_off_timeout',timeout_before]
                state['screen_timeout_restored']=shell(*command).strip() in ['', 'Deleted 1 rows']
            except Exception as exc:state['restore_error']=str(exc)
        save()


if __name__=='__main__':main()
