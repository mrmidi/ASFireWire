# Transaction Label (tLabel) Handling Notes

This project got stuck reusing the same tLabel (e.g., 2/3) after bus reset. This note documents the findings and the reference implementations from Linux and Apple.

## Problem

- After bus reset, the label allocator bitmap still had bits set (e.g., 0x4103), so the next allocation always returned the first free bit (tLabel=2), causing all reads to reuse the same label.
- PH Y packets and retries can leave labels set if not freed. Stale labels across resets pin the allocator.

## Linux Approach (reference: `drivers/firewire/core-transaction.c`)

- `allocate_tlabel(card)`:
  - Maintains `current_tlabel` and `tlabel_mask`.
  - Scans from `current_tlabel` until a free bit is found; wraps at 64; fails if all set.
  - Sets the bit in `tlabel_mask`.
  - Advances `current_tlabel` to the next value after the one returned.
- Free clears the bit in `tlabel_mask`.
- On bus reset: `tlabel_mask = 0; current_tlabel = 0;` (fresh slate).
- Driver never embeds label allocation inside hardware paths; it is purely a software bitmap with round-robin.

## Apple Approach (IOFireWireFamily + AppleFWOHCI)

- AppleFWOHCI does **not** allocate tLabels. The async transmit path consumes a tLabel provided by the upper IOFW stack.
  - In `AppleFWOHCI_AsyncTransmitRequest::asyncRead`, the tLabel parameter is written directly into the header (`quadlet5 = tLabel | nodeID<<16`).
  - No allocator exists in the lower driver; the IOFireWireController tracks pending transactions by tLabel and frees on completion.
- IOFireWireController warns when out of tLabels but otherwise rotates through its `fTrans[]` array (size 64), marking entries in use and freeing on completion. No bitmap persistence across resets.

## Takeaways / Guidance

- Keep the allocator purely software and reset it on bus reset.
- Use a round-robin cursor + bitmap (Linux pattern): return first free bit, advance cursor, clear bit on completion.
- Ensure all completion paths free the label (AT-only PHY, timeout, errors).
- On bus reset, zero the bitmap and cursor; do not rely on descriptor parsing to infer label ownership.

