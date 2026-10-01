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

AudioAnalysisEngine now owns a separate actor executor. Pipeline setup runs through an `@concurrent` factory. GPU completion extracts immutable results and validates the ring, then metric acceptance, cursor/filter transitions, CPU loudness accumulation and 10 Hz history preparation execute on the engine actor. History crosses to rendering through a lock-protected non-observable sidecar. Pure DSP models explicitly opt out of the app target's default MainActor isolation. `stop()` suspends until pending completion has finished; the owner awaits it before closing the mapping. Channel changes are applied before consuming the next range. At most one completion notification can be pending on MainActor, preventing a stalled UI from accumulating stale completion tasks. SwiftUI publication and AppKit view operations remain on MainActor. Moving engine work alone cannot remove SwiftUI graph costs shown by the A/B experiment. Runtime CPU improvement for this actor change remains unmeasured until the same Xcode configuration is profiled.

## Metal captures

The read-only reference `/Users/mrmidi/DEV/Wisdom/docs/metal-instrumentation.md` documents headless Metal System Trace and exports for `metal-application-command-buffer-submissions`, `metal-command-buffer-completed`, `metal-command-buffer-error`, and `metal-gpu-intervals`. Use these to distinguish CPU encoding, submission latency and GPU execution. CPU Time Profiler is insufficient to establish GPU occupancy or rendering frame pacing. No Metal System Trace was captured for this experiment.

## Xcode runtime verification after actor migration

On 2026-10-01 the user launched the Debug app from Xcode (PID 49881). A requested 20-second Time Profiler capture estimated 46.14% total CPU (21.206-second sample span). Main-thread samples accounted for 6,885 ms, approximately 32.47% of one core. `AudioAnalysisEngine.consume`, the actor-isolated `finish` body, and `publishPlotHistory` appeared on worker threads; the main-thread engine-named frames were the intended UI notification closure, not actor DSP work.

A second requested 15-second capture on the same process with scalar publication disabled estimated 35.88%. SwiftUI graph inclusive sampled CPU fell from 6,025 ms / 21.206 s (about 28.41% of one core) to 689 ms / 15.802 s (about 4.36%). Total CPU did not fall by the same amount: Metal/analysis work increased in the second capture, so the captures are not a fixed-throughput microbenchmark. The prior Xcode baseline was 59.53%; the observed new run is about 22.5% lower, but this is not a controlled actor-only comparison.

The normal capture attributed 25.61% of sampled stacks to Metal libraries; the scalar-disabled capture attributed 48.52%. These are CPU stack categories, not GPU utilization, and inclusive categories overlap. Both Xcode trace metadata records include the Metal debug-layer configuration. A validation-disabled run and Metal System Trace are the next measurements before changing render/command-buffer cadence. The temporary scalar-publication override was removed and normal 10 Hz restored. Trace artifacts are `/tmp/asfw-actor-xcode-20261001.trace` and `/tmp/asfw-actor-xcode-0hz.trace`.

## Apple-guided submission and leaf-view changes

Apple recommends minimizing command buffers per frame, preferably one: https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/CommandBuffers.html . The analyzer previously submitted separate command buffers for Monitor, stereo histories, loudness bars/history, spectrum, goniometer and waveform. AnalyzerRenderSubmission now batches synchronous render callbacks into one visual command buffer on one queue, with at most two visual frames in flight. The cursor-driven audio-analysis command buffer remains separate. Each drawable is presented on the shared visual buffer and each renderer retains its completion/resource ownership. SwiftUI `updateNSView` configures plot inputs but no longer calls `draw()` on scalar publication.

Apple's SwiftUI performance session recommends controlling frequent dependencies and using the SwiftUI instrument to inspect update causes: https://developer.apple.com/videos/play/wwdc2025/306/ . Observation has now moved from whole-panel containers into numerical readouts and loudness session controls. Plot geometry, anchors, grids, tabs, generator controls and Metal representables no longer depend on scalar publication. Publication still runs at 10 Hz; this change does not trade away numeric or visual cadence. SwiftUI trace `/tmp/asfw-swiftui-before-batching.trace` provides the pre-change update evidence. A test verifies eight render requests share one submitted command buffer; existing tests cover Monitor's hosted layout and GPU reductions. Runtime gain for this second change is pending a user-launched Xcode capture.

## Runtime verification of batching and leaf readouts

