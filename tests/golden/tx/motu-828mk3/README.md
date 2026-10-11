# MOTU 828mk3 TX golden

`vendor-start-rx-48k.txt` records DATA `(cycle,first SPH)` produced by the real
ASFW TX producer from the compact official-session device RX capture at
`documentation/fixtures/MOTU/828mk3-48k/`. It is generated ASFW output, not the
official driver's trace and not an ASFW hardware capture.

The rig feeds every observed DATA stamp and reconstructs observed NO-DATA only
inside the declared continuous 4096-cycle range. It invokes the production
MotuRxTimingObserver with the RX frame cursor, uses the real cumulative clock
bridge, and drives the production synthesizer, stamper and payload writer.
Transport completions and host time are emulated. PCM is not scored.

Besides the exact generated trace, assertions require first lead 9216 ticks,
settled same-cycle RX-to-TX offset 6656 +/-32 ticks, settled lead within
[9216-32,12288+32), lock, unmute and zero hard resyncs. The settled tail starts
3000 cycles into the run, after preparation/queue latency. Four starting
cadence rotations are checked from the same recorded input; only the original
rotation is saved as a golden. These properties are not hardware claims.

Regenerate only after reviewing a deliberate timing change:

```sh
cmake --build build/tests_build --target AudioDriverTxProducerTests
ASFW_UPDATE_GOLDEN=1 build/tests_build/audio/AudioDriverTxProducerTests \
  --gtest_filter='MotuTxCaptureGolden.*'
git diff -- tests/golden/tx/motu-828mk3
```

Normal runs omit `ASFW_UPDATE_GOLDEN`. The 90-second bursts are used by offline
paired-capture and device-calibration regressions; they are not joined into a
fictional continuous TX stream.
