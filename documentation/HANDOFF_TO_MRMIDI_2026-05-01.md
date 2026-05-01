# Handoff To Mr MIDI: 2026-05-01

This branch reconciles the v28 Alesis/Midas app work with upstream `main`
without directly pushing or merging into the upstream repo.

## Proven Locally

- Alesis MultiMix Firewire publishes through ASFW to CoreAudio.
- The observed working shape is `12 in / 2 out @ 48000`.
- Logic can record cleanly from Alesis inputs while using a separate output.
- Driver install/update/repair is now handled by the app plus a small
  privileged maintenance helper instead of relying only on `debug.sh`.
- The app can distinguish the audio path from the debug user-client path:
  audio can be healthy even when debug panels are disconnected.
- A full-debug local development profile with
  `com.apple.developer.driverkit.allow-any-userclient-access` restores the
  app-to-driver debug user-client panels on Chris's provisioned Mac.

## Diagnostic-Only Or Incomplete

- Midas Venice F32 is recognized by exact metadata:
  `vendor=0x10c73f`, `model=0x000001`, `unit spec=0x10c73f`,
  `unit version=0x000001`.
- Midas remains generic-DICE, fail-closed diagnostics. No guessed stream
  geometry, channel count, mixer, routing, or clock writes are added.
- The Device Library imports FFADO/systemd identity metadata. Recognition does
  not mean support.
- The Midas, Device Library, ROM, topology, AV/C, and metrics UI are primarily
  status/diagnostic surfaces unless the debug user-client entitlement is
  available.

## Reconciled Upstream Items

- Preserved upstream async completion fixes, including ACK normalization and
  legacy `ackCode=0x8` handling.
- Preserved upstream descriptor scanning behavior for completed
  `OUTPUT_MORE`/`OUTPUT_LAST` precursor pairs inside the refactored scanner.
- Preserved upstream controller/bus reset safety changes, including reasserting
  `cycleMaster` on `cycleTooLong`.
- Preserved local ARM64 aligned AR payload copy fix.
- Preserved local v28 lifecycle UI clarity, maintenance helper, device metadata,
  Midas status UI, and Lychzord tester packaging lane.

## Chris-Specific

- `tools/local/` is a private/local signing example for Chris's developer team,
  certificates, bundle IDs, and provisioning profiles.
- See `documentation/LOCAL_SIGNING_EXAMPLE_CHRIS.md`.
- Do not commit provisioning profiles, notarised zips, DerivedData, or package
  artifacts.

## Lychzord-Specific

- Lychzord tester packages use `com.lychzord.ASFWTest` and
  `com.lychzord.ASFWTest.ASFWDriver`.
- The tester path is SIP-disabled/ad-hoc and Tahoe/macOS 26 only.
- It is for prebuilt bundle testing, not local Xcode building.
- See `documentation/LYCHZORD_SIP_DISABLED_TESTER.md`.

## Warning Triage

- Treat build warnings as follow-up unless they affect runtime safety,
  provisioning, or async completion behavior.
- Priority warnings to clean next: Swift concurrency/sendability warnings around
  app lifecycle tasks, ignored `[[nodiscard]]` return values in test scaffolding,
  and unused diagnostic variables in new metadata tests.
- Non-priority: noisy legacy logging strings and historical comments that do not
  change behavior.
