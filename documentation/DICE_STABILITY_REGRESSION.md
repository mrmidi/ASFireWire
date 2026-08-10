# DICE stability regression — b122c74 → dev

**Status:** **Bisect ABANDONED — its GOOD verdicts were unsound (§5). No culprit commit
is established.** One real defect was found and hardware-validated en route: the
CLOCK_SELECT request-vs-achieved bug (§3). Next step is a direct review of the
DICE choreography diff `94cbd067` → `dev` (11 files, +358/−226), not more bisecting.
**Date:** 2026-08-04
**Device:** Focusrite Saffire Pro 24 DSP (GUID `0x00130e0402004713`), 48 kHz, 16 in / 8 out.
**Scope:** loss of *sustained* DICE stream stability. Bring-up correctness and audio
quality at start are not the subject — both are fine on the broken builds.

---

## 0. The symptom, stated precisely

The baseline ran DICE streams **stable for hours**. On `dev` they come up cleanly and
then lose stability unpredictably. This is not distortion and not a bring-up failure;
every affected build starts, locks, and plays.

That framing matters because it invalidates the obvious tests. A 60-second listen scores
a broken build as good, and the TX telemetry does not distinguish them (§4).

---

## 1. The test criterion — necessary but NOT sufficient

> **Correction (2026-08-04).** This criterion detects one failure mode, not all of them,
> and **absence of an ARX1 drop does not mean healthy**. Two facts killed its use as a
> pass/fail gate:
>
> 1. `bed57d5b` showed no drop in 15 s and was scored GOOD; a later run on the same
>    installed build died. A short clean window proves nothing.
> 2. That later failure kept `ext=lock[ARX1] slip[none] healthy` throughout while the
>    stream died anyway — accompanied by `DiceAudioBackend: health probe timed out`
>    (probe missed by ~2 ms; the response landed just after the deadline). So there is at
>    least one failure mode this criterion cannot see.
>
> Use an ARX1 drop as **positive evidence of breakage**. Never use its absence as
> evidence of health, and never on a sample shorter than the tens of minutes the baseline
> was known to survive.

**A spontaneous ARX1 unlock with no corresponding host event.**

`ExtStatusBits::kArx1Locked` (`DICETypes.hpp:230`) reports whether the device has locked
to *our* transmit stream — `DICETypes.hpp:221-222`: "ARX1 is our host→device transmit
stream." So `ext=lock[none]` mid-stream is **the device rejecting what we are sending it**,
reported by the device itself, independent of any host-side telemetry.

Method: grep `ext=lock` and `[FSM]` together. An unlock with an adjacent
`[FSM] terminal state=Idle` / `StopStreaming` is ordinary CoreAudio churn. An unlock with
**no** host event is the regression.

Measured (the "no drop" rows are **inconclusive**, not passes — see the correction above):

| build | ARX1 locks | drops | host event at drop |
|---|---|---|---|
| C `94cbd067` | 09:11:12.383 | none in 23 min | — |
| C `94cbd067` (earlier run) | 08:43:13.442 | 08:43:14.145 | **yes** — FSM Idle @14.172 (host-initiated, not a fault) |
| `b2cc91bf` | 09:55:41.030 | none in ~20 s — too short | — |
| `cded6ada` | 10:04:58.827 | none in 84 s — too short | — |
| `bed57d5b` | 11:20:22.287 | none in 15 s — **later run on this build died anyway** | — |
| **`687fbc2c`** | 09:40:48.366 | **09:40:51.446 (+3.1 s)**, relock +200 ms | **none** — zero FSM/Stop/Recover in the window |

---

## 2. The working path

What a healthy DICE bring-up looks like, from the `94cbd067` capture at 09:11. Order is
load-bearing; every step below was present and in this sequence on all known-good builds.

