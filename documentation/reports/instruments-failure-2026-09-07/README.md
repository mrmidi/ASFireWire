# Instruments-start playback failure — September 7

User reported audio stopping when clicking Record in Instruments, then later
resuming. This capture was read passively after MCP was enabled. No test signal,
restart, hardware transaction, or settings change was issued by the investigator.

The complete frozen flight history survived: 64 records, epoch 1, all source 1
(interrupt callback). Final two entry timestamps were 17563363886018 and
17563364074778. Captured host timebase 125/3 ns gives **7.865 ms** between
refill entries. The final cycle-read bracket is only **3.417 microseconds**;
this is a gap between observations, not a long final cycle-timer register read.
Physical IRQ arrival and preceding refill completion times were not captured,
so IRQ delivery delay, dispatch delay, or a slow preceding batch remain distinct
possibilities. The Instruments-click association comes from the user, not an
independent timestamp in this driver trace.

Final record:

```
cycle=24a554ce previous=24a165aa slots=12/27 inferredDelta=63
completionBefore=652236 mappedBefore=652284 committedBefore=652356
failedPacket=652284 fill=0 flags=7 failure=8
```

At sequence 1477 the estimator adds a lap (15 modulo packets -> 63 inferred).
At 1480 the seal check fails **exactly at the exclusive mapped frontier**, packet
652284, with expected seal 1e67d0bff0196f84 and observed 403bdd0cb8820dd4.
That packet was prepared but beyond the pre-batch descriptor mapping. This is
stronger evidence for incorrect retirement identity than the previous trace,
which supplied completion but not the mapped frontier. It does not establish
that all 63 inferred executions happened. The driver clears RUN at 1482.

Recovery disables the producer and prefills 168 packets, then fails at packet
168 (1612–1617). Thus the built-in fault recovery does not restore this playback.
Later, a normal StopIO occurs at 1891 and a new StartIO at 1929, about **205.65 s**
after the original fatal stop. This is the observed route back to playback;
the trace does not identify which application action requested that restart.

Current snapshots show advancing TX (426588 -> 434976) and RX, streaming true,
no new content fault counter increments. Ten expired-copy attempts are present
and unchanged between snapshots. No conclusion about physical latency follows.

Retention: oldest sequence 1, zero drops, generation 3. All 192 TxFlight lines
are present. Frozen history remains epoch 1 despite the later playback epoch,
confirming that restart did not overwrite the first fault.

Evidence: [original MCP capture](evidence/capture.json),
[readable chronology](evidence/trace.txt), [host timebase](evidence/timebase.json).

## Subsequent recurrence: interrupt silence and failed restart

Captured after the user's next "dead again" report. Generation remains 3,
oldest retained sequence 1, zero drops. See [recurrence](evidence/recurrence.json),
[readable chronology](evidence/recurrence-trace.txt), and
[restart transaction records](evidence/restart-all-categories.json).

This is not another observed in-run seal failure:

- 3824/4142: TX interrupt watchdog kicks; 4143/4144 attempt interrupt re-arm.
  4145 reports 16 consecutive silent kicks. Context RUN/ACTIVE remain set
  (`ctrl=0x8411`); event register shows `0x003000c0`.
- At 4283 and 4286, IRQ count is unchanged at **181172**, while completed TX
  packets advance **1418894 -> 1430129**, all carried by watchdog polling
  (`carriedPerMille=1000`). No recovered laps; maxDelta 42. This establishes
  loss of observed interrupt servicing, not loss of DMA progress.
- 4289: normal StopIO begins. 4293: uncommitted-slot fatal at packet 1432582,
  equal to committedEnd, while disconnect is pending. This is the previously
  observed producer-off/transport-still-refilling shutdown failure.
- 4309/4323: iPCR and oPCR reads time out. The idle-fault guard avoids re-arm.
- 4423/4482: two new StartIO attempts. Their PrepareDuplex async FCP writes
  remain ATPosted, receive no observed ACK/completion, exhaust both deadline
  extensions and fail. Prepare/StartIO return **0xe00002bc**.
- Current snapshots are idle, streaming false, TX stopped, counters reset.

The immediate restart blocker is the missing async transmit completions.
Concurrent loss of observed IT interrupt servicing and AT completion delivery
points toward the controller/interrupt/async service path and warrants tracing
that shared mechanism. It does not prove device non-response: an ATPosted
transaction without a handled completion is not evidence an AV/C command reached
the device and was ignored. The trace also does not timestamp the user's first
audible loss, so watchdog-serviced progress is not proof that sound remained
correct until StopIO. No reset/restart/probe transaction was issued by this capture.

### macOS 27 beta hypothesis

The user suspects macOS 27 beta scheduler changes or another kernel-level
interrupt/DriverKit delivery issue contributed to the last recurrence. Loss of
observed IT interrupt servicing alongside missing AT completions makes an
OS-level delivery or scheduling problem worth investigating, but the current
trace does not distinguish it from a driver/controller fault. No macOS-version
regression has been established. Correlate a system scheduling/interrupt trace
with the driver timestamps; a matched comparison with another OS build would
help test the beta-specific hypothesis.
