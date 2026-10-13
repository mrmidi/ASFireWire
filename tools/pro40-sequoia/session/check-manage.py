#!/usr/bin/env python3
"""Refusal checks with temporary files only; no service installation."""
import copy
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
import manage

def rejects(work):
    try:work()
    except RuntimeError:return
    raise AssertionError('Invalid payload was accepted')

def main():
    with tempfile.TemporaryDirectory(prefix='pro40-install-check-',dir='/private/tmp') as temporary:
        folder=Path(temporary);(folder/'binary').write_bytes(b'one')
        manifest={'binary':manage.sha(folder/'binary')}
        manage.verify_tree(folder,manifest)
        (folder/'binary').write_bytes(b'two');rejects(lambda:manage.verify_tree(folder,manifest))
        (folder/'binary').write_bytes(b'one');(folder/'extra').write_bytes(b'x')
        rejects(lambda:manage.verify_tree(folder,manifest));(folder/'extra').unlink()
        (folder/'alias').symlink_to(folder/'binary');rejects(lambda:manage.sha(folder/'alias'));(folder/'alias').unlink()
        (folder/'directory-alias').symlink_to(folder,target_is_directory=True)
        rejects(lambda:manage.sha(folder/'directory-alias'/'binary'))
    p=manage.paths(False)
    required=['development-peer-pins.txt','provisioning-service','ownership-query-service','host-probe','manage.py',manage.APP+'/Contents/MacOS/Pro40ControllerLab',manage.APP+'/Contents/MacOS/Pro40VerifierLab',manage.APP+'/Contents/MacOS/Pro40DiagnosticWorker',p['bundle']+'/Contents/Info.plist']
    record=dict(version=1,hardware=False,uid=manage.os.getuid(),gid=manage.os.getgid(),app_id=p['app_id'],service=p['service'],bundle=p['bundle'],files={name:'a'*64 for name in required})
    manage.validate_record(record,p)
    for key,value in [('uid',0),('service','local.other'),('bundle','../wrong'),('files',{'../outside':'a'*64})]:
        bad=copy.deepcopy(record);bad[key]=value;rejects(lambda:manage.validate_record(bad,p))
    record['installation_id']='b'*32
    jobs=manage.definitions(p,record)
    assert jobs[0][1]['UserName']=='root' and jobs[1][1]['UserName']!='root'
    assert jobs[2][1]['KeepAlive']=={'SuccessfulExit':True} and jobs[2][1]['ExitTimeOut']>=30
    assert len(jobs[2][1]['ProgramArguments'])==3 # Never a stored FireWire generation.
    record['installation_id']='../bad';rejects(lambda:manage.definitions(p,record))
    before=['default 64496e20 <none>','default 644f7574 BuiltInSpeakerDevice','default 734f7574 BuiltInSpeakerDevice']
    after=['default 64496e20 local.pro40.session.device',*before[1:]]
    assert manage.defaults_preserved(before,after,True) and not manage.defaults_preserved(before,after,False)
    assert not manage.defaults_preserved(after,before,True)
    after[1]='default 644f7574 local.pro40.session.device';assert not manage.defaults_preserved(before,after,True)
    target=list(after)
    with patch.object(manage,'defaults',side_effect=[target,target]),patch.object(manage,'run') as runner:
        manage.restore_selections(p,before,target);runner.assert_not_called()
    with patch.object(manage,'defaults',side_effect=[before,target]),patch.object(manage,'run') as runner:
        manage.restore_selections(p,before,target)
        assert [call.args[0][-1] for call in runner.call_args_list]==['--select-default-input','--select-default-output']
    unrelated=list(target);unrelated[2]='default 734f7574 AnotherDevice'
    with patch.object(manage,'defaults',return_value=unrelated),patch.object(manage,'run') as runner:
        rejects(lambda:manage.restore_selections(p,before,target));runner.assert_not_called()
    # Replaying a stream failure is forbidden until its separate cleanup proof
    # is complete. In particular, a dead process or clean native status alone
    # does not establish that joined cleanup and worker collection finished.
    clean_keys=('supervision_finished','local_cleanup_complete','worker_cleanup_complete',
        'worker_result_observed','native_outcome_observed','diagnostic_worker_released',
        'delegated_cleanup_confirmed','duplex_local_cleanup_complete')
    final=dict.fromkeys(clean_keys,True)
    final.update(native_outcome=dict(cleanup=0,evidence=-1),worker_collection_fault=0,
        worker_setup_cleanup_error=0,callback_pending=False,managed_cleanup_overdue=False,
        session_restart_permitted=False)
    stopped=SimpleNamespace(returncode=0,stdout='state = not running\n')
    assert manage.recovery_ready([final],stopped)
    assert not final['session_restart_permitted'] # Recovery never changes fault evidence.
    for key in (*clean_keys,'native_outcome','worker_collection_fault','worker_setup_cleanup_error','callback_pending','managed_cleanup_overdue'):
        bad=copy.deepcopy(final);bad.pop(key);assert not manage.recovery_ready([bad],stopped),key
    bad=copy.deepcopy(final);bad['native_outcome']['cleanup']=1;assert not manage.recovery_ready([bad],stopped)
    running=SimpleNamespace(returncode=0,stdout='state = running\n\tpid = 123\n')
    assert not manage.recovery_ready([final],running)
    assert manage.recovery_ready([dict(delegated_controller_registered=True,pid=123)],running)
    assert not manage.recovery_ready([dict(delegated_controller_registered=True,pid=122)],running)
    assert not manage.recovery_ready([final],SimpleNamespace(returncode=5,stdout=''))
    assert not manage.recovery_ready([],stopped)
    boot='00000000-0000-0000-0000-000000000001'
    report=dict(schema=1,boot_uuid=boot,guid='00130e0400000001',status='completed',
        worker=dict(state='closed',restart_blocked=False,completion=dict(error=None,process_exit=0,
            result=dict(exit=0,outcome=dict(reason='completed',transfer=0,evidence=0,cleanup=0)))))
    assert manage.warmup_complete(report,boot)
    assert not manage.warmup_complete(report,'00000000-0000-0000-0000-000000000002')
    for field in ('schema','boot_uuid','guid','status','worker'):
        bad=copy.deepcopy(report);bad.pop(field);assert not manage.warmup_complete(bad,boot)
    for phase in ('checking','waiting','initializing','needs-attention','skipped','failed','cancelled'):
        bad=copy.deepcopy(report);bad['status']=phase;assert not manage.warmup_complete(bad,boot)
    bad=copy.deepcopy(report);bad['worker']['restart_blocked']=True;assert not manage.warmup_complete(bad,boot)
    bad=copy.deepcopy(report);bad['worker']['completion']['result']['outcome']['cleanup']=1;assert not manage.warmup_complete(bad,boot)
    record['installation_id']='b'*32
    _,keeper,_=manage.keep_definition(p,record)
    assert keeper['KeepAlive'] is False and keeper['RunAtLoad'] is True
    assert '--fixture' in keeper['ProgramArguments'] and '--run-keep-active' in keeper['ProgramArguments']
    assert 'Pro40KeepActive' not in keeper['ProgramArguments'] # Validated warm-up gate precedes exec.
    print('PASS installation tamper/refusal, fixed service definitions, and recovery refusal for incomplete cleanup, uncollected workers and stale owners; no services or hardware')

if __name__=='__main__':main()
