#!/usr/bin/env python3
"""Local Pro 40 session installation, status, recovery and removal.

Run as the logged-in user. Changes need sudo authorization in Terminal.
All privileged destinations derive from the fixed profile below. Recovery may
restart CoreAudio only after independent proof that native cleanup finished.
It never retries incomplete cleanup or clears a journal or hardware lease.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import pwd
import grp
import re
import signal
import shutil
import stat
import subprocess
import sys
import time
import uuid

APP='Pro40 Controller Lab.app'
OWNERS=('Pro40DiagnosticWorker','Pro40VerifierLab','Pro40ControllerLab')

def require(ok,message):
    if not ok:raise RuntimeError(message)

def run(argv,required=True,timeout=30):
    p=subprocess.run(list(map(str,argv)),capture_output=True,text=True,timeout=timeout)
    if required and p.returncode:raise RuntimeError(f'{argv}: exit {p.returncode}\n{p.stderr[-2000:]}')
    return p

def save(path,value):
    temporary=path.with_suffix(path.suffix+'.new')
    with temporary.open('x') as file:
        json.dump(value,file,indent=2);file.write('\n');file.flush();os.fsync(file.fileno())
    temporary.replace(path)

def plain(path,owner=None):
    # Reject links in every existing ancestor, including the leaf.
    for part in (path,*path.parents):
        require(not part.is_symlink(),f'Symlink refused: {part}')
    if path.exists() and owner is not None:require(path.stat().st_uid==owner,f'Unexpected owner: {path}')

def sha(path):
    plain(path);require(path.is_file() and stat.S_ISREG(path.stat().st_mode),f'Not a regular file: {path}')
    return hashlib.sha256(path.read_bytes()).hexdigest()

def paths(hardware):
    suffix='Session' if hardware else 'SessionFixture'
    app_id='local.pro40.session-controller' if hardware else 'local.pro40.session-fixture-controller'
    service='local.pro40.session-provision' if hardware else 'local.pro40.session-fixture-provision'
    user=Path.home()/('Library/Application Support/ASFW-Pro40-'+suffix+'Package')
    root=Path('/Library/Application Support')/('ASFW-Pro40-'+suffix)
    bundle='Pro40Session.driver' if hardware else 'Pro40SessionFixture.driver'
    return dict(user=user,root=root,bundle=bundle,app_id=app_id,service=service,
        hal=Path('/Library/Audio/Plug-Ins/HAL')/bundle,
        logs=Path.home()/('Library/Logs/ASFW-Pro40-'+suffix),
        journal=Path.home()/('Library/Application Support/ASFW-Pro40-'+suffix+'Controller'),
        receipt=user/'installation.json',controller=user/APP/'Contents/MacOS/Pro40ControllerLab',
        probe=root/'host-probe')

def validate_record(record,p):
    require(record.get('version')==1 and type(record.get('hardware')) is bool,'Invalid installation version/profile')
    require(record.get('uid')==os.getuid() and record.get('gid')==os.getgid(),'Installation belongs to another user')
    for key in ('app_id','service','bundle'):require(record.get(key)==p[key],f'Wrong profile {key}')
    files=record.get('files');require(isinstance(files,dict) and files,'Missing file manifest')
    for name,digest in files.items():
        rel=Path(name)
        require(not rel.is_absolute() and '..' not in rel.parts and str(rel)==name,'Invalid manifest path')
        require(rel.parts[0] in (APP,p['bundle'],'development-peer-pins.txt','provisioning-service','ownership-query-service','host-probe','manage.py','Pro40KeepActive'),'Unknown manifest payload')
        require(re.fullmatch('[0-9a-f]{64}',digest) is not None,'Invalid digest')
    require(all(name in files for name in ('development-peer-pins.txt','provisioning-service','ownership-query-service','host-probe','manage.py',APP+'/Contents/MacOS/Pro40ControllerLab',APP+'/Contents/MacOS/Pro40VerifierLab',APP+'/Contents/MacOS/Pro40DiagnosticWorker',p['bundle']+'/Contents/Info.plist')),'Incomplete payload')

def verify_tree(folder,files):
    plain(folder)
    actual={str(p.relative_to(folder)) for p in folder.rglob('*') if p.is_file()}
    require(actual==set(files),'Payload file list changed')
    for name,digest in files.items():require(sha(folder/name)==digest,f'Changed payload: {name}')

def no_native(include_controller=False):
    for name in OWNERS if include_controller else OWNERS[:2]:
        require(run(['pgrep','-x',name],False).returncode==1,f'Close audio apps and wait for {name} to finish')

def defaults(probe,mode):
    output=run([probe,mode]).stdout
    lines=[line for line in output.splitlines() if line.startswith('default ')]
    require(len(lines)==3,'Cannot observe audio defaults')
    return lines

def defaults_preserved(before,after,hardware):
    # A Mac with no input device may automatically choose its first microphone.
    # Preserve every existing selection, especially both speaker destinations.
    if len(before)!=3 or len(after)!=3:return False
    for old,new in zip(before,after):
        if old==new:continue
        if hardware and old=='default 64496e20 <none>' and new=='default 64496e20 local.pro40.session.device':continue
        return False
    return True

def rows(path):
    if not path.exists():return []
    require(path.stat().st_size<=32*1024*1024,'Controller log requires archival review')
    return [json.loads(line) for line in path.read_text().splitlines(keepends=True) if line.startswith('{') and line.endswith('\n')]

def wait_for(predicate,seconds=10):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if predicate():return
        time.sleep(.1)
    raise RuntimeError('Timed out waiting for service state; installation retained for inspection')

def job(target):return run(['launchctl','print',target],False)

def authorize():
    require(sys.stdin.isatty(),'Run installation/recovery/removal from Terminal for administrator authorization')
    require(subprocess.run(['sudo','-v']).returncode==0,'Administrator authorization failed')

def privilege(argv):return run(['sudo','-n',*argv])

def definitions(p,record):
    identifier=record['installation_id'];require(re.fullmatch('[0-9a-f]{32}',identifier) is not None,'Bad installation ID')
    label=p['app_id']+'.audio.'+identifier
    broker=dict(Label=p['service'],ProgramArguments=[str(p['root']/'provisioning-service'),'--serve'],
        UserName=pwd.getpwuid(os.getuid()).pw_name,GroupName=grp.getgrgid(os.getgid()).gr_name,
        MachServices={p['service']:True},RunAtLoad=True,KeepAlive=False,
        StandardOutPath=str(p['logs']/'service.out'),StandardErrorPath=str(p['logs']/'service.err'))
    ownership=dict(Label=p['service']+'.ownership',ProgramArguments=[str(p['root']/'ownership-query-service'),'--serve'],
        UserName='root',GroupName='wheel',MachServices={p['service']+'.ownership':True},RunAtLoad=True,KeepAlive=False,ExitTimeOut=5,
        StandardOutPath=str(p['root']/'ownership.out'),StandardErrorPath=str(p['root']/'ownership.err'))
    controller=dict(Label=label,ProgramArguments=[str(p['controller']),'--run-managed-duplex-delegated',label],
        MachServices={label:True},RunAtLoad=True,KeepAlive=dict(SuccessfulExit=True),ThrottleInterval=1,ExitTimeOut=40,
        StandardOutPath=str(p['logs']/'controller.out'),StandardErrorPath=str(p['logs']/'controller.err'))
    return [(Path('/Library/LaunchDaemons')/(p['service']+'.ownership.plist'),ownership,'system'),
            (Path('/Library/LaunchDaemons')/(p['service']+'.plist'),broker,'system'),
            (Path.home()/'Library/LaunchAgents'/(label+'.plist'),controller,f'gui/{os.getuid()}')]

def keep_definition(p,record):
    label=p['app_id']+'.keep-active'
    definition=dict(Label=label,ProgramArguments=['/usr/bin/python3','-B',str(p['user']/'manage.py'),
        *([] if record['hardware'] else ['--fixture']),'--run-keep-active'],
        RunAtLoad=True,KeepAlive=False,ExitTimeOut=75,LimitLoadToSessionType='Aqua',
        StandardOutPath=str(p['logs']/'keep-active.out'),StandardErrorPath=str(p['logs']/'keep-active.err'))
    return Path.home()/'Library/LaunchAgents'/(label+'.plist'),definition,f'gui/{os.getuid()}'

def warmup_complete(report,boot):
    worker=report.get('worker') or {};completion=worker.get('completion') or {}
    result=completion.get('result') or {}
    return report.get('schema')==1 and report.get('boot_uuid')==boot and \
        report.get('guid')=='00130e0400000001' and report.get('status')=='completed' and \
        worker.get('state')=='closed' and worker.get('restart_blocked') is False and \
        completion.get('error') is None and completion.get('process_exit')==0 and \
        result.get('exit')==0 and result.get('outcome')==dict(reason='completed',transfer=0,evidence=0,cleanup=0)

def run_keep_active(hardware):
    # Before exec this process owns no audio. Exit immediately on shutdown so
    # a signal at the warm-up/exec boundary cannot be lost by a replacement.
    def stop(_sig,_frame):raise SystemExit(0)
    signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
    p,record=load(hardware);require(record['phase']=='installed','Device is not installed')
    verify_installed_files(p,record)
    # Existing cold-login warm-up remains the sole owner of that workaround.
    # Wait for this boot's final proof, never merely a delay or old success.
    if hardware and record.get('wait_for_login_warmup'):
        boot=run(['/usr/sbin/sysctl','-n','kern.bootsessionuuid']).stdout.strip().upper()
        require(re.fullmatch(r'[0-9A-F]{8}(?:-[0-9A-F]{4}){3}-[0-9A-F]{12}',boot),'Invalid boot identity')
        report_path=Path.home()/'Library/Application Support/ASFW-Pro40-LoginWarmup/state'/boot/'status.json'
        deadline=time.monotonic()+120
        print(json.dumps(dict(keep_active='waiting-for-login-warmup',pid=os.getpid())),flush=True)
        while True:
            if report_path.exists():
                plain(report_path,os.getuid());report=json.loads(report_path.read_text())
                if warmup_complete(report,boot):break
                require(report.get('status') not in ('failed','skipped','cancelled','completed'),'Login warm-up did not complete; keep-active remains off')
            require(time.monotonic()<deadline,'Login warm-up did not finish within 120 seconds; keep-active remains off')
            time.sleep(.2)
        require(run(['pgrep','-x','pro40-native-worker'],False).returncode==1,'Login warm-up worker is still present')
    os.execv(str(p['root']/'Pro40KeepActive'),['Pro40KeepActive','--run'])

def stop_keep_active(p,record,wait_native=True):
    destination,definition,domain=keep_definition(p,record)
    target=domain+'/'+definition['Label'];current=job(target)
    require(current.returncode in (0,113),'Keep-active service state unavailable')
    plain(destination,os.getuid())
    if destination.exists():
        require(destination.read_bytes()==plistlib.dumps(definition,sort_keys=True),'Keep-active definition changed')
    else:require(current.returncode==113,'Unowned keep-active job exists')
    if current.returncode==0:run(['launchctl','bootout',target])
    wait_for(lambda:run(['pgrep','-x','Pro40KeepActive'],False).returncode==1,75)
    # AudioDeviceStop can return before the independent controller receipt.
    if wait_native:wait_for(lambda:all(run(['pgrep','-x',name],False).returncode==1 for name in OWNERS[:2]),40)

def start_keep_active(p,record):
    require('Pro40KeepActive' in record['files'],'Install the package containing keep-active first')
    destination,definition,domain=keep_definition(p,record)
    target=domain+'/'+definition['Label'];current=job(target)
    plain(destination,os.getuid())
    if destination.exists():
        require(destination.read_bytes()==plistlib.dumps(definition,sort_keys=True),'Keep-active definition changed')
    else:
        require(current.returncode==113,'Unowned keep-active service exists')
        destination.write_bytes(plistlib.dumps(definition,sort_keys=True));destination.chmod(0o644)
    require(current.returncode==113,'Keep-active job already loaded; stop it before starting again')
    offset=len(rows(p['logs']/'keep-active.out'))
    run(['launchctl','bootstrap',domain,destination])
    def ready():
        fresh=rows(p['logs']/'keep-active.out')[offset:]
        live=job(target)
        if fresh and fresh[-1].get('keep_active')=='running':
            return live.returncode==0 and re.search(r'^\s*pid = '+str(fresh[-1]['pid'])+r'\s*$',live.stdout,re.M) is not None
        require(live.returncode==0 and 'state = not running' not in live.stdout,'Keep-active failed; inspect its log before recovery')
        return False
    wait_for(ready,160)

def keep_active(hardware,enabled):
    p,record=load(hardware);require(record['phase']=='installed','Device is not installed')
    verify_installed_files(p,record)
    if enabled:
        no_native()
        _,controller,domain=definitions(p,record)[-1]
        require(recovery_ready(rows(p['logs']/'controller.out'),job(domain+'/'+controller['Label'])),'Controller is not ready; use recovery first')
        record['keep_active_enabled']=True;save(p['receipt'],record)
        start_keep_active(p,record)
    else:
        stop_keep_active(p,record,wait_native=False)
        destination,_,_=keep_definition(p,record)
        if destination.exists():destination.unlink()
    record['keep_active_enabled']=enabled;save(p['receipt'],record)
    print('Keep-active '+('enabled; FireWire stays running between apps and starts after login warm-up.' if enabled else 'disabled; audio returns to app-controlled start/stop.'),flush=True)

def archive_removed(p):
    old=json.loads(p['receipt'].read_text());validate_record(old,p)
    require(old['phase']=='removed' and not p['hal'].exists(),'Existing installation is not removed')
    no_native(True)
    for destination,definition,domain in definitions(p,old):
        require(not destination.exists() and job(domain+'/'+definition['Label']).returncode==113,'Removed installation still has a startup job')
    require(job(f"gui/{os.getuid()}/{p['app_id']}.verifier").returncode==113,'Old verifier is still present')
    verify_installed_files(p,old,partial=True)
    for key in ('root','user','logs'):
        source=p[key];plain(source,0 if key=='root' else os.getuid())
        if not source.exists():continue
        destination=source.with_name(source.name+'.removed-'+old['installation_id'])
        plain(destination);require(not destination.exists(),'Removed-installation archive exists')
        if key=='root':privilege(['/bin/mv',source,destination])
        else:source.rename(destination)

def restore_selections(p,before,target):
    # macOS can remember this UID across HAL removal and select it immediately
    # on re-publication. Accept the exact pre-upgrade selection in that case.
    observed=defaults(p['probe'],'--inspect')
    require(len(target)==len(before)==len(observed)==3,'Incomplete default selections')
    for index,(original,fallback,actual,option) in enumerate(zip(target,before,observed,
            ('--select-default-input','--select-default-output','--select-default-system'))):
        prefix=('default 64496e20 ','default 644f7574 ','default 734f7574 ')[index]
        require(original.startswith(prefix) and fallback.startswith(prefix) and actual.startswith(prefix),'Invalid default selector order')
        if actual==original:continue
        require(original==prefix+'local.pro40.session.device' and actual==fallback,'Unrelated audio selection changed')
        run([p['probe'],option])
    require(defaults(p['probe'],'--inspect')==target,'Previous audio defaults were not restored')

def install(prepared,defaults_target=None):
    require(os.geteuid()!=0,'Run as the logged-in user, not root')
    plain(prepared,os.getuid())
    record=json.loads((prepared/'prepared-install.json').read_text());p=paths(record['hardware']);validate_record(record,p)
    verify_tree(prepared/'payload',record['files'])
    record.update(installation_id=uuid.uuid4().hex,phase='preparing')
    jobs=definitions(p,record)
    authorized=False
    if p['receipt'].exists():
        authorize();authorized=True;archive_removed(p)
    for path in [p['user'],p['root'],p['hal'],p['logs'],*(item[0] for item in jobs)]:
        plain(path);require(not path.exists(),f'Existing installation/path retained: {path}')
    require(not list(p['hal'].parent.glob('Pro40*.driver')),'Another Pro40 HAL is installed')
    no_native(True)
    for _,definition,domain in jobs:require(job(domain+'/'+definition['Label']).returncode==113,'A planned service already exists')
    require(job(f"gui/{os.getuid()}/{p['app_id']}.verifier").returncode==113,'Verifier job already exists')
    record['defaults_before']=defaults(prepared/'payload/host-probe','--absent')
    if not authorized:authorize()
    p['user'].mkdir(mode=0o700);p['logs'].mkdir(mode=0o700)
    # Record intent before copying or starting anything. Partial installation
    # can be examined and removed using these same exact paths and hashes.
    record['phase']='installing';save(p['receipt'],record)
    try:
        shutil.copytree(prepared/'payload'/APP,p['user']/APP)
        for name in ('development-peer-pins.txt','manage.py'):shutil.copy2(prepared/'payload'/name,p['user']/name)
        (p['user']/'development-peer-pins.txt').chmod(0o600)
        run(['codesign','--verify','--deep','--strict',p['user']/APP])
        identity=json.loads(run([p['controller'],'--check-development-identity']).stdout)
        require(identity['self_validated'],'Relocated controller identity failed')
        plain(p['journal'],os.getuid())
        if not p['journal'].exists():
            p['journal'].mkdir(mode=0o700)
            fd=os.open(p['journal']/'owner.lock',os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
            os.fsync(fd);os.close(fd)
            fd=os.open(p['journal'],os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)
        preflight=json.loads(run([p['controller'],'--preflight-verifier']).stdout)
        save(p['user']/'installation-preflight.json',preflight)
        # Read-only preflight's generic policy does not authorize first-use
        # registration. The controller separately obtains its reviewed empty
        # journal and independent ownership evidence before that mutation.
        ready=preflight['policy_prepared'] or preflight['service_raw_status']==3
        require(preflight['intent_candidate_valid'] and preflight['journal_clear'] and ready and preflight['service_raw_status'] in (0,3),'Verifier preflight failed')
        privilege(['/bin/mkdir','-m','755',p['root']])
        for name in ('provisioning-service','ownership-query-service','host-probe',*(['Pro40KeepActive'] if 'Pro40KeepActive' in record['files'] else [])):
            privilege(['/usr/bin/install','-o','root','-g','wheel','-m','755',prepared/'payload'/name,p['root']/name])
            require(sha(p['root']/name)==record['files'][name],f'Staged {name} differs')
        for index,(destination,definition,domain) in enumerate(jobs):
            source=p['user']/('install-'+str(index)+'.plist');source.write_bytes(plistlib.dumps(definition,sort_keys=True))
            if domain=='system':privilege(['/usr/bin/install','-o','root','-g','wheel','-m','644',source,destination])
            else:
                destination.parent.mkdir(exist_ok=True);shutil.copy2(source,destination);destination.chmod(0o644)
            require(destination.read_bytes()==source.read_bytes(),'Installed service definition changed')
            if domain=='system':privilege(['/bin/launchctl','bootstrap',domain,destination])
            else:run(['launchctl','bootstrap',domain,destination])
            ready_path=p['root']/'ownership.out' if index==0 else p['logs']/('service.out' if index==1 else 'controller.out')
            key=('ownership_query_ready','provision_service_ready','delegated_controller_registered')[index]
            wait_for(lambda:any(row.get(key) for row in rows(ready_path)))
        privilege(['/usr/bin/ditto',prepared/'payload'/p['bundle'],p['hal']])
        privilege(['/usr/sbin/chown','-R','root:wheel',p['hal']])
        privilege(['/bin/chmod','-R','u+rwX,go+rX,go-w',p['hal']])
        run(['codesign','--verify','--strict',p['hal']])
        verify_installed_files(p,record)
        privilege(['/usr/bin/killall','coreaudiod'])
        wait_for(lambda:run([p['probe'],'--inspect'],False).returncode==0)
        if defaults_target is not None:
            restore_selections(p,record['defaults_before'],defaults_target)
            record['defaults_target']=defaults_target
        record['defaults_after']=defaults(p['probe'],'--inspect')
        require(record['defaults_after']==defaults_target if defaults_target is not None else
            defaults_preserved(record['defaults_before'],record['defaults_after'],record['hardware']),'An existing audio default changed')
        record['phase']='installed';save(p['receipt'],record)
        print('Installed '+('Saffire Pro 40 (Experimental)' if record['hardware'] else 'Pro 40 Session Fixture (No Hardware)')+'. Audio starts when an app selects it.',flush=True)
    except BaseException as error:
        record['error']=str(error);save(p['receipt'],record)
        print('Installation incomplete; exact payload and services retained for status/removal.',file=sys.stderr)
        raise

def verify_installed_files(p,record,partial=False):
    for name,digest in record['files'].items():
        rel=Path(name)
        if rel.parts[0] in (APP,'development-peer-pins.txt','manage.py'):path=p['user']/rel
        elif rel.parts[0]==p['bundle']:path=p['hal']/Path(*rel.parts[1:])
        else:path=p['root']/rel
        if partial and not path.exists():continue
        require(sha(path)==digest,f'Installed file differs: {path}')

def load(hardware):
    p=paths(hardware);plain(p['receipt'],os.getuid())
    record=json.loads(p['receipt'].read_text());validate_record(record,p)
    return p,record

def status(hardware):
    p,record=load(hardware)
    observed=rows(p['logs']/'controller.out')
    last=observed[-1] if observed else {}
    jobs=definitions(p,record);live=job(jobs[-1][2]+'/'+jobs[-1][1]['Label'])
    keep_path,keep_job,keep_domain=keep_definition(p,record)
    keep_rows=rows(p['logs']/'keep-active.out')
    result=dict(phase=record['phase'],device='Saffire Pro 40 (Experimental)' if hardware else 'Generated session fixture',
        controller_idle=bool(last.get('delegated_controller_registered')) and recovery_ready(observed,live),
        keep_active_enabled=record.get('keep_active_enabled',False),
        keep_active_job_loaded=job(keep_domain+'/'+keep_job['Label']).returncode==0,
        keep_active_last_event=keep_rows[-1] if keep_rows else None,
        last_event=last,services=[dict(label=d['Label'],present=job(domain+'/'+d['Label']).returncode==0) for _,d,domain in definitions(p,record)],
        evidence=str(p['logs']),receipt=str(p['receipt']))
    print(json.dumps(result,indent=2))

def recovery_ready(observed,current):
    """A failed stream may be replaced; an unproven hardware release may not."""
    if current.returncode not in (0,113):return False
    last=observed[-1] if observed else {}
    if last.get('delegated_controller_registered'):
        # Do not confuse a historical registration with the current owner.
        return current.returncode==0 and re.search(r'^\s*pid = '+str(last.get('pid',-1))+r'\s*$',current.stdout,re.M) is not None
    stopped=current.returncode==113 or 'state = not running' in current.stdout
    return stopped and all(last.get(key) is True for key in (
        'supervision_finished','local_cleanup_complete','worker_cleanup_complete',
        'worker_result_observed','native_outcome_observed','diagnostic_worker_released',
        'delegated_cleanup_confirmed','duplex_local_cleanup_complete')) and \
        last.get('native_outcome',{}).get('cleanup')==0 and \
        last.get('worker_collection_fault')==0 and last.get('worker_setup_cleanup_error')==0 and \
        last.get('callback_pending') is False and last.get('managed_cleanup_overdue') is False

def verify_jobs(jobs):
    for destination,definition,_ in jobs:
        plain(destination,0 if destination.parent==Path('/Library/LaunchDaemons') else os.getuid())
        require(destination.read_bytes()==plistlib.dumps(definition,sort_keys=True),'Service definition changed; recovery refused')

def verify_released(p,hardware):
    no_native(True)
    raw=run([p['controller'],'--audit']).stdout;audit=json.loads(raw)
    require(audit.get('available') and audit['audit']['unresolved_count']==0,'Unresolved ownership journal; recovery refused')
    require(job(f"gui/{os.getuid()}/{p['app_id']}.verifier").returncode==113,'Verifier job remains; recovery refused')
    if hardware:
        lease=Path.home()/'Library/Application Support/ASFW-Pro40-Workers/00130e0400000001.lock'
        plain(lease,os.getuid())
        require(lease.read_text()=='clean\n','Native lease is unresolved; recovery refused')
    return dict(available=True,unresolved_count=0,records=len(audit['audit'].get('records',[])),
        sha256=hashlib.sha256(raw.encode()).hexdigest(),journal=str(p['journal']))

def recover(hardware):
    p,record=load(hardware);require(record['phase']=='installed','Recovery requires an installed device')
    verify_installed_files(p,record)
    jobs=definitions(p,record);verify_jobs(jobs)
    if record.get('keep_active_enabled'):stop_keep_active(p,record)
    no_native()
    destination,definition,domain=jobs[-1];target=domain+'/'+definition['Label']
    if record.get('keep_active_enabled'):
        wait_for(lambda:recovery_ready(rows(p['logs']/'controller.out'),job(target)),40)
    observed=rows(p['logs']/'controller.out');current=job(target)
    require(recovery_ready(observed,current),'Close audio apps and wait for proven joined cleanup before recovery')
    # Keep original faults intact. The new process is a separate attempt, never
    # a retroactive pass or an automatic retry of the failed native session.
    attempt=p['logs']/('recovery-'+uuid.uuid4().hex+'.json')
    result=dict(installation_id=record['installation_id'],phase='prepared',last_event=observed[-1])
    save(attempt,result)
    authorize()
    try:
        if current.returncode==0:run(['launchctl','bootout',target])
        wait_for(lambda:run(['pgrep','-x','Pro40ControllerLab'],False).returncode==1,40)
        result['audit']=verify_released(p,hardware)
        verify_installed_files(p,record);verify_jobs(jobs)
        offset=len(rows(p['logs']/'controller.out'))
        run(['launchctl','bootstrap',domain,destination])
        def ready():
            fresh=rows(p['logs']/'controller.out')[offset:]
            return bool(fresh and fresh[-1].get('delegated_controller_registered') and recovery_ready(fresh,job(target)))
        wait_for(ready)
        privilege(['/usr/bin/killall','coreaudiod'])
        wait_for(lambda:run([p['probe'],'--inspect'],False).returncode==0)
        if record.get('keep_active_enabled'):start_keep_active(p,record)
        result['phase']='recovered';result['new_controller']=rows(p['logs']/'controller.out')[-1]
        save(attempt,result)
    except BaseException as error:
        result.update(phase='blocked',error=str(error));save(attempt,result);raise
    print('Recovered the CoreAudio device without rebooting. Original session evidence retained at '+str(attempt),flush=True)

def remove(hardware):
    p,record=load(hardware);require(record['phase']!='removed','Already removed')
    verify_installed_files(p,record,partial=True)
    if record.get('keep_active_enabled'):stop_keep_active(p,record)
    jobs=definitions(p,record)
    for destination,definition,_ in jobs:
        plain(destination,0 if destination.parent==Path('/Library/LaunchDaemons') else os.getuid())
        if destination.exists():require(destination.read_bytes()==plistlib.dumps(definition,sort_keys=True),'Service definition changed; removal refused')
    no_native()
    observed=rows(p['logs']/'controller.out')
    current=job(jobs[-1][2]+'/'+jobs[-1][1]['Label'])
    idle=bool(observed and observed[-1].get('delegated_controller_registered'))
    stopped=current.returncode==113 or (current.returncode==0 and 'state = not running' in current.stdout)
    require(idle or stopped,'Close audio apps and wait for joined cleanup before removing the device')
    authorize()
    # Stop admissions first. If an app races the idle observation, SIGTERM uses
    # the same joined shutdown; unresolved journal/lease state below blocks HAL removal.
    if current.returncode==0:run(['launchctl','bootout',jobs[-1][2]+'/'+jobs[-1][1]['Label']])
    wait_for(lambda:run(['pgrep','-x','Pro40ControllerLab'],False).returncode==1,40)
    no_native(True)
    if p['controller'].exists():
        audit=json.loads(run([p['controller'],'--audit'],False).stdout)
        require((audit.get('available') and audit['audit']['unresolved_count']==0) or
                (not p['journal'].exists() and audit.get('error')=='journal path missing'),
                'Unresolved ownership journal; installation retained')
    require(job(f"gui/{os.getuid()}/{p['app_id']}.verifier").returncode==113,'Verifier job remains; installation retained')
    if hardware:
        lease=Path.home()/'Library/Application Support/ASFW-Pro40-Workers/00130e0400000001.lock'
        require(lease.read_text()=='clean\n','Native lease is unresolved; installation retained')
    if p['hal'].exists():
        disabled=p['root']/(p['bundle']+'.disabled');require(not disabled.exists(),'HAL removal destination exists')
        privilege(['/bin/mv',p['hal'],disabled]);privilege(['/usr/bin/killall','coreaudiod'])
        wait_for(lambda:run([p['probe'],'--absent'],False).returncode==0)
    for destination,definition,domain in reversed(jobs):
        target=domain+'/'+definition['Label'];present=job(target)
        require(present.returncode in (0,113),'Service state unavailable')
        if present.returncode==0:
            if domain=='system':privilege(['/bin/launchctl','bootout',target])
            else:run(['launchctl','bootout',target])
            wait_for(lambda:job(target).returncode==113)
        if destination.exists():
            saved=p['root']/(destination.name+'.disabled') if domain=='system' else p['user']/(destination.name+'.disabled')
            require(not saved.exists(),'Service archive already exists')
            if domain=='system':privilege(['/bin/mv',destination,saved])
            else:destination.rename(saved)
    keep_path,_,_=keep_definition(p,record)
    if keep_path.exists():
        stop_keep_active(p,record);keep_path.rename(p['user']/(keep_path.name+'.disabled'))
    record['phase']='removed';save(p['receipt'],record)
    print('Removed the CoreAudio device and startup jobs. Package, logs and ownership journal retained for inspection.')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture',action='store_true',help='Use the separate generated-audio installation')
    action=parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--install',type=Path);action.add_argument('--status',action='store_true');action.add_argument('--remove',action='store_true');action.add_argument('--recover',action='store_true')
    action.add_argument('--keep-active',action='store_true');action.add_argument('--on-demand',action='store_true')
    action.add_argument('--run-keep-active',action='store_true',help=argparse.SUPPRESS)
    args=parser.parse_args();require(os.geteuid()!=0,'Run as the logged-in user')
    if args.install:
        record=json.loads((args.install/'prepared-install.json').read_text())
        require(bool(record['hardware'])!=args.fixture,'Explicit fixture/hardware profile mismatch')
        install(args.install)
    elif args.status:status(not args.fixture)
    elif args.recover:recover(not args.fixture)
    elif args.keep_active or args.on_demand:keep_active(not args.fixture,args.keep_active)
    elif args.run_keep_active:run_keep_active(not args.fixture)
    else:remove(not args.fixture)

if __name__=='__main__':
    try:main()
    except (RuntimeError,OSError,ValueError,subprocess.SubprocessError) as error:
        print(str(error),file=sys.stderr);sys.exit(1)
