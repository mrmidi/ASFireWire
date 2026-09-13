# MIDI fix review — 2026-09-14

Reviewed the uncommitted working-tree fixes on `c73cc986`. Driver sources were
not edited. The previous findings are partly fixed; qualification is still
blocked by the remaining correctness issues below.

## Remaining findings

### [P1] A teardown timeout must not be treated as successful quiescence

`ASFWDriver/Midi/DriverKit/ASFWMidiDevice.cpp:245-254`

The new atomic pointer removes the ordinary-pointer race, but the writer wait
stops after 1000 iterations of IODelay(100), approximately 100 ms, regardless
of whether activeWriters is still nonzero. The following code resets converters
that the writer can still be mutating. A suspended or unusually delayed writer
therefore races Reset; the comment claiming callers are quiesced is false on
the timeout path. The device/driver teardown then proceeds toward releasing
state and mappings. Do not continue destructive cleanup when quiescence fails:
retain the callback state and mapping until completion, or complete a verified
framework stop/join first. Check SetIOBlock's return value as well; its header
does not establish that removing a block joins every invocation already entered.
This is a code-level timeout-path defect, not a reproduced hardware crash.

### [P1] RX still joins messages across a gap that occurs during draining

`ASFWDriver/Midi/DriverKit/ASFWMidiDevice.cpp:282-299`

Discarding backlog fixes the old case where the gap was already visible at
entry. The counter is still sampled only once before the entire drain loop,
however. The producer can mark a gap and append post-gap bytes after that
sample; Peek/Push then consume them with pre-gap data or existing parser state
and Send publishes the fabricated message before any recheck.

The new reproducer models exactly this interleaving: queue `90 3C`, let the
consumer perform its gap check, mark a discontinuity, append `40`, then resume
Peek/Push. Result: `0x20903C40` is emitted. Carry ordered gap positions through
the ring, or use a synchronized discard/snapshot protocol that detects a gap
before publishing decoded messages. Merely checking at the next wake is too
late, and the new while-loop makes it possible to consume multiple producer
updates without ever rechecking.

### [P2] AM824 NO-DATA is still accepted by the new format guard

`ASFWDriver/Audio/Engine/Direct/Rx/RxAudioPacketProcessor.cpp:87-90`

Checking FMT == 0x10 rejects other formats but does not reject AM824 NO-DATA.
That form still uses FMT 0x10 and is identified by FDF 0xff. The added
kFormatNoData = 0x3f constant is neither consulted nor a substitute for checking
that FDF. Local Linux `references/linux-sound-firewire-stack/firewire/amdtp-stream.c:768-770`
sets the data-block count to zero for FMT 0x10/FDF 0xff even with payload bytes.

The reproducer calls the actual ProcessPacket with DBS 2, FMT 0x10, FDF 0xff,
SYT 0xffff and a MIDI-looking payload slot. It delivers one MIDI byte. Validate
FDF/disposition before extraction and test payload-bearing NO-DATA at the
processor boundary, not only against the pure demux helper.

## Readiness/epoch finding: improved, not yet closed

Moving allocation to ASFWMidiNub::init fixes the null-buffer early-arm path.
It does not itself enforce a nonzero initialized epoch before RegisterService:
Start still publishes unconditionally, the coordinator arms separately, and
the MIDI driver snapshots the epoch once. There is still no explicit ready
handshake or stale-epoch rebind. Closing this requires either a source-level
ordering guarantee for Create/Start/EndpointReady on the actual queues or a
handoff initialized before publication. I did not reproduce the adverse
startup ordering on hardware and am not treating it as a measured failure.

## Disposition of the original nine findings

| Original finding | Current assessment |
|---|---|
| 1. Interrupted-SysEx bounds | Fixed. State-dependent capacity check rejects the two-word span and accepts three. ASan reproducer no longer overflows; regression test passes. |
| 2. Callback teardown | Partly fixed. Atomic pointer and writer tracking improve it; timeout still proceeds unsafely. |
| 3. RX attachment after restart | Addressed in code. Detaching per-run consumers no longer removes the endpoint MIDI sink/wake. Hardware restart not exercised. |
| 4. RX batch wake | Addressed in code. Transport calls EndReceiveBatch, which issues a final wake in addition to the early wake. All bytes in that batch are published before the final notification. |
| 5. Large TX callback tail | Addressed for the reported case. The new loop consumes all 256 words/768 bytes in the reproduction; truncated input now returns an error. Ring saturation can still accept a prefix before returning NoSpace, so this is not whole-callback atomicity. |
| 6. Rejected SysEx Start | Addressed for the reported case. AbortSysEx suppresses later Continue/End; new test passes. Recovery of an already-started physical SysEx whose End is dropped remains a policy/qualification item. |
| 7. RX discontinuity position | Not fixed for concurrent gaps. New deterministic interleaving still fabricates a Note On. |
| 8. Readiness/epoch handoff | Early allocation fixed; publication/epoch ordering still needs proof or explicit synchronization. |
| 9. RX format/disposition validation | Partly fixed. Wrong FMT rejected; AM824 FDF 0xff NO-DATA still emits bytes. |

## Validation

- Rebuilt the five MIDI targets: all 174 selected Midi/Mpx/Ump tests passed.
- Rebuilt RxAudioPacketProcessorTests: all 7 tests passed.
- Rebuilt IsochReceiveContextTests: all 27 enabled tests passed; 1 disabled.
- Rebuilt the original bounds reproducer with AddressSanitizer: the two-word
  output span is now rejected without an overflow.
- New deterministic cases in `repro.cpp` demonstrate the remaining gap and
  FDF defects, and confirm full consumption by the updated TX loop.
- No Xcode build, installation, MIDI host scheduling probe or hardware test.
- `git diff --check` reports trailing whitespace in CIPHeader.hpp and trailing
  blank lines in the two changed converter test files; these are not functional
  findings.

From the repository root, build the new reproducer after the host test support
library has been built:

```sh
clang++ -std=c++23 -fsanitize=address -g -DASFW_HOST_TEST \
  -I . -I ASFWDriver -I tests/mocks -I tests/support \
  documentation/reviews/midi-2026-09-14/repro.cpp \
  ASFWDriver/Midi/Ump/MidiByteStreamToUmp.cpp \
  ASFWDriver/Midi/Ump/UmpToMidiByteStream.cpp \
  ASFWDriver/Audio/Engine/Direct/Rx/RxAudioPacketProcessor.cpp \
  build/tests_build/libasfw_test_support.a \
  -o /tmp/asfw_midi_rereview_cases
/tmp/asfw_midi_rereview_cases
```

Observed output:

```text
gap during drain: 1 UMP word(s), 0x20903c40
FMT10/FDFff NO-DATA: MIDI bytes delivered=1
updated TX loop: 256/256 words, 768 bytes
```
