# MIDI implementation review

Reviewed `c73cc986` (2026-09-13). Review only: driver sources were not changed,
and no installation or hardware operation was performed.

The implementation contains the planned capability projection, MIDIDriverKit
service/nub, byte rings, converters, RX extraction and audio-active TX
composition. The offer producer/notification route is now connected. These are
substantial pieces, but the integration is not ready for Saffire qualification.
The findings below distinguish reproduced pure-code failures from concurrency
and lifecycle defects found by tracing the call sites.

## Findings

### 1. [P1] Reserve three output words for an interrupted SysEx

`ASFWDriver/Midi/Ump/MidiByteStreamToUmp.cpp:187-193`

`Push` accepts a two-word remaining span, but an `F6` byte during SysEx first
calls `TerminateSysEx` (two words) and then emits Tune Request (one word at
line 123). This exceeds the advertised `kMaxWordsPerByte = 2` bound and writes
past the caller's span. Reproduced under AddressSanitizer: feed `F0 01`, then
feed `F6` with a two-word output buffer. Reserve the actual worst-case output
before changing parser state, and test interruption at the capacity boundary.
The current service's oversized scratch reduces exposure in that caller; it
does not make the public bounded parser safe.

### 2. [P1] Quiesce the RT callback before clearing its transport pointer

`ASFWDriver/Midi/DriverKit/ASFWMidiDevice.cpp:192-197`

`WriteToWire` reads `bound` once and then accesses the ordinary `block` pointer
several times. `UnbindTransport` can clear that pointer after the callback has
already observed `bound == true`. A release store is not a wait for readers:
this is a C++ data race and can become a null dereference. The driver calls
UnbindTransport before superclass Stop, so any quiescence performed by that
superclass happens too late to protect the pointer mutation. Stop/detach the
I/O callbacks and establish completion of in-flight readers before clearing the
pointer, resetting converters or releasing the mapping. Retaining the memory
alone does not protect the mutable pointer or captured ivars.

### 3. [P1] Restore the MIDI RX attachment on ordinary stream restart

`ASFWDriver/Audio/Duplex/IsochDuplexHostTransport.cpp:87-96`

`DetachReceiveConsumers` clears both `midiSink_` and `midiWake_`. It runs from
normal `StopPreparedReceive` and `StopAll`, but the only non-null
`SetMidiReceiveTransport` call is in endpoint discovery/publication in
AudioCoordinator. Starting audio again on the same published endpoint does
not perform that discovery step. `AttachReceiveConsumer` sees an unbound sink
and builds a consumer without MIDI extraction, so MIDI input disappears after
an audio stop/start (and paths through the same teardown during recovery).
Keep the endpoint's retained MIDI attachment separate from per-run consumers,
or explicitly restore it before each receive preparation.

### 4. [P1] Notify after the receive batch is complete, or use a generation handshake

`ASFWDriver/Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.cpp:252-261`

Only the first MIDI-bearing packet in a batch triggers a wake. The MIDI queue
can run immediately, drain those bytes, and finish before the receive queue
writes the remaining packets. Those later writes get no wake. If the batch
contains the last Note Off or SysEx tail and no later MIDI arrives, it remains
queued indefinitely. The drain performs a single snapshot and there is no
pending-generation/recheck protocol. Notify at batch completion, or implement
producer/consumer coalescing that detects writes made during a drain. An early
wake alone cannot cover bytes not yet published.

### 5. [P1] Preserve the unconsumed tail of a MIDIIOBlock callback

`ASFWDriver/Midi/DriverKit/ASFWMidiDevice.cpp:59-67`

The callback calls `Pull` once with 512 bytes of scratch and ignores
`wordsConsumed` and `needsMoreWords`. The converter deliberately stops when
scratch capacity is exhausted, but the callback enqueues the prefix and
returns success for the entire input. In the reproduction, 256 valid Note On
words consume only 169 words and produce 507 bytes, although all 768 output
bytes would fit an empty transport ring. The other 87 messages disappear.
Process all complete packets with a bounded capacity policy or reject before
publishing a prefix; never return success with unhandled input. Also give
truncated multiword input an explicit policy rather than silently accepting it.

### 6. [P1] Commit converter state only when the TX bytes are accepted

`ASFWDriver/Midi/DriverKit/ASFWMidiDevice.cpp:59-65`

