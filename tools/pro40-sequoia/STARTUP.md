# Initial data payload quarantine

An intermittent restart failure rejected the first received data packet before
CoreAudio audio admission. Native status, length and CIP headers were valid,
but the payload had invalid analog/MIDI labels. Two recent failures each reported
eight bad analog labels and seven nonzero invalid analog words.

A retained older raw capture shows that signature directly: the first data
payload has a leading zero quadlet and subsequent labels displaced by one word.
The other packets in the first 64-packet receive group have valid labels and
DBC continuity. The capture establishes the transient; it does not prove whether
the physical cause is in the interface or the native host stack. The actual
microphone capture is private. `StartupPrerollChecks.cpp` reproduces the shape
with generated samples, and can optionally replay the retained local capture.

`CaptureStartup` now separates the initial payload from normal sample decoding:

1. Validate the packet envelope, sender, channel layout, rate and timestamp.
2. Mark exactly the first data payload as pre-roll, regardless of its labels.
3. Retain its frame/packet identity and checked timing for clock fitting and
   output sequence replay; never decode its samples as audio.
4. Use the existing receive-content validation for every subsequent packet.

Native transport status, arrival continuity, DBC and SYT remain mandatory.
Malformed headers, bad later audio/MIDI labels, broken sequence and queue loss
remain terminal failures. There is no retry, byte-shifting repair, or permission
to turn later malformed packets into silence.

The sustained input producer emits a distinct timing-only `preroll` event. The
receiver's readable window starts at frame 8; frames 0–7 are absent. The first
batch is [8,512), then the usual 512-frame boundaries resume. Keeping those
boundaries is necessary for clock-prefix publication/admission. Native timeline
coordinates, playback buffering and presentation offsets do not change.

At 48 kHz the excluded eight frames span 0.1667 ms. This is once per native
stream start, before recording apps are admitted, not each time Record is
pressed. The application cannot read that discarded range as microphone PCM.

Linux's `amdtp-stream.c`, `drop_tx_packets_initially`, discards initial receive
batches before normal processing; DICE starts that domain with zero additional
skip cycles. The local reference was cross-checked at lines 1442–1535 of
`amdtp-stream.c` and 446–454 of `dice-stream.c`. This implementation's narrower
one-payload policy comes from the observed Pro40 data, not a claim that its
scheduling is identical to Linux's. No reference implementation was copied.

Offline checks cover the displaced capture, both layouts, 0–63 initial header
packets, inaccessible discarded frames, exact later PCM, unchanged replay,
strict later/header failures, continuity, queue pressure and clock publication.
Twelve subsequent live native restarts passed, but their first payloads all had
valid labels. The malformed case was reproduced offline, not newly observed in
that live run. This is a bounded fix for the known signature, not proof against
every possible startup fault.
