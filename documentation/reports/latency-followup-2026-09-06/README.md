# Duet RTL follow-up — 2026-09-06

Retained outputs from the earlier session in this task; no new hardware run
was made while updating the plan. Parent: [latency plan](../../AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md).
Duet, 48 kHz, output/input channel 0, amplitude 0.10, 20 trials per run.

| Callback frames / search window | Accepted | SNR | RTL raw | RTL ts | Residual |
|---|---:|---:|---:|---:|---:|
| 64 / 4096 | 10/20 | 14.6 dB | 4038.97 fr / 84.145 ms | 3810.97 fr | +3703.97 fr |
| 64 / 16384 | 6/20 | 18.3 dB | 4038.92 fr / 84.144 ms | 3810.92 fr | +3703.92 fr |
| 128 / 16384 | 11/20 | 17.6 dB | 422.92 fr / 8.811 ms | 66.92 fr | −40.08 fr |
| 128 / 16384, repeat | 9/20 | 16.5 dB | 422.91 fr / 8.811 ms | 66.91 fr | −40.09 fr |

Each log reports the requested callback span and zero overloads, frame gaps,
re-anchors, clock disagreement or missing timestamps. Rejections are no-signal.
All returns are weak. The installed driver build and continuous epoch identity
between the 64- and 128-frame sessions were not established; these are not a
controlled comparison showing an effect of changing only client buffer size.

Measured scheduling is 228 frames at 64 and 356 at 128, matching two client
buffers plus the two 50-frame safety offsets. Declared device/stream latency
totals 107 frames. At 128, their sum is 463 frames / 9.646 ms round trip and the
output declaration is 245 frames / 5.104 ms, matching Logic's rounded display.

The scheduling-normalized results differ by approximately 3744 frames, or
13 × 288. That spacing does not establish the mechanism or distinguish delayed
content from a repeated marker. Neither this residual nor the tool's
`hardware latency` label isolates a device-only interval. The reporting-only
self-check and stronger repeat-start baseline remain open.

The later alignment/24.145 ms run reported in `3a133a09` is a separate session;
these four files do not validate its epoch, settings or trace completeness.

Raw outputs:

- [64 frames](evidence/asfw-duet-rtl-64-2026-09-06.txt)
- [64 frames, larger window](evidence/asfw-duet-rtl-64-window16384-2026-09-06.txt)
- [128 frames](evidence/asfw-duet-rtl-logic-128-2026-09-06.txt)
- [128 frames, repeat](evidence/asfw-duet-rtl-logic-128-repeat-2026-09-06.txt)
