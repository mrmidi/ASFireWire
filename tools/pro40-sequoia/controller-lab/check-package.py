#!/usr/bin/env python3
"""Build and statically verify a sealed lab bundle; never launch or register it."""
import json
import argparse
import os
import re
import plistlib
import shutil
import subprocess
import tempfile
import provisioning_package
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = Path(__file__).resolve().parent
APP_ID = "local.pro40.controller-lab"
HELPER_ID = APP_ID + ".verifier"
PROGRAM = "Contents/MacOS/Pro40VerifierLab"
LOG = []


def run(*args, expect_success=True):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True,
                            timeout=120)
    LOG.append(dict(argv=list(map(str, args)), exit=result.returncode,
                    stdout=result.stdout, stderr=result.stderr))
    if expect_success and result.returncode != 0:
        raise RuntimeError(f"Command failed: {args}: {result.stderr}")
    return result


def write_plist(path, value):
    path.write_bytes(plistlib.dumps(value, sort_keys=True))


def definition(profile):
    agent = {"Label": HELPER_ID, "BundleProgram": PROGRAM,
             "RunAtLoad": False, "KeepAlive": False}
    if profile == "on-demand":
        agent.update(ProgramArguments=["Pro40VerifierLab", "--serve-verifier"],
                     MachServices={HELPER_ID: True})
    return agent


def verify_signing_metadata(metadata, team=None):
    adhoc = "Signature=adhoc" in metadata or "flags=0x2(adhoc)" in metadata
    if team is None:
        assert adhoc, "Expected local ad-hoc signing"
    else:
        assert not adhoc, "Development signing must not fall back to ad-hoc"
        assert f"TeamIdentifier={team}\n" in metadata, "Unexpected signing team"
        assert re.search(r"^Authority=Apple Development: .+$", metadata, re.M), "Expected Apple Development certificate"


def check(app, profile, team=None, worker_pin=None, managed_stub=False, device_state_pin=None):
    contents = app / "Contents"
    info = plistlib.loads((contents / "Info.plist").read_bytes())
    agent = plistlib.loads((contents / "Library/LaunchAgents" /
                           (HELPER_ID + ".plist")).read_bytes())
    assert info["CFBundleIdentifier"] == APP_ID
    assert agent == definition(profile)
    assert type(agent["RunAtLoad"]) is bool and type(agent["KeepAlive"]) is bool
    if profile == "on-demand":
        assert type(agent["MachServices"][HELPER_ID]) is bool
    assert not any(p.is_symlink() for p in app.rglob("*"))
    helper = app / agent["BundleProgram"]
    controller = contents / "MacOS" / info["CFBundleExecutable"]
    hashes = {}
    binaries_to_check=[(helper, HELPER_ID), (controller, APP_ID)]
    if worker_pin:binaries_to_check.append((contents/"MacOS/Pro40DiagnosticWorker",APP_ID+".diagnostic-worker"))
    if device_state_pin:binaries_to_check.append((contents/"MacOS/Pro40DeviceState",APP_ID+".device-state"))
    for binary, identifier in binaries_to_check:
        assert binary.resolve().is_relative_to(app.resolve())
        assert binary.is_file()
        run("/usr/bin/codesign", "--verify", "--strict", binary)
        metadata = run("/usr/bin/codesign", "-d", "--verbose=4", binary).stderr
        hashes[identifier] = re.search(r"^CDHash=([0-9a-f]{40})$", metadata, re.M).group(1)
        assert f"Identifier={identifier}\n" in metadata
        verify_signing_metadata(metadata, team)
        # First line is the inspected filename (our repository contains FireWire).
        dependencies = run("/usr/bin/otool", "-L", binary).stdout.splitlines()[1:]
        allowed = ("/usr/lib/libSystem.B.dylib", "/usr/lib/libobjc.A.dylib",
                   "/usr/lib/libbsm.0.dylib",
                   "/usr/lib/libc++.1.dylib",
                   "/System/Library/Frameworks/Security.framework/",
                   "/System/Library/Frameworks/ServiceManagement.framework/",
                   "/System/Library/Frameworks/AppKit.framework/",
                   "/System/Library/Frameworks/CoreFoundation.framework/",
                   "/System/Library/Frameworks/Foundation.framework/")
        allowed+=("/System/Library/Frameworks/CoreAudio.framework/",)
        if (managed_stub and binary.name=="Pro40DiagnosticWorker") or binary.name=="Pro40DeviceState":allowed+=("/System/Library/Frameworks/IOKit.framework/",)
        assert dependencies
        assert all(line.strip().startswith(allowed) for line in dependencies)
    if worker_pin:assert hashes[APP_ID+".diagnostic-worker"]==worker_pin
    if device_state_pin:assert hashes[APP_ID+".device-state"]==device_state_pin
    run("/usr/bin/codesign", "--verify", "--deep", "--strict", app)
    metadata = run("/usr/bin/codesign", "-d", "--verbose=4", app).stderr
    assert "Info.plist=not bound" not in metadata
    assert "Sealed Resources version=" in metadata
    assert (contents / "_CodeSignature/CodeResources").is_file()
    assert str(ROOT).encode() not in (contents / "Info.plist").read_bytes()
    return {"app": str(app), "controller_identifier": APP_ID,
            "helper_identifier": HELPER_ID, "bundle_program": PROGRAM,
            "static_verification": "passed", "controller_cdhash": hashes[APP_ID],
            "helper_cdhash": hashes[HELPER_ID], "signing_team": team,
            "signing_mode": "apple-development" if team else "adhoc"}