```
Controller init ─ bus reset ─ topology gen N
  SPro24DspProtocol created, profile matched
  ReadGeneralSections            global=40/380 tx=420/568 rx=988/1128
  ReadGlobalState                clock=LOCKED 48000Hz  status=0x00000201
  ReadTx/RxStreamConfig          TX pcm=16 midi=1 am824=17 · RX pcm=8 midi=1 am824=9
  nub + ASFWAudioDriver published, ADK graph built
StartIO
  Allocate TX isoch resources    numSlots=912 slotSize=296  transferDelay=12800
  TX prefill seeded              912/912 NO-DATA packets BEFORE isoch start
  PrepareDuplex48k               pre-claim owner=0xffff000000000000
  owner claim
  CLOCK_SELECT                   written, or skipped iff device is genuinely at target (§3)
  clock confirmed                via active check, status=0x00000201 rate=48000 locked=1
  Prepared IR (ch 1) / IT (ch 0) IT layout 48 packets, 192 blocks, Z=4
  Device clock stable before isoch start   reads=3
  DoProgramRx / DoProgramTx      RX iso ch 0, TX iso ch 1, stride=280
  Start IR, Start IT             cycle seeded, ring primed 48, run=1 active=1 dead=0 evt=0x00
  re-read config                 confirms iso=0 / iso=1 programmed
[FSM] terminal state=Running → StartStreaming ok
  ext=lock[ARX1] slip[none]      ← device accepts our TX stream
```

Steady-state health markers on a good build (`94cbd067`, 23-minute soak):

- `[TxPrep] late1500` frozen at its startup value, `maxLatUs` frozen, margin floor `min`
  never revisited after the initial drain, margin steady at 672.
- `[TxSyt] pkt` advancing at 7998/s — the isoch cycle rate.
- `decisions/pkt = 0.750` — the known F9 ratio, unchanged.
- `[Zts] UPD` at 31.25 anchors/s (period 1536).
- `ext=lock[ARX1] slip[none]`, no further transitions.

### Geometry across the three reference points

Geometry is **not** the regression — `94cbd067` runs 2.4× the baseline lead and is stable —
but the values are recorded because they were mid-investigation suspects and because
`dev` changes all of them at once.

| | baseline `b122c74` | C `94cbd067` | `dev` |
|---|---|---|---|
| HAL profile | dice-working-1536 | dice-working-1536 | hal-max-4096 |
| ring / io / zts | 1536 / 512 / 1536 | 1536 / 512 / 1536 | 12288 / 4096 / 12288 |
| TX shared slots | 384 | 912 | 1776 |
| steadyLead (pkts) | 336 | 678 | 1728 |
| exposureLead (frames) | 576 | 2400 | 4160–4608 |
| prefill before IT RUN | whole ring (384) | whole ring (912) | **144** |

The prefill change is the one still worth attention: baseline and C both seed the *entire*
TX ring with NO-DATA before IT RUN; `dev` seeds 144. That is 114 ms of runway → 18 ms, and
it is device-observable. It lives in the 08-01→08-03 window, beyond the current bisect.

---

## 3. Defect found en route: the CLOCK_SELECT skip trusts the wrong register

Independent of the stability regression. Introduced by `dfabe0d6`.

`DICEDuplexBringupController.cpp:546` skipped the CLOCK_SELECT write whenever
`preClaimClockSelect_ == diceClock_.clockSelect`, with `preClaimClockSelect_ =
state.clockSelect` (`:443`).

**CLOCK_SELECT is the rate we requested. GLOBAL_STATUS is the rate the device reached.**
They disagree after a rate change the device did not complete. Observed:

```
global state rate=44100 clockSelect=0x0000020c status=0x00000101
                        ^^ rate index 2 = 48000      ^^ rate index 1 = 44100, locked
```

The skip then suppressed the one write that would have forced a relock. Bring-up polled
**188 times over 15 s** and failed with `kIOReturnTimeout (0xe00002d6)`,
`[FSM] Failed cause=Prepare`, then retried and failed again.

**Fix:** require the achieved rate to match as well, mirroring the existing double-check at
`:606-608` / `:720-722` which tests both `NominalRateHz(state.status)` and
`state.sampleRate` against the target. The pre-claim log line now also carries `status=`
and `rate=` so the disagreement is visible directly.

**Hardware-validated** on a device stuck at 44.1 k:

```
09:40:47.528  clockSelect=0x0000020c already requests 48000 Hz but device reports 44100 Hz; rewriting
09:40:47.752  notification RxCfgChg|TxCfgChg|ClockAccepted     ← device accepted
09:40:47.754  clock=UNLOCKED 44100                             ← PLL drops
09:40:47.856  clock=LOCKED 48000Hz                             ← relocked at target
09:40:48.264  StartStreaming ok
```

