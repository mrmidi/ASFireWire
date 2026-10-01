# CoreAudio Output Analyzer v1 — Implementation Plan

## Summary and fixed decisions

Build one dashboard containing **Monitor, Stereo, Spectrum, Loudness, and Diagnostics**, with Stereo as the largest panel and a shared device/channel header.

Production PCM processing runs entirely in **Metal compute**. CPU responsibilities are connection lifecycle, cursor/epoch validation, settings, command submission, and publication of small measurement snapshots. A scalar Double CPU implementation exists only for tests against synthetic and official vectors.

Decisions established for v1:

- Observe the selected CoreAudio **output pair A/B**, defaulting to outputs 1 and 2.
- Full loudness and true-peak support starts at **48 kHz**. Existing visualizations remain available at other rates; unsupported measurements are explicitly marked.
- Live observation is **best effort**, with explicit quality and discontinuity status.
- Full measurement sessions are capped at **24 hours of included audio**.
- Correlation uses the **uncentered normalized cross-product**.
- Sustained negative correlation is labeled **Cancellation risk**, without diagnosing wiring polarity.
- Production Metal buffers use Float32 and integer counters; Metal does not support the draft’s `double` fields.

## 1. Shared engine, types, and ownership

Separate connection, analysis, and presentation so all five panels share one observer and one set of measurements.

- `AudioAnalyzerSession`: main-actor device/routing/settings model; owns the observer and engine lifecycle.
- `AudioMappingLease`: retains mapped memory through every in-flight GPU command and metadata query.
- `AudioAnalysisEngine`: serial cursor-driven scheduler, provisional/committed GPU state, validation, discontinuity handling, and snapshot publication.
- `LoudnessMeasurementSession`: Start/Pause/Continue/Reset, 24-hour limit, completeness, and measurement interval identity.
- GPU processors/resources for levels/stereo, K-weighting/loudness, true peak, spectrum, and display histories.
- Five panel views consume shared snapshots and GPU plot resources; drawing a second goniometer must not duplicate measurement accumulation.

Introduce named data structures:

- `AudioChannelPair`, `AudioRingGeometry`, and `AudioFrameToken` identify routing, geometry, generations, and frame ranges.
- `AudioLevelMetrics`, `AudioStereoMetrics`, `AudioLoudnessMetrics`, and `AudioDiagnosticsMetrics` replace indexed metric arrays.
- `AudioAnalyzerSnapshot` contains scalar summaries, validity flags, measurement identity, and timestamps.
- `AudioMeasurementStatus` distinguishes unsupported, warming-up, valid, idle, and discontinuous results.
- `SpectrumFrameResources` and `StereoHistoryResources` retain GPU arrays together with their frame/configuration tokens.

Use one serial Metal command queue. GPU results become visible to SwiftUI only after successful completion. Keep small result slots separate from private GPU history and DSP buffers.

Work remains in the main repository folder; generated Xcode project files are not edited.

## 2. Continuous consumption and GPU measurement pipeline

Run cursor polling on a dedicated serial worker at a nominal **10 ms cadence**, independently of rendering. Coalesce ticks while a candidate is in flight; process new frames once rather than reprocessing the same window.

Use a two-stage transaction:

1. Capture metadata and dispatch computation into provisional GPU state.
2. On completion, query metadata again. Accept the derived results or reject them before updating committed state and histories.

Reject candidates on generation/routing changes, cursor rollback, explicit discontinuity, loss of retained frames, GPU failure, or insufficient overwrite margin. Start consumption at the current cursor when connecting; do not treat earlier playback as part of the new measurement.

For v1, reserve **4096 frames of writer headroom** when assessing a read. This is an assumption based on the advertised ADK limit, not a synchronization proof; label all live results best effort. Do not modify the driver’s audio behavior to enforce it. A rejected range increments diagnostics and creates a measurement discontinuity.

Processing stages:

- **Levels/stereo:** GPU reductions gather sums, squared sums, cross-product, sample peaks, Mid/Side energies, and sample-over-range counts.
- **Correlation:** compute `sum(LR) / sqrt(sum(L²)sum(R²))`; silence is undefined. Maintain a GPU rolling summary and history.
- **Mono compatibility:** report negative `Mono Energy Retention (dB)`. Avoid the ambiguous negative “loss” label.
- **Cancellation risk:** require eligible signal energy and correlation below −0.3 for one second; clear after correlation exceeds −0.1 for one second. This is an explicitly custom indicator.
- **K-weighting:** one sequential GPU lane per selected channel processes the two persistent biquads. Use Float32 coefficients/state, compensated energy accumulation, and comparison with the Double oracle.
- **Loudness:** accumulate **10 ms energy chunks**, evaluate 400 ms Momentary and 3 s Short-term windows on that grid, and emit Integrated blocks every 100 ms. Capture maxima internally, independently of UI cadence.
- **Integrated:** retain complete block energies; perform absolute-gated and relative-gated reductions at 1 Hz.
- **LRA:** retain Short-term samples at 10 Hz; recompute at 1 Hz. Use GPU radix selection for the four order statistics needed by linearly interpolated P10/P95, avoiding a full-history sort.
- **True peak:** implement the BS.1770 four-phase FIR path at 48 kHz, with persistent sample history, delayed-output timestamps, and tested interval/tail handling.
- **Histories:** append accepted stereo/loudness summaries directly to GPU circular buffers.

Pause stops Integrated/LRA inclusion while live measurements continue. No measurement window may bridge an excluded interval. Reset clears Integrated/LRA and associated maxima without remapping the ring.

