# RME Fireface 400/800 support plan

**Status:** staged implementation plan, updated 2026-09-27. Current work is
Stage 1 only. The approved target is duplex audio on Fireface 400 and 800 at
48 kHz only, with 18 and 28 channels per direction respectively. MIDI,
Fireface UFX/UCX/802, flash updates, and mixer writes are outside scope.

Neither model has been tested on hardware; the maintainer has no unit. Both
models will be enabled by default after the staged implementation and review,
while remaining clearly hardware-unverified. Until then, both models remain
recognized but unsupported. AV/C/FCP remains blocked for these RME personas.

## User and device constraints

- Preserve the device's saved clock source and mixer state. Do not send mixer
  writes or initialize/reset the mixer.
- Internal clock operation is fixed at 48 kHz. An external source is accepted
  only when the device reports it locked at 48 kHz; do not change the user's
  source selection.
- The FF800 has 28 capture and 28 playback channels at 48 kHz. The FF400 has
  18 capture and 18 playback channels at 48 kHz.
- Device stream register values are little-endian. Reject failed, zero, or
  below-minimum firmware revision reads. Minimums are FF800 2.77 and FF400
  1.70, based on the RME 3.41 package/settings evidence described below.

## Identity and evidence

Linux's Fireface table matches root vendor `0x000a35`, root model `0x101800`,
unit specifier `0x000a35`, and unit version 1 (FF800) or 2 (FF400):
`references/linux-sound-firewire-stack/firewire/fireface/ff.c:181-200` and
`ff.h:37-39`. This exact identity is used by Stage 1. The RME kext classifies
FF800 versus FF400 from GUID bits (`docs/RME_VENDOR_ANALYSIS.md`), which is
corroboration only and is not the catalog match rule.

Protocol behavior comes from independent evidence sources, each used only as
behavioral evidence and never copied as code:

- Linux `references/linux-sound-firewire-stack/firewire/fireface/` is the
  source for headerless PCM framing, clock policy, and RX sequence replay. Its
  device-control sequence is an independent comparison, not the chosen RME
  3.41 control sequence.
- FFADO `references/libffado-2.5.0/src/rme/` and RME FireWire driver 3.41
  corroborate the FF800 high-address-2 register sequence. Static vendor
  analysis is recorded in `docs/RME_VENDOR_341_ANALYSIS.md`; the 2.75 analysis
  is in `docs/RME_VENDOR_ANALYSIS.md`.
- These analyses contain no live ASFW hardware trace. Packet tags and the
  precise hardware timing contract remain unverified.

The supplied RME 3.41 executable analyzed as `FFAD-431` has SHA-256
`a04ce313a4e27b4476213e84373e19b0c41a2523339ca6faab571d6165087074`.
Static x86_64 analysis identifies `hwInitDevice` at `0x7850`,
`hwStartDevice` at `0x793c`, `hwStopDevice` at `0x79d0`, and `hwStart` at
`0x3d40`. The bundled Settings app's `initDialog` at `0x1000029c2` warns below
revision `0x24d` (FF800 2.77) and `0x146` (FF400 1.70); zero bypasses the
warning. This is a Settings UI warning, not a kext stream-start gate. ASFW's
policy to refuse failed, zero, or below-minimum revision reads is an
intentional stricter admission rule. These binary-analysis addresses and hash
make the vendor evidence reviewable without relying on ignored report files;
they do not prove bus behavior on the maintainer's hardware.

## Required protocol behavior for later stages

### Content framing and transport seam

The stream has no CIP header or SYT. Packets use tag 0 and sync 0, and each
48 kHz data packet carries eight audio frames. PCM is the upper signed 24 bits
of a little-endian 32-bit word; there are no MIDI slots in the audio packet.
Transmit cycle omissions must be represented as an explicit skipped cycle,
distinct from an empty audio packet. Implement these through the existing
shared packetizer, payload writer, and transport interfaces. Keep content
framing in `Audio/Wire/`; transport remains payload-opaque.

### Clock startup and receive replay

Transmit starts with a nominal 48 kHz cadence before receive packets arrive.
Once receive begins, replay the device's sequence of data-block counts while
keeping one continuous frame cursor across the transition. Reconstruct one
skipped receive cycle, and anchor timing to the existing hardware receive
timeline. Do not create a per-device TX buffer or separate frame cursor.

### Device registers and resource ownership