User-launched Xcode Debug process PID 56933, 2026-10-01, with `MTL_DEBUG_LAYER=1` still enabled. The requested 20-second normal capture estimated **31.27% CPU**, compared with 46.14% in the prior actor-only run (approximately 32.2% lower). Inclusive SwiftUI graph sampled CPU normalized to wall time fell from 28.41% to 11.89% of one core; Metal libraries fell from 11.82% to 8.76%. Categories overlap and must not be added. These short user-driven captures are not a controlled throughput benchmark.

The same-process scalar-disabled capture estimated **33.23% CPU**, not lower than the normal run. SwiftUI graph dropped to about 5.10%, while Metal and engine stack categories increased to 14.52% and 13.64% respectively. This means scalar disabling is not a stable estimate of the minimum total CPU: non-UI work differed between intervals. Analysis/submission throughput and scheduler overhead need to be measured next before changing batch sizes or render cadence. Normal 10 Hz publication was restored by removing the temporary override. Artifacts: `/tmp/asfw-leaf-batched-xcode.trace` and `/tmp/asfw-leaf-batched-xcode-0hz.trace`.

## Native Metal capture before the five optimizations

Headless Metal System Trace of the user-launched Xcode Debug process PID 56933 completed on 2026-10-01. Over a 6.776-second submission span, the export contains 716 app command buffers and 716 matching completions, with zero command-buffer error rows. All 255 labeled `ASFW analyzer visual frame` submissions contained 13 encoders (approximately 37.63 visual submissions/s). The remaining 461 unlabeled/two-encoder submissions averaged 68.04/s. This verifies shared command-buffer submission; it does not establish display FPS or total GPU occupancy. GPU interval rows include nested stages and cannot be interpreted as whole-frame duration. Artifacts: `/tmp/asfw-batched-metal.trace`, `/tmp/asfw-batched-metal-summary.json`.

## Five follow-up optimizations implemented

- The structured acquisition loop runs inside `AudioAnalysisEngine.run`, reads the small driver control state, updates non-observable rendering state and consumes ranges without awaiting MainActor. Only lifecycle notifications and visual completion notifications cross to MainActor. The obsolete UI-driven poller was removed. Routing generations are applied between GPU batches; stale routing requests cannot replace a newer generation. Cancellation and stop still drain GPU completion before mapping teardown.
- Acquisition runs every 20 ms instead of 10 ms (since raised to 40 ms with a 12-chunk / 5760-frame batch cap; see the end of this file). Cursor-driven consumption retains all accepted frames, with the existing maximum 1920-frame/40-ms batch and 4096-frame overwrite slack. GPU loudness chunks remain 480 frames/10 ms, preserving measurement resolution. This is not a guarantee of exactly 50 submissions/s under scheduler delays.
- Explicit draw calls are gated independently: Monitor/phase up to 60 Hz, spectrum/waveform up to 30 Hz, loudness canvas 10 Hz. History uploads occur only when the engine's 10-Hz history revision changes. Stereo history reuses its uploaded buffer in the shared phase canvas. Cadence deadlines accommodate differing analysis/render frequencies without half-rate quantization or queued catch-up bursts.
- SwiftUI anchor slots describe plot geometry. Monitor uses one surface for its meters/indicators; Stereo uses one transparent surface/render pass for phase and both histories; Loudness uses one for its three bars and three history series. Spectrum uses one surface/render pass for either the combined L/R power trace or both separate M/S plots, with independent lane FFT/smoothing buffers. Diagnostics retains one two-lane waveform surface. Labels, controls and static grids remain SwiftUI. Each canvas encodes into the shared visual command buffer, bounded to two in-flight visual frames.
- Human-facing numerical readouts default to **4 Hz / 250 ms**, diagnostics to **2 Hz / 500 ms**. Each readout publishes only when its formatted string changes. Loudness session controls subscribe to phase changes separately from numerical metrics. Graphic rendering and DSP do not depend on numerical publication. The Debug `/tmp/asfw-analyzer-ui-hz` experiment still overrides scalar cadence; deleting it now restores 4 Hz.

Regression coverage includes acquisition progress while MainActor is deliberately blocked, GPU shutdown/drain, formatted-value deduplication, cadence scheduling, a single hosted Metal surface across multiple slots and resizing, Monitor geometry, and existing GPU/DSP correctness tests. Runtime CPU improvement for this final group is **unmeasured** until the user builds and runs Release without debugger/Metal validation. The app changes do not require reinstalling the driver.

