# DICE Audio — Field Notes from Real Hardware (Alesis MultiMix FireWire)

Device-validated findings from bringing the **Alesis MultiMix FireWire** (DICE/TCAT) from
clicky/poppy capture to clean multi-hour Logic recording (12-in / 2-out @ 48 kHz).

These are observations about how real DICE hardware behaves, intended to inform the audio
path. The code they originally lived in is superseded by the direct-only audio rewrite; the
**findings** are preserved here because they are the kind of thing that is easy to lose in a
rewrite.

Provenance: distilled from local stability work (branch `alesis-recording-stability-v4`,
2026-04-28 → 04-30), validated against a physical Alesis MultiMix. 2026-06-04.

## Already upstream (context — not new)

The device-identity / stream-shape findings already landed via the Alesis profile (PR #12):
12 active input channels (not 14), device→host = DICE TX on **iso channel 1**, host→device =
**iso channel 0**, and stream-table entries with `iso = -1` are **disabled** (ignore — do not
count as usable audio). Listed here only so the notes below are self-contained.

## DICE probing / register findings

- Resolve everything from the **DICE section-table offsets** — not absolute `GLOBAL_*`
  addresses. Vendors place the global block at different offsets.
- Register areas observed as reliable: section table; global notification / status /
  extStatus / **sampleRate**; the **active** TX/RX stream tables; RX `seqStart`; AVS
  **DBS / system-mode** registers.
- Treat the **active** TX/RX stream-table entries as the source of truth for channel shape.
  Do not infer geometry from model id or guesswork.
- Cross-checked against the historical `dice files` reference: there is **no hidden
  queue/FIFO register** that fixes stream timing — timing must be handled host-side.

## RX timing & stability techniques (most likely to be lost in a rewrite)

These are the changes that actually removed the clicks/pops and made multi-hour capture
stable:

1. **Startup zero-fill is expected, not corruption.** Account for zero-filled frames with an
   explicit *startup hold*; do not treat startup zeros as mid-recording dropouts.
2. **Rebase timing twice:** once at startup, and again when transport **lock** is acquired
   (startup rebase + transport-lock rebase).
3. **Slew on RX backlog before overflow.** Apply high-water backlog slewing as the queue
   fills, rather than hard-dropping at overflow.
4. **`SYT == 0xffff` means "no timestamp info", NOT clock loss.** Reacting to it as a clock
   dropout was a source of false recovery churn.
5. **AM824 data-block-count continuity is authoritative** for detecting real stream
   discontinuities (more reliable than timestamp heuristics alone).

## Diagnostics worth keeping (driver-side)

Periodic `IO-RX` / `IR RX HEALTH` logging — callback counts, requested/sent frames, queue
fill vs capacity, producer drops, consumer underreads, CIP fields, q8/rate, alignment,
SYT-loss — was what made *startup alignment* separable from *ongoing capture stability*
while debugging. Worth keeping equivalent counters in the rewritten RX path. (The macOS
app-side plumbing for these is Chris-local and not proposed for upstream.)

## Midas Venice (next, separate)

Standard DICE caps return empty (`runtime_caps_not_ready`) on Midas Venice until the **EAP /
current-config** path and clock setup exist. Detailed EAP probing notes are held separately
and will accompany the dedicated EAP slice; see also the `feat/midas-venice-recognition`
branch (identity-only, fail-closed recognition).
