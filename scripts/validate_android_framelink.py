#!/usr/bin/env python3
"""One-shot, serial Android validation of the pinned renderer/FrameLink pair."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ['vkrender_renderer', 'vkr_portable_output_check',
           'vkr_gpu_sync_fd_test', 'vkr_gpu_output_caps', 'vkr_gpu_output_pixels']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--framelink-dir', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--gradle', required=True)
    parser.add_argument('--serial', help='Optional real-device native smoke tests')
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sdk = Path(os.environ.get('ANDROID_SDK_ROOT', Path.home()/'Android/Sdk'))
    cmake = sdk/'cmake/3.22.1/bin/cmake'
    ndk = sdk/'ndk/27.0.12077973'
    framelink = args.framelink_dir.resolve()
    state = {'state': 'starting', 'steps': [],
             'renderer_base': subprocess.check_output(['git','-C',ROOT,'rev-parse','HEAD'],text=True).strip(),
             'framelink_revision': (ROOT/'cmake/framelink-revision.txt').read_text().strip()}
    source_files = ['CMakeLists.txt','README.md','cmake/FrameLinkDependency.cmake',
                    'cmake/framelink-revision.txt','android/renderer/src/main/cpp/CMakeLists.txt',
                    'docs/FRAMELINK_CURRENT.md','scripts/validate_android_framelink.py']
    state['source_sha256'] = {p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in source_files}
    (output/'renderer.patch').write_bytes(subprocess.check_output(['git','-C',ROOT,'diff','HEAD']))
    def save(): (output/'status.json').write_text(json.dumps(state,indent=2)+'\n')
    save()
    def step(label, command, timeout=1800, allow_skip=False):
        state.update(state='running', current=label); save()
        (output/(label+'.command.json')).write_text(json.dumps(list(map(str,command)),indent=2)+'\n')
        print(label,flush=True)
        with (output/(label+'.log')).open('w') as log:
            p=subprocess.run(list(map(str,command)),stdout=log,stderr=subprocess.STDOUT,
                             timeout=timeout, env={**os.environ,'ANDROID_HOME':str(sdk),'ANDROID_SDK_ROOT':str(sdk)})
        result={'step':label,'exit':p.returncode,'outcome':'pass' if p.returncode==0 else 'skip' if allow_skip and p.returncode==77 else 'fail'}
        state['steps'].append(result);save()
        if result['outcome']=='fail': raise RuntimeError(f'{label} failed: exit {p.returncode}')
    try:
        for abi in ['arm64-v8a','x86_64']:
            build = output/abi
            step('configure-'+abi,[cmake,'-S',ROOT/'android/renderer/src/main/cpp','-B',build,'-G','Ninja',
                 f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake',
                 '-DANDROID_ABI='+abi,'-DANDROID_PLATFORM=android-29',
                 '-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DFRAMELINK_DIR='+str(framelink)])
            step('build-'+abi,[cmake,'--build',build,'--parallel','2','--target',*TARGETS])
        step('renderer-apk',[args.gradle,'-p',ROOT/'android','--configure-on-demand','--no-daemon',
             '--max-workers=2',':renderer:assembleDebug','-PframelinkDir='+str(framelink)])
        apk = ROOT/'android/renderer/build/outputs/apk/debug/renderer-debug.apk'
        if not apk.is_file(): raise RuntimeError('expected renderer APK missing')
        state['apk_sha256']=hashlib.sha256(apk.read_bytes()).hexdigest()
        # Native smoke tests do not replace/install an existing renderer app.
        # The portable output checker requires a paired renderer, so compile it
        # but do not launch it as a stand-alone device test.
        if args.serial:
            adb=['adb','-s',args.serial]
            remote='/data/local/tmp/vkrender-framelink-'+state['framelink_revision'][:8]
            step('device-build',adb+['shell','getprop','ro.build.fingerprint'],timeout=30)
            step('device-directory',adb+['shell','mkdir','-p',remote],timeout=30)
            for target in ['vkr_gpu_output_caps','vkr_gpu_sync_fd_test','vkr_gpu_output_pixels']:
                step('push-'+target,adb+['push',output/'arm64-v8a'/target,remote+'/'+target],timeout=30)
                step('chmod-'+target,adb+['shell','chmod','700',remote+'/'+target],timeout=30)
                step('run-'+target,adb+['shell','timeout','35',remote+'/'+target],timeout=45,allow_skip=True)
        state['state']='finished_with_skips' if any(s['outcome']=='skip' for s in state['steps']) else 'finished'
    except Exception as exc:
        state.update(state='failed',error=str(exc));raise
    finally:
        save()


if __name__ == '__main__':
    main()
