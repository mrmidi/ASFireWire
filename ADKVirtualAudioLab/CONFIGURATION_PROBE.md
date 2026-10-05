# Multi-rate HAL configuration probe

This source-only experiment publishes a second, silent device named
**ASFWConfigurationProbe**. The existing 48 kHz packet/verifier device remains
available. The probe performs no FireWire operations and is not a device-family
implementation. Its synthetic channel counts are 16 at 1x, 12 at 2x and 8 at 4x.

## Questions the runtime run must answer

- Does a nominal-rate callback staging `RequestDeviceConfigurationChange`
  result in host stop, custom perform and automatic restart?
- Does a stream-format callback returning Busy while staging that same window
  get retried coherently? The SDK requires success to mean the stream format
  is already installed; the probe deliberately does not report early success.
- Can existing device/stream object IDs remain stable while both channel counts
  and ZTS period change? Are duplex formats consistent after commit?
- Do IO callbacks resume after the change, without the application reopening
  the device? `stop` records IO counts; the following `start` identifies the
  new configuration. There is no audible output from this device.

The probe stages one token at a time, rejects competing changes, and performs
setters only inside the host window. Failed setters attempt the prior complete
projection; failed restoration gates StartIO. This is a minimal SDK experiment,
not a replacement for the production resolved-configuration reducer.

Buffers allocate 49152 frames at the maximum 16-channel stride. Configuration
changes reuse these descriptors; active ZTS periods are taken from the production
HAL geometry table. Timer epochs reset on every StartIO. Stream callbacks retain
their owner for the call; Shutdown breaks that ownership before removal and
retains the timer target until cancellation completes. IO counters are owned by
the callback block, independently of device ivar lifetime.

## Build and first manual run

From this directory:

```sh
xcodegen generate
xcodebuild -project ADKVirtualAudioLab.xcodeproj -scheme ADKLabHost \
  -configuration Debug -derivedDataPath build/multirate-probe build
```

The lab defaults to ad-hoc signing with arm64e for Apple Silicon; see BENCH.md
for activation prerequisites and the separately provisioned signing lane.
No production ASFW extension is replaced by activating this lab extension.

1. Open `build/multirate-probe/Build/Products/Debug/ADKLabHost.app`, click
   Activate and approve the lab extension if macOS asks.
2. Select **ASFWConfigurationProbe** in Audio MIDI Setup. Confirm its initial
   48 kHz / 16-channel duplex formats.
3. With a client actively using the probe, change 48 → 96 → 44.1 → 48 kHz,
   allowing a few seconds between requests. Expected channel counts are
   16 → 12 → 16 → 16. Capture the Input and Output format values.
4. If that sequence is coherent, repeat 32, 88.2, 176.4 and 192 kHz, then
   return to 48 kHz. Expected channel counts are 16, 12, 8 and 8.
5. Return the application/output selection to your physical device.

The agent can read logs without asking the user to run a tracing command:

```sh
/usr/bin/log show --last 15m --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[ADKConfigProbe]"'
```

Acceptance for this first run: unchanged device and stream IDs, `perform` sees
running=0, hardware-independent HAL/input/output rates agree at commit, the
expected ZTS period and channel count appear, and IO resumes automatically.
Failure/abort injection and deliberately competing host requests are subsequent
runs; this first run does not establish those outcomes or any FireWire timing.
Keep production advertisement gated until the callback contract is understood.
