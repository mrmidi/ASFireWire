# BUGLIST — 2026-07-17 investigation (right-only playback, PHASE 88 streaming, device name)

Evidence sources: FireBug captures `!right-only.txt` (Saffire, 22:15) and `!duet-right.txt`
(Duet, 22:20), unified-log excerpts for the PHASE 88 start failure (22:33, branch), the
22:56 branch DICE failure, and the 23:02–23:09 session on **main (405cb99)**. All three
devices were exercised the same evening.

## Verdict on "roll back everything" (updated 23:15)

**A mass revert is not justified.** Evidence from the main-build session:

- Saffire/DICE **streams fine on main** (23:02:42 → Running, ZTS in 66 ms, PCM on the wire),
  and its single failed attempt on the branch (22:56) died at `ReservePlayback` =
  `kIOReturnNoResources` from a drained bandwidth pool (Bug 6) — the *identical* first-attempt
  failure occurred on main at 23:02:41. There is **no evidence the branch broke DICE**.
- The right-only-channel bug was **host-side CoreAudio speaker configuration** (Bug 1,
  user-confirmed) — no driver change involved.
- The Duet 44.1-vs-48 k mismatch (Bug 5) reproduces **on main**.
- PHASE 88 fails **on main too** (ConfirmStart `kIOReturnNotResponding` loop, 23:08–23:09)
  — the branch's profile-shadowing (Bug 2a) makes it worse but is not the sole cause (Bug 7).
- The dext **crashed on main** at the device-removal bus reset (Bug 8).

The only confirmed branch-specific regression is Bug 2a/3 (profile shadowing) — a one-line
lookup-order fix, not a revert.

---

## Bug 1 — Playback audible on right channel only (Duet **and** Saffire) — **CONFIRMED, host-side**

**Status 23:0x: user changed the channel assignment in CoreAudio (Configure Speakers) and
set it back — stereo restored on both devices.** Root cause confirmed as the persisted
speaker/stereo-pair configuration, not driver code. The durable driver-side fix below
(unique per-device UID) is still needed so one device's speaker config can never poison
every ASFW device again.

### Wire evidence

- **Saffire** (our TX, isoch ch 0, CIP `000900xx 9002xxxx` → DBS=9 = 8 PCM + 1 MIDI, FDF 48 k):
  every 9-quadlet data block reads `[00000000, L, R, 0, 0, 0, 0, 0, 80000000]`.
  Stereo sits in **wire slots 1 and 2 instead of 0 and 1**; slot 0 is a raw-encoded zero;
  the MIDI slot (0x80000000) is at the correct position 8. Constant across every block and
  packet — no drift, a pure one-slot displacement of the PCM only.
  (Raw sign-extended 24-in-32 with no AM824 labels is **intentional** for Saffire TX —
  `FocusriteSaffireProfile.cpp:48`, since d1d0288b, pre-dates the HW-validated milestone. Not a bug.)
- **Duet** (our TX, isoch ch 0, DBS=2, FDF 44.1 k): every block is
  `[40000000, music]` — slot 0 is an **AM824-encoded exact digital zero** (0.0f through the
  encoder, not template fill), slot 1 carries one music channel. The second music channel
  appears nowhere → it was dropped, not shifted into the next block.

### What was ruled out (all verified straight-through, buffer channel *n* → wire slot *n*)

| Component | Verdict |
|---|---|
| `AmdtpPayloadWriter::WriteFloat32Interleaved` (`AmdtpPayloadWriter.cpp:126-153`) | writes slot `ch` at `dest + ch*4`, source `source[srcOffset+ch]`, `srcOffset=0` for the primary stream. Pinned by green test `AmdtpDirectTxTests.PayloadWriterReadsMappedInt32RingDirectly` (ch0 → payload bytes 8–11). |
| `AmdtpTxPacketizer::WriteDataPacketDefaults` | zero-fills payload, writes `defaultNonAudioSlotWord` only into slots `[pcmChannels, dbs)` — explains intact MIDI slot; cannot displace PCM. |
| `DiceStreamConfigMapper::ToAmdtpConfig` | 1:1 field copy, no offset. |
| Slot image ↔ wire alignment | CIP header appears intact at wire bytes 0–7, so `slot[0] == wire[0]`; no ±4-byte slab/descriptor offset possible. |
| Packet-image writers | `AmdtpPayloadWriter` is the **only** PCM-encode call site in the tree (grep `PcmSlotCodec::EncodeFloat32`). |
| Shared-buffer plumbing | `IOUserAudioStream::Create` gets the *same* IOBMD that `outputMap` maps (`ASFWAudioDriverGraph.cpp:498-500`, `ASFWAudioDriverDirect.cpp:304-322`); stream format channels == buffer stride == `device.outputChannelCount`, cross-checked hard at bind (`ASFWAudioDriverGraph.cpp:398-411`). `PublishPlaybackRingWriteEnd` is bookkeeping only — nothing else writes `outputBase`. |

