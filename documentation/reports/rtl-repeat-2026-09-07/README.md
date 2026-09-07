# Fixed-setting RTL capture — 2026-09-07

User reported replugging the controller immediately before this capture. Three
runs were planned; capture stopped after run 2 admitted no signal. No driver
build/install, reset, gain adjustment or transmit-depth tuning was performed.
Only the standalone RTL tester was compiled from the current source and its
self-test run. Source HEAD: `e3bf0765`; exact installed binary identity was not
independently established. MCP generation was 3 at preflight.

Duet, 48 kHz, channel 0 output to channel 0 input, 20 trials/run, 4096-frame
window, amplitude 0.90. Requested and observed client buffer: 128 frames
(preflight before setting it was 512). Each invocation starts and stops IO;
these are sequential runs on one controller lifetime, not uninterrupted IO.
The preparation trace reports target 120 packets, guard 48, nominal lead 720
frames. Declared latency/safety: 67/50 output, 40/50 input.

| Run | Accepted | Raw RTL | RTL_ts | SNR | Result |
|---|---:|---:|---:|---:|---|
| 1 | 14/20 | 422.95 fr / 8.811 ms | 66.95 fr / 1.395 ms | 31.3 dB | Six no-signal rejections; admitted raw/ts SD 0.01 fr |
| 2 | 0/20 | unavailable | unavailable | not reported | All no signal; input peak 0.004 versus 0.067 in run 1 |
| 3 | not run | — | — | — | Stopped after run 2 failure |

Both runs reported 128-frame callback spans and zero overloads, delivered-frame
gaps, re-anchors, clock disagreements and missing timestamps. Run 1 scheduling
was 356 frames measured and declared; residual was -40.05 frames. These do not
establish the absolute timestamp origin or a converter/input/output split.

## Captured stop chronology

The fatal errors are **after StopIO begins**, not evidence of an in-run fatal
causing the missing impulses:

- **Run 1:** StopIO sequence 19849; uncommitted-slot fatal 19861
  (`fatalAbs=57120`, `committedEnd=57120`); fatal stop 19864; asynchronous fault
  handler 19895 reports `txWasActive=0` and declines re-arm at 19896.
- **Run 2:** StartIO 20011; super::StartIO succeeds at 20163. StopIO 21108;
  iPCR disconnect 21110–21116; uncommitted-slot fatal 21117
  (`fatalAbs=56880`, `committedEnd=56880`); fatal stop 21120; oPCR disconnect
  completes 21132; Idle 21151; inactive fault handler 21154–21155.

This is consistent with StopIO disabling/draining the producer while transport
refill continues during protocol disconnect and reaches the committed frontier.
It is a shutdown-ordering defect to investigate. The later inactive-fault guard
prevents repeated re-arms in this capture. It does **not** establish why run 2
had no useful returned signal while IO was active. Do not label that unanswered
part as a lap failure: no TxLapRecover record was captured.

BackendTiming conversion-failure records are also retained. Their presence and
relationship to completion/correlation timestamps need separate investigation;
no physical cause is assigned here.

Post-stop health misleadingly reports `streaming=false` alongside transmit
`running`, `faulted=false`, and `receivingData`. Those cumulative/status fields
are not proof of currently working playback or capture.

## Evidence and retention

[Readable selected-category chronology](evidence/trace-readable.txt),
[run 1](evidence/run-1-rtl.txt), [run 2](evidence/run-2-rtl.txt),
[provenance](evidence/provenance.json), and per-run health/cursor snapshots are
saved beside the original paginated MCP responses. Polling captured Audio,
DirectAudio, Isoch, Zts, Controller, Hardware, BusReset, CMP and FCP records at
approximately one-second intervals while the tester ran. Async/Oxfw chatter
was excluded; the capture is not an all-category bus trace.

Ring stats: oldest sequence remained **1**, latest 18578 before / 21255 after,
**zero dropped records**. No retention eviction occurred in the captured window.
The tester output contains aggregate results and rejection counts; it does not
export individual trial waveforms. No numeric result was salvaged from run 2.

## Apple Music playback failure captured afterward

A passive eight-second check found advancing TX/RX, no new reported content
faults, and alignment toggling 0/-8 frames. User subsequently reported playback
had died. No RTL or hardware mutation was performed during either check.

[Passive snapshot](evidence/apple-music-passive-check.json) and
[failure chronology plus snapshots](evidence/apple-music-failure.json) preserve
these records, starting at the prior capture cursor 49590:

1. **55581:** `TxLapRecover prevSlot=0 slot=26 elapsedCycles=74 naive=26
   lifted=74 lapsLost=1`. This is an inferred advance, not independently proven
   execution; elapsed cycles alone cannot distinguish a lap from skipped cycles.
2. **55583:** `TxLapAbandon delta=74 ring=48 abandoned=26 firstAbs=1076832
   resumeAbs=1076858`.
3. **55584:** payload seal mismatch at packet **1076880**, shared slot 0,
   length 72: expected `39634a13d3202647`, observed `b60345242c0274c1`.
   Committed end 1076952, completion cursor still 1076832.
4. **55586:** TX RUN cleared and interrupts masked. No intervening StopIO:
   this is an actual playback fault, unlike the earlier RTL teardown fatals.
5. **55588–55589:** fault handler sees `txWasActive=1`, switches producer off,
   and prefills 168 packets for recovery.
6. **55730–55736:** recovery exhausts that prefill at packet 168
   (`committedEnd=168`, `prepReq=1`, `prepHandled=0`), fatal-stops again;
   inactive guard declines another re-arm. About **118 ms** separates the first
   and second fatal stops.

The first failure is exactly at old completion + one descriptor ring:
1076832 + 48 = 1076880. The new completion walk skips the first 26 positions and
walks 48 *new absolute identities*, crossing beyond the old armed ring. A
replayed descriptor does not acquire the next prepared shared-slot identity
merely because wall-clock cycles passed. The observed boundary failure therefore
strongly implicates the lap walk's identity/seal attribution; it is not proof
that a producer corrupted the bytes of a correctly identified transmitted
packet. Actual executed descriptor-to-payload bindings must govern retirement.
The skipped-cycle ambiguity in the initial inferred delta remains separately
unresolved.

Health still reports `streaming=true`, transmit `running`, `faulted=false`
after both fatal stops, while RX counters continue advancing. This independently
confirms that the current summary cannot be used to establish healthy playback.
Generation stayed 3; dropped records 0. Oldest retained sequence was 18346 at
fault capture, before the entire queried window, so this failure chronology was
not evicted. No restart, rebuild or reset was issued.
