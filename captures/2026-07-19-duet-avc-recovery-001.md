# Live reproduction: AVC-RECOVERY-001 (2026-07-19 18:55, Duet)

**Trigger (self-inflicted):** an `asfw://telemetry/snapshot` MCP read during an
active audio run. It is not a passive resource — it runs `GetAVCUnits` and a full
PHY register dump. `PHY Read reg 0` then **timed out for 115 ms**.

That 115 ms exceeds the 78 ms producer-stall cliff measured in `tools/asfw_sim`
(FINDINGS F2), so it is a textbook instance of the modelled stall.

## Chain, verbatim from the unified log

```
18:55:33.892085  [UserClient] GetAVCUnits: returning 1 units in 32 bytes
18:55:33.894687  [PHY] Read reg 0: wrote PhyControl=0x00008000
18:55:34.009255  [PHY] Read reg 0 TIMEOUT                       <-- 115 ms blocked
18:55:34.107533  [TxPrep] margin=672 min=138 lead=678 wakes=49152
18:55:34.147150  AVCAudioBackend: RX replay stalled past settle;
                 restarting duplex attempt=1 GUID=0x0003db0a0000d112
   ... CMP DisconnectIPCR/OPCR, IT stopped (305952 pkts IRQs=49209),
       IRM releases both channels + 2x596 units, FSM terminal Idle restartId=1,
       then full re-acquire: channels 0/1, bandwidth, AUDIO DUPLEX START ...
18:55:34.231322  IT: Prime failed - committed prefill=306582 must cover
                 48 descriptors within 912 slots                <-- AVC-RECOVERY-001
18:55:34.231331  IT: Failed to prime descriptor ring against shared payload slab
18:55:34.248574  [FSM] terminal state=Failed class=StageFailure cause=StartTransmit
                 status=0xe00002c9 restartId=2
18:55:34.248589  AVCAudioBackend: timing-loss recovery failed kr=0xe00002c9
```

## What this confirms

1. **`AVC-RECOVERY-001` is real and fires exactly as described.** Recovery reached
   `IsochTransmitContext::Start()`, which read the live `committedEnd = 306,582`
   and passed it to `Prime`, which accepts only `[48, 912]`. `0xe00002c9`.
   The triage documents predicted this value shape precisely; here it is on
   hardware. Note 912, not the 408 in the older markdown.
2. **`AVC-RECOVERY-002` is real: the escalation is destructive.** A 115 ms
   transient — not a device outage, not a bus reset (`busResetCount=0`) — was
   escalated into a full CMP teardown, IRM release, and a terminal `Failed`
   state. The device was healthy the entire time.
3. **The recovery leaves nothing running.** CMP disconnected, both isoch channels
   and 2x596 bandwidth units released, engine `Failed`. CoreAudio was never told.

## What it does NOT show

The audio was **already 100% silent before the trigger** (`w=0`, deficit 108,292
frames, ramping at 22.5 frames/s — see F6). The snapshot read did not kill the
audio; it killed the *session*, and in doing so reproduced the recovery defect.

## Operational rule

During an active audio run use **only** `asfw_log_query`. Never
`read asfw://telemetry/snapshot`, `health`, `summary`, or discovery: they issue
MMIO on the driver's queue and a PHY timeout alone exceeds the stall cliff.
