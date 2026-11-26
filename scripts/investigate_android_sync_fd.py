#!/usr/bin/env python3
"""One-shot attribution of the emulator SYNC_FD failure; no gate relaxation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prior-emulator-run',type=Path,required=True)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    prior=args.prior_emulator_run.resolve();binary=args.binary.resolve()
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    command=json.loads((prior/'emulator.command.json').read_text())
    port=command[command.index('-port')+1];serial='emulator-'+port;adb=['adb','-s',serial]
    env={**os.environ,'ANDROID_SDK_ROOT':str(Path.home()/'Android/Sdk'),
         'ANDROID_HOME':str(Path.home()/'Android/Sdk'),
         'ANDROID_AVD_HOME':str(prior/'avds'),'ANDROID_EMULATOR_HOME':str(prior/'emulator-home')}
    state={'state':'starting','serial':serial,'steps':[],
           'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
           'test_source_sha256':hashlib.sha256((ROOT/'test/gpu_sync_fd.cpp').read_bytes()).hexdigest(),
           'framelink_pin':(ROOT/'cmake/framelink-revision.txt').read_text().strip(),
           'production_code_changed':False}
    def save():(out/'status.json').write_text(json.dumps(state,indent=2)+'\n')
    def step(label,cmd,timeout=45,checked=True):
        state.update(state='running',current=label);save();print(label,flush=True)
        (out/(label+'.command.json')).write_text(json.dumps(list(map(str,cmd)),indent=2)+'\n')
        p=subprocess.run(list(map(str,cmd)),env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=timeout)
        (out/(label+'.log')).write_text(p.stdout)
        state['steps'].append({'step':label,'exit':p.returncode});save()
        if checked and p.returncode:raise RuntimeError(label+' failed: '+str(p.returncode))
        return p
    proc=None;log=None;save()
    try:
        (out/'test-source.cpp').write_bytes((ROOT/'test/gpu_sync_fd.cpp').read_bytes())
        (out/'test.patch').write_bytes(subprocess.check_output(['git','-C',ROOT,'diff','HEAD','--','test/gpu_sync_fd.cpp']))
        for candidate in [int(port),int(port)+1]:
            with socket.socket() as sock:sock.bind(('127.0.0.1',candidate))
        log=(out/'emulator.log').open('w')
        proc=subprocess.Popen(command,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        state['emulator_pid']=proc.pid;save()
        step('wait-for-device',adb+['wait-for-device'],timeout=180)
        time.sleep(60)
        boot=step('boot-complete',adb+['shell','getprop','sys.boot_completed'])
        if boot.stdout.strip()!='1':raise RuntimeError('boot incomplete after fixed allowance')
        step('build-fingerprint',adb+['shell','getprop','ro.build.fingerprint'])
        remote='/data/local/tmp/vkr_gpu_sync_fd_diagnostic'
        step('push',adb+['push',binary,remote]);step('chmod',adb+['shell','chmod','700',remote])
        raw=step('raw-vulkan-fence',adb+['shell','timeout','20',remote,'--raw-fence'],checked=False)
        control=step('eventfd-transport-control',adb+['shell','timeout','20',remote,'--eventfd-control'],checked=False)
        original=step('original-contract',adb+['shell','timeout','20',remote],checked=False)
        records=[json.loads(line) for line in raw.stdout.splitlines() if line.startswith('{')]
        result={'raw_probe_exit':raw.returncode,'raw_probe':records,
                'eventfd_control_exit':control.returncode,'original_contract_exit':original.returncode,
                'eventfd_is_gpu_proof':False,'original_failure_retained':original.returncode!=0}
        if raw.returncode==0 and len(records)==1:
            r=records[0]
            result['raw_export_early_readiness']=r['before_pollin'] and r['scm_before_pollin'] and r['event_before']==4 and r['completion_before']==1
            if result['raw_export_early_readiness'] and control.returncode==0:
                result['finding']='Exported FD is readable before event release and Vulkan queue completion, before FrameLink receives it; generic FrameLink FD wait/reuse controls pass.'
                result['boundary']='Raw emulator Vulkan/native-fence export or readiness path; exact underlying driver/emulator cause not isolated.'
            else:result['finding']='Raw probe did not establish the early-export hypothesis; inspect raw and original return codes.'
        else:result['finding']='Raw probe failed or produced no complete record; no causal conclusion.'
        state.update(state='finished',analysis=result)
        (out/'analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    except Exception as exc:
        state.update(state='failed',error=str(exc));raise
    finally:
        if proc is not None and proc.poll() is None:
            subprocess.run(adb+['emu','kill'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=15)
            try:proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=10)
        if log:log.close()
        save()


if __name__=='__main__':main()
