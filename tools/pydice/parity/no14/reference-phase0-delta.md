# no14 vs Reference Phase 0 Delta

- Reference timeline: `tools/pydice/parity/reference-phase0-timeline.md`
- no14 timeline: `tools/pydice/parity/no14/reference-phase0-timeline.md`
- Compare summary: reference `51` merged transactions / `17` phases vs no14 `94` merged transactions / `20` phases

## Meaningful Differences

1. `no14` does more early introspection before the actual start sequence.
   It adds extra layout reads, an early `104B` global read, `512B` block reads of the TX/RX stream sections, and a TCAT extension-header read before the owner/clock/start path.

2. The original reference trace is leaner in phase 0.
   It goes to `GLOBAL_STATUS`, layout, owner claim, clock select, stream inspection, IRM, then stream programming with fewer preparatory reads.

3. The important startup tail is now effectively in parity.
   Both traces end with the same functional sequence:
   `RX_SIZE -> RX_ISO -> RX_SEQ_START -> TX_SIZE -> TX_ISO -> TX_SPEED -> GLOBAL_ENABLE`.

4. Neither trace shows raw router-matrix programming in phase 0.
   Both look like stream-bringup/control traffic, not explicit headphone/router patching.

5. The practical conclusion is good news.
   `no14` is no longer missing the old async/bringup step; the remaining gap is above basic DICE stream start.

## Takeaway

`no14` differs mainly by doing extra reads and TCAT inspection before startup. The decisive stream-enable tail now matches the reference capture closely enough that any remaining issue is unlikely to be the old AR/DICE transport blocker.