After a detected gap, live windows warm up again; Integrated/LRA accumulation freezes and remains marked discontinuous until Reset. Device, routing, or format changes start a new measurement configuration. At 24 hours, stop accumulation visibly and retain the completed results.

## 3. GPU resources and spectrum/rendering

Allocate bounded resources before measurement begins; avoid allocation in the recurring processing path.

| Resource | v1 allocation |
|---|---:|
| Source PCM | Existing mapped descriptor; no PCM staging buffer |
| Loudness energy ring | 300 × 32-byte records: approximately 9.4 KiB |
| Integrated records | 864000 Float32 energies: approximately 3.3 MiB |
| LRA records | 864000 Float32 values: approximately 3.3 MiB |
| Display histories | 600 × 32-byte records per history: approximately 18.8 KiB |
| GPU-to-CPU summaries | Three 256-byte slots, with verified ABI layout |
| Provisional DSP state | Mirrored committed state and bounded pending chunk outputs |
| Integrated/LRA reductions | Preallocated hierarchical reduction and radix-selection scratch |
| True-peak history | Two-channel FIR state; no full oversampled PCM buffer |
| Spectrum | Shared two-curve buffers, scaled to the configured FFT size |
| Goniometer persistence | Two 512×512 RGBA16Float textures: 4 MiB total |

For FFT length N, positive-frequency arrays contain N/2+1 bins; sizes scale accordingly. The current one-threadgroup 2048 implementation cannot simply be resized to 8192: its scratch would grow to 64 KiB, so query hardware limits and use an appropriate alternative algorithm when needed.

Spectrum defaults to **2048-point periodic Hann**. Support FFT sizes **1024, 2048, and 4096** in v1; 8192 is excluded from this release because retained-ring safety and the existing FFT implementation require additional work.

Provide periodic Hann, periodic Hamming, and four-term Blackman–Harris windows. Normalize using each actual window sum. Preserve separate DC/Nyquist treatment and headroom for orthonormal Mid/Side.

One spectrum engine produces the selected pair’s two curves for either overlay or split rendering. Default averaging is Fast, 150 ms; Slow is 1 second. Peak hold defaults to 2 seconds followed by 12 dB/s decay.

Render goniometer points from accepted GPU-derived XY data into the persistence texture, with a default one-second decay. Compact and large goniometers share that texture. Reset it on configuration changes and discontinuities.

Separate presentation cadence from processing:

- Numeric snapshots and display histories: 10 Hz.
- FFT: 30 Hz.
- Integrated/LRA recomputation: 1 Hz.
- Rendering: up to 60 Hz.

Waveform and diagnostic raw-sample visualization remain GPU rendered. CPU does not read live PCM or retrieve spectrum/history arrays.

## 4. Dashboard behavior and delivery sequence

The shared header contains device selection, A/B output assignments, sample rate, channel count, stream status, and measurement controls. Analyzer Settings uses a popover.

- **Monitor:** L/R and Mid/Side levels, sample peak, true peak, RMS, correlation, balance, Side energy, and compact goniometer.
- **Stereo:** large goniometer, correlation/Side-energy histories, M/S meters, mono retention, balance, and Cancellation risk.
- **Spectrum:** large overlay/split plots with axes and channel/basis legend.
- **Loudness:** Momentary, Short-term, Integrated, maxima, LRA, maximum true peak, PLR, crest factor, and history.
- **Diagnostics:** waveform, GPU sample visualization, cursor/geometry/epochs, per-stage timings, overwrite margin, rejected ranges, and measurement completeness.

Use display labels that distinguish Momentary/Short-term from stereo Mid/Side. Mark LRA provisional during its first 60 seconds. Report sample over-range separately from inferred clipping.

Implement in reviewable increments:

1. Shared session, typed models, GPU resource ownership, and continuous transactional consumption.
2. Unified GPU levels/stereo pipeline and five-panel layout using available measurements.
3. GPU K-weighting, energy chunks, live loudness, and maxima.
4. Measurement controls, Integrated, LRA, and bounded full-session storage.
5. True peak and derived Monitor/Loudness values.
6. Shared spectrum controls, histories, goniometer persistence, and diagnostics.
7. Combined conformance, lifecycle, and performance qualification.

Each increment includes its relevant checks. Unsupported or unfinished measurements display an explicit state rather than a fabricated number.

## 5. Validation and acceptance

Create a Double scalar CPU oracle that operates only on test PCM. Compare GPU outputs against it and against the official EBU/ITU vectors.

Required scenarios:

- K-weighting and FIR invariance across arbitrary chunk boundaries and ring wrap.
- EBU loudness, gating, LRA, and true-peak vectors with their specified tolerances.
- Offset bursts that test Maximum Momentary/Short-term on the 10 ms grid; refine evaluation if required by those tests.
- Full-scale/bin-centered sine calibration, DC/Nyquist, window changes, channel isolation, and Mid/Side cancellation.
- Gate boundaries, silence, NaN/Infinity, Float32 values above unity, and long-duration accumulation.
- Start/Pause/Continue/Reset, playback stop/restart, routing changes, reconnect, mapping replacement, delayed GPU execution, and 24-hour cap behavior.
- Shared resource reuse: all panels visible without duplicate observers or repeated loudness accumulation.
- Debug and Release builds, Metal validation, and Instruments measurements with dashboard closed/open.

Acceptance requires no CPU live-PCM reads, no PCM staging copies, no graphics waits in audio callbacks, successful standard-vector results at 48 kHz, and explicit reporting of rejected or discontinuous live observation.

Performance qualification must show that the recurring analysis keeps up without exhausting retained history. Measure long-session Integrated/LRA workloads at the full 24-hour capacity. No live standards-compliance claim may imply that the shared-ring observer guarantees lossless capture.