⇒ The shared output buffer **genuinely contains** `[0.0, L, R, …]` per frame. The only writer
of that buffer is the CoreAudio HAL.

### Root-cause hypothesis (fits every observation, both devices, incl. the Duet's dropped R)

CoreAudio is mixing the client's stereo onto the device's **preferred stereo pair = channels
{2,3}** (1-based) instead of the default {1,2}:

- Saffire (8-out): L→idx 1, R→idx 2 → wire slots 1,2 — exactly observed.
- Duet (2-out): L→ch 2 = idx 1, R→ch 3 which **does not exist** → dropped — exactly observed
  (slot 0 = exact 0.0 every frame; the surviving channel is L, so the right speaker plays
  left-channel content — consistent with the Saffire percept too).

**Why every ASFW device at once:** `ASFWAudioDriverGraph.cpp:257` hardcodes the device UID
`"ASFWAudioDevice"` for **all** devices. macOS persists Audio MIDI Setup speaker
configuration (incl. the stereo pair) **per device UID**. A stereo-pair set while the 10-out
"BeBoB Device" (PHASE 88) was active poisons the Duet and Saffire as well, because they
present the same UID. Timeline fits: PHASE 88 bring-up work and both captures happened the
same evening. This also explains why nothing in the wire/audio code path changed and playback
still "broke".

### Verification (no code change)

1. Audio MIDI Setup → select the ASFW device → **Configure Speakers…** → check which
   channels are assigned L/R. Expected: 2 & 3 (or anything ≠ 1 & 2).
2. Set the pair back to 1 & 2 → replay → FireBug should show music in slots 0,1 and both
   speakers alive.
3. If AMS shows 1 & 2 anyway, the hypothesis is wrong → next probe: log the first 12 floats
   of `outputBase` at `WriteEnd` (one-shot instrumentation) to see where HAL puts the samples.

### Fix strategy

- Make the device UID unique per device, e.g. `"ASFW-<GUID hex>"` (keep `modelUID` as the
  human-readable model). One-time side effect: users lose persisted per-device volume/speaker
  settings once.
- Optionally publish an explicit default preferred-stereo-pair {1,2} so a stale host
  preference cannot silently re-map output.

**✅ UID fix applied on refactor/BeBoB (2026-07-17):** `ASFWAudioDriverGraph.cpp` now builds
the device UID as `ASFW-<GUID hex>` per device. Expect each device to appear "new" to
CoreAudio once (persisted volumes/speaker config reset — which is exactly the point).

---

## Bug 2 — PHASE 88 (TerraTec) streaming completely broken

### 2a Root cause: per-GUID generic BeBoB profile shadows the curated Phase88Profile

`AudioProfileRegistry::FindProfile` consults the **dynamic per-GUID map first**
(`AudioProfileRegistry.cpp:26-32`, "Per-GUID BeBoB profiles take precedence"), while the
discovery side documents the opposite intent — `AVCDiscovery.cpp:425-427`: *"For Phase88 this
is superseded by the static Phase88Profile in the registry"*. Since a747bd19 + 20491bee,
`AVCDiscovery.cpp:428` registers a dynamic `BeBoBProfile` for **every** discovered BeBoB
device, so for the PHASE 88 the curated profile is now unreachable. Regressions inherited
from the generic profile:

1. **Name**: `BeBoBProfile::Name()` = `"BeBoB Device"` (`BeBoBProfile.hpp:24`) replaces
   `kPhase88RackFwModelName` → the wrong name in the screenshot (this *is* Bug 3).
