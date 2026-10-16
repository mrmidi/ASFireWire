#!/usr/bin/env python3
"""Generated duplex audio, managed child, temporary GUI job; no hardware or HAL install.

--build-only compiles/signs a fresh local lab. --run DIRECTORY runs it once.
Uses the existing controlled-checkout helper verifier, not the packaged service.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'tools/pro40-sequoia'

def run(args, timeout=60):
    return subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=timeout)

def checked(args, timeout=60):
    r=run(args,timeout)
    if r.returncode: raise RuntimeError((args,r.returncode,r.stdout,r.stderr))
    return r

def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def build(native=False):
    base=ROOT/'build/pro40-sequoia/duplex-worker';base.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='candidate-',dir=base))
    manifest={}
    for sanitizer in ('address,undefined','thread'):
        folder=out/sanitizer;folder.mkdir()
        flags=['xcrun','clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror',f'-fsanitize={sanitizer}']
        pins={}
        targets=[('worker','worker/NativeWorker.cpp'),('controller','DuplexWorkerProcessChecks.cpp'),('helper','NativeIdentityIsolationFixture.cpp')] if native else [('worker','DuplexWorkerProcessChecks.cpp'),('helper','NativeIdentityIsolationFixture.cpp')]
        for name,source in targets:
            binary=folder/name
            extra=[]
            if native and name=='controller': extra=['-DPRO40_NATIVE_DUPLEX_RUNNER_CHECK']
            if native and name=='worker': extra=['-DPRO40_DUPLEX_WORKER','-DPRO40_MANAGED_WORKER','-DPRO40_LIVE_PLAYBACK','-DPRO40_SUSTAINED_INPUT','-DPRO40_DMA_SUSTAINED','-DPRO40_WORKER_TEST_LOCKS',str(SRC/'worker/DuplexNativeFixture.cpp'),str(ROOT/'ASFWDriver/Audio/Wire/IEC61883/CipHeader.cpp'),str(ROOT/'ASFWDriver/Audio/Wire/IEC61883/DbcCounter.cpp')]
            checked([*flags,*extra,SRC/source,'-framework','CoreAudio','-framework','CoreFoundation','-framework','Security','-lbsm','-o',binary])
            checked(['codesign','--force','--sign','-','--identifier',f'local.pro40.duplex-lab.{name}',binary])
            checked(['codesign','--verify','--strict',binary])
            pins[name]=re.search(r'^CDHash=([0-9a-f]{40})$',checked(['codesign','-d','--verbose=4',binary]).stderr,re.M).group(1)
            imports=checked(['nm','-u',binary]).stdout
            assert not re.search(r'IOFireWire|IOService(Open|GetMatchingService)|AudioDevice(Start|CreateIOProcID)',imports),imports
            (folder/(name+'.imports')).write_text(imports)
        manifest[sanitizer]={'native_worker':native,'pins':pins,'hashes':{name:digest(folder/name) for name in pins}}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(out,flush=True)
    return out

def execute(out):
    out=out.resolve();manifest=json.loads((out/'manifest.json').read_text())
    # No rerunning a consumed job or overwriting an earlier report.
    with (out/'consumed').open('x') as f: f.write('one isolated process validation\n')
    reports=[]
    for sanitizer,entry in manifest.items():
        folder=out/sanitizer
        for name,value in entry['hashes'].items(): assert digest(folder/name)==value
        label='local.pro40.duplex-fixture.'+uuid.uuid4().hex
        target=f'gui/{os.getuid()}/{label}'
        plist=folder/'service.plist';output=folder/'controller.out';errors=folder/'controller.err'
        plist.write_bytes(plistlib.dumps({'Label':label,'RunAtLoad':True,
            'ProgramArguments':[str(folder/('controller' if entry.get('native_worker') else 'worker')),label,str(folder/'worker'),entry['pins']['worker'],str(folder/'helper'),entry['pins']['helper'],str(folder)],
            'MachServices':{label:True},'StandardOutPath':str(output),'StandardErrorPath':str(errors)}))
        registration=run(['launchctl','bootstrap',f'gui/{os.getuid()}',plist])
        (folder/'bootstrap.txt').write_text(registration.stdout+registration.stderr)
        if registration.returncode: raise RuntimeError(f'bootstrap failed: {registration.stderr}; no existing job changed')
        try:
            deadline=time.monotonic()+35
            while time.monotonic()<deadline:
                state=run(['launchctl','print',target]);(folder/'job.txt').write_text(state.stdout+state.stderr)
                if 'state = not running' in state.stdout and re.search(r'last (exit code|terminating signal) =',state.stdout): break
                time.sleep(.1)
            else: raise RuntimeError('fixture did not exit before its bounded test deadline')
            stdout=output.read_text();stderr=errors.read_text()
            if 'last exit code = 0' not in state.stdout or stderr or 'PASS all managed duplex process checks' not in stdout:
                raise RuntimeError((sanitizer,stdout,stderr,state.stdout))
            modes=('normal','reject','cancel','wrong-release','parent-stop','underrun','unclean') if entry.get('native_worker') else ('normal','reject','cancel','wrong-release','wrong-worker','wrong-parent','wrong-session','unclean')
            for mode in modes:
                child=(folder/(mode+'.err')).read_text()
                expected=mode in ('normal','unclean','parent-stop','underrun')
                if entry.get('native_worker'):
                    assert (folder/(mode+'.session')).exists()==expected,(mode,child)
                    if expected:
                        report=json.loads((folder/(mode+'.session.duplex.json')).read_text())
                        assert report['cutover']==report['started_at'] and report['cutover']>0,report
                        assert report['prepared_frames']-report['cutover']==report['application_frames'],report
                        assert report['application_frames']>20000,report
                        if mode=='underrun':
                            assert report['bridge_fault'] in (5,6) and report['startup_fault']==5 and report['playback_fault']==6,report
                        else:
                            assert not report['bridge_fault'] and not report['startup_fault'] and not report['playback_fault'],report
                        assert report['native_cleanup']==(mode!='unclean'),report
                        positions=report['tx_positions']
                        buffers=report['buffering']
                        assert (buffers['lead_frames'],buffers['prime_frames'],buffers['input_history_frames'])==(4096,1024,2816),report
                        assert 8<=buffers['queue_low_water_frames']<=buffers['queue_high_water_frames']<=8192,report
                        assert [p['frame'] for p in positions]==[0,report['cutover'],report['cutover']+buffers['prime_frames']],report
                        assert all(p['frames']==8 and p['syt']!=65535 and p['cycle']==(positions[0]['cycle']+p['packet']-positions[0]['packet'])%64000 for p in positions),report
                        assert report['have_clock'] and report['input_clock_origin']>0 and report['controller_origin_host']>0,report
                        presentation=report['output_presentation']
                        assert presentation['applied'] and presentation['offset_frames']!=0,report
                        assert abs(presentation['offset_frames']-presentation['unrounded_offset_frames'])<=.500001,report
                        assert presentation['sample_at_cutover']==report['cutover']+presentation['offset_frames'],report
                        assert (folder/(mode+'-leases')/'00130e0400000001.lock').read_text()==('clean\n' if mode!='unclean' else 'active\n')
                        session=(folder/(mode+'.session')).read_text()
                        early=re.search(r'^early-input packets=(\d+) replay=(\d+) max-lead=(\d+) max-batch=(\d+) fault=(\d+)$',session,re.M)
                        assert early and int(early[1])>int(early[2]) and early.groups()[2:]==('512','64','0'),session
                        assert session.endswith('generated-native-closed\n')
                    else: assert not list((folder/(mode+'-leases')).iterdir())
                else:
                    assert ('duplex-entered' in child)==expected,(mode,child)
                    if expected: assert f'duplex-finished frames=55312 clean={int(mode=="normal")}' in child,child
                assert not re.search(r'Sanitizer|runtime error:',child),child
            print(stdout,end='',flush=True)
            reports.append({'sanitizer':sanitizer,'passed':True,'modes':len(modes)})
        finally:
            removed=run(['launchctl','bootout',target]);(folder/'bootout.txt').write_text(removed.stdout+removed.stderr)
            absent=run(['launchctl','print',target]);(folder/'after.txt').write_text(absent.stdout+absent.stderr)
            missing=f'Could not find service \"{label}\"' in absent.stderr
            if absent.returncode==0 or not missing:
                raise RuntimeError(f'Cleanup unresolved: {target}: {removed.stderr} {absent.stderr}')
    (out/'results.json').write_text(json.dumps(reports,indent=2)+'\n')
    print(f'EVIDENCE {out}')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--build-only',action='store_true');parser.add_argument('--run',type=Path);parser.add_argument('--native-worker',action='store_true')
    args=parser.parse_args();out=args.run or build(args.native_worker)
    if not args.build_only: execute(out)
