# Reported Release run: Instruments Record triggers exhaustion

September 7, 2026. The user reported building/installing Release and loss of
playback when clicking Record in Instruments. Build configuration was not
independently fingerprinted from the installed binary. Capture was read-only;
no reset, installation or RTL test was performed.

Sequence 176001: mapped-region-exhausted; 176002: TX fatal stop.
The final flight record (epoch 1, index 63) has completion=4111523,
mapped=4111571, committed=4111643. Thus 48 mapped/unretired packets and another
72 committed packets beyond the mapped frontier. CommandPtr=0x802408b0 names
OUTPUT_LAST in physical slot 34, exactly (mappedEnd-1) modulo 48. Control
0x00008011 has RUN set, ACTIVE/DEAD clear. The finite queue stopped at its tail;
no seal mismatch is reported.

Callback entry ticks 17933407171715 -> 17933407328028 span 6.513042 ms using the
locally verified 125/3 ns Mach timebase. The final cycle-read bracket spans
3.333 us. Cycle fields differ by 52 cycles. These observations still cannot
separate a slow/preempted preceding refill from delayed callback delivery.

Recovery restarts TX, then faults at packet 168 (sequence 176131), with
committedEnd=168, prepReq=1 and prepHandled=0. The second fatal stop is sequence
176134, approximately 122.71 ms after the first. Audio fault records confirm
txWasActive=1 on the first recovery and txWasActive=0 on the second. This repeats
the known disabled-producer recovery failure.

The reported Release configuration did not eliminate the Record-associated
failure. This does not establish an OS scheduler regression. Next evidence
needed: refill/interrupt/watchdog entry-exit and gate boundaries correlated with
System Trace. Coordinated recovery remains the functional repair.

Initial ring stats: capacity 39718, oldestSequence=136633,
latestSequence=176350, droppedRecords=0. Earlier history was evicted despite
zero droppedRecords; the current failure and all 64 frozen records were retained.
DirectAudio accounts for 173868 emitted records. The captured pre-fault sample
is repeated TxAlign delta 0/-1, step +/-1 output. Reduce this routine jitter
logging before further instrumented runs; its causal contribution is unproven.

Raw responses and merged trace are adjacent. User's click timing is reported
context, not a independently recorded Instruments event timestamp.
