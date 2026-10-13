# Experimental Saffire Pro 40 on macOS Sequoia

**Draft research contribution — work in progress, not a supported driver release
or a general-purpose installer.** These sources implement a separate native
Sequoia transport and CoreAudio HAL path. They do not modify or replace ASFW's
DriverKit/OHCI driver for macOS Tahoe.

An original DICE II/TCD2220 Saffire Pro 40 has been used successfully for Nuendo
recording/playback and VLC six-channel playback on one M1 Mac Studio running
macOS Sequoia 15.7.9. The owner also reports practical project use. That is useful
hardware evidence, not qualification of other interfaces, hosts or long sessions.
Development used AI assistance; the interface owner performed the reported
audible checks and physical-meter observations.

## Scope

The current device exposes 8 analog inputs and 6 outputs at internal 48 kHz.
Output order is L, R, C, LFE, LS, RS. Once provisioned, it is a normal CoreAudio
device: applications do not need separate driver adaptations. Simultaneous
Nuendo/browser playback and VLC speaker mapping have been observed working.

This path depends on Apple's native IOFireWireLib/NuDCL support retained in
Sequoia. It is not a way to provide native IOFireWireLib on Tahoe, and the code
must not be merged into the payload-opaque ASFW transport layers as-is. The
working host uses Apple's Thunderbolt 3-to-2 and Thunderbolt-to-FireWire adapters.
The tested original Pro 40 reports model `0x000005`, firmware 1.0.12. Newer
Pro 40 hardware revisions have not been tested.

## Architecture and review map

| Area | Entry points | Responsibility |
| --- | --- | --- |
| Native device and lifetime | `NativeFireWire.cpp`, `NativeSessionRunner.cpp`, `DmaPreparation.hpp` | Owned native session, bounded descriptor reuse, asynchronous control, generation checks and joined teardown |
| Packet content | `Pro40Packets.hpp`, `Pro40Replay.hpp`, `Pro40CaptureStartup.hpp` | CIP/AM824 format, measured timing replay, initial-payload quarantine |
| Input and clock | `SustainedInputHandoff.hpp`, `SustainedInputReceiver.hpp`, `SustainedClockPublication.hpp` | Copied samples, bounded queues/history, continuous device-derived timing |
| Playback and CoreAudio | `NativeDuplexBridge.hpp`, `PlaybackStartupGate.hpp`, `HalSilenceDriver.cpp` | Admission, six-channel output, eight-channel input, CoreAudio properties and callbacks |
| Process ownership | `worker/`, `controller-lab/`, `DuplexProvisionServiceMain.cpp` | Development-signed worker/controller identity, CoreAudio helper delegation and independent release receipts |
| Session management | `session/manage.py`, `session/KeepActive.cpp` | Rebootless recovery and silent CoreAudio output client that holds the native stream open |

Some names retain their original lab names, including `HalSilenceDriver.cpp` and
`Pro40 Controller Lab.app`; the session profile carries application audio. The
snapshot retains supporting diagnostic types and conditional build profiles, but
only the build/check paths documented here are intended for this contribution.
It reuses the repository's existing `CipHeader` and `DbcCounter` implementation.

## Build and offline checks

Use a Mac with Xcode command-line tools and an SDK that includes the native
FireWire APIs (the working environment is Sequoia). From the repository root:

```sh
bash tools/pro40-sequoia/check-startup-preroll.sh
python3 tools/pro40-sequoia/session/check-manage.py
python3 tools/pro40-sequoia/check-duplex-worker-processes.py --build-only --native-worker
```

The first script runs ASan/UBSan packet, input, replay and clock checks. Its
optional archived-capture argument is for local research; no capture is needed
for the public test suite. The last command builds generated-audio process
fixtures under ASan/UBSan and TSan. Its `--run DIRECTORY` mode registers a
temporary user job, runs the generated source and removes the job. It neither
installs a HAL plug-in nor accesses FireWire hardware.

A static development package can also be built. Set `PRO40_SIGNING_IDENTITY`
to an available Apple Development certificate SHA-1 and `PRO40_SIGNING_TEAM`
to its team identifier. The following uses a **synthetic fixture GUID**:

```sh
python3 tools/pro40-sequoia/controller-lab/check-package.py \
  --profile on-demand --signed-namespace \
  --signing-identity "$PRO40_SIGNING_IDENTITY" \
  --team-identifier "$PRO40_SIGNING_TEAM" \
  --enable-original-peer-diagnostic --enable-gated-worker-diagnostic \
  --duplex-hardware --duplex-delegated --session-profile \
  --real-guid 00130e0400000001
```

This builds and verifies the signed package without registering it or starting
the interface. Given the package directory printed by that command:

```sh
python3 tools/pro40-sequoia/hal-silence/build-delegated-host.py PACKAGE_DIRECTORY --hardware
```

This creates and checks a matching, uninstalled HAL bundle. Development pins
are generated locally. Do not replace exact identity/ownership checks with
wildcards to get a package running.

**Portable hardware installation is unfinished.** `session/manage.py` and
`session/prepare.py` are included to make the tested recovery/installation logic
reviewable, not as a turnkey deployment procedure. Their hardware lease and
warm-up checks still describe the development-host contract; the private
device identifier has been replaced with the synthetic fixture above. Merely
changing the package's `--real-guid` does not port that contract. The original
machine's login warm-up package, commissioning scripts and private receipts are
not distributed. Do not install or register this snapshot on another machine
until those policies have been reviewed and adapted to its actual identity,
startup, routing and release evidence. No ready-to-install binaries are supplied.

## Known limitations and next work

- The cold-start mute issue still needs a root-cause fix. The working host uses
  one discarded warm-up session at login. Keep-active waits for successful
  current-boot warm-up evidence; it does not retry failed hardware starts.
- Physical round-trip latency was approximately **225.8 ms** in an earlier
  loopback test. Direct monitoring is currently the practical microphone path.
  The **10–11 ms** warm-client Start time is a different measurement.
- Three-hour hardware sessions, sleep/wake, hot unplug, power-cycle recovery,
  portable installation and login with the new keep-active client remain
  unqualified. A 24-hour counter bound is not endurance certification.
- Other sample rates, clock sources, digital channels, MIDI, other interface
  revisions, hardware volume controls and QuickTime 5.1 behavior are unfinished
  or untested. QuickTime's observed surround mapping is not yet correct; VLC's
  six-output tone check passed on the working host.

See [validation](VALIDATION.md), [startup handling](STARTUP.md) and
[session recovery](SESSION_LIFECYCLE.md) for the evidence and its limits.
This contribution is for architectural review and collaboration before
considering an upstream-supported deployment path.

## Source and evidence provenance

These are selected development sources, tests and build helpers from a separate
local research checkout, exported onto current upstream without its local Git
history. No raw audio, device captures, logs, machine paths, certificates, private
keys, installed packages or reference-stack sources are included. Unit tests use
synthetic samples and identifiers. Reference comments describe behavioral
cross-checks against Linux and Apple sources; their implementations were not
copied. The contribution uses the repository's Apache-2.0 license.