Do **not** re-exonerate this from a run where the device was already at 48 kHz — that path
never executes. A companion defect in the same area, the nub adopting the device's momentary
rate (`EnsureNubForGuid: applied runtime geometry rate=44100`), is fixed separately by
`7ba06722`.

---

## 4. Observability traps that cost time

Each of these silently produced a wrong or unreadable answer.

1. **`log stream` drops messages.** 34 × `Messages dropped during live streaming` in a
   28-second capture; all `[Zts] CLKDELTA` lines lost, only 15 `[TxSyt]` survived.
   **Use `log show --last Nm`** — it reads the persisted store and drops nothing.

2. **`[TxPrep]` does not discriminate.** C floor 137 vs D floor 132; both recover to 672;
   D's `maxLatUs` (587) is *better* than C's (1640). Judging on TX telemetry scores the
   broken build as healthy.

3. **W > E telemetry went ring-only.** Baseline logs `[PayloadWriter] … deficit=` via
   `ASFW_LOG` (`IsochReceiveContext.cpp:884`) → reaches `log show`. From `94cbd067` it is
   `ASFW_LOG_RING_ONLY` (`DirectAudioReceiveConsumer.cpp:475/488/504`) → internal ring only,
   never mirrored regardless of `ASFWMirrorToOsLog`. Read it via the ring viewer
   (`DiagnosticsHandler.cpp:160`, app side `ASFW/MCP/ASFWMCPLogTools.swift`), available from
   `37f60e34` onward.

4. **RX liveness markers were deleted.** `IR RX CADENCE ESTABLISHED` and
   `IR SYT ZTS QUALIFIED` (`IsochReceiveContext.cpp:531,537` at baseline) are gone after the
   receive-seam refactor. Their absence in a later capture proves nothing.

5. **The receive seam fails silently.** `IsochReceiveContext.cpp:204-214` gates both
   `BeginReceiveBatch` and `ConsumePacket` on `if (receiveConsumer_)`. A null consumer drops
   every RX packet with no counter, no log, and no fault — and every RX-alive signal flows
   *through* that same object. The layering is correct; the failure mode is not observable.

---

## 5. Bisect — ABANDONED, and why

Known good `b122c74c` (2026-07-03) → known bad `dev` `4cd3df4a` (2026-08-03), 290 commits.

**The bisect produced no usable answer and its GOOD verdicts must not be reused.**

| commit | observed | trust |
|---|---|---|
| `37f60e34`, `1c6112ee` | not run — ancestors of `94cbd067` | inherited from the row below |
| `94cbd067` | no failure in **23 min** | the only GOOD with a defensible sample |
| `b2cc91bf` | no failure in ~20 s | **inconclusive** |
| `cded6ada` | no failure in 84 s | **inconclusive** |
| `4914eca9` | no failure in 15 s | **inconclusive** |
| `bed57d5b` | no failure in 15 s → **a later run on the same build died** | **FALSE GOOD** |
| `687fbc2c` | ARX1 drop at +3.1 s | BROKEN (failures are trustworthy) |

**Root cause of the bad methodology:** the pass criterion (§1) was generalised from a
*single* observation — `687fbc2c` failing at 3.1 s — and turned into a ~15 s pass gate,
against a baseline known to run for **hours** and a failure the operator had already
described as unpredictable. Every short GOOD is therefore a statement about the sample,
not the build.

Consequences:

- **`687fbc2c` is NOT established as the culprit.** Its neighbours were never validly
  cleared. The regression could be anywhere from `cded6ada` onward, or earlier.
- Two mechanism theories were pursued and **both are dead**, recorded so they are not
  re-proposed:
  - *Replay-ring overrun* (TX preparation lead outgrowing `RxSequenceReplayState::kCapacity`)
    — refuted: `94cbd067` runs `exposureLead=2400`, far past the baseline's 576, and is
    the most stable build measured.
  - *Sticky `remoteLostGuids_`* in `687fbc2c`'s `DuplexOperationGate` — refuted: the gate
    is only reachable via `AudioCoordinator::OnDeviceRemoved` (`:148`), and the failing run
    contains no bus reset, generation change, removal, or discovery event anywhere near the
    drop. The 8 `IsDeviceOperationCancelled` sites never fired.

