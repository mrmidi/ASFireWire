# Assessment of PR #41 Code Review

**Subject:** [`PR_41_CODE_REVIEW.md`](PR_41_CODE_REVIEW.md) — review of
[PR #41 — (DICE-4) 44k Sample Rate Implementation Switcher](https://github.com/mrmidi/ASFireWire/pull/41)

**Assessed:** 2026-07-11. Every load-bearing claim in the review was verified
against the PR head (`3285b13378d3db721d1b98557d02defa0688d9b4`), the
AudioDriverKit SDK headers (DriverKit 27.0 SDK), and `ADKVirtualAudioLab`
(the project's designated runtime-truth source for ADK behavior). Analysis
only — no implementation code, tests, or PR state were modified.

## Overall verdict: mostly agree, with two meaningful corrections

The review's findings are real and well-anchored — nearly every factual claim
was confirmed in code. But its severity framing is off in both directions:

1. **The P0 overstates what AudioDriverKit itself requires.** The SDK's own
   default `HandleChangeSampleRate` does exactly what the PR does.
2. **The P1 failure-ordering finding understates one mitigating fact** (the
   clock RPC is synchronous, so hardware failures propagate before the HAL
   publish) **and misses a nastier defect** (a 15-second blocking poll inside
   the ADK callback).

## Finding-by-finding

### P0 — "Bypasses the required ADK configuration transaction": agree with the gap, disagree with the framing

- The SDK header states the **default** implementation of
  `HandleChangeSampleRate` "will call SetSampleRate() and return
  kIOReturnSuccess" (`IOUserAudioClockDevice.iig:316`, DriverKit 27.0 SDK).
  The ADK lab does literally that (`ADKVirtualAudioLab/Driver/VirtualAudioDevice.cpp:819`).
  So "bypasses the required AudioDriverKit transaction" is **wrong as an
  API-contract claim** — Apple's own default does what the PR does.
  `RequestDeviceConfigurationChange` is the mechanism for **driver-initiated**
  configuration changes, not HAL-initiated rate changes.
- What **is** true: the functional gap. Rejecting with `kIOReturnBusy`
  whenever IO runs means no rate change while any client holds IO, and the
  project's own design doc (`SAMPLE_RATE_EXPANSION.md`, audit item 6, ~L210 at
  the PR head — the review's `#L225`/`#L240` anchors are drifted) explicitly
  calls for wiring `PerformDeviceConfigurationChange` → duplex restart via
  `DiceDuplexRestartCoordinator`.
- The PR's busy-rejection is a **deliberate, safe interim policy** — its code
  comment describes the empirically observed failure mode of hot
  reconfiguration (device PLL flaps 44.1k↔48k, audio starves until replug).
  This is a scope narrowing, not an unsafe transaction.
- Caveat on the PR side: the code comment citing "the validated ADK contract
  (ADKVirtualAudioLab)" **overclaims**. The lab advertises exactly one sample
  rate (`SetAvailableSampleRates(&sampleRate, 1)`,
  `VirtualAudioDevice.cpp:267`), so it has never exercised a real multi-rate
  switch. The single-rate lab cannot validate rate-change format
  follow-through either way.

### P1 — "Formats not updated / failed publish leaves device changed": agree on formats, half-agree on ordering

- **Formats: confirmed.** `IOUserAudioStream::DeviceSampleRateChanged` is a
  driver-called helper — "Call to update stream formats when the owning audio
  device changes sample rate. Goes through all the available stream formats
  and selects the closest format with the matching sample rate"
  (`IOUserAudioStream.iig:254-266`). The PR never calls it (nor
  `SetCurrentStreamFormat`), so ADK stream formats stay at the old rate's
  descriptor after a switch. Legitimate finding.
- **Ordering: overdrawn.** `DiceDuplexRestartCoordinator::RequestClockConfig`
  **blocks up to 15 s** polling `TryTakeCompletedClockRequest` and returns the
  real hardware completion status (`DiceDuplexRestartCoordinator.cpp`,
  `kClockRequestWaitTimeoutMs = 15000`). `AudioCoordinator` and the nub only
  mutate binding state (`endpoint->SetCurrentSampleRate`,
  `ivars->currentSampleRateHz`) **on success**. So a failed DICE commit
  propagates cleanly and `SetSampleRate` is never reached; the review's "DICE
  and the direct binding have already been updated before `SetSampleRate` can
  fail" is only accurate for the success path.
- **Residual real defects (confirmed, narrow):**
  - A `SetSampleRate` failure *after* a successful hardware commit has no
    unwind (device + binding at new rate, HAL at old).
  - The `audioNub == nullptr` branch calls `SetSampleRate` without touching
    hardware (`ASFWAudioDevice.cpp`, `HandleChangeSampleRate`).
- **Missed by the review:** the 15-second `IOSleep` poll runs *inside*
  `HandleChangeSampleRate` on the ADK work queue. A wedged device stalls the
  audio device's queue for 15 s. Arguably worse than anything listed under
  this finding.

### P1 — "No per-device capability contract": fully agree

All confirmed at the PR head:

- `DiceDeviceProfile::SupportedSampleRates()` unconditionally returns
  `{44100, 48000}` (`DiceDeviceProfile.hpp:51`).
- The graph overwrites nub-provided rates with the profile list
  (`ASFWAudioDriverGraph.cpp:~165`).
- `DiceClockCapsSupportRate` is defined (`DICETypes.hpp:518`) and **never
  called anywhere** (git grep over the PR head: one hit, the definition).
- `HandleChangeSampleRate` does a bare `static_cast<uint32_t>` with no
  membership check against advertised rates.
- `IsSupportedClockConfig` explicitly accepts 32 kHz — "Internal clock at a
  validated 1x rate (32 / 44.1 / 48 kHz)" (`DICERestartSession.hpp:250-268`) —
  while CoreAudio is advertised only two rates, so an unadvertised 32 kHz
  request would reach `CLOCK_SELECT`.
- This matches the project's own design doc (audit item 5): read
  `GLOBAL_CLOCK_CAPS` (0x64) "to populate the nub's supported-rate list
  instead of assuming."

Severity caveat: the HAL should not request unadvertised rates, and
essentially all DICE devices support both 1x rates — so this is
defense-in-depth plus roadmap hygiene rather than a live bug.

### P1 — "`selectedClock_` poisons the next start": fully agree

Confirmed: `DICETcatProtocol::ApplyClockConfig` stores
`selectedClock_ = desiredClock` **before** dispatching to the duplex
controller, the completion lambda has no rollback on failure, and
`PrepareDuplex48k` feeds the stored value into the next `StartIO`
(`DICETcatProtocol.cpp:~190` and `~253`). A rejected 44.1 kHz request leaves
CoreAudio at 48 kHz while the next bring-up programs 44.1 kHz — exactly the
clock-mismatch starvation scenario the PR's own comments warn about elsewhere.
**This is the strongest correctness finding in the review.**

### P2 — "Stale docs/simulator": fully agree

- `print_wiring_status()` in `tools/amdtp_blocking_cadence_sim.py` still
  lists as TODO things this PR did:
  - "`AmdtpTxPacketizer.cpp:43` Configure() still rejects
    sampleRate != 48000" — at the PR head the packetizer accepts the 44.1
    family in blocking mode (rejects only non-blocking ≠ 48 k).
  - The ZTS-period static assert — now present:
    `zeroTimestampPeriodFrames % kMaxBlockingFramesPerDataPacket == 0`
    inside `IsValidAudioHalBufferProfile`, statically asserted for all three
    profiles (`AudioHalBufferProfiles.hpp:60-69`).
- `SAMPLE_RATE_EXPANSION.md` still opens with "Implementation not started."
- Trivial to fix, genuinely misleading if left.

## Bottom line

The **"Request changes" verdict is defensible** — the `selectedClock_`
rollback, the capability intersection, and the stream-format sync are real
pre-merge items, and they align with the project's own design doc rather than
reviewer taste. But the severity should be re-weighted:

- **Downgrade P0:** not an ADK-correctness violation (Apple's default does the
  same thing). Idle-only switching is a legitimate Phase 0 policy — track the
  `PerformDeviceConfigurationChange` wiring as a follow-up, not a blocker.
- **Add the missed finding:** the 15 s blocking poll inside
  `HandleChangeSampleRate` on the ADK work queue ranks above most of what the
  review flagged.
- **Keep as blockers:** `selectedClock_` rollback on failed clock apply;
  stream-format sync (`DeviceSampleRateChanged`) or an empirical demonstration
  it is unnecessary on the HAL-initiated path; the capability-set
  intersection (or at minimum an exact-membership check in
  `HandleChangeSampleRate`).
- **Nits on the review itself:** `SAMPLE_RATE_EXPANSION.md#L225/#L240` anchors
  don't land on the contract text at the PR head (it's ~L210), and the
  failure-ordering narrative is only accurate for the success path.
]