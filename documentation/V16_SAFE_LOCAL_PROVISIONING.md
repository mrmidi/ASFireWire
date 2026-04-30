# v16 Safe Local Provisioning

This lane is for Chris's Mac Studio while the full DriverKit UserClient entitlement request is still pending.

It does not remove `ASFW/App.entitlements`. That file remains the future app-control/UserClient lane. The safe local lane builds the app with `ASFW/AppInstallOnly.entitlements`, which keeps SystemExtension install/update permission but does not request `com.apple.developer.driverkit.userclient-access`.

## Profiles

Use these local development profiles:

- App: `/Users/chrisizatt/Downloads/ASFW_Chris_Mac_Studio(1).provisionprofile`
- Driver: `/Users/chrisizatt/Downloads/ASFWDriver_Chris_MaciOS.provisionprofile`

The scripts install them into:

```sh
~/Library/MobileDevice/Provisioning Profiles/
```

## Build

From the main repo:

```sh
cd /Users/chrisizatt/Documents/Codex/2026-04-28/so-mr-midi-has-asked-me/ASFireWire
./tools/local/build_v16_safe_local.sh
```

Useful checks:

```sh
./tools/local/build_v16_safe_local.sh --settings-only
```

Stage the app only when ready to replace the current `/Applications/ASFWLocal.app`:

```sh
./tools/local/build_v16_safe_local.sh --stage-app
```

Do not stage casually while the known-good v15 audio path is being used.

## Expected Limits

This build can install/update the ASFW system extension, publish AudioDriverKit/CoreAudio devices, and use the v16 repair helper path.

The app's direct DriverKit UserClient control/debug channel may still show disconnected until Apple approves the UserClient entitlement and a matching provisioning profile is used. That is separate from the audio path:

```text
FireWire device -> ASFW DriverKit driver -> AudioDriverKit/CoreAudio -> Logic
ASFW app -> DriverKit UserClient -> ASFW driver debug/control panels
```

So Logic and Audio MIDI Setup can work even when the app's Device Discovery or AV/C panels say the driver is not connected.

## Build Overrides

The safe script builds with:

- `ASFW_APP_BUNDLE_IDENTIFIER=com.chrisizatt.ASFWLocal`
- `ASFW_DRIVER_BUNDLE_IDENTIFIER=com.chrisizatt.ASFWLocal.ASFWDriver`
- `ASFW_APP_ENTITLEMENTS=ASFW/AppInstallOnly.entitlements`
- `ASFW_APP_PROVISIONING_PROFILE_SPECIFIER=ASFW Chris Mac Studio`
- `ASFW_DRIVER_PROVISIONING_PROFILE_SPECIFIER=ASFWDriver Chris Mac/iOS`
- `DEVELOPMENT_TEAM=8CZML8FK2D`
- `CODE_SIGN_IDENTITY=Apple Development: boggspa@hotmail.co.uk (QWB2SUQVJ3)`
- `RUN_CLANG_STATIC_ANALYZER=NO`

The helper is signed by the same team and embedded at:

```text
ASFW.app/Contents/Library/Helpers/ASFWPrivilegedHelper
ASFW.app/Contents/Library/LaunchDaemons/com.chrisizatt.ASFWLocal.PrivilegedHelper.plist
```

## 2026-04-30 Recovery Notes

Failure mode seen during the first v16 maintenance-helper pass:

- `/Applications/ASFWLocal.app` was replaced while stale bundle contents were still present.
- The embedded helper copy could be left unsigned or launchd could keep running an older helper process.
- `Repair Driver` could then sit in `Repairing driver state...` because the app had no timeout around the helper XPC call.

Fixes now in the local lane:

- `tools/debug/stage_local_app.sh` removes the old `/Applications/ASFWLocal.app` before copying the new signed bundle.
- `tools/local/build_v16_safe_local.sh` verifies the embedded helper and re-signs the app after any bundle normalization.
- Helper calls have explicit timeouts, so a broken helper path reports an error instead of leaving the UI busy forever.

Known-good v16 verification after recovery:

```text
systemextensionsctl: com.chrisizatt.ASFWLocal.ASFWDriver (1.0/16) [activated enabled]
IORegistry CDHash: c399525140b9b15e3775dc979ae2fdc95397ee9c
CoreAudio: Alesis MultiMix Firewire, 12 in / 2 out, 48000 Hz
Snapshot: /tmp/asfw-v16-recovery-after-20260430-0400
```

If the app is stuck in a maintenance state after replacing the bundle, quit ASFW, stage with the clean staging script, reopen `/Applications/ASFWLocal.app`, then run exactly one `Install / Update Driver` or `Repair Driver` attempt. If it still does not publish the Alesis, reboot before trying more install loops.
