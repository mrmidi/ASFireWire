# Alesis MultiMix Recording Stability Log

Status as of 2026-04-30: v15 remains the clean recording checkpoint for the currently connected Alesis MultiMix FireWire unit.

This document records the work done across 2026-04-28 and 2026-04-29 to bring the Alesis MultiMix recording path to a usable/professional baseline before any separate packaging pass for external testers.

## Current Checkpoint

- Branch: `alesis-recording-stability-v4`
- Installed local system extension: `com.chrisizatt.ASFWLocal.ASFWDriver (1.0/15)`
- Active DriverKit CDHash: `e08b9dff83f69254f70363038e3d9d68e5e9f69a`
- Device exposed to CoreAudio: `Alesis MultiMix Firewire`
- CoreAudio channel shape: `12 in / 2 out @ 48 kHz`
- Logic result reported by Chris on 2026-04-29: input 3 microphone and input 4 synth are both clean in Logic.
- Recovery result on 2026-04-30: a no-reboot controlled refresh cleared a duplicate half-uninstall SystemExtensions state and republished the Alesis device to CoreAudio.
- Packaging for Lychzord has not been started from this checkpoint.

The v15 checkpoint should be treated as the baseline before creating any separate external-test copy.

## Work Summary

The project moved from semi-usable Alesis capture with audible clicks/pops to clean Logic recording on channels 3 and 4. The key change was to stop treating the MultiMix as a generic or guessed DICE shape and instead line up the host behavior with the active DICE stream configuration and Linux AMDTP/DICE assumptions.

Important outcomes:

- Alesis MultiMix now advertises `12` active input channels rather than the earlier broader `14` input view.
- Runtime DICE stream discovery now records active stream channel counts, AM824 slots, and iso channels.
- The active Alesis device-to-host stream is treated as DICE TX stream on iso channel `1`; host-to-device playback is iso channel `0`.
- Disabled DICE stream entries with `iso=-1` are ignored rather than counted as usable audio streams.
- Startup zero-fill is now explicit diagnostic behavior, not confused with mid-recording corruption.
- RX health logging is detailed enough to separate startup alignment from ongoing capture stability.
- Driver refresh hygiene is now repeatable using local debug scripts.

## DICE Files Findings

Lychzord provided the historical `dice files` folder. Two delegated analysis passes were run against it. The useful findings were:

- Use active DICE TX/RX stream table entries as authoritative.
- `iso=-1` means a stream table entry is disabled.
- The folder did not reveal a hidden queue/FIFO register that directly solves timing.
- Useful register areas are section table, global notification/status/extStatus/sampleRate, active TX/RX stream tables, RX `seqStart`, and AVS DBS/system-mode registers.
- Newer code should use the DICE section table offsets rather than old absolute `GLOBAL_*` assumptions.

These findings directly informed the v15 runtime-capability and iso-channel work.

## Driver Changes

### Alesis/DICE Capabilities

- Added runtime DICE iso-channel fields to `AudioStreamRuntimeCaps`.
- Added active-stream helpers in DICE stream config handling.
- Updated TCAT runtime caps to use active PCM/AM824 slots only.
- Preserved known-profile fallback for the currently connected Alesis unit.
- Updated known DICE profiles with default Alesis iso channel direction:
  - device-to-host: `1`
  - host-to-device: `0`
- Updated the DICE restart coordinator to resolve duplex channels from runtime caps first, then known profile fallback, then conservative defaults.

### RX Timing And Queue Behavior

- Added explicit startup hold accounting for zero-filled frames.
- Added startup rebase and transport-lock rebase logging.
- Added high-water RX backlog slewing before queue overflow.
- Kept `0xffff` SYT as no-info rather than clock loss.
- Treated AM824 data block count continuity as authoritative.
- Added per-callback input counters and requested-frame counters.
- Added periodic `IO-RX` logging with callback counts, requested/sent frames, queue fill, underread counters, q8/rate, authority, alignment, and active RX profile.
- Added `IR RX HEALTH` logging from the RX pipeline, including packet counts, data/empty packet counts, decoded frames, queue fill/capacity, producer drops, consumer underreads, CIP fields, q8, and SYT loss.

### Diagnostics/UI

- Extended UI metrics plumbing for RX diagnostics.
- Extended shared metrics/user-client models for RX queue and parser health.
- Added focused recording-health capture script:
  - `tools/debug/recording_health.sh`
- Added CoreAudio capture harness:
  - `tools/debug/coreaudio_channel_capture.c`
- Added lightweight hygiene snapshot script:
  - `tools/debug/hygiene_snapshot.sh`
- Added controlled local driver refresh script:
  - `tools/debug/refresh_local_driver.sh`

The refresh script captures hygiene before/after, quits the local app, terminates ASFW DriverKit user-server PIDs through one admin prompt, restarts `coreaudiod`, and verifies the expected CDHash when supplied.

## Validation

Host tests:

- Focused CTest suite: `94/94` passed.
- Full host CTest suite: `519/519` passed.
- `git diff --check`: passed before the v15 build.

Build/install:

- Signed local Debug build succeeded for v15.
- Staged app: `/Applications/ASFWLocal.app`
- Staged driver CDHash: `e08b9dff83f69254f70363038e3d9d68e5e9f69a`
- System extension replacement accepted by macOS.
- A controlled refresh was required before IORegistry moved from the v14 CDHash to the v15 CDHash.

Live/harness captures:

- 10 second channel-4 synth capture:
  - Output: `/tmp/asfw-v15-channel4-triangle-10s.wav`
  - Frames: `480000`
  - Duration: `10.000000 s`
  - Post-start scan: no exact-zero runs, no clipping, no large waveform jumps.
