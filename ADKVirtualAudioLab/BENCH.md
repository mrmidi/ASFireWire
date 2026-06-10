# ADKVirtualAudioLab — Milestone 3 bench runbook

How to load the lab dext, drive it with real HAL pacing, and read back the
verifier + O/C instrumentation. The dext code answers the README's O1–O3 and
C1–C4 questions with counters; this file is the procedure around it.

## What the M3 build does

- `VirtualAudioDevice` is its own clock master: an `IOTimerDispatchSource`
  fires once per ZTS period (512 frames ≈ 10.667 ms) on the mach-absolute
  timebase, anchors `UpdateCurrentZeroTimestamp(n*512, fire_time)` with raw
  values (nominal deadline chain, actual fire times — the host smooths), and
  exposes the next period's packets (the stand-in for an IT-ring interrupt).
- The Step 6 `Verifying(Fake)` decorator runs for the whole IO session.
- `StopIO` dumps everything via `IOLog` with the `ADKLab[dump]` prefix.
- Output ring = 8 ZTS periods (4096 frames). Transport type reports FireWire.

## Build

```bash
xcodegen generate
xcodebuild -scheme ADKLabHost -derivedDataPath build/dd build   # app + embedded dext
```

Unsigned by default (`CODE_SIGNING_ALLOWED: NO`) — loadable only after one of
the signing lanes below.

### Lane A — ad-hoc, SIP-off bench (mrmidi's rig)

Requires SIP/AMFI relaxed + `systemextensionsctl developer on`.

```bash
xcodebuild -scheme ADKLabHost -derivedDataPath build/dd build \
  CODE_SIGNING_ALLOWED=YES CODE_SIGNING_REQUIRED=YES CODE_SIGN_IDENTITY="-"
systemextensionsctl developer on   # allows running from the build directory
```

### Lane B — real DriverKit entitlements, SIP-on (Chris's machine)

Requires an Apple Development (or Developer ID) identity whose provisioning
profile carries `com.apple.developer.driverkit` +
`com.apple.developer.driverkit.family.audio`, with the bench machine's UDID in
the profile. The bundle ids must match what the profiles were issued for —
override them if they differ from the lab defaults:

```bash
xcodebuild -scheme ADKLabHost -derivedDataPath build/dd build \
  CODE_SIGNING_ALLOWED=YES CODE_SIGNING_REQUIRED=YES \
  CODE_SIGN_IDENTITY="Apple Development" DEVELOPMENT_TEAM=<TEAMID> \
  CODE_SIGN_STYLE=Manual \
  PROVISIONING_PROFILE_SPECIFIER=<dext-profile-name-for-the-dext-target>
```

(Per-target overrides are easiest from Xcode's Signing pane after
`xcodegen generate`; with SIP on, the app must also be moved to
`/Applications` before activation.)

The dext entitlements file is `Driver/ADKVirtualAudioLab.entitlements`; the
host app's is `Host/ADKLabHost.entitlements` (system-extension install).

## Run

1. Launch `ADKLabHost.app`, click **Activate**, approve in System Settings →
   General → Login Items & Extensions.
2. Capture logs in a second terminal **before** starting audio:

   ```bash
   log stream --predicate 'sender CONTAINS "ADKVirtualAudioLab"' --style compact
   ```

3. The virtual device ("VirtualADKAudioLabDevice", FireWire transport) appears
   in Audio MIDI Setup. Play audio at it:

   ```bash
   # simplest: set it as output in Audio MIDI Setup, then
   afplay /System/Library/Sounds/Submarine.aiff
   # or run minutes of pink noise from Music/Logic for a soak
   ```

4. Stop playback (coreaudiod stops IO a moment later), or switch default
   output away — `StopIO` fires and the `ADKLab[dump]` lines appear.
5. Deactivate from the app when done (or leave active for repeat runs —
   each StartIO resets counters).

## Reading the dump

```
ADKLab[dump] zts:      anchors, before_first_io, period, ring_frames, prepare_failures
ADKLab[dump] writeend: count, frames, min/max io size, sample_breaks, first_sample,
                       first_host_delta (ticks from StartIO seed), other_ops
ADKLab[dump] verifier: violations + the P1..P4 breakdown (Step 6 ids)
ADKLab[dump] packets:  published/data/nodata/acquire_failures
ADKLab[dump] payload:  visited/written/without_packet/outside_packet
ADKLab[free] ...:      io_after_stop / timer_after_stop (O2, logged at teardown)
```

## Mapping counters → README questions

| Question | Where it lands |
|---|---|
| **C1** minimal IO trigger / anchors before first cycle | `zts: before_first_io`, `writeend: first_sample`, `first_host_delta` |
| **C2** ZTS tolerance (jitter/late/skip) | rerun with the timer chain perturbed; watch `sample_breaks` + audible glitches; flip `SetClockAlgorithm` Raw vs default |
| **C3** WriteEnd shape | `writeend: min/max`, `sample_breaks` (continuity), restart runs for sample-time reset behavior |
| **C4** period vs HAL buffer coupling | vary `kRingPeriods` / HAL IO size (Audio MIDI Setup), compare dumps |
| **O1** retain/teardown order | clean activate→deactivate cycles with no dext crash logs |
| **O2** callbacks after StopIO | `ADKLab[free] io_after_stop / timer_after_stop` |
| **O3** init-failure leak path | forced-failure experiment (edit init to fail after AddStream) |
| **M1 invariants under real pacing** | `verifier: violations == 0` over minutes of playback |

**Milestone 3 exit:** zero verifier violations over minutes of real HAL
pacing; the O/C answers recorded back into `README.md`; a captured WriteEnd
trace added to the host regression suite (trace extraction is the planned
follow-up — current dump carries aggregates + first-callback tuple).