2. **`emptyPacketsDuringIdle` regresses `true` → `false`**
   (`Phase88Profile.cpp:91` vs `BeBoBProfile.cpp:123`): idle cycles now emit 8-byte CIP
   NO-DATA instead of genuine zero-length packets — reverting the wire-validated FW-105
   recipe (946be2c1, validated 2026-07-16). Log confirms the consequence: 36,282 IT packets
   sent, **`IT WIRE final data=0`** (we never left idle), the device never began multiplexing
   (`Phase88Profile.hpp` wire note: it starts only after receiving host packets for ~1 s),
   so **`initial hardware ZTS timed out after 4000 ms`** → StartIO failure → teardown.
3. Rate set, safety offsets, latency figures and any future curated quirks silently regress
   the same way. (`ApogeeDuetProfile` is *not* shadowed — the Duet takes the
   `IsApogeeDuet` discovery path that never registers a dynamic profile.)

**Fix strategy:** invert the lookup order in `FindProfile` — static curated matches
(ApogeeDuet, Phase88, DICE registry) first, the per-GUID dynamic map only as the generic
BeBoB fallback.

**✅ FIXED on refactor/BeBoB (2026-07-17):** `AudioProfileRegistry::FindProfile` reordered
(curated statics → DICE registry → dynamic per-GUID map → DICE generic fallback). Regression
tests added: `DiceProfileTests.DynamicBeBoBProfileNeverShadowsCuratedPhase88` (registered
dynamic profile for the Phase 88 GUID must still resolve to the curated name and
`emptyPacketsDuringIdle == true`) and `…ServesUncuratedBeBoBDevices` (dynamic profile still
serves unknown BeBoB devices, generic DICE only after unregister). All 14 DiceProfileTests
pass; full dext build green.

### 2b Secondary: AT Request context wedges permanently after the failed start

At 22:33:18.194, mid-teardown:
`ctx=AT Request txid=91 gen=0 P2_FALLBACK cause=RUN0 ctrl=0x00000000` — the hot-append found
the AT Request context stopped. From then on **every** async write dies at `ATPosted` with no
ACK (t29, t31…), so FCP/CMP is dead, attempts 2 and 3 fail at `Prepare`
(`StartStreaming failed stage=AudioDuplexCoordinator kr=0xe00002bc`), and the device is
unusable until re-plug/reset. Notes:

- `ATManagerImpl.hpp:190-197` marks exactly this window as the *"suspected AT-resp loss
  window (LS-9000 wedge investigation)"*.
- The PATH-1 fallback *is* taken (`ATManagerImpl.hpp:74-85`), yet later submissions never
  reach the wire — so the PATH-1 re-arm after RUN0 is not actually restarting the context
  (CommandPtr/ring state), or the ring was left inconsistent by `UnlinkTail_`.
- The trace prints `gen=0` while the bus is at generation 2 — the AT manager's internal
  generation looks reset during the audio teardown; find who resets/stops the shared AT
  engine from the `failStart` path.

**Fix strategy:** reproduce with AT trace verbosity; audit the audio-teardown path for
anything that stops the AT Request context or resets `ATManager` state while transactions
are in flight; make the RUN0 fallback provably re-arm (write CommandPtr, set RUN, verify
readback) and add a wedge counter/telemetry.

### 2c Teardown ordering + leaked oPCR

In the failure teardown (22:33:18.191): IRM bandwidth + channels are released **before** the
CMP disconnects run — the known-bad order (same class as the earlier leaked-channel-0 issue).
The oPCR clear then fails (`ReadQuadlet oPCR` timeout → `BreakBothConnections: oPCR clear
status=5`), leaving the device's oPCR connected (`0x81018082`) while its IRM resources are
already freed. **Fix strategy:** break CMP connections first, release IRM after, and make the
oPCR clear robust against the device being slow/wedged (bounded retry, generation-checked).

---

## Bug 3 — Device name shows "BeBoB Device" instead of "PHASE 88 Rack…"

Same root cause and fix as **2a** (profile shadowing). No separate work needed.

---

## Bug 4 — CIP SID hardcoded to 0 (wrong on any bus where we are not node 0)

Every profile hardcodes `sid = 0` (`Phase88Profile.cpp:24`, `ApogeeDuetProfile.cpp:20`,
`FocusriteSaffireProfile.cpp:21`, `GenericDiceProfile.cpp:19`) and the value flows unchanged
into the CIP builder (`AmdtpTxPacketizer.cpp:80`). Wire proof: both captures show CIP SID=0;
in the Duet capture our Mac was **node 2** (Saffire capture only looked right because the Mac
happened to be node 0). IEC 61883-1 requires SID = transmitter node ID; Linux
`amdtp-stream.c` writes the card's node id and refreshes it across bus resets. Also note
`ASFWAudioDevice.cpp:257-259` logs `txConfig.sid` as `channel=` — the field is being
overloaded as an isoch-channel carrier in at least one layer.

