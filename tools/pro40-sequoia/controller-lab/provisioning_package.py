"""Prepare immutable provisioning policy for an explicit delegated lab profile."""
import hashlib
import json
import re
from pathlib import Path
import ownership_query_package

SOURCE = Path(__file__).resolve().parent.parent
SERVICE = 'local.pro40.delegated-provision-lab'
PROBE_ID = 'local.pro40.delegated-hal-probe'
APPLE_HELPER = 'identifier "com.apple.audio.Core-Audio-Driver-Service.helper" and anchor apple'

def designated(run, binary):
    result = run('codesign', '-d', '-r-', binary)
    text = result.stdout + result.stderr
    return re.search(r'^designated => (.+)$', text, re.M)[1]

def prepare(run, folder, controller_id, signing_identity, hardware=False, application_trial=False, endurance_trial=False, session_profile=False):
    assert not endurance_trial or (hardware and not application_trial)
    assert not session_profile or not (application_trial or endurance_trial)
    # A signed, never-run empty program obtains codesign's certificate/identifier
    # requirement without a circular controller<->broker CDHash dependency. The
    # final controller/probe MUST match these exact designated requirements.
    stub = folder / 'requirement-stub.cpp'
    stub.write_text('int main(){return 0;}\n')
    requirements = {}
    identities = [('controller', controller_id)] + ([] if hardware else [('probe', PROBE_ID)])
    for name, identifier in identities:
        binary = folder / (name + '-requirement-stub')
        run('xcrun', 'clang++', stub, '-o', binary)
        run('codesign', '--force', '--sign', signing_identity, '--identifier', identifier, binary)
        run('codesign', '--verify', '--strict', binary)
        requirements[name] = designated(run, binary)
        assert 'anchor apple generic' in requirements[name] and 'certificate ' in requirements[name]
        assert 'identifier "' + identifier + '"' in requirements[name]
    # The signed probe is an explicit generated-audio lab peer, never a hardware
    # exception. Actual CoreAudio uses the separate Apple designated requirement.
    helper = APPLE_HELPER if hardware else '(' + APPLE_HELPER + ') or (' + requirements['probe'] + ')'
    service = 'local.pro40.native-provision-lab' if hardware else SERVICE
    if session_profile:service='local.pro40.session-provision' if hardware else 'local.pro40.session-fixture-provision'
    def header(path, values):
        path.write_text('// Generated trusted lab policy.\n' + ''.join('#define ' + key + ' ' + json.dumps(value) + '\n' for key, value in values.items()))
    header(folder / 'DuplexProvisionServicePins.hpp', {
        'PRO40_PROVISION_SERVICE': service,
        'PRO40_PROVISION_CONTROLLER_REQUIREMENT': requirements['controller'],
        'PRO40_PROVISION_HELPER_REQUIREMENT': helper})
    if application_trial or endurance_trial:
        with (folder / 'DuplexProvisionServicePins.hpp').open('a') as file:
            file.write('#define '+('PRO40_APPLICATION_TRIAL' if application_trial else 'PRO40_ENDURANCE_TRIAL')+'\n')
    if session_profile:
        with (folder / 'DuplexProvisionServicePins.hpp').open('a') as file:
            file.write('#define PRO40_SESSION_PROFILE\n')
    broker = folder / 'provisioning-service'
    run('xcrun', 'clang++', '-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I', folder,
        SOURCE / 'DuplexProvisionServiceMain.cpp', '-framework', 'CoreFoundation', '-framework', 'Security', '-o', broker)
    run('codesign', '--force', '--sign', '-', '--identifier', service + '.service', broker)
    run('codesign', '--verify', '--strict', broker)
    metadata = run('codesign', '-d', '--verbose=4', broker).stderr
    cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', metadata, re.M)[1]
    header(folder / 'DuplexProvisionClientPins.hpp', {
        'PRO40_PROVISION_SERVICE': service,
        'PRO40_PROVISION_BROKER_REQUIREMENT': 'cdhash H"' + cdhash + '"',
        'PRO40_PROVISION_HELPER_REQUIREMENT': helper})
    ownership = ownership_query_package.prepare(run, folder, controller_id, requirements['controller'], service, application_trial, endurance_trial, session_profile)
    return dict(service=service, broker=str(broker), broker_cdhash=cdhash, broker_sha256=hashlib.sha256(broker.read_bytes()).hexdigest(),
                controller_requirement=requirements['controller'], probe_requirement=requirements.get('probe'),
                helper_requirement=helper, hardware_allowed=hardware, ownership=ownership, application_trial=application_trial, endurance_trial=endurance_trial, session_profile=session_profile)

def finish(run, folder, controller, signing_identity, policy):
    assert designated(run, controller) == policy['controller_requirement']
    if policy['hardware_allowed']:
        assert policy['helper_requirement'] == APPLE_HELPER and policy['probe_requirement'] is None
        return policy # Real worker may be delegated ONLY to the Apple helper.
    probe = folder / 'delegated-hal-probe'
    run('xcrun', 'clang++', '-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I', folder,
        SOURCE / 'DelegatedHalProcessProbe.cpp', '-framework', 'CoreAudio', '-framework', 'CoreFoundation', '-framework', 'Security', '-lbsm', '-o', probe)
    run('codesign', '--force', '--sign', signing_identity, '--identifier', PROBE_ID, probe)
    run('codesign', '--verify', '--strict', probe)
    assert designated(run, probe) == policy['probe_requirement']
    policy.update(probe=str(probe), probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest())
    for binary in (Path(policy['broker']), probe, controller):
        imports = run('nm', '-u', binary).stdout
        assert not re.search(r'IOFireWire|IOService(Open|GetMatchingService)|AudioDevice(Start|CreateIOProcID)', imports), imports
    return policy