**Do not resume this bisect without a pass sample of tens of minutes per point**, which
makes it prohibitively expensive. The recommended direction instead is §7.

---

## 7. Recommended direction: review the DICE choreography directly

`94cbd067` is the best-attested DICE build (23 min clean). The DICE-specific drift from it
to `dev` is small enough to read:

```
git diff --stat 94cbd067..dev -- ASFWDriver/Audio/Protocols/DICE/ \
    ASFWDriver/Audio/Protocols/Backends/Dice* \
    ASFWDriver/Audio/Engine/Direct/Tx/DiceTxStreamEngine.cpp
→ 11 files, +358 / −226
```

Dominated by `DICEDuplexBringupController.cpp` (312 lines) — the bring-up and restart
choreography. Reviewing that against the known-good version answers the original question
(*what changed in the DICE path*) without further hardware cycles, and does not depend on
naming a culprit commit.

Known differences already identified, to be judged **as a whole choreography** rather than
in isolation as was done here:

- the CLOCK_SELECT skip (§3) — defect confirmed and fixed
- CLOCK_ACCEPTED timeout 2000 ms → 150 ms, and removal of the 200 ms active status poll
- clock waits rescheduled off `IOSleep` onto `ITimerScheduler` (`89123de7`)
- `DiceAudioBackend`'s health probe still polls with `IOSleep`
  (`DiceAudioBackend.cpp:378`) — observed timing out by ~2 ms after a 727 ms wait, on a
  codebase where blocking the single Default queue is a documented hazard. Present since
  April, so not itself the regression, but a candidate for something newly *exposed*.
- `SetTimingLossCallback` is a single slot on `IsochService`; at `94cbd067` both
  `DiceAudioBackend` and `AVCAudioBackend` register on it and `AudioCoordinator.hpp:74-75`
  declares `dice_` before `avc_`, so **AVC's registration silently overwrites DICE's** —
  i.e. the known-good build ran DICE with no timing-loss recovery at all. `687fbc2c`
  centralises this and gives DICE a live path for the first time. A latent defect worth
  fixing on its own; it did not fire in the failing run.

---

## 6. Method notes

Per-build recipe (old-branch worktree, so `dev` stays untouched):

```bash
git checkout -- ASFW.xcodeproj      # reset generated project + SwiftPM pins
git checkout <hash>
./build.sh
```

The §3 CLOCK_SELECT patch currently lives **uncommitted** in the old-branch worktree at a
detached HEAD (`DICEDuplexBringupController.{cpp,hpp}`). It is validated and independent of
the stability question — land it on `dev` before that worktree is reset or moved.

**Sample duration is the trap.** A run that shows no failure proves only that the sample was
short. The baseline ran for hours; `bed57d5b` survived 15 s and died later on the same
installed build. Any "healthy" claim needs tens of minutes.

Capture (one continuous file; the build banner from `ControllerCore::LogBuildBanner()`,
`ControllerCoreLifecycle.cpp:480`, separates sections and carries hash + branch):

```bash
log show --last 20m --info --debug --predicate 'eventMessage CONTAINS "ASFWDriver v" OR eventMessage CONTAINS "[Controller] Build" OR eventMessage CONTAINS "DIRTY BUILD" OR eventMessage CONTAINS "[DICE]" OR eventMessage CONTAINS "[Audio]" OR eventMessage CONTAINS "[DirectAudio]" OR eventMessage CONTAINS "[Isoch]" OR eventMessage CONTAINS "[TxSyt]" OR eventMessage CONTAINS "[Zts]"' > tmp/dice-bisect.log
```

`--info --debug` is required; `[Zts]`, `[TxPrep]` and `[TxSyt]` log below default level.
Add `[Async]` only for short bring-up windows — it is ~8 lines per DICE transaction.

**Device hygiene:** confirm `status=0x00000201` / `rate=48000` before each run. A device left
at 44.1 k makes every point fail bring-up instead of testing stability.
