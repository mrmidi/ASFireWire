# Safety offsets: Apple archive findings, 2026-09-09

Sources extracted read-only from /Users/mrmidi/DEV/wayback-machine-downloader/coreaudio_archive.db. Database IDs are local identifiers, not Message-ID headers. Text exports preserve source bodies, including quoted replies; distinguish the author’s own statements from quoted material.

## Findings

- M14602, Jeff Moore, 2004/Oct/msg00166: safety offset is not compensation for HAL I/O-thread scheduling latency. It specifies access distance from hardware now, primarily physical transfer restrictions and secondarily timestamp inaccuracy that can steer the HAL clock incorrectly.
- M14623, William Stewart, 2004/Oct/msg00187: distinguishes DMA Transfer Limit (samples, reasonable worst-case estimate of DMA mechanics) from Time Stamp Resolution (accuracy of the sample-number/CPU-clock relationship, in samples). Proposed separate properties were a historical proposal, not evidence of a current API.
- M34926, Stewart, 2004/Oct/msg00267: explicitly gives input timestamp jitter times two and output times three, in addition to physical transfer restrictions. No derivation of those multipliers or statistical estimator is specified.
- M16906 and M34926 validate the minimum through-time equation for the described duplex IOProc arrangement. M14558 gives a measured MOTU example and warns application buffering can change measured results.
- M2565 (2012) again separates proximity-to-now limitations from additional latency beyond digital transfer. M280 (2013) extends the accurate-timestamp/safety-offset guidance to AudioServerPlugIns.

## What is not established

These messages do not define jitter as standard deviation, peak-to-peak adjacent-ZTS interval variation, or maximum residual of a fitted line. The proposed two/three independent uncertainty-boundary explanation in the supplied research is not present. There is no claim that tripling standard deviation guarantees zero dropouts.

The 69 ns and 786 ns statistics measured earlier are interval-variation diagnostics, not established bounds on timestamp accuracy against hardware. A stable phase bias can be invisible to them.

## Proposed ASFW 48 kHz policy (engineering application, not an Apple quote)

Resolve each preset to actual transport/content deadlines first. Derive worst-case RX visibility lag and TX immutable-content lead in frames, relative to a verified sample/host reference plane. Driver queue servicing can affect those transfer constraints; do not equate it with HAL client wake-up jitter or count it again as a generic margin.

Keep timestamp accuracy allowance separately named J. Given a justified bound J in frame units, use input transfer limit + 2J and output transfer limit + 3J, rounding only the final result to the actual required alignment and preserving validated device constraints. At 48 kHz one sample is 20.833333 us. A chosen bound of one sample contributes two input and three output frames; this is illustrative, not a measured ASFW bound.

Presets should select validated transfer policies and reserve headroom, not falsify timestamp accuracy, physical latency declarations, or change every ring size. Keep ZTS cadence and storage capacity separate from delay. Validate under load and with a loopback measurement whose timestamp reference and compensation behavior are documented.
