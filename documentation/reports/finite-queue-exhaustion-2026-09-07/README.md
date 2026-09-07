# Finite TX queue exhaustion — September 7, 2026

Read-only capture after the user reported lost playback. No reset, installation,
control write or RTL measurement performed. The exact installed source revision
was not independently identified. The observed stop is consistent with the
finite-queue implementation and its retained completion anchor.

## First failure

Sequence 1025 reports `reason=mapped-region-exhausted`; 1026 clears RUN and
masks TX interrupts. Frozen recorder epoch 1 includes all 64 records.

The final two callback entry ticks are 17907066750959 and 17907066910235.
The local Mach timebase was read as 125/3 ns per tick: **6.6365 ms between
refill entries**. The final cycle-read bracket spans only **3.542 us**.
The corresponding cycle fields differ by 53 cycles. These bounds do not
separate a slow preceding refill from scheduling or interrupt-delivery delay.

At failure:

- completion = 268247; mappedEnd = 268295: 48 mapped, unretired packets.
- committedEnd = 268367: the producer had 72 packets committed beyond mappedEnd.
- CommandPtr = 0x802885b0: OUTPUT_LAST of physical slot 22.
- Last mapped packet is 268294, also physical slot 22.
- Context control = 0x00008011: RUN set, ACTIVE clear, DEAD clear.
- No payload-seal failure is reported.

Hardware was inactive at the published terminal descriptor. This supports the
intended finite-tail stop rather than stale-payload replay. The first failure
is failure to extend the DMA queue in time, despite available committed work;
it is not evidence that the producer ran out of prepared packets. The driver
then deliberately takes its existing exhausted-region fault path.

## Recovery failure

Sequence 1028 clears the active-producer state and enters recovery. CMP
transactions complete successfully; duplex restarts. Sequence 1352 then faults
at packet 168 with committedEnd=168, prepReq=1 and prepHandled=0. The restarted
TX drains its prefill without continuing production. Sequence 1357 reports
`txWasActive=0`; the inactive guard prevents another restart. First and second
fatal-stop records are approximately 115.85 ms apart.

RX continues progressing afterward. The health endpoint nevertheless reports
TX `running`, `faulted=false`, and streaming=true. That projection contradicts
the retained fatal-stop chronology and must not be used as evidence of working
playback.

## Next work

The ownership fix did not promise continuity through queue exhaustion. The next
functional repair is coordinated recovery: quiesce the old epoch, reset and
prefill coherent producer/transport state, and resume production with TX. Then
investigate why a 6.64 ms service interval occurred and measure sustainable
queue headroom. This capture does not establish a macOS 27 regression.

Evidence is in the adjacent raw MCP responses and `trace.txt`. Initial stats:
oldestSequence=1, latestSequence=1391, droppedRecords=0. Final stats are saved
in `stats.json`; no retention gap covered the captured failure.