For the reported loudness holes, distinguish renderer spacing from rejected measurement ranges: the history shader breaks segments separated by more than 200 ms or explicitly marked discontinuous, and also suppresses non-finite loudness values. Rejected/overwritten analysis ranges or epoch changes mark the session discontinuous. At 48 kHz, a 12288-frame ring minus 4096-frame slack allows about 170.7 ms of backlog. Acquisition no longer depends on MainActor, but real discontinuities still require Reset; no fabricated interpolation or automatic reset was introduced.

Transparent plot pipelines now use alpha blending, so invisible rectangles/invalid history segments preserve prior geometry rather than erase it in the shared pass. The GPU plot test uses the production pipeline and checks nonzero alpha for all five primitive modes. Final full Swift test run passed (416 reported test-case entries); CPU and display cadence for the new Release build remain unmeasured.

## Release capture and display ballistics

The user-launched Release app PID 7136 was captured on 2026-10-01 before adding display smoothing. The 20-second requested Time Profiler capture produced a 20.895-second sample span and estimated **24.75% CPU**. Trace metadata still contains `MTL_DEBUG_LAYER=1`, Main Thread Checker and GPU Tools injection. This is a Release-with-validation result, not the requested clean validation-disabled baseline; the prior Debug result was 31.27%, but build/workload differences prevent attributing that reduction solely to the five optimizations. Artifacts: `/tmp/asfw-release-20261001.trace`, `/tmp/asfw-release-20261001.json`, `/tmp/asfw-release-20261001-toc.xml`.

Display-only ballistics now smooth Monitor values before supplying Metal primitive parameters: RMS uses exponential dB-domain attack of 35 ms and release of 300 ms, peak markers rise immediately and release over 450 ms, and correlation/balance/width (plus loudness bars) use a 180-ms time constant. Filtering uses actual elapsed time, resets on invalid/idle input and session/discontinuity/memory epoch changes, and does not modify measurement metrics, loudness accumulation, plotted histories or clipping counts. No extra timer, GPU submission or SwiftUI publication is added. Numerical readouts retain their existing 250-ms cadence. Regression tests check reset, fast attack/slow release and independence from render frequency.

The same Release executable was relaunched by the user through Finder (PID 8646). The next 20-second requested capture estimated **20.73% CPU** over a 20.758-second sample span. Trace metadata contains none of the earlier Metal debug-layer, Main Thread Checker or GPU Tools injection environment entries. This is the clean-launch baseline before display smoothing. Relative to the Xcode-launched 24.75% run, the observed reduction is 4.02 percentage points (approximately 16.2%); the short sequential runs are not a fixed-throughput benchmark. Inclusive stack time normalized to wall duration is approximately 7.95% for SwiftUI graph, 4.31% for Metal libraries, and 3.15% for analysis engine; categories overlap and must not be summed or confused with GPU occupancy. Artifacts: `/tmp/asfw-release-clean-20261001.trace`, `/tmp/asfw-release-clean-20261001.json`, `/tmp/asfw-release-clean-20261001-toc.xml`.

Debug with smoothing (user-launched Xcode PID 9474) estimated **25.66% CPU** in a requested 20-second capture, 20.718-second sample span. Metadata confirms `MTL_DEBUG_LAYER=1` and Main Thread Checker/GPU Tools injection. Stacks containing `AnalyzerDisplaySmoother` account for only 3 ms of sampled CPU, about 0.0145% of one core; this tiny sample count establishes no precise microbenchmark but shows no material smoothing hotspot. This run is not directly comparable with the 20.73% clean Release baseline because configuration and workload differ. Capture artifacts: `/tmp/asfw-debug-smoothing-20261001.trace`, `/tmp/asfw-debug-smoothing-20261001.json`, `/tmp/asfw-debug-smoothing-stacks.xml`.

## Acquisition at 40 ms, 2026-10-01

A Debug capture with Xcode's Metal frame capture attached (23.9% estimated CPU) put about 74% of samples off the main thread: the analysis loop, its command-buffer submissions (`IOGPUCommandQueueSubmitCommandBuffers`) and worker-thread wakeups. Acquisition now runs every 40 ms (5 ms tolerance), halving analysis submissions, completions and driver state reads to about 25 per second. The batch cap rises from 4 to 12 loudness chunks (5760 frames: 120 ms at 48 kHz, 60 ms at 96 kHz) so a late wakeup drains in one pass instead of letting the backlog creep toward the overwrite margin. Loudness keeps 10 ms chunks. Runtime effect unmeasured until a capture without the Metal capture layer.