- 60 second channel-4 synth capture:
  - Output: `/tmp/asfw-v15-channel4-triangle-60s.wav`
  - Frames: `2880000`
  - Duration: `60.000000 s`
  - Post-start scan after first 50 ms: two isolated one-sample zeros, no 10 ms dropout, no clipping, no large waveform jumps.

Logic validation:

- Chris tested Logic with synth on input 4 and microphone on input 3.
- Both inputs were clean in Logic.
- This is the recording milestone for the currently connected Alesis MultiMix profile.

## Captured Evidence

Useful local evidence folders/files:

- `/tmp/asfw-hygiene-20260429-222139`
- `/tmp/asfw-hygiene-post-v15-20260429-222717`
- `/tmp/asfw-recording-health-v15-channel4-20260429-222928`
- `/tmp/asfw-recording-health-v15-channel4-60s-20260429-223242`
- `/tmp/asfw-v15-channel4-triangle-10s.wav`
- `/tmp/asfw-v15-channel4-triangle-60s.wav`

Important log interpretation:

- WAV dropout scripts report a startup `28 ms` exact-zero run, but it begins at `0.000 s`.
- The driver logs identify this as startup hold/alignment, not mid-capture corruption.
- After startup, `IR RX HEALTH` showed no DBC/CIP errors and no RX drops during active capture.
- Queue fill stayed near the target during capture.
- Transport q8 settled around a measured `47998.5 Hz` rate.

One remaining useful observation:

- After CoreAudio capture stops, RX can continue briefly until the queue fills and producer drops resume. This is likely part of the stale-state/performance hygiene issue and should be addressed with better stream quiesce/refresh behavior before future long test loops.

## Operational Hygiene

A stale v14 DriverKit user-server remained active after macOS accepted v15. The reliable refresh pattern was:

1. Stage the new local app/driver.
2. Submit system extension activation/replacement.
3. Verify `systemextensionsctl list`.
4. Verify IORegistry CDHash.
5. If IORegistry is still bound to the old CDHash:
   - quit `/Applications/ASFWLocal.app`
   - terminate the ASFW DriverKit user-server PIDs
   - restart `coreaudiod`
   - verify IORegistry CDHash again

This is now encoded in `tools/debug/refresh_local_driver.sh` and should become an app-side “Driver Refresh / Quiesce Audio” workflow later.

### v15 Restore Checklist

On 2026-04-30 the machine fell out of the known-good v15 runtime state after the app was quit/reopened and the working driver was reinstalled. Before recovery, SystemExtensions showed one active v15 driver plus one identical v15 driver `terminating for uninstall but still running`, and CoreAudio no longer published `Alesis MultiMix Firewire`.

The successful no-reboot recovery was:

1. Capture the baseline state before touching anything:
   - `tools/debug/hygiene_snapshot.sh --out /tmp/asfw-recovery-v15-before-20260430-015415`
   - `tools/debug/probe_local_state.sh > /tmp/asfw-recovery-v15-before-20260430-015415/probe.txt`
2. Quit Logic and `/Applications/ASFWLocal.app`.
3. Run one controlled refresh:
   - snapshot output: `/tmp/asfw-recovery-v15-refresh-20260430-015527`
   - expected CDHash: `e08b9dff83f69254f70363038e3d9d68e5e9f69a`
4. Reopen `/Applications/ASFWLocal.app`.
5. Verify:
   - `systemextensionsctl list` shows only `com.chrisizatt.ASFWLocal.ASFWDriver (1.0/15)` active/enabled, with no stale ASFW terminating entry.
   - IORegistry reports CDHash `e08b9dff83f69254f70363038e3d9d68e5e9f69a`.
   - CoreAudio reports `Alesis MultiMix Firewire`, `12 in / 2 out @ 48 kHz`.

The useful mechanism was refresh, not another reinstall: terminate the stale ASFW DriverKit user-server PIDs, restart `coreaudiod`, wait for macOS to settle on a single active v15 system extension, then let the ASFW audio nub republish into CoreAudio.

For non-technical testers, the plain checklist is:

1. Open the app from `/Applications/ASFWLocal.app` only.
2. Do not open copies from Xcode, DerivedData, Downloads, or private test folders.
3. Do not reinstall repeatedly if the audio device disappears.
4. If the device disappears, quit Logic or any other audio app, then quit ASFWLocal.
5. Run one Repair Driver / Refresh Audio action. Today this is `tools/debug/refresh_local_driver.sh`; later it should be an app button.
6. Reopen `/Applications/ASFWLocal.app`.
7. Check Audio MIDI Setup, System Settings, or Logic for `Alesis MultiMix Firewire`, `12 in / 2 out @ 48 kHz`.
8. If it is still missing, power-cycle or unplug/replug the MultiMix/FireWire adapter once.
9. If it is still missing, or if SystemExtensions shows a driver `terminating for uninstall but still running`, reboot before trying another install.

`tools/debug/refresh_local_driver.sh` was also made compatible with macOS's system `/bin/bash` by removing `mapfile`, which is not available in the Bash 3.2 shipped by macOS.

## Pause Boundary

Do not begin the Lychzord packaging/private-test-copy pass from an uncommitted working tree.

Next planned work, after this checkpoint is committed:

- Create a separate self-contained external-test copy of the Xcode project.
- Remove or neutralize local provisioning/signing assumptions.
- Keep instructions simple enough for Lychzord to run the app, install the driver, and test his DICE device.
- Do not broaden Alesis behavior to the second MultiMix model until this v15 recording milestone remains stable.