**Fix strategy:** plumb the live local node ID into the CIP config at stream (re)start and on
every bus-reset/generation change; separate `channel` from `sid` in the config structs so the
two can't be conflated. Most devices tolerate SID=0, but some firmwares validate it — this is
a wire-conformance bug.

---

## Bug 5 — Duet: host/device sample-rate disagreement — **CONFIRMED live on main**

Wire capture: our TX CIP says **FDF 0x01 (44.1 k)** while the cadence and the device's own
stream (ch 19, FDF 0x02) run **48 k**. Main-build session (23:03) proves it numerically:
`[Zts] CLKDELTA … ppm=-81264…-81296` ≈ exactly 44100/48000 − 1 = −8.13 % — the Duet delivers
48 k frames into a session configured at 44.1 k. User-observed symptom matches ("trying to
start in 44100").

Where the rate programming goes missing (main and branch share this code):

1. The start transaction *does* run an idle clock apply
   (`AudioDuplexCoordinator.cpp:1535-1569` → `deviceControl.ApplyClockConfig(desiredClock)`),
   and `ApogeeDuetProtocol::ApplyClockConfig` (`ApogeeDuetProtocol.cpp:520`) implements a
   full read→write→verify signal-format transition. **But it short-circuits**: if the
   pre-read signal format already claims the desired rate, the write is skipped
   (`ApogeeDuetProtocol.cpp:622,645` `MatchesRequestedRate(...Before...) → skip`). The
   Duet's signal-format read reports 44.1 k ("Device is locked to 44100 Hz" at discovery)
   while its real stream clock runs 48 k — so the transition is skipped, verification
   "passes" against the same lying register, and the mismatch survives to streaming.
   This skip-if-matches optimization is the prime suspect for "where did the rate-set go":
   an unconditional write would have kicked the device to the session rate.
2. `WaitForStableGlobalClock` printed `Device clock stable before isoch start rate=48000
   status=0x00000000` **for a 44.1 k Duet session** — it prints `desiredClock.sampleRateHz`
   and compared against a `ReadDuplexHealth` result whose fields are evidently synthetic for
   the AV/C path (status 0x0, rate matching "desired" of 48000 while the session was 44100
   — the desired clock handed to the coordinator did not match the session rate either;
   check who builds `desiredClock` for AV/C backends).
3. TX replay then slaves our cadence to the device's real 48 k pattern
   (`ReplayOverridesLocalCadencePerPhysicalCycle`) while FDF/config stay 44.1 k.

**Fix strategy:** (a) make the Duet clock transition write the signal format
unconditionally at stream start (drop the skip, or verify against the *live RX FDF*
rather than the same register that was read before); (b) make the AV/C
`ReadDuplexHealth`/clock-stability data honest (real rate or explicit "unsupported");
(c) reconcile `desiredClock` with the actual session rate for AV/C backends; (d) assert
replay-cadence rate == config rate and fail loudly instead of streaming skewed.

---

## Bug 6 — Deterministic first-attempt `ReservePlayback` failure: BANDWIDTH_AVAILABLE reads 0x3f (NEW, both builds)

Both the branch (22:56, Saffire) and main (23:02:41 Saffire; 23:03:37 Duet) fail the **first**
StartIO after a driver (re)start at `ReservePlayback` = `kIOReturnNoResources` (0xe00002be)
because the local IRM read returns `BANDWIDTH_AVAILABLE = 0x0000003f` (63 of 4915 units).
The **immediately following** attempt reads a sane pool (`0x12f5` = 4853) and succeeds — with
no bandwidth-release CAS logged in between. Observations:

- `0x3f` is suspicious in itself: it is the "unset BUS_MANAGER_ID" pattern (0x3F) that the
  BM election reads/CASes moments earlier (`[BM Election] WON … oldValue=0x3F`). Prime
  suspect: the local CSR loopback read path (IRMClient selector→register mapping) returning
  the wrong register's value in some window, rather than true exhaustion — 63 units cannot
  be a real residue of balanced 884/884 alloc/release cycles.
- Alternative: device-side CMP restoration after bus reset (BeBoB/DICE firmware re-allocating
  its old connections' bandwidth within ~1 s of reset) transiently draining the pool, then
  releasing. Distinguishable by logging the raw register + reader identity at fail time.
- Separately, the channel pools erode across the evening
  (`CHANNELS_AVAILABLE_31_0` down to `0x00000287`, and `_63_32 = 0xfffff899` — high channels
  taken that we never allocate), consistent with leaked leases from failed teardowns, the
  23:03:28 crash (no releases at all), and possibly device-side allocations.

**Fix strategy:** instrument the ReservePlayback failure path to dump both IRM CSRs *and*
BUS_MANAGER_ID in one shot; audit `IRMClient` local CSR selector mapping vs the BM-election
loopback; make StartIO retry ReservePlayback once internally (the second attempt reliably
succeeds today); reconcile leases on driver start (release-all-local before first use).

---

## Bug 7 — PHASE 88 fails on **main** too: ConfirmStart `kIOReturnNotResponding` restart loop (NEW)

23:08–23:09 on main (curated Phase88Profile active — "Phase 88 hardware mixer configured and
unmuted" runs, so Bug 2a shadowing is *not* in play here): every attempt reaches Running,
then fails ConfirmStart with 0xe00002ed (`kIOReturnNotResponding`) and tears down;
`restartId` churned 16 → 28. Each short-lived IT context shows `IT WIRE final data=0` —
we sent only NO-DATA for the context's lifetime, and the device never began multiplexing.
Additional evidence of accumulated device/host state damage:

- The device's PCRs carry stale bits from crashed/failed sessions: iPCR read
  `0x80140000` (p2p=0 but channel field still 20), oPCR `0x80009482`/`0x80149482`.
- 23:09:50 attempt: the channel allocator requests **channel 20 twice** in one start (second
  request finds it self-taken, falls back to ch 0), flipping the d2h/h2d channel roles
  between attempts (`ir=20 it=0` → `ir=0 it=20`).

**Interpretation:** Bug 2a (branch) worsens Phase 88, but the base bring-up is broken on main
as well tonight — either the device is wedged from the evening's crashes/leaks (BeBoB
firmware state; power-cycle the PHASE 88 before further debugging) or the FW-105 warm-up
still never emits the DATA packets the device needs to lock. **Next step:** power-cycle the
device, single clean start on main, and capture whether our IT ever emits DATA during the
4000 ms budget and whether the device's stream appears — that separates device-wedge from
warm-up logic.

**Also fix:** the per-attempt channel preference should not request a channel it just
allocated (double ch-20 request), and role flips between retries should be avoided while a
device may hold stale PCR state.

---

## Bug 8 — dext crash on device-removal bus reset (main, FW-60 class) (NEW)

23:03:28, main: Saffire unplug → bus reset → device marked lost → nub termination →
`net.mrmidi.ASFW.ASFWDriver[3955] Corpse failure, too many 6` →
`IOPCIDevice::ClientCrashed_Impl` → dext force-closed and relaunched
(`Driver … has crashed 2 time(s)`). `ASFWAudioDriver: StopAudioStreaming failed in Stop():
0xe00002bf (kIOReturnIPCError)` immediately before. This is the FW-60 cross-service
teardown-crash class, alive on main; every crash also leaks all IRM leases and device PCR
state (feeds Bugs 6 and 7). Pull the `.ips` for this crash before it rotates.

---

## Minor notes (recorded while auditing)

- `BeBoBProfile` derives **both** Tx and Rx geometry from
  `discoveryModel.input.supportedFormations[0]` only (`BeBoBProfile.cpp:32-39`) — wrong for
  BeBoB devices with asymmetric channel counts, and formation[0]'s rate becomes the default
  rate regardless of direction.
- `PublishBeBoBAudioConfig` hardcodes the 10 PCM + 1 MIDI duplex check for *all* generic
  BeBoB devices (`AVCDiscovery.cpp:430-444`) — any BeBoB device without that exact formation
  is refused a nub.
- FW-105's 4000 ms ZTS budget is present in **both** profiles (`BeBoBProfile.hpp:41`,
  `Phase88Profile.hpp:38`), so the budget itself did not regress — only the idle-packet
  policy did (Bug 2a).
