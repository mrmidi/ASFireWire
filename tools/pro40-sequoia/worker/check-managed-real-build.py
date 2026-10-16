#!/usr/bin/env python3
"""Compile and inspect the managed real worker. NEVER execute the output."""
import argparse,hashlib,json,subprocess,tempfile
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--duplex',action='store_true');args=parser.parse_args()
root=Path(__file__).resolve().parents[3]
base=root/'build/pro40-sequoia';base.mkdir(parents=True,exist_ok=True)
out=Path(tempfile.mkdtemp(prefix='managed-real-inspection-',dir=base))
def run(name,args):
    p=subprocess.run(list(map(str,args)),cwd=root,capture_output=True,text=True,timeout=120)
    (out/(name+'.json')).write_text(json.dumps(dict(argv=list(map(str,args)),exit=p.returncode,stdout=p.stdout,stderr=p.stderr),indent=2)+'\n')
    assert p.returncode==0,(name,p.stderr)
    return p
build=run('build',['bash','-x','tools/pro40-sequoia/build-transmit-return.sh','--managed-duplex-worker-real-inspect' if args.duplex else '--reuse-queue-managed-worker-real-inspect'])
invocation=[line for line in build.stderr.splitlines() if line.startswith('+ xcrun clang++ ')]
assert len(invocation)==1
command=invocation[0]
for required in ['-DPRO40_MANAGED_WORKER','-DPRO40_DUPLEX_WORKER' if args.duplex else '-DPRO40_DMA_CYCLE_BUDGET=65536','-DPRO40_DMA_SOURCE_QUEUE',
                 'tools/pro40-sequoia/NativeFireWire.cpp','tools/pro40-sequoia/worker/NativeWorker.cpp','tools/pro40-sequoia/NativeSessionRunner.cpp']:
    assert required in command,required
for forbidden in ['PRO40_WORKER_TEST_LOCKS','PRO40_WORKER_ADMISSION_TEST','NativeStub.cpp','LivenessFixture.cpp','-fsanitize']:
    assert forbidden not in command,forbidden
binary=base/('pro40-managed-duplex-worker-real-inspect' if args.duplex else 'pro40-managed-worker-real-inspect')
symbols=run('imports',['nm','-u',binary]).stdout
assert '_IOServiceGetMatchingServices' in symbols
strings=run('strings',['strings',binary]).stdout
assert '--managed' in strings and '/Library/Application Support/ASFW-Pro40-Workers' in strings
for marker in ['PRO40_WORKER_TEST_LOCK_DIR','PRO40_ADMISSION_TEST_FD','test-native-open: no hardware','fixture-entered']:
    assert marker not in strings,marker
run('dependencies',['otool','-L',binary]);run('architecture',['file',binary])
summary=dict(result='passed',executed=False,signed_for_deployment=False,packaged=False,
    binary=str(binary),sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
    duplex=args.duplex,cycle_budget=691200000 if args.duplex else 65536,
    nominal_stream_seconds=None if args.duplex else 8.192,stream_watchdog_seconds=None if args.duplex else 8.768,
    real_lease_directory=True,test_hooks_absent=True,evidence=str(out))
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
