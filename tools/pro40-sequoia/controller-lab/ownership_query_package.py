"""Trusted package policy for the separate, metadata-only root reader."""
import hashlib
import json
import os
from pathlib import Path
import re

SOURCE = Path(__file__).resolve().parent

def prepare(run, folder, controller_id, requirement, namespace, application_trial=False, endurance_trial=False, session_profile=False):
    assert not (application_trial and endurance_trial)
    assert not session_profile or not (application_trial or endurance_trial)
    assert os.getuid() != 0 and 'anchor apple generic' in requirement and 'certificate ' in requirement
    service = namespace + '.ownership'
    def header(path, values):
        path.write_text('// Generated trusted ownership-query policy.\n'+''.join(
            '#define '+key+' '+json.dumps(value)+'\n' for key,value in values.items()))
    header(folder/'OwnershipQueryServerPins.hpp',dict(PRO40_OWNERSHIP_SERVICE=service,
        PRO40_OWNERSHIP_UID=os.getuid(),PRO40_OWNERSHIP_CONTROLLER_REQUIREMENT=requirement,
        PRO40_OWNERSHIP_APP_ID=controller_id,PRO40_OWNERSHIP_LABEL=controller_id+'.verifier'))
    if application_trial or endurance_trial:
        with (folder/'OwnershipQueryServerPins.hpp').open('a') as file:
            file.write('#define '+('PRO40_APPLICATION_TRIAL' if application_trial else 'PRO40_ENDURANCE_TRIAL')+'\n')
    if session_profile:
        with (folder/'OwnershipQueryServerPins.hpp').open('a') as file:
            file.write('#define PRO40_SESSION_PROFILE\n')
    binary=folder/'ownership-query-service'
    run('xcrun','clang++','-std=c++20','-fobjc-arc','-O2','-g','-Wall','-Wextra','-Werror','-I',folder,
        SOURCE/'OwnershipQueryService.mm','-framework','Foundation','-framework','Security','-lbsm','-o',binary)
    run('codesign','--force','--sign','-','--identifier',service+'.service',binary)
    run('codesign','--verify','--strict',binary)
    metadata=run('codesign','-d','--verbose=4',binary).stderr
    cdhash=re.search(r'^CDHash=([0-9a-f]{40})$',metadata,re.M)[1]
    header(folder/'OwnershipQueryClientPins.hpp',dict(PRO40_OWNERSHIP_SERVICE=service,
        PRO40_OWNERSHIP_SERVER_REQUIREMENT='cdhash H"'+cdhash+'"'))
    imports=run('nm','-u',binary).stdout
    assert not re.search(r'IOFireWire|IOService(Open|GetMatchingService)|AudioDevice(Start|CreateIOProcID)',imports)
    return dict(service=service,binary=str(binary),cdhash=cdhash,sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
        client_uid=os.getuid(),controller_requirement=requirement,controller_id=controller_id,
        hardware_allowed=False,operation='driver-scoped background-service observation')