`Pull` mutates the per-port SysEx state before `TryWrite` checks ring space.
When a SysEx Start is rejected by a full ring, the converter nevertheless
remembers an open SysEx. A later End then emits payload plus `F7`, without the
`F0` that never reached the wire. Reproduced with a full ring: reject Start
containing `01`, drain the ring, then submit End containing `02`; output is
`02 F7`. Those bytes can be interpreted under an unrelated device running
status. Stage conversion and enqueue as one transaction, or enter an explicit
loss/recovery state on rejection that discards continuations until a new Start.
The inverse failure (a rejected End leaving an unterminated physical SysEx)
also needs a defined recovery policy.

### 7. [P1] Associate RX discontinuities with a byte position

`ASFWDriver/Midi/DriverKit/ASFWMidiDevice.cpp:224-234`

A global discontinuity count tells the consumer that a gap occurred but not
where. Resetting before draining the existing backlog resets on the wrong
side: pre-gap bytes can immediately recreate a partial message, then combine
with later post-gap data without another reset. Reproduced by queuing `90 3C`,
marking a gap, applying the service's reset/drain sequence, then queuing `40`:
the parser emits `0x20903C40`, a fabricated Note On spanning the gap. Publish
ordered gap markers/cursor boundaries, or use a conservative synchronized
backlog discard and parser reset that guarantees no message crosses the gap.
Malformed/lost wire packets should enter this same discontinuity path.

### 8. [P1] Make transport readiness and epoch handoff explicit

`ASFWDriver/Audio/Core/AudioCoordinator.cpp:132-140`

The coordinator tries to arm/get the nub's buffer immediately after EnsureNub.
The buffer is allocated in the nub's Start; `ArmTransport` silently returns if
that has not happened, and there is no readiness callback or retry. Conversely,
the nub publishes with its epoch still zero, and the matching MIDI driver
copies that epoch once into its device. If driver matching wins before the
coordinator arms the real epoch, `Usable(capturedEpoch)` stays false afterward.
Neither interleaving has a recovery route. This finding is a missing-ordering
contract from code inspection, not a measured startup failure. Initialize the
buffer/epoch before publishing, then notify the coordinator with a retained
ready descriptor; bind the MIDI service to that same versioned handoff.

### 9. [P2] Validate AM824 format and packet disposition before extracting MIDI

`ASFWDriver/Audio/Engine/Direct/Rx/RxAudioPacketProcessor.cpp:77-87`

The new extraction checks length divisibility and matching DBS but never checks
CIP FMT/FDF or the expected DATA disposition. CIP decode only accepts the EOH
markers. A packet with another FMT (or a payload-bearing NO-DATA form) and the
same DBS can therefore deliver a coincidental `0x81` label as MIDI. Header-only
NO-DATA is handled, but that does not validate the remaining packet forms.
Validate the supported Saffire AM824 formation before demux, and add tests at
ProcessPacket rather than only against the already-bounded pure demux helper.

## Validation

Built these current targets successfully:

- MidiUmpConversionTests
- MidiEndpointCapabilitiesTests
- MidiTransportBlockTests
- MpxMidiDemuxTests
- MpxMidiTxTests

`ctest --test-dir build/tests_build -R 'Midi|Mpx|Ump' --output-on-failure`
passed all 172 selected tests. This selection includes existing related audio
cases; it is not 172 new integration tests. These passing tests do not exercise
MIDIDriverKit callbacks or their lifecycle and notification interleavings.

`repro.cpp` is an independent temporary-review harness, not registered in the
production test suite. From the repository root:

```sh
clang++ -std=c++23 -fsanitize=address -g -I . \
  documentation/reviews/midi-2026-09-13/repro.cpp \
  ASFWDriver/Midi/Ump/MidiByteStreamToUmp.cpp \
  ASFWDriver/Midi/Ump/UmpToMidiByteStream.cpp \
  -o /tmp/asfw_midi_review_repro
/tmp/asfw_midi_review_repro
/tmp/asfw_midi_review_repro bounds
```

The first run prints the partial conversion, rejected-SysEx state and fabricated
message results. The second intentionally exits with an AddressSanitizer
stack-buffer-overflow report at MidiByteStreamToUmp.cpp:123. These reproduce the
underlying logic and service call sequences; they do not simulate DriverKit.

No Xcode build, hardware streaming, scheduling probe, or hot-unplug test was
performed for this review.

## Remaining scope, not newly introduced bugs

WP-6 shared stream ownership, MIDI-only pumping, bounded fill deadlines and
PCM-unavailable handling are explicitly unfinished in the implementation plan.
They should not be mistaken for delivered functionality merely because RX is
now extracted before the PCM-binding check. The host scheduling/provider gates
and Saffire qualification still require empirical evidence. Fix the memory and
integration defects above before spending a hardware qualification cycle;
then complete the shared session and measure accepted-offer loss and latency.
