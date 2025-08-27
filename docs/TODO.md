codex
Here’s a pragmatic way to start that delivers value fast, keeps risk low, and fits both PLAN.md and the refactor docs.

Where to start

- Goal: one small, end-to-end “vertical slice” that proves the refactor is useful without boiling the ocean.
- Pick: Config ROM + bus/link sequencing + Self‑ID robustness. It’s self‑contained, visible in logs, and unblocks later async/
isoch work.

First vertical slice (1–2 sessions)

- BusManager: handle BusReset/SelfIDComplete
    - Coalesce resets; re‑arm Self‑ID; gate CycleTimer after first stable Self‑ID.
    - Success: stable logs for BusReset → Self‑ID → CycleTimerEnable, no repeated “cycleInconsistent”.
- Config ROM (minimal but valid)
    - Build tiny ROM (Bus_Info_Block + Root_Dir).
    - Map ROM (32‑bit IOVA), program ConfigROMmap, set BIBimageValid in the link sequence.
    - Success: bring‑up logs show BIBimageValid set; BusOptions optionally loaded from ROM[2].
- Logging hygiene
    - Keep current detailed interrupt logs; add a few targeted lines: “ConfigROM mapped @ 0x…” and “BIBimageValid set”.

Why this first

- High signal: easy to verify entirely via logs with or without devices plugged in.
- Low risk: isolated, won’t destabilize DMA paths.
- Unblocks: later async/isoch (CSR space needs ROM defined).

Next two milestones

- Async RX “recycle + observe” (no payload parsing)
    - Minimal AR recycle loop: retire/requeue descriptors and log packet headers.
    - Success: with any bus chatter, see ARRQ/ARRS/RqPkt/RsPkt + context logs without stalls.
- AT “header‑only” send path
    - Minimal OUTPUT_LAST_Immediate builder for header‑only frames; plumb BAR index use in AT context; IODMACommand for any
pools.
    - Success: AT context wakes/runs; Tx‑complete interrupts arrive; no dead contexts.

Then expand horizontally

- 1394a enhancements
    - Detect via PHY reg 2; program reg 5; add paged PHY read helper. Clear programPhyEnable afterward.
    - Success: logs show “1394a configured” and stable timing.
- Error handling
    - Recovery hooks for regAccessFail, unrecoverableError, cycleInconsistent (mask → quiesce → restart contexts; optional soft
reset).
    - Success: graceful recovery logs, no driver spin.

Suggested order of work (small, verifiable steps)

- Step 1: Config ROM minimal + BIBimageValid in bring‑up (BusManager + ROM builder)
- Step 2: Self‑ID robustness (coalesce resets, re‑arm, cycle timer gating) and assert LC state in logs
- Step 3: AR recycle + header logging (no deep parse)
- Step 4: AT header‑only OUTPUT_LAST_Immediate + DMA IOVA correctness
- Step 5: 1394a PHY programming (reg 2/5 + paged access)
- Step 6: Error recovery hooks (regAccessFail/unrecoverable/cycleInconsistent)

Validation loop
- Use ./log.sh to capture 60–120s around bring‑up and one bus reset.
- Expected patterns after Step 1–2:
    - “ConfigROM mapped @ …”, “BIBimageValid set”
    - “=== CONFIG ROM DUMP HEX (BIG-ENDIAN) === size=… dump=…” (first 32 bytes match expected)
    - “Bus Reset (bit 17)” → “Self‑ID Complete (bit 16)” → “CycleTimerEnable now set”
    - No recurring “Cycle Inconsistent”

Status Checklist (Aug 27, 2025)
- [x] ROM image written big-endian into buffer (endianness fix).
- [x] ConfigROM hex dump added with trimming and ASOHCI prefix.
- [x] `ConfigROMmap` programmed before LinkEnable; `BIBimageValid` set.
- [x] BusReset commit path writes BusOptions then ConfigROMhdr.
- [ ] Add Self-ID generation consistency check before parse.
- [ ] Stop/flush AT contexts during bus reset; restart after.
- [ ] Optional: schedule bus reset after bring-up to finalize config.
- [ ] Implement live "next ROM" update path (shadow map + reset).
- After Step 3–4:
    - ARRQ/ARRS/RqPkt/RsPkt bits with corresponding context logs
    - AT ReqTxComplete/RespTxComplete seen on header‑only packets

De‑risking tips

- Keep all new pointers as ivars (in .iig) and initialize/free only in Start/Stop.
- Always use IODMACommand with maxAddressBits=32 for descriptor/buffer pools; log IOVA.
- When uncertain about device traffic, prioritize bring‑up and ROM—those validate without external nodes.
- Don’t refactor large files while adding features; land the slice first, then modularize.
