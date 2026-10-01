# Analyzer CPU profiling

Use the headless Time Profiler capture and symbol summary:

```sh
python3 tools/profile_analyzer.py --pid <ASFW_PID> --seconds 15 \
  --trace /tmp/asfw-analyzer.trace --json /tmp/asfw-analyzer.json
```

Omit `--pid` to summarize an existing trace. The script resolves xctrace XML ID/reference compression and reports leaf symbols and overlapping inclusive categories. Estimated CPU is sampled CPU time divided by sample timestamp span, not an authoritative FPS or energy measurement. Capture without builds/tests in the background, with the same app build, window size, playback and validation settings. Keep traces outside Git.

## Controlled scalar-publication experiment, 2026-10-01

One standalone Debug process (PID 44830), active Apogee Duet playback, FFT 2048/Hann, 12-second requested captures. Analysis, completion-driven Metal drawing and plot history remained active for every capture.

| SwiftUI scalar publication | Estimated CPU |
| --- | ---: |
| 10 Hz | 36.33% |
| Disabled | 8.98% |
| 5 Hz | 26.53% |

The test strongly implicates scalar SwiftUI updates. This does not establish linear scaling or an FPS result. A prior Xcode-launched baseline estimated 59.53%, but Metal validation was enabled there; it is not a controlled before/after comparison with the standalone build.

Debug builds temporarily read `/tmp/asfw-analyzer-ui-hz` once per second. A number from 0 through 60 selects scalar publication cadence; zero disables only scalar publication. Delete the file to restore 10 Hz. Release builds do not read this file. The experiment file was removed after capture.

## Boundary implemented

The dashboard publishes only connection/lifecycle/geometry changes. Four separately observed child states publish relevant scalar changes; a diagnostics timing update does not invalidate Monitor, Loudness or the dashboard. Plot history is held in a non-observable sidecar and uploaded by completion-driven renderers, independently of SwiftUI scalar publication. Metric reads omit history unless explicitly requested. A regression test checks publication isolation.

## MainActor follow-up

AudioAnalysisEngine is still MainActor-isolated. GPU completion already extracts output and validates the ring on the completion callback, but metric acceptance, cursor/filter transitions and CPU loudness accumulation return to MainActor. Move this serial engine work to a dedicated actor/executor in a separate change. Pass immutable snapshots and retain mappings until pending command buffers finish; serialize channel changes, stop and completion before releasing resources. Keep SwiftUI publication and AppKit view operations on MainActor. Moving engine work alone cannot remove SwiftUI graph costs shown by the A/B experiment.

## Metal captures

The read-only reference `/Users/mrmidi/DEV/Wisdom/docs/metal-instrumentation.md` documents headless Metal System Trace and exports for `metal-application-command-buffer-submissions`, `metal-command-buffer-completed`, `metal-command-buffer-error`, and `metal-gpu-intervals`. Use these to distinguish CPU encoding, submission latency and GPU execution. CPU Time Profiler is insufficient to establish GPU occupancy or rendering frame pacing. No Metal System Trace was captured for this experiment.
