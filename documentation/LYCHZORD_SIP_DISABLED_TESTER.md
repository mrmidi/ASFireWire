# Lychzord SIP-Disabled Tester Lane

This lane packages ASFW for a technical tester who has SIP disabled and system extension developer mode enabled. It does not use Chris provisioning profiles, Developer ID notarisation, or Apple-approved DriverKit entitlement profiles.

It is separate from the normal Chris-signed v16 local lane.

## Build

From the main repo:

```sh
cd /Users/chrisizatt/Documents/Codex/2026-04-28/so-mr-midi-has-asked-me/ASFireWire
./tools/lychzord/build_sip_disabled_tester.sh
```

The script defaults to:

- app bundle ID: `com.lychzord.ASFWTest`
- driver bundle ID: `com.lychzord.ASFWTest.ASFWDriver`
- app package name: `ASFWLychzord.app`
- bundle version: `16`
- macOS deployment target: `15.5`
- DriverKit deployment target: `24.0`
- architecture: `arm64`
- signing: ad-hoc / Sign to Run Locally style
- maintenance health: driver-only by default, so the app does not require an Alesis CoreAudio device before reporting a clean refresh

If Lychzord has already installed an earlier tester build with the same System
Extension version, build the replacement with a higher bundle version so
macOS will treat the dext as an update:

```sh
CURRENT_PROJECT_VERSION=17 ./tools/lychzord/build_sip_disabled_tester.sh
```

Useful checks:

```sh
./tools/lychzord/build_sip_disabled_tester.sh --settings-only
```

Outputs are written under:

```text
build/lychzord-sip/
```

The deliverable is:

```text
ASFWLychzordSIP-v16-<stamp>.zip
ASFWLychzordSIP-v16-<stamp>.zip.sha256
```

## Tester Install Steps

On Lychzord's Mac:

```sh
csrutil status
sudo systemextensionsctl developer on
```

Then unzip, copy `ASFWLychzord.app` to `/Applications`, and run:

```sh
sudo xattr -dr com.apple.quarantine /Applications/ASFWLychzord.app
codesign --verify --strict --deep --verbose=4 /Applications/ASFWLychzord.app
open /Applications/ASFWLychzord.app
```

In the app, use `Install / Update Driver` once and approve any System Settings prompt.

If the app reports `Repair needed: ASFW driver CDHash does not match the staged
driver` immediately after replacing the app, macOS is probably still running an
older same-version dext. Use a higher-version package, or uninstall, reboot,
then install once from the new package.

For Midas testing, do not keep pressing `Repair Driver` just because no Midas
device appears in the app. The direct debug UI can still be unavailable in this
SIP-disabled lane. If the Midas does not publish in Audio MIDI Setup, capture
diagnostics and send the snapshot/log output back.

Do not re-sign the delivered app:

```sh
sudo codesign --force --deep --sign - /Applications/ASFWLychzord.app
```

That command replaces the entitlements embedded by the package script and can produce `Missing entitlement com.apple.developer.system-extension.install` or `Taskgated Invalid Signature`.

## Expected Limits

- This is a SIP-disabled development/testing path, not a normal-user distribution path.
- It may still require a reboot after first driver approval or a stuck SystemExtensions state.
- The maintenance helper is ad-hoc signed and uses a relaxed bundle-identifier XPC signing requirement only when the build has an empty Team ID.
- Maintenance health is intentionally driver-only in this package. A missing Midas CoreAudio device is a device-support/logging result, not automatically a repair/install failure.
- Midas Venice audio is not expected to be fully implemented yet. First-pass success is discovery, ROM/DICE logging, and clear failure classification.
- Do not hot-unplug the FireWire device during driver initialisation; current driver teardown/init handling is not yet robust enough for that test.

## If Launch Fails

Collect:

```sh
sw_vers
csrutil status
systemextensionsctl developer
codesign --verify --strict --deep --verbose=4 /Applications/ASFWLychzord.app
codesign -dv --verbose=4 /Applications/ASFWLychzord.app 2>&1
codesign -d --entitlements :- /Applications/ASFWLychzord.app 2>/dev/null
codesign -d --entitlements :- /Applications/ASFWLychzord.app/Contents/Library/SystemExtensions/com.lychzord.ASFWTest.ASFWDriver.dext 2>/dev/null
```

If install fails, also collect:

```sh
systemextensionsctl list
log show --last 15m --style compact --predicate 'process == "sysextd" OR eventMessage CONTAINS "ASFW" OR eventMessage CONTAINS "com.lychzord.ASFWTest"'
```