def main():
    global APP_ID, HELPER_ID
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=["dormant", "on-demand"], default="dormant")
    parser.add_argument("--enable-verifier-experiment", action="store_true")
    parser.add_argument("--signing-identity", help="Exact SHA-1 of an available Apple Development identity")
    parser.add_argument("--team-identifier", help="Expected ten-character certificate team identifier")
    parser.add_argument("--signed-namespace", action="store_true", help="Build the isolated full verifier identity; requires Apple Development signing")
    parser.add_argument("--original-peer-cdhash", help="Embed an independently measured original-peer executable pin for preparation checks")
    parser.add_argument("--enable-original-peer-diagnostic", action="store_true", help="Enable the explicitly invoked, hardware-free real-peer diagnostic")
    parser.add_argument("--enable-gated-worker-diagnostic", action="store_true", help="Build and pin the bundled fake-runner worker; never launch/register it")
    parser.add_argument("--reject-gated-worker-identity", action="store_true", help="Negative diagnostic only: embed a deliberately nonmatching worker requirement")
    parser.add_argument("--managed-socket-stub", action="store_true", help="Bundle the real session runner with NativeStub and managed socket admission; no hardware")
    parser.add_argument("--managed-timed-stub", action="store_true", help="Select the timed managed NativeStub command; requires managed socket stub")
    parser.add_argument("--duplex-fixture", action="store_true", help="Build the isolated signed duplex controller with generated native packets; no hardware")
    parser.add_argument("--duplex-delegated", action="store_true", help="Build authenticated HAL provisioning for an explicit duplex profile; hardware permits only the Apple helper")
    parser.add_argument("--application-trial", action="store_true", help="Allow ten minutes of idle app setup; keep the existing120-second audio-session limit")
    parser.add_argument("--endurance-trial", action="store_true", help="Separate ten-minute delegated hardware test with a660-second run envelope")
    parser.add_argument("--session-profile", action="store_true", help="Delegated app-controlled sessions; retain admission/cleanup limits, no diagnostic run timeout")
    parser.add_argument("--duplex-hardware", action="store_true", help="Build a signed 90-second real duplex profile; never launch hardware during packaging")
    parser.add_argument("--duplex-loopback", action="store_true", help="With hardware duplex, select 24-second output6/input7 cable calibration instead of speaker tones")
    parser.add_argument("--managed-real-inspect", action="store_true", help="Sign and inspect the real managed worker; all worker launch commands disabled")
    parser.add_argument("--managed-real-recording", action="store_true", help="Build a separately gated finite real recording profile; never run it during packaging")
    parser.add_argument("--real-guid", help="Exact nonzero 16-digit hardware GUID for the real recording profile")
    parser.add_argument("--real-record-timing", action="store_true", help="Add existing timing/active-route observations to the bounded real recording")
    parser.add_argument("--real-post-release-peaks", action="store_true", help="Opt in to finite post-release peak observation; requires recording and timing")
    parser.add_argument("--real-pre-close-quiet-hold", action="store_true", help="Explicit three-second quiet hold; requires post-release peaks")
    parser.add_argument("--real-owned-idle-observation", action="store_true", help="Observe before ownership release; requires explicit quiet hold")
    parser.add_argument("--real-assigned-idle-observation", action="store_true", help="Observe disabled streams before channel restoration; requires owned idle")
    parser.add_argument("--real-existing-clock-only", action="store_true", help="Verify existing internal48k without rewriting; requires assigned idle")
    parser.add_argument("--real-clock-excursion", action="store_true", help="Finite 48/44.1/48 clock comparison; requires enable-first and assigned idle")
    parser.add_argument("--real-enable-first", action="store_true", help="Finite enable-before-host-start experiment; requires assigned idle and clock refresh")
    args=parser.parse_args();profile=args.profile
    if args.session_profile and (not args.duplex_delegated or args.application_trial or args.endurance_trial):
        parser.error("session profile requires delegated duplex and excludes application/endurance trials")
    if args.application_trial and not (args.duplex_delegated and args.duplex_hardware):
        parser.error("application trial requires the explicit delegated hardware profile")
    if args.endurance_trial and not (args.duplex_delegated and args.duplex_hardware and not args.application_trial):
        parser.error("endurance trial requires the separate delegated hardware profile, without application trial")
    if args.duplex_delegated and ((args.duplex_fixture == args.duplex_hardware) or args.reject_gated_worker_identity or args.duplex_loopback):
        parser.error("delegated provisioning requires exactly one explicit duplex profile, without deliberate identity rejection or controller-rendered loopback")
    if args.duplex_loopback and not args.duplex_hardware:parser.error("loopback requires explicit hardware duplex")
    if args.duplex_hardware and (args.duplex_fixture or not args.enable_gated_worker_diagnostic or not args.signed_namespace or
            args.managed_socket_stub or args.managed_timed_stub or args.managed_real_inspect or args.managed_real_recording or args.reject_gated_worker_identity):
        parser.error("hardware duplex requires signed gated worker and excludes other worker profiles")
    if args.duplex_fixture and (not args.enable_gated_worker_diagnostic or not args.signed_namespace or
            args.managed_socket_stub or args.managed_timed_stub or args.managed_real_inspect or args.managed_real_recording):
        parser.error("duplex fixture requires a signed gated worker and excludes other worker profiles")
    if args.real_clock_excursion and (not args.real_enable_first or not args.real_assigned_idle_observation or args.real_existing_clock_only):parser.error("clock excursion requires enable-first and assigned idle, and excludes existing-clock-only")
    if args.real_enable_first and (not args.real_assigned_idle_observation or args.real_existing_clock_only):parser.error("enable-first requires assigned idle and excludes existing-clock-only")
    if args.real_existing_clock_only and not args.real_assigned_idle_observation:parser.error("existing clock requires assigned idle")
    if args.real_assigned_idle_observation and not args.real_owned_idle_observation:parser.error("assigned observation requires owned idle")
    if args.real_owned_idle_observation and not args.real_pre_close_quiet_hold:parser.error("owned observation requires quiet hold")
    if args.real_pre_close_quiet_hold and not args.real_post_release_peaks:parser.error("quiet hold requires post-release peaks")
    if args.real_post_release_peaks and not args.real_record_timing:parser.error("post-release peaks require real record timing")
    if args.real_record_timing and not args.managed_real_recording:
        parser.error("real timing observations require real recording profile")
    real_worker=args.managed_real_inspect or args.managed_real_recording or args.duplex_hardware
    if args.managed_real_recording and args.managed_real_inspect:
        parser.error("real recording and inspection are separate profiles")
    if bool(args.real_guid)!=(args.managed_real_recording or args.duplex_hardware) or (args.real_guid and (not re.fullmatch(r"[0-9A-Fa-f]{16}",args.real_guid) or not int(args.real_guid,16))):
        parser.error("real recording requires an exact nonzero 16-digit GUID; other profiles forbid it")
    if real_worker and (not args.enable_gated_worker_diagnostic or args.managed_socket_stub or args.managed_timed_stub or args.reject_gated_worker_identity):
        parser.error("real inspection requires gated worker and excludes stub/rejection profiles")
    if args.managed_timed_stub and not args.managed_socket_stub:
        parser.error("managed timed stub requires managed socket stub")
    if args.managed_socket_stub and not args.enable_gated_worker_diagnostic:
        parser.error("managed socket stub requires gated worker diagnostic")
    if args.reject_gated_worker_identity and not args.enable_gated_worker_diagnostic:
        parser.error("identity rejection requires the gated fake-worker diagnostic")
    if args.enable_gated_worker_diagnostic:
        if args.enable_verifier_experiment or args.original_peer_cdhash or not args.enable_original_peer_diagnostic or not args.signed_namespace or profile!="on-demand":
            parser.error("gated worker requires signed on-demand original-peer diagnostic and generates its own worker pin")
    if args.enable_original_peer_diagnostic and ((not args.original_peer_cdhash and not args.enable_gated_worker_diagnostic) or not args.signed_namespace or profile!="on-demand"):
        parser.error("original-peer diagnostic requires an original pin, signed namespace and on-demand profile")
    if args.original_peer_cdhash:
        if not re.fullmatch(r"[0-9a-f]{40}",args.original_peer_cdhash) or not int(args.original_peer_cdhash,16):
            parser.error("original peer pin must be 40 lowercase hexadecimal digits and nonzero")
        if args.enable_verifier_experiment:
            parser.error("original-peer package session wiring is not enabled yet; use preparation checks only")
    if args.signed_namespace:
        if not args.signing_identity:parser.error("signed namespace requires Apple Development signing")
        APP_ID="local.pro40.signed-controller-lab"
        if args.duplex_fixture:APP_ID="local.pro40.duplex-controller-lab"
        if args.duplex_hardware:APP_ID="local.pro40.duplex-hardware-lab"
        if args.session_profile:APP_ID="local.pro40.session-controller" if args.duplex_hardware else "local.pro40.session-fixture-controller"
        HELPER_ID=APP_ID+".verifier"
    identity_flags=["-DPRO40_LAB_SIGNED_NAMESPACE"] if args.signed_namespace else []
    if args.duplex_fixture:identity_flags.append("-DPRO40_LAB_DUPLEX_NAMESPACE")
    if args.duplex_hardware:identity_flags.append("-DPRO40_LAB_DUPLEX_HARDWARE_NAMESPACE")
    if args.session_profile:identity_flags.append("-DPRO40_LAB_SESSION_NAMESPACE")
    if args.original_peer_cdhash:
        identity_flags.append('-DPRO40_LAB_ORIGINAL_PEER_CDHASH="'+args.original_peer_cdhash+'"')
    if bool(args.signing_identity) != bool(args.team_identifier):
        parser.error("signing identity and team identifier must be supplied together")
    if args.signing_identity and (not re.fullmatch(r"[0-9A-Fa-f]{40}",args.signing_identity) or
                                 not re.fullmatch(r"[A-Z0-9]{10}",args.team_identifier)):
        parser.error("invalid signing identity or team identifier")
    signing_identity=args.signing_identity or "-"
    if args.signing_identity:
        available=run("/usr/bin/security","find-identity","-v","-p","codesigning").stdout
        assert re.search(r'^\s*\d+\) '+re.escape(args.signing_identity.upper())+r' "Apple Development: ',available,re.M), "Development identity is not available and valid"

    if args.enable_verifier_experiment and profile != "on-demand":
        parser.error("verifier experiment requires on-demand profile")
    experiment_flags=["-DPRO40_ENABLE_VERIFIER_EXPERIMENT"] if args.enable_verifier_experiment else []
    if args.enable_original_peer_diagnostic:experiment_flags.append("-DPRO40_ENABLE_ORIGINAL_PEER_DIAGNOSTIC")
    if args.enable_gated_worker_diagnostic:experiment_flags.append("-DPRO40_ENABLE_GATED_WORKER_DIAGNOSTIC")
    if args.managed_socket_stub:experiment_flags.append("-DPRO40_ENABLE_MANAGED_SOCKET_DIAGNOSTIC")
    if args.managed_timed_stub:experiment_flags.append("-DPRO40_ENABLE_MANAGED_TIMED_DIAGNOSTIC")
    if args.duplex_fixture:experiment_flags.extend(["-DPRO40_ENABLE_DUPLEX_FIXTURE","-DPRO40_ENABLE_MANAGED_SOCKET_DIAGNOSTIC","-DPRO40_ENABLE_MANAGED_TIMED_DIAGNOSTIC"])
    if args.duplex_delegated:experiment_flags.append("-DPRO40_ENABLE_DUPLEX_DELEGATED")
    if args.application_trial:experiment_flags.append("-DPRO40_APPLICATION_TRIAL")
    if args.endurance_trial:experiment_flags.append("-DPRO40_ENDURANCE_TRIAL")
    if args.session_profile:experiment_flags.append("-DPRO40_SESSION_PROFILE")
    if args.duplex_hardware:experiment_flags.extend(["-DPRO40_ENABLE_DUPLEX_HARDWARE","-DPRO40_ENABLE_MANAGED_SOCKET_DIAGNOSTIC","-DPRO40_ENABLE_MANAGED_TIMED_DIAGNOSTIC","-DPRO40_MANAGED_REAL_GUID=0x"+args.real_guid.lower()+"ULL"])
    if args.duplex_loopback:experiment_flags.append("-DPRO40_ENABLE_DUPLEX_LOOPBACK")
    if args.managed_real_inspect:experiment_flags.append("-DPRO40_MANAGED_REAL_INSPECT")
    if args.managed_real_recording:
        experiment_flags.extend(["-DPRO40_ENABLE_MANAGED_SOCKET_DIAGNOSTIC", "-DPRO40_ENABLE_MANAGED_TIMED_DIAGNOSTIC",
            "-DPRO40_ENABLE_MANAGED_REAL_RECORDING", "-DPRO40_MANAGED_REAL_GUID=0x"+args.real_guid.lower()+"ULL"])
    if args.real_clock_excursion:experiment_flags.append("-DPRO40_MANAGED_REAL_CLOCK_EXCURSION")
    if args.real_enable_first:experiment_flags.append("-DPRO40_MANAGED_REAL_ENABLE_FIRST")
    if args.real_existing_clock_only:experiment_flags.append("-DPRO40_MANAGED_REAL_EXISTING_CLOCK")
    if args.real_assigned_idle_observation:experiment_flags.append("-DPRO40_MANAGED_REAL_ASSIGNED_IDLE")
    if args.real_owned_idle_observation:experiment_flags.append("-DPRO40_MANAGED_REAL_OWNED_IDLE")
    if args.real_pre_close_quiet_hold:experiment_flags.append("-DPRO40_MANAGED_REAL_QUIET_HOLD")
    if args.real_post_release_peaks:experiment_flags.append("-DPRO40_MANAGED_REAL_POST_RELEASE")
    if args.real_record_timing:experiment_flags.append("-DPRO40_MANAGED_REAL_RECORD_TIMING")
    base = ROOT / "build/pro40-sequoia/controller-lab"
    base.mkdir(parents=True, exist_ok=True)
    # Each attempt owns a fresh directory; no stale code or destructive cleanup.
    attempt = Path(tempfile.mkdtemp(prefix="package-", dir=base))
    report = {"attempt": str(attempt), "profile": profile, "launched": False, "registered": False,
              "on_demand_lifecycle_enabled": False, "signed_namespace": args.signed_namespace,"duplex_fixture":args.duplex_fixture,"duplex_hardware":args.duplex_hardware,
              "duplex_hardware_blocks":(8409 if args.duplex_loopback else 31533) if args.duplex_hardware else 0,
              "duplex_loopback":args.duplex_loopback,
              "original_peer_cdhash": args.original_peer_cdhash, "original_peer_session_enabled": False,
              "original_peer_diagnostic_enabled": args.enable_original_peer_diagnostic,
              "gated_worker_diagnostic_enabled": args.enable_gated_worker_diagnostic,
              "diagnostic_identity_rejection": args.reject_gated_worker_identity, "managed_socket_stub": args.managed_socket_stub, "managed_timed_stub": args.managed_timed_stub, "managed_real_inspect": args.managed_real_inspect,"managed_real_recording":args.managed_real_recording,"real_guid":args.real_guid,"real_record_timing":args.real_record_timing,"real_post_release_peaks":args.real_post_release_peaks,"real_pre_close_quiet_hold":args.real_pre_close_quiet_hold,"real_owned_idle_observation":args.real_owned_idle_observation,"real_assigned_idle_observation":args.real_assigned_idle_observation,"real_existing_clock_only":args.real_existing_clock_only,"real_enable_first":args.real_enable_first,"real_clock_excursion":args.real_clock_excursion}
    try:
        if args.duplex_delegated:
            report['provisioning']=provisioning_package.prepare(run,attempt,APP_ID,signing_identity,hardware=args.duplex_hardware,application_trial=args.application_trial,endurance_trial=args.endurance_trial,session_profile=args.session_profile)
            experiment_flags.extend(['-I',str(attempt)])
        report['duplex_delegated']=args.duplex_delegated
        report['application_trial']=args.application_trial
        report['endurance_trial']=args.endurance_trial
        report['session_profile']=args.session_profile
        app = attempt / "Pro40 Controller Lab.app"
        contents = app / "Contents"
        binaries = contents / "MacOS"
        agents = contents / "Library/LaunchAgents"
        binaries.mkdir(parents=True)
        agents.mkdir(parents=True)
        write_plist(contents / "Info.plist", {
            "CFBundleIdentifier": APP_ID, "CFBundleExecutable": "Pro40ControllerLab",
            "CFBundleName": "Pro40 Controller Lab", "CFBundlePackageType": "APPL",
            "CFBundleVersion": "1", "CFBundleShortVersionString": "0.1",
            "LSMinimumSystemVersion": "15.0", "NSHighResolutionCapable": True})
        write_plist(agents / (HELPER_ID + ".plist"), definition(profile))
        worker_pin=None
        device_state_pin=None
        if args.session_profile and args.duplex_hardware:
            state_binary=binaries/"Pro40DeviceState"
            run("xcrun","clang++","-std=c++20","-O2","-Wall","-Wextra","-Werror","-Wl,-dead_strip",
                "-DPRO40_MANAGED_REAL_GUID=0x"+args.real_guid.lower()+"ULL",SOURCE/"DeviceStateMain.cpp",
                SOURCE.parent/"NativeFireWire.cpp","-framework","IOKit","-framework","CoreFoundation","-o",state_binary)
            run("codesign","--force","--sign",signing_identity,"--identifier",APP_ID+".device-state",state_binary)
            metadata=run("codesign","-d","--verbose=4",state_binary).stderr
            verify_signing_metadata(metadata,args.team_identifier)
            device_state_pin=re.search(r"^CDHash=([0-9a-f]{40})$",metadata,re.M).group(1)
            (attempt/"SessionDeviceStatePins.hpp").write_text('#define PRO40_DEVICE_STATE_REQUIREMENT '+json.dumps('cdhash H"'+device_state_pin+'"')+'\n')
            symbols=run("nm",state_binary).stdout
            assert "_pro40_state" in symbols and "_pro40_open" in symbols
            assert not re.search(r"_pro40_(read|set_idle_rate|native_interface|clock_state)\b",symbols)
            refused=run(state_binary,"--invalid",expect_success=False)
            assert refused.returncode==64 and not refused.stdout and not refused.stderr
            report["device_state"]={"binary":str(state_binary),"cdhash":device_state_pin,"hardware_executed":False,"fixed_guid":args.real_guid.lower()}
        if args.enable_gated_worker_diagnostic:
            worker=binaries/"Pro40DiagnosticWorker"
            if args.duplex_fixture:
                run("xcrun","clang++","-std=c++20","-O2","-g","-Wall","-Wextra","-Werror",
                    "-DPRO40_DUPLEX_WORKER","-DPRO40_MANAGED_WORKER","-DPRO40_LIVE_PLAYBACK","-DPRO40_SUSTAINED_INPUT","-DPRO40_DMA_SUSTAINED","-DPRO40_WORKER_TEST_LOCKS",
                    *(["-DPRO40_DELEGATED_FIXTURE"] if args.duplex_delegated else []),
                    *(["-DPRO40_SESSION_PROFILE"] if args.session_profile else []),
                    SOURCE.parent/"worker/NativeWorker.cpp",SOURCE.parent/"worker/DuplexNativeFixture.cpp",
                    ROOT/"ASFWDriver/Audio/Wire/IEC61883/CipHeader.cpp",ROOT/"ASFWDriver/Audio/Wire/IEC61883/DbcCounter.cpp",
                    "-framework","CoreFoundation","-lbsm","-o",worker)
            elif real_worker:
                inspection=run("python3",SOURCE.parent/"worker/check-managed-real-build.py",*(["--duplex"] if args.duplex_hardware else []))
                inspected=json.loads(inspection.stdout)
                assert inspected["result"]=="passed" and not inspected["executed"]
                report["real_worker_inspection"]=inspected
                shutil.copyfile(inspected["binary"],worker)
                worker.chmod(0o755)
            elif args.managed_socket_stub:
                run("bash",SOURCE.parent/"build-transmit-return.sh","--reuse-queue-managed-worker-package-stub")
                shutil.copyfile(ROOT/"build/pro40-sequoia/pro40-managed-worker-package-stub",worker)
                worker.chmod(0o755)
            else:
                run("xcrun","clang++","-std=c++20","-O2","-Wall","-Wextra","-Werror",
                    "-DPRO40_WORKER_TEST_LOCKS","-DPRO40_WORKER_ADMISSION_TEST","-DPRO40_WORKER_CAPTURE_TEST","-DPRO40_WORKER_NATIVE_DIAGNOSTIC_TEST",
                    SOURCE.parent/"worker/NativeWorker.cpp",SOURCE.parent/"worker/WorkerAdmissionTestGate.cpp",
                    SOURCE.parent/"worker/LivenessFixture.cpp","-o",worker)
            run("/usr/bin/codesign","--force","--sign",signing_identity,"--identifier",APP_ID+".diagnostic-worker",worker)
            metadata=run("/usr/bin/codesign","-d","--verbose=4",worker).stderr
            verify_signing_metadata(metadata,args.team_identifier)
            worker_pin=re.search(r"^CDHash=([0-9a-f]{40})$",metadata,re.M).group(1)
            policy_pin=worker_pin
            if args.reject_gated_worker_identity:
                policy_pin=format(int(worker_pin[0],16)^1,'x')+worker_pin[1:]
                assert policy_pin!=worker_pin and int(policy_pin,16)
            args.original_peer_cdhash=policy_pin
            identity_flags.append('-DPRO40_LAB_ORIGINAL_PEER_CDHASH="'+policy_pin+'"')
            symbols=run("/usr/bin/nm","-u",worker).stdout
            if real_worker:assert "_IOServiceGetMatchingServices" in symbols
            else:assert not any(name in symbols for name in ("IOService", "IOConnect", "IOFireWire"))
            text=run("/usr/bin/strings",worker).stdout
            if real_worker:
                assert "--managed" in text and "/Library/Application Support/ASFW-Pro40-Workers" in text
                for marker in ("PRO40_WORKER_TEST_LOCK_DIR","PRO40_ADMISSION_TEST_FD","test-native-open: no hardware","fixture-entered"):
                    assert marker not in text
            elif args.duplex_fixture:
                assert "generated-native-entry" in text and "--duplex-session" in text and "--managed" in text
            elif args.managed_socket_stub:
                assert "test-native-open: no hardware" in text and "--managed" in text
                assert "fixture-entered" not in text
            else:assert "fixture-entered" in text and "fixture-cleanup stop=" in text
            report["original_peer_cdhash"]=policy_pin
            report["worker"]={"path":str(worker),"cdhash":worker_pin,"fake_runner":not (args.managed_socket_stub or real_worker or args.duplex_fixture),"native_stub":args.managed_socket_stub,"duplex_fixture":args.duplex_fixture,"duplex_hardware":args.duplex_hardware,"real_inspection_only":args.managed_real_inspect,"hardware_symbols_absent":not real_worker}
        connection_object = attempt / "NativeVerifierConnection.o"
        adapter_object = attempt / "NativeDevelopmentConnection.o"
        server_object = attempt / "NativeVerifierServer.o"
        server_adapter_object = attempt / "NativeDevelopmentServer.o"
        run("xcrun", "clang++", "-std=c++20", *identity_flags, "-Wall", "-Wextra", "-Werror", "-O2",
            "-fblocks", "-mmacosx-version-min=15.0", "-c", SOURCE / "NativeVerifierServer.cpp",
            "-o", server_object)
        run("xcrun", "clang++", "-std=c++20", *identity_flags, "-Wall", "-Wextra", "-Werror", "-O2",
            "-fobjc-arc", "-mmacosx-version-min=15.0", "-c", SOURCE / "NativeDevelopmentServer.mm",
            "-o", server_adapter_object)
        run("xcrun", "clang++", "-std=c++20", *identity_flags, "-Wall", "-Wextra", "-Werror", "-O2",
            "-fblocks", "-mmacosx-version-min=15.0", "-c", SOURCE / "NativeVerifierConnection.cpp",
            "-o", connection_object)
        run("xcrun", "clang++", "-std=c++20", *identity_flags, "-Wall", "-Wextra", "-Werror", "-O2",
            "-fobjc-arc", "-mmacosx-version-min=15.0", "-c", SOURCE / "NativeDevelopmentConnection.mm",
            "-o", adapter_object)
        run("xcrun", "clang++", "-std=c++20", *identity_flags, "-Wall", "-Wextra", "-Werror", "-O2",
            "-mmacosx-version-min=15.0", "-fobjc-arc", "-framework", "AppKit",
            "-framework", "Security", "-framework", "ServiceManagement", "-lbsm",
            *experiment_flags, SOURCE / "Controller.mm", SOURCE / "RegistrationJournal.mm",
            SOURCE / "DevelopmentControllerCommand.mm",
            SOURCE / "NativeRegistrationCollector.mm",
            connection_object, adapter_object,
            "-o", binaries / "Pro40ControllerLab")
        run("xcrun", "clang++", "-std=c++20", *identity_flags, "-Wall", "-Wextra", "-Werror", "-O2",
            "-mmacosx-version-min=15.0", "-fobjc-arc", "-framework", "Foundation",
            "-framework", "Security", *experiment_flags, SOURCE / "Verifier.mm", connection_object, adapter_object,
            server_object, server_adapter_object,
            "-o", app / PROGRAM)
        run("/usr/bin/codesign", "--force", "--sign", signing_identity, "--identifier",
            HELPER_ID, app / PROGRAM)
        run("/usr/bin/codesign", "--force", "--sign", signing_identity, app)
        report["original"] = check(app, profile, args.team_identifier, worker_pin, args.managed_socket_stub or real_worker, device_state_pin)
        if args.duplex_delegated:
            report['provisioning']=provisioning_package.finish(run,attempt,binaries/'Pro40ControllerLab',signing_identity,report['provisioning'])
        if not args.enable_gated_worker_diagnostic:
            disabled=run(binaries/"Pro40ControllerLab","--run-gated-worker-diagnostic",expect_success=False)
            assert disabled.returncode==78 and "not enabled" in disabled.stderr
            report["gated_worker_default_refusal_checked"]=True
        refused_command="--run-gated-worker-diagnostic" if args.managed_socket_stub else "--run-managed-socket-diagnostic"
        disabled=run(binaries/"Pro40ControllerLab",refused_command,expect_success=False)
        assert disabled.returncode==78 and "not enabled" in disabled.stderr
        report["alternate_worker_mode_refused"]=True
        timing_refused="--run-managed-socket-diagnostic" if args.managed_timed_stub else "--run-managed-timed-diagnostic"
        disabled=run(binaries/"Pro40ControllerLab",timing_refused,expect_success=False)
        assert disabled.returncode==78 and "not enabled" in disabled.stderr
        report["alternate_timing_mode_refused"]=True
        if not args.managed_real_recording:
            refused=run(binaries/"Pro40ControllerLab","--run-managed-real-recording",expect_success=False)
            assert refused.returncode==78 and "not enabled" in refused.stderr
            report["real_recording_refused"]=True
        else:
            for command in ("--run-gated-worker-diagnostic","--check-gated-worker-terminal-stop","--run-managed-socket-diagnostic","--run-managed-timed-diagnostic"):
                refused=run(binaries/"Pro40ControllerLab",command,expect_success=False)
                assert refused.returncode==78 and "not enabled" in refused.stderr
            report["diagnostic_commands_refused"]=True
            report["real_recording_executed"]=False
        if args.managed_real_inspect or args.duplex_hardware:
            for command in ("--run-gated-worker-diagnostic","--check-gated-worker-terminal-stop","--run-managed-socket-diagnostic","--run-managed-timed-diagnostic"):
                refused=run(binaries/"Pro40ControllerLab",command,expect_success=False)
                assert refused.returncode==78 and "not enabled" in refused.stderr
            report["all_worker_commands_refused"]=True
        if args.duplex_hardware:
            disabled=run(binaries/"Pro40ControllerLab","--run-managed-duplex-diagnostic","invalid",expect_success=False)
            assert disabled.returncode==78 and "not enabled" in disabled.stderr
            report["generated_duplex_refused"]=True
        else:
            disabled=run(binaries/"Pro40ControllerLab","--run-managed-duplex-hardware","invalid","0",expect_success=False)
            assert disabled.returncode==78 and "not enabled" in disabled.stderr
            report["hardware_duplex_refused"]=True
        report["verifier_experiment_enabled"] = args.enable_verifier_experiment
        # Local provisioning boundary outside the app seal: generated only AFTER
        # final signing, with no circular embedded code-hash dependency.
        pins = ("v1\n" + report["original"]["controller_cdhash"] + "\n" +
                report["original"]["helper_cdhash"] + "\n").encode("ascii")
        pin_path = attempt / "development-peer-pins.txt"
        fd = os.open(pin_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as output:
            output.write(pins)
            output.flush()
            os.fsync(output.fileno())
        assert pin_path.stat().st_mode & 0o777 == 0o600
        assert attempt.stat().st_mode & 0o777 == 0o700
        assert pin_path.read_bytes() == pins
        report["development_pins"] = {"path": str(pin_path), "format": 1,
            "shipping_trust_root": False, "runtime_connected": False}
        run("/usr/bin/codesign", "--verify", "--deep", "--strict", app)
        if args.original_peer_cdhash:
            report["original_peer_policy_checks"]={}
            for name in ["Pro40ControllerLab","Pro40VerifierLab"]:
                checked=run(binaries/name,"--check-original-peer-policy")
                value=json.loads(checked.stdout)
                assert value["self_validated"] and value["enabled"] and value["passed"] and value["checks"]==6
                assert not value["original_peer_authenticated"] and not value["service_mutation_attempted"] and not value["audio_admitted"]
                report["original_peer_policy_checks"][name]=value
            report["original_peer_preparation_checks"]={}
            for name in ["Pro40ControllerLab","Pro40VerifierLab"]:
                checked=run(binaries/name,"--check-verifier-preparation",expect_success=False)
                value=json.loads(checked.stdout)
                helper=name=="Pro40VerifierLab"
                assert checked.returncode==(0 if helper else 2)
                assert value["prepared"]==helper and value["connections_finalized"]==2 and value["accepted_peers"]==0
                assert not value["peer_authenticated"] and not value["service_mutation_attempted"] and not value["audio_admitted"]
                report["original_peer_preparation_checks"][name]=value
            if not args.enable_original_peer_diagnostic:
                disabled=run(binaries/"Pro40VerifierLab","--serve-verifier",expect_success=False)
                assert disabled.returncode==78 and "not enabled" in disabled.stderr
            report["original_peer_live_activation_blocked"]=not args.enable_original_peer_diagnostic
        relocated = attempt / "copied location" / app.name
        shutil.copytree(app, relocated)
        report["relocated"] = check(relocated, profile, args.team_identifier, worker_pin, args.managed_socket_stub or real_worker, device_state_pin)
        tampered = attempt / "tampered copy" / app.name
        shutil.copytree(app, tampered)
        plist = tampered / "Contents/Library/LaunchAgents" / (HELPER_ID + ".plist")
        value = plistlib.loads(plist.read_bytes())
        value["RunAtLoad"] = True
        write_plist(plist, value)
        rejected = run("/usr/bin/codesign", "--verify", "--deep", "--strict",
                       tampered, expect_success=False)
        assert rejected.returncode != 0, "Modified agent definition was accepted"
        report["tampered_agent_rejected"] = True
        report["result"] = "passed"
    finally:
        report["commands"] = LOG
        (attempt / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(attempt / "results.json")


if __name__ == "__main__":
    main()
