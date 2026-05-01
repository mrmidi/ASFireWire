# Local Signing Example: Chris Mac Studio

This is a private/local signing example for Chris's Mac Studio. It is not a
portable distribution recipe and should not be treated as a required ASFireWire
setup path.

The scripts in `tools/local/` are examples for one developer account and one
set of local provisioning profiles. Other developers should either provide
equivalent environment overrides or use the SIP-disabled tester lane for
non-distribution testing.

## Current Local Lane

The local scripts currently default to build `28` unless
`ASFW_CURRENT_PROJECT_VERSION` is overridden.

Chris's local app/driver identifiers are:

```text
ASFW_APP_BUNDLE_IDENTIFIER=com.chrisizatt.ASFWLocal
ASFW_DRIVER_BUNDLE_IDENTIFIER=com.chrisizatt.ASFWLocal.ASFWDriver
```

The regular local install/update lane uses:

```sh
./tools/local/build_v16_safe_local.sh --stage-app
```

The script name is historical. It no longer implies bundle version 16; check
the `ASFW_CURRENT_PROJECT_VERSION` default or pass it explicitly.

The full-debug local lane, with the development driver profile that grants
`com.apple.developer.driverkit.allow-any-userclient-access`, uses:

```sh
./tools/local/build_full_debug_local.sh --stage-app
```

## Important Distinction

Audio can work through AudioDriverKit/CoreAudio without the app's direct
DriverKit debug user-client being available:

```text
FireWire device -> ASFW DriverKit driver -> AudioDriverKit/CoreAudio -> DAW
ASFW app -> DriverKit UserClient -> ASFW driver debug/control panels
```

So Logic and Audio MIDI Setup can work even when Device Discovery, AV/C, ROM,
or topology panels are unavailable. The full-debug lane is only needed for the
direct app-to-driver debug/control channel.

## Private Profile Defaults

These defaults are intentionally Chris-specific and should be overridden by
other developers:

```text
ASFW_APP_PROFILE_SPECIFIER=ASFW Chris Mac Studio
ASFW_DRIVER_PROFILE_SPECIFIER=ASFWDriver Chris Mac/iOS
ASFW_CODE_SIGN_IDENTITY=Apple Development: boggspa@hotmail.co.uk (QWB2SUQVJ3)
DEVELOPMENT_TEAM=8CZML8FK2D
```

Profile file paths in `tools/local/install_safe_profiles.sh` and
`tools/local/build_full_debug_local.sh` are examples only. Do not commit private
profiles or package zips.

## Known-Good Local Scope

The local v28 path has proven:

- ASFW driver install/update on Chris's Mac Studio with SIP enabled.
- Alesis MultiMix Firewire published to CoreAudio as `12 in / 2 out @ 48000`.
- Logic can record from the Alesis while using a separate output device.
- Maintenance helper can perform controlled ASFW/CoreAudio refresh and cleanup.
- Full-debug profile enables the app's user-client panels for diagnostics.

For unexpected stale System Extension state, run one app-level Recheck or Repair
only when the app asks for it. If the stale state remains, reboot before trying
another install/update cycle.