Read device clock/revision state first. Preserve saved clock selection and
mixer state. Internal clock is configured for 48 kHz; an external source is
accepted only if it is locked at 48 kHz. Before initialization, write the
zero-filled fetch mask at `0x801c0000` (28 words for FF800, 18 for FF400).

For FF800, use the vendor-confirmed three-word init at `0x00020000001c`:
`[48000, (28 << 11) + playbackChannel, 28 | S800]`; poll the device-selected
capture channel from `0x801c0008` for up to 500 ms without reserving it at the
IRM. Start with `0x80000000 | 28 | S800` at `0x000200000028`; stop with three
zero words at `0x000200000034`. `S800` is `0x800` when the bus link reports
S800 speed.

For FF400, initialize three words at `0x80100500`:
`[48000, (18 << 11) + playbackChannel, 18]`. Start with
`0x80000000 | 18 | (captureChannel << 5)` at `0x8010050c`; stop with
`[0, 0, 0, 1]` at `0x80100504`.

The isoch resource assignment is per direction and model. FF800 reserves the
host playback channel; capture binds the device-selected channel without a
host reservation. FF400 uses channels 0 through 7 for both directions. The
family's `AssignChannels` operation must return the final channel values
before DMA preparation begins. Any partial failure rolls back acquired
resources and resets device stream state. After enable, wait 5 ms, then start
host receive before host transmit. On stop, quiesce contexts, stop the device,
then release host resources.

The RME 3.41 kext is the chosen control reference for both models. FF800
init/start/stop are at `0x00020000001c`, `0x000200000028`, and
`0x000200000034`; its start word includes `0x80000000`. The FF400 sequence is
at `0x80100500`, `0x8010050c`, and `0x80100504`, as detailed above. Vendor
analysis found a 500 ms status poll and IR-before-IT host order. Linux remains
an independent behavioral reference for content format and FF400 behavior;
its FF400 control sequence is not the selected one. Static analysis does not
establish behavior on the user's hardware. The vendor DCL builder varies
tag/sync inputs with its group index while Linux uses tag 0, so emitted vendor
packet tags remain unresolved and must not be inferred from API calls.

All asynchronous protocol boundaries must carry the current device route and
bus generation and honor cancellation. A reset requires channel rediscovery.
Replay recovery must detect persistent sequence loss and larger RX
discontinuities in addition to reconstructing one skipped cycle. Switch from
bootstrap cadence to RX replay at a future writable packet index: already
committed ring packets remain immutable, and the shared frame cursor continues
across that transition.

## Five reviewed stages

1. **Evidence and catalog (current):** record exact identities, mark both
   models `RecognizedUnsupported`, block AV/C/FCP traffic, and add
   reference-derived identity fixtures. These fixtures describe the Linux
   reference match table; they are not claimed as captures from ASFW hardware.
2. **Headerless wire path:** add little-endian PCM framing and a neutral TX
   skip-cycle representation, using shared packetizer/writer/transport code.
   Keep transport free of audio/CIP knowledge.
3. **Clock and receive timing:** implement nominal TX bootstrap, then RX
   sequence replay with a continuous frame cursor, one skipped-cycle
   reconstruction, and the existing hardware timeline.
4. **RME protocol and resources:** implement register sequencing and
   per-direction ownership. `AssignChannels` supplies final channels before
   DMA preparation; all failures roll back resources and reset device state.
5. **Profiles and publication:** integrate the FF400/FF800 profiles, backend,
   and device publication. Enable both by default after stages 1–4 and review;
   document their lack of hardware validation.

Each stage should be reviewed before proceeding to the next. Do not make a
commit that combines stages. Preserve existing DICE, AV/C, M-Audio, and MOTU behavior, TX ownership, and
ZTS invariants unless a reviewed, explicitly justified shared-layer change
requires an update. Regression coverage should include those families, session
choreography, transmit ownership, and ZTS.

## Stage 1 implementation record

The catalog recognizes only the exact root vendor/model and unit
specifier/version pairs above. Both entries have no family backend, profile,
or protocol implementation and use `NoAutomaticTraffic`; the resolved command
filter blocks all AV/C commands. Added tests should assert exact matching,
non-matching nearby identities, unsupported routing, and the block-all filter.

## Evidence limits and bring-up gate

The Linux and FFADO trees are local behavioral references; source code must
not be copied. Vendor 3.41 findings come from static binary analysis, not a
bus trace. The approved scope has no hardware owner, so all wire behavior
remains unverified on ASFW. Test fixtures derived from the references must be
named and described as such. Do not claim validated support or make firmware,
mixer, clock-source, or flash changes in software.
