# asfw_sim

Four-cursor TX geometry simulator for ASFireWire. It exists because the existing
`tools/*.py` simulators model only **three** cursors and are therefore
structurally blind to the failure they were being used to rule out.

```
W = CoreAudio write frontier      host frames that exist
E = exposed frame end             host frames mapped into prepared TX packets
C = completion / committed end    packet slots safe for OHCI ownership
R = RX replay reader vs producer  observed timing available to build TX SYT
```

`tools/tx_data_horizon_burst_sim.py` and `tools/asfw_timing_geometry_sim.py`
contain **zero** references to `R`. They assume `E` advances to `W + horizon`
whenever the producer runs, so they cannot express a stalled exposure frontier.

**Results so far: [FINDINGS.md](FINDINGS.md).** They falsify the mechanism both
2026-07 triage reports assert, and identify a different one.

## Anti-drift

Constants are parsed live from the driver headers by `headers.py`
(`AudioTimingGeometry.hpp`, `AudioHalBufferProfiles.hpp`, `RxSequenceReplay.hpp`)
and `tests/test_constants_match_headers.py` fails if they move. Nothing here
hard-codes a geometry: both triage reports reached wrong conclusions from stale
constants, and that is the failure this design prevents.

## Run

```bash
uv sync --extra dev
uv run pytest -v

uv run asfw-sim                           # ALL live driver geometry (default)
uv run asfw-sim plan-io --frames 4096     # cost a larger CoreAudio buffer size
uv run asfw-sim run --stall-ms 100        # one scenario
uv run asfw-sim cliff                     # bisect the stall-tolerance cliff
uv run asfw-sim sweep                     # readDelay / horizon comparison
```

## Fidelity

`producer.py` mirrors `PrepareTransmitSlots` (`ASFWAudioDriverZts.cpp:189-547`)
including its loop bound, `frameTargetSatisfied` early exit, the
`ahead` / `overwritten` / epoch branches, and the frame-cursor projection at
`:399-437`. `replay.py` mirrors `RxSequenceReplay.hpp:136-345` branch for branch.

Deliberate limits: not a bit-accurate SYT encoder, no OHCI descriptor model, no
CoreAudio HAL model. The seqlock failure classes cannot fire in a single-threaded
model and are present only for telemetry parity. Only 1x rates (44.1/48 kHz) are
implemented in the driver today — the 2x/4x tiers in `geometry.py` are carried so
the math is ready, and are not a validation gate.
