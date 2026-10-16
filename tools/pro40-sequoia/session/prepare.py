#!/usr/bin/env python3
"""Prepare an exact local-development installation; no services or hardware access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[3]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def prepare(build):
    build=build.resolve()
    prepared=json.loads((build/'prepared.json').read_text())
    package=Path(prepared['package'])
    built=json.loads((package/'results.json').read_text())
    hardware=bool(built['duplex_hardware'])
    assert build.parent==package and built['result']=='passed' and built['session_profile'] and built['duplex_delegated']
    assert not built['application_trial'] and not built['endurance_trial'] and prepared['hardware']==hardware
    assert built['original']['controller_identifier']==('local.pro40.session-controller' if hardware else 'local.pro40.session-fixture-controller')
    assert built['provisioning']['service']==('local.pro40.session-provision' if hardware else 'local.pro40.session-fixture-provision')
    assert built['provisioning']['ownership']['client_uid']==os.getuid()
    if hardware:assert built['device_state']['fixed_guid']==built['real_guid']=='00130e0400000001'
    manifest=json.loads((build/'manifest.json').read_text())
    for name,expected in manifest.items():
        path=Path(name)
        assert path.is_relative_to(package) and not path.is_symlink() and digest(path)==expected
    app=package/'Pro40 Controller Lab.app'
    subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
    out=Path(tempfile.mkdtemp(prefix='session-install-',dir=ROOT/'build/pro40-sequoia'))
    payload=out/'payload';payload.mkdir()
    assert not any(p.is_symlink() for p in app.rglob('*'))
    shutil.copytree(app,payload/app.name)
    for source,name in [(package/'development-peer-pins.txt','development-peer-pins.txt'),
        (Path(built['provisioning']['broker']),'provisioning-service'),
        (Path(built['provisioning']['ownership']['binary']),'ownership-query-service'),
        (Path(prepared['probe']),'host-probe'),(Path(__file__).with_name('manage.py'),'manage.py')]:
        assert source.is_file() and not source.is_symlink()
        shutil.copy2(source,payload/name)
    bundle=Path(prepared['bundle'])
    assert not any(p.is_symlink() for p in bundle.rglob('*'))
    shutil.copytree(bundle,payload/bundle.name)
    keep=payload/'Pro40KeepActive'
    subprocess.run(['xcrun','clang++','-std=c++20','-O2','-Wall','-Wextra','-Werror','-mmacosx-version-min=15.0',
        *([] if hardware else ['-DPRO40_KEEP_ACTIVE_FIXTURE']),str(Path(__file__).with_name('KeepActive.cpp')),
        '-framework','CoreAudio','-framework','CoreFoundation','-o',str(keep)],check=True)
    subprocess.run(['codesign','--force','--sign','-',str(keep)],check=True)
    subprocess.run(['codesign','--verify','--strict',str(keep)],check=True)
    files={str(p.relative_to(payload)):digest(p) for p in sorted(payload.rglob('*')) if p.is_file()}
    record=dict(version=1,hardware=hardware,uid=os.getuid(),gid=os.getgid(),source_build=str(build),
        app_id=built['original']['controller_identifier'],service=built['provisioning']['service'],
        bundle=bundle.name,controller_cdhash=built['original']['controller_cdhash'],
        helper_cdhash=built['original']['helper_cdhash'],files=files,keep_active_enabled=False,
        wait_for_login_warmup=hardware and (Path.home()/'Library/LaunchAgents/local.pro40.login-warmup.plist').exists())
    (out/'prepared-install.json').write_text(json.dumps(record,indent=2)+'\n')
    pointer=ROOT/'build/pro40-sequoia'/('session-hardware-install.json' if hardware else 'session-fixture-install.json')
    pointer.write_text(json.dumps(dict(prepared=str(out)),indent=2)+'\n')
    print(out)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('build',type=Path)
    prepare(parser.parse_args().build)
