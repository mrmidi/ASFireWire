#!/usr/bin/env python3
"""Build an isolated delegated HAL for an explicit sealed package. Never install."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / 'tools/pro40-sequoia'

def run(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise RuntimeError((args, result.stdout, result.stderr))
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('--hardware', action='store_true')
    args = parser.parse_args()
    package = args.package.resolve()
    built = json.loads((package / 'results.json').read_text())
    assert built['result'] == 'passed' and built['duplex_delegated'] and built['duplex_hardware'] == args.hardware and built['duplex_fixture'] != args.hardware
    assert built['provisioning']['hardware_allowed'] == args.hardware
    ownership = built['provisioning']['ownership']
    assert ownership['service'] == built['provisioning']['service'] + '.ownership'
    assert ownership['client_uid'] == os.getuid() and not ownership['hardware_allowed']
    assert ownership['controller_id'] == built['original']['controller_identifier']
    assert ownership['controller_requirement'] == built['provisioning']['controller_requirement']
    assert Path(ownership['binary']) == package / 'ownership-query-service'
    assert hashlib.sha256(Path(ownership['binary']).read_bytes()).hexdigest() == ownership['sha256']
    run('codesign', '--verify', '--strict', ownership['binary'])
    run('codesign', '--verify', '--deep', '--strict', package / 'Pro40 Controller Lab.app')
    out = Path(tempfile.mkdtemp(prefix='delegated-hal-', dir=package))
    session=bool(built.get('session_profile'))
    name = ('Pro40Session' if args.hardware else 'Pro40SessionFixture') if session else ('Pro40NativeLab' if args.hardware else 'Pro40DelegatedLab')
    bundle = out / (name + '.driver')
    executable = bundle / 'Contents/MacOS' / name
    executable.parent.mkdir(parents=True)
    factory = 'F8C4AA9C-7145-4BE0-AC5A-40D8C810EDF9' if args.hardware else 'D0B66B29-545B-43F7-87D1-79ADB166B8C4'
    if session:factory='D48D5072-B661-4932-8BA1-E27BA757778B' if args.hardware else 'B458D34A-C116-4D8B-88DE-F27F6516E0A7'
    plist = plistlib.loads((SRC / 'hal-silence/Info.plist').read_bytes())
    plist.update(CFBundleIdentifier='local.pro40.native-lab' if args.hardware else 'local.pro40.delegated-lab',
                 CFBundleName='Pro40 Native Connection Test' if args.hardware else 'Pro40 Connection Test (No Hardware)', CFBundleExecutable=name,
                 CFPlugInFactories={factory: 'Pro40SilenceRegisteredFactory'}, CFPlugInTypes={'443ABAB8-E7B3-491A-B985-BEB9187030DB': [factory]},
                 AudioServerPlugIn_MachServices=[built['provisioning']['service']])
    if session:
        plist.update(CFBundleIdentifier='local.pro40.session' if args.hardware else 'local.pro40.session-fixture',CFBundleName='Saffire Pro 40 (Experimental)' if args.hardware else 'Pro 40 Session Fixture (No Hardware)')
    (bundle / 'Contents/Info.plist').write_bytes(plistlib.dumps(plist))
    flags = ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-mmacosx-version-min=15.0']
    profile = ['-DPRO40_HAL_DUPLEX_AUDIO', '-DPRO40_HAL_DELEGATED_AUDIO', '-DPRO40_HAL_CALLBACK_TIMING', '-DPRO40_HAL_CALLBACK_TIMING_LOG', '-DPRO40_HAL_CALLBACK_TIMING_SUMMARY', '-I', package]
    if args.hardware: profile.append('-DPRO40_HAL_DELEGATED_HARDWARE')
    if built.get('session_profile'):
        assert not (built.get('application_trial') or built.get('endurance_trial')) and built['provisioning']['session_profile']
        profile.append('-DPRO40_SESSION_PROFILE')
    if built.get('endurance_trial'):
        assert args.hardware and not built.get('application_trial') and built['provisioning']['endurance_trial']
        profile.append('-DPRO40_ENDURANCE_TRIAL')
    frameworks = ['-framework', 'CoreAudio', '-framework', 'CoreFoundation', '-framework', 'Security', '-lbsm']
    run('xcrun', 'clang++', *flags, *profile, '-bundle', SRC / 'HalSilenceDriver.cpp', *frameworks, '-o', executable)
    run('codesign', '--force', '--sign', '-', bundle)
    run('codesign', '--verify', '--strict', bundle)
    run('xcrun', 'clang++', *flags, *(['-DPRO40_SESSION_PROFILE'] if session else []), SRC / 'HalSilenceBundleChecks.cpp', '-framework', 'CoreAudio', '-framework', 'CoreFoundation', '-o', out / 'bundle-checks')
    discovery = run(out / 'bundle-checks', bundle, '2816', '--delegated-hardware' if args.hardware else '--delegated', '8192')
    (out / 'discovery.txt').write_text(discovery.stdout + discovery.stderr)
    run('xcrun', 'clang++', *flags, *profile, SRC / 'HalDelegatedHostProbe.cpp',
        '-framework', 'CoreAudio', '-framework', 'CoreFoundation', '-o', out / 'host-probe')
    imports = run('nm', '-u', executable).stdout
    assert not re.search(r'IOFireWire|IOService(Open|GetMatchingService)|AudioDevice(Start|CreateIOProcID)', imports), imports
    (out / 'imports.txt').write_text(imports)
    files = [executable, bundle / 'Contents/Info.plist', out / 'host-probe', package / 'DuplexProvisionClientPins.hpp', Path(built['provisioning']['broker']),
             Path(ownership['binary']), package / 'OwnershipQueryClientPins.hpp', package / 'OwnershipQueryServerPins.hpp']
    manifest = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    prepared = dict(package=str(package), build=str(out), bundle=str(bundle), probe=str(out / 'host-probe'), hardware=args.hardware, installed=False)
    (out / 'prepared.json').write_text(json.dumps(prepared, indent=2) + '\n')
    # Fixed pointer for the Terminal runner; every referenced payload has its own
    # immutable hash manifest. Updating this file never installs or starts IO.
    (ROOT / ('build/pro40-sequoia/delegated-hardware-test.json' if args.hardware else 'build/pro40-sequoia/delegated-host-test.json')).write_text(json.dumps(prepared, indent=2) + '\n')
    print(out)

if __name__ == '__main__':
    main()
