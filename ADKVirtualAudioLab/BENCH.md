# ADKVirtualAudioLab — Milestone 3 bench runbook

How to load the lab dext, drive it with real HAL pacing, and read back the
verifier + O/C instrumentation. The dext code answers the README's O1–O3 and
C1–C4 questions with counters; this file is the procedure around it.

## What the M3 build does

- `VirtualAudioDevice` uses the production 48 kHz HAL geometry: 12288 active
  frames and ZTS frames inside a 24576-frame descriptor allocation. The
  nominal client budget is 1024 frames; AudioDriverKit's calculated maximum
  is 4096 frames.
- One `IOTimerDispatchSource` publishes raw ZTS anchors every 12288 frames
  (256 ms). A separate 1 ms timer advances the simulated capture cursor and
  maintains a bounded packet lead. Packet preparation is independent of ZTS.
- The Step 6 `Verifying(Fake)` decorator runs for the whole IO session.
- `StopIO` dumps everything via `IOLog` with the `ADKLab[dump]` prefix.
- Output and input descriptors each allocate 24576 frames. PCM wraps at the
  active 12288-frame ring. Transport type reports FireWire.
- The host's Phase Scope tab maps the same output descriptor read-only and
  wraps the mapped pages with `bytesNoCopy`; Metal plots the first two
  interleaved channels as `(L-R, L+R)`. A compute pass reports L/R peak and
  correlation, plus GPU/queue/completion timing, sample age, overwrite margin,
  wrap counts, restart epoch/history, and periodic exact CPU/GPU sample checks.
  There is no PCM-copy fallback.
- The adjacent Waveform tab remains available as a simple channel-0
  time-domain view over that same mapped ring.

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
   While playback is running, select the **Phase Scope** tab. It reports the
   mapped capacity, active ring, WriteEnd cursor, valid history, epoch, L/R
   peaks, stereo correlation, GPU timing, sample age, overwrite margin, and
   wrap/validation counters. For a 1 kHz sine at 48 kHz, use equal in-phase
   left/right channels (vertical trace) and then invert one channel
   (horizontal trace); the exact CPU/GPU sample mismatch count should remain
   zero while the window crosses the ring boundary. `Zero-copy Metal import
   succeeded` means the mapped address was accepted as an `MTLBuffer`; the
   live trace plus exact sample checks confirm the shader reads live CoreAudio
   writes. A rejected import is shown as
   `ZERO-COPY IMPORT FAILED` and does not silently switch to a copied buffer.

4. Stop playback (coreaudiod stops IO a moment later), or switch default
   output away — `StopIO` fires and the `ADKLab[dump]` lines appear.
5. Deactivate from the app when done (or leave active for repeat runs —
   each StartIO resets counters).

## Reading the dump

```
ADKLab[dump] zts:      anchors, before_first_io, period, active/allocated frames,
                       prepare_failures, coverage_shortfalls, pump_after_stop
ADKLab[dump] writeend: count, frames, min/max io size, sample_breaks, first_sample,
                       first_host_delta (ticks from StartIO seed), other_ops
ADKLab[dump] verifier: violations + the P1..P4 breakdown (Step 6 ids)
ADKLab[dump] packets:  published/data/nodata/acquire_failures
ADKLab[dump] payload:  visited/written/without_packet/outside_packet/raced_reuse
ADKLab[free] ...:      io_after_stop / timer_after_stop (O2, logged at teardown)
```

## Packet inspector (live dumps)

The host app has a **Packets** tab with the packet inspector. **Dump** (or ⌘D)
snapshots the most recent N packets (8/16/32/64) from the running
dext — any time, including mid-stream. The capture is one `DispatchSync` onto
the dext work queue (~µs, serialized with the pump); the RT path is never
touched, so it is soak-safe.

Plumbing: `LabDiagUserClient` (connection type `'LDBG'`, selector 0) returns a
`PacketDumpBlob` (`Lab/PacketDumpBlob.hpp` is the layout contract, mirrored in
`Host/PacketDumpClient.swift`). The host app needs the
`com.apple.developer.driverkit.userclient-access` entitlement (already in
`ADKLabHost.entitlements`); on the SIP-on lane the provisioning profile must
carry it too.

Reading the inspector:
- Cadence strip: blue **D** = data, gray **N** = no-data, red **·** = evicted
  from the 1024-cycle packet history (older than ~128 ms at 48 k).
- Detail pane: decoded CIP (sid/dbs/fn/qpc/sph/dbc, fmt/fdf/syt) plus the
  frames×channels slot grid — hex wire word and decoded raw-24 float per cell;
  zero slots are dimmed.
- "LIVE" on a record means the slot is still `ExposedForAudio`: the RT writer
  may land PCM into those bytes after the copy. Published/Completed records
  are frozen.
- The context line carries the capture-time cursors (zts sample time, exposed
  frames, writeEnd count) and the payload counters, so a dump is
  interpretable as a point on the stream timeline.

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
