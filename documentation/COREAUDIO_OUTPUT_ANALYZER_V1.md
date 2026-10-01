# CoreAudio Output Analyzer v1 — reviewed design and implementation contract

Status: proposed design; this document does not claim that the planned measurements are implemented or conformant.

Reviewed on 2026-10-01 against ASFW commit `19822320`, ITU-R BS.1770-5 (November 2023), EBU Tech 3341 v4 (November 2023), and EBU Tech 3342 v4 (November 2023). This document translates and consolidates the supplied design proposal, records review corrections, and incorporates the agreed dashboard, data model, and buffer plan.

## 1. Scope and terminology

The analyzer observes the **selected output channel pair A/B** in the Float32 PCM ring written by CoreAudio. A is interpreted as Left and B as Right. Both assignments default to output channels 1 and 2 and are shared by every panel.

This measures the selected pair at the CoreAudio output-ring observation point. It does not measure the complete programme on an arbitrary multichannel device, the physical converter output, input audio, or a loopback path. The observation point and selected channels must be visible in the UI.

The dashboard contains five panels simultaneously: **Monitor, Stereo, Spectrum, Loudness, and Diagnostics**. There is no top-level page selector. Stereo receives the most space. Device, channel routing, sample rate, connection state, and measurement controls form the common context. Smaller windows rearrange the same panels into a scrollable layout.

Standard-derived measurements and custom stereo analysis must have distinct semantics:

- BS.1770 / EBU: K-weighted loudness, Momentary, Short-term, Integrated, LRA, and true peak.
- ASFW analysis: sample peak, RMS, M/S, correlation, balance, Side energy, mono energy retention, goniometer, spectrum, and stereo histories.

In loudness terminology, **M/S/I means Momentary / Short-term / Integrated**. In stereo terminology, **Mid/Side** denotes the channel transform. Prefer spelled-out labels and type names to avoid ambiguity.

```text
CoreAudio Float32 output ring
              |
        retained new frames
              v
   Continuous AudioAnalysisEngine
              |
              +-- raw sample / level measurements
              +-- stereo measurements
              +-- spectrum analysis
              +-- BS.1770 / EBU loudness processing
              |
              v
     AudioAnalyzerSnapshot + GPU plot resources
              |
              +-- Monitor
              +-- Stereo
              +-- Spectrum
              +-- Loudness
              +-- Diagnostics
```

## 2. Review findings and required corrections

| Finding | Decision |
|---|---|
| The proposal's continuous consumer and separate connection/measurement lifetimes are sound. | Adopt them as the foundation before adding loudness. |
| `writeEnd - lastConsumed > ringFrames` is only a preliminary gap check. | Also inspect oldest-valid frame, epochs, memory generation, routing generation, and the state after reading. Guard unsigned arithmetic against cursor rollback. |
| CoreAudio can begin overwriting old slots before publishing the next WriteEnd. | Cursor validation alone does not establish an immutable snapshot. Reserve headroom for an in-progress writer and validate the live contract. Do not describe the mapping as a lossless stream. |
| A 100 ms internal quantum gives suitable gating boundaries but can miss Maximum Momentary/Short-term peaks between boundaries. | Use a finer provisional 10 ms energy grid for live maxima; retain 100 ms Integrated gating steps. Confirm the resolution against phase-offset conformance vectors. |
| The draft's correlation formula omits DC removal; current code computes Pearson correlation. | Preserve and explicitly define the centered Pearson metric in v1. A future uncentered metric must be separately named. |
| The draft describes symmetric Hann; the current shader uses periodic Hann. | Keep periodic Hann as the current default and normalize by the actual window sum. Do not silently switch definitions. |
| Exact-bin sine invariance is claimed too broadly for every window. | Require it for the current periodic Hann and calibrated configurations; use window-specific tolerances for other windows and off-bin tones. |
| The mono-retention formula is negative while “loss” commonly implies a positive amount. | Use `Mono Energy Retention (dB)` for the negative value, or explicitly negate it for a positive `Mono Energy Loss`. |
| A 60-second LRA indicator is a prescribed display convention, not proof of statistical stability. | Expose `provisional` during the first 60 seconds; thereafter show `available`, not an unconditional statistical guarantee. |
| A fixed histogram or short display history could approximate or discard measurement history. | The CPU reference retains full Integrated and LRA measurement records. Display history is a separate bounded ring. |
| Conformance appears at the end of the proposed sequence. | Develop and exercise vectors alongside each algorithm, then run the combined suite before a compliance claim. |

The review also corrects an earlier conversation statement: the existing `tools/validate_audio_spectrum.swift` already contains synthetic GPU calibration checks. What is missing is the **standard loudness/true-peak conformance suite and user-facing calibration tooling**, not all synthetic tests.

## 3. Existing implementation baseline

The current app maps the production output descriptor read-only and imports it into Metal with `bytesNoCopy`. It already computes sample peak and RMS for L/R and Mid/Side, short-window centered correlation, energy balance, and Side energy. It renders a goniometer and waveform, and implements a 2048-point radix-2 FFT with periodic Hann, coherent-gain amplitude normalization, L/R or Mid/Side selection, power-domain exponential averaging, and peak hold.

Current analysis is tied to rendering: the goniometer renderer measures a recent window, and each spectrum view owns a separate compute/render queue and buffers. This is appropriate as a proof of the memory path but is not a continuous loudness consumer.

Relevant sources:

- [Observer connection, mapping, and metadata](../ASFW/AudioObserver/AudioObserverClient.swift)
- [Raw level and stereo kernel](../ASFW/AudioObserver/AudioObserver.metal)
- [Goniometer and waveform renderer](../ASFW/AudioObserver/MetalAudioObserverView.swift)
- [Spectrum kernels](../ASFW/AudioObserver/Spectrum.metal)
- [Spectrum renderer](../ASFW/AudioObserver/MetalSpectrumView.swift)
- [Existing synthetic GPU validation](../tools/validate_audio_spectrum.swift)
- [Driver WriteEnd publication](../ASFWDriver/Audio/DriverKit/ASFWAudioDriverOutputWrite.cpp)
- [Playback-ring range bookkeeping](../ASFWDriver/Audio/Runtime/PlaybackRingRange.hpp)

## 4. Continuous consumption and overwrite safety

Within one stream/configuration generation, keep `lastConsumedFrame` and consume the half-open range `[lastConsumedFrame, writeEndFrame)` exactly once **when that range is still readable and successfully validated**. Advance the committed cursor only after accepting the range.

A read transaction contains:

1. A mapping lease retaining the descriptor-backed host mapping for the entire read or GPU command.
2. A before-read token: memory generation, session epoch, discontinuity epoch, routing generation, sample rate, channels, oldest-valid frame, and write end.
3. The candidate frame range and provisional DSP-state updates.
4. An after-read token and overwrite-margin assessment.
5. Commit of DSP state, cursor, and measurement records, or rejection and a discontinuity event.

Reject or reinitialize on cursor rollback, generation/format/routing changes, explicit discontinuity, or an unread cursor below the oldest-valid frame. The preliminary `W - consumed > R` check must not underflow; `R` is retained **active** history, not backing allocation.

The producer does not reserve slots for the observer. A metadata query can occur after CoreAudio has started rewriting slots but before the corresponding WriteEnd is published. Therefore before/after cursor checks need conservative writer headroom and empirical verification; they do not provide an unconditional synchronization guarantee. The current driver bounds a WriteEnd span by active capacity, so a smaller universal in-progress-write bound must not be assumed from the nominal IO budget. If the live contract cannot establish an adequate bound, the observer must report that limitation rather than claim lossless conformant observation.

Keep the audio writer independent of graphics and analysis. Do not introduce PCM copies or blocking into the audio callback. The initial CPU reference reads the mapped samples directly; copying small filter states or derived measurements is allowed. A rejected read must not contaminate committed K-weighting/FIR state or append measurement history.

At 48 kHz the active ring retains 12288 frames, or 256 ms. Even the 400 ms Momentary window exceeds that capacity. Streaming accumulators are consequently necessary. A larger 24576-frame allocation does not create a longer active history.

Processing cadence follows cursor progress and safety margin. UI publication at 10–30 Hz and rendering at a display cadence are separate concerns. Duplicate UI refreshes must not consume the same audio twice.

## 5. BS.1770 signal path and selected-pair semantics

For the selected stereo pair, process each channel through continuous K-weighting, accumulate mean-square energy, combine with `G_L = G_R = 1`, and convert to logarithmic loudness. Apply gating only where required by the particular measurement.

The selected pair is the measurement programme for v1. Full multichannel programme measurement would require explicit speaker-role mappings, weighting rules, and LFE handling. Arbitrary device channel count must not silently imply those mappings.

Authoritative algorithm reference: [ITU-R BS.1770-5, Annex 1](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.1770-5-202311-I!!PDF-E.pdf).

### K-weighting at 48 kHz

The supplied proposal correctly specifies two cascaded biquads. With denominator convention `1 + a1 z^-1 + a2 z^-2`, the coefficients are:

```text
Stage 1:
b = [1.53512485958697, -2.69169618940638, 1.19839281085285]
a = [1, -1.69065929318241, 0.73248077421585]

Stage 2:
b = [1, -2, 1]
a = [1, -1.99004745483398, 0.99007225036621]
```

These are the 48 kHz coefficients from Annex 1, Tables 1 and 2. Generate and validate equivalent-response coefficients for each additional supported sample rate. Never reuse the 48 kHz coefficients at another rate without validation.

Use double precision in the CPU reference for coefficients, filter state, and accumulated energy. There are four persistent biquad states: two stages for each selected channel. Keep their sign convention explicit and test splitting the same signal into different processing chunks.

For filtered channel samples `y_i[n]`:

```text
z_i = sum(y_i[n]^2) / N
E   = z_L + z_R
L_K = -0.691 + 10 log10(E)
```

The generalized expression is `-0.691 + 10 log10(sum(G_i z_i))`. Silence has negative-infinite loudness; unavailable/warming-up data is a different state and must not be represented as silence.

## 6. Energy chunks, live windows, and maxima

The proposal's 100 ms chunks can exactly compose 400 ms and 3 s rectangular windows **at their chunk boundaries**. They also align with the 100 ms gating hop. That alone is not enough to promise correct live maxima: a short event starting between those boundaries can be underestimated.

Use a provisional 10 ms energy quantum for live-window evaluation. At 48 kHz this is 480 frames; at 44.1 kHz it is 441 frames. Each chunk stores raw sum-of-squares and actual frame count, not an average of LUFS values. Handle other rates through rational frame-boundary scheduling or explicitly restrict supported rates.

```text
LoudnessEnergyChunk
  startFrame: UInt64
  sumSquaresLeft: Double
  sumSquaresRight: Double
  frameCount: UInt32
```

Forty 10 ms chunks form the Momentary window; 300 form the Short-term window. Maintain rolling sums to avoid rescanning all chunks for every reading. Emit Integrated gating blocks every ten chunks. A consumer pass may complete multiple chunks and must emit every required measurement boundary.

Maximum detection runs on the internal measurement cadence. Redrawing the UI more often does not repair an undersampled maximum. The 10 ms choice is provisional until the EBU offset/maximum vectors pass; increase resolution or use finer cumulative-energy bookkeeping if necessary.

No filtered multi-second PCM ring is required: continuous filter state and energy sums suffice.

## 7. Momentary and Short-term loudness

Momentary uses an ungated 400 ms rectangular window; Short-term uses an ungated 3 s rectangular window. Compute each from total filtered energy divided by actual sample count, then apply the loudness formula. Do not add attack/release smoothing to the standard-derived values.

Store Maximum Momentary and Maximum Short-term separately from current readings. Their reset is linked to Integrated reset. UI animation may interpolate presentation, but published measured values and maxima must remain identifiable.

See [EBU Tech 3341, sections 2.1 and 2.2](https://tech.ebu.ch/docs/tech/tech3341.pdf) for time scales, live-update requirements, maxima, and ballistics. ASFW's internal quantum and UI refresh choices are implementation decisions subject to conformance testing.

## 8. Integrated loudness and gating

Generate a complete 400 ms block every 100 ms; discard incomplete blocks at the measurement endpoint. A block stores its channel-weighted mean-square energy `q_j`.

The supplied equations are retained:

```text
l_j       = -0.691 + 10 log10(q_j)
J_a       = { j : l_j > -70 }
L_a       = -0.691 + 10 log10(mean(q_j over J_a))
Gamma_r   = L_a - 10
J_g       = { j : l_j > -70 and l_j > Gamma_r }
Integrated = -0.691 + 10 log10(mean(q_j over J_g))
```

Define empty gated sets explicitly: no valid Integrated value exists in that case. Preserve the standard's threshold comparisons when implementing and testing boundary values.

Relative gating changes as the measurement grows. Retain the measurement's block energies and recompute gating when updating Integrated. The initial CPU reference favors this transparent method over a quantized histogram. Optimization can follow profiling and must preserve the accepted tolerances.

```text
IntegratedBlock
  weightedMeanSquare: Double
```

Ten records per second require 36000 doubles per hour: 288000 bytes, or 281.25 KiB of payload. Additional timestamps or containers have separate overhead.

See [ITU-R BS.1770-5, Annex 1](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.1770-5-202311-I!!PDF-E.pdf) and [EBU Tech 3341, section 2.3](https://tech.ebu.ch/docs/tech/tech3341.pdf).

## 9. Connection lifetime versus measurement lifetime

`AudioAnalyzerSession` owns the CoreAudio connection and mapping lifetime. `LoudnessMeasurementSession` owns the measurement lifecycle. Resetting loudness must not reconnect the UserClient or remap PCM.

Support Start, Pause, Continue, and Reset. Integrated and LRA share these controls and their reset boundary. Maximum Momentary and Maximum Short-term reset with Integrated. Momentary and Short-term can remain live while Integrated/LRA accumulation is paused.

Do not concatenate samples across a pause as if there were no boundary. Pending gating blocks and LRA windows must not bridge an excluded interval. Previously accepted measurement records may remain for Continue; new accumulation begins with windows containing only included audio. Keep continuously running filters separate from inclusion bookkeeping.

Define maximum true-peak reset/aggregation consistently with the displayed measurement interval. PLR must compare maximum true peak and Integrated from the same included interval.

Distinguish user Pause from stopped playback, disconnect, and a lost sample range. Wall-clock pause duration is not audio duration. Persist a measurement identifier and accepted audio duration; both are useful for diagnostics and provisional LRA status.

## 10. Loudness Range

LRA uses a distribution of ungated Short-term readings sampled at least at the required rate. The supplied power-domain derivation is valid:

```text
Keep S_i >= -70 LUFS.
P       = mean(10^(S_i / 10))
L_abs   = 10 log10(P)
Gate    = L_abs - 20 LU
Keep eligible Short-term readings above the relative gate.
LRA     = percentile95 - percentile10
```

The relative threshold differs from Integrated's threshold. Averaging LUFS directly is incorrect. Document the percentile interpolation convention and test it against the allowed tolerances.

Store the full measurement's Short-term records, independently of the bounded plot history. At 10 Hz a double-only record stream also costs 281.25 KiB/hour. Show LRA as provisional during the first 60 seconds of measurement. This convention does not establish that every later estimate is statistically stable.

See [EBU Tech 3342, section 3.1](https://tech.ebu.ch/docs/tech/tech3342.pdf) for the algorithm and [EBU Tech 3341, section 2.4](https://tech.ebu.ch/docs/tech/tech3341.pdf) for the display convention.

## 11. True peak

True peak estimates the maximum of the reconstructed waveform, not just stored sample magnitudes. Preserve FIR history across consumer passes and account for interpolation delay, startup, and end-of-measurement tail handling.

For the initial 48 kHz path, use a validated four-phase interpolation filter and 4x oversampling. BS.1770-5 Annex 2 gives a coefficient example and discusses higher input rates; its example is not the only possible conformant implementation. Do not choose factors solely by a rounded target-rate rule: validate each supported rate and filter response.

The integer-headroom attenuation discussed in Annex 2 is unnecessary for floating-point processing. Keep separate sample peak in dBFS, current true peak in dBTP, and measurement maximum true peak in dBTP:

```text
TP   = max(abs(interpolated samples))
dBTP = 20 log10(TP)
```

Use a polyphase streaming implementation without allocating a full oversampled PCM history. Keep original sample peaks available; report sample over-range and reconstructed over-range separately. Finite Float32 values above unity must not be clipped before analysis.

Reference: [ITU-R BS.1770-5, Annex 2](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.1770-5-202311-I!!PDF-E.pdf).

## 12. Sample peak, RMS, crest factor, PLR, and clipping

```text
Peak        = max(abs(x_n))
Peak_dBFS   = 20 log10(Peak)
RMS         = sqrt(sum(x_n^2) / N)
RMS_dBFS    = 20 log10(RMS)
Crest_dB    = Peak_dBFS - RMS_dBFS
PLR         = maximumTruePeak_dBTP - Integrated_LUFS
```

Specify channel, measurement interval, and peak convention for every derived value. A window peak minus a lifetime RMS is not a meaningful crest factor. PLR requires the same measurement interval for both inputs and inherits their validity.

Use `sampleOverRangeCount` and threshold-crossing flags rather than claiming that every sample at or above unity proves upstream clipping. Float PCM can legally contain over-range values. Actual clipping detection, if added, needs a separate stated heuristic.

Reset counters and maxima according to documented scopes: recent window, selected stream, or measurement session. Treat NaN/Infinity as an invalid-data anomaly rather than silently dropping them from standard measurements.

## 13. Mid/Side, correlation, balance, and width

Retain the orthonormal stereo transform:

```text
Mid  = (L + R) / sqrt(2)
Side = (L - R) / sqrt(2)
L^2 + R^2 = Mid^2 + Side^2
```

With consistent windows:

```text
E_Mid  = sum(Mid^2)
E_Side = sum(Side^2)
Side energy % = 100 * E_Side / (E_Mid + E_Side)
Balance       = (E_R - E_L) / (E_R + E_L)
```

Side energy is a custom energy ratio. It should not be presented as an independent standardized “Stereo Width” measurement. If Width is another representation of that ratio, declare the relationship and avoid displaying two differently labeled values that imply independent information.

The proposal's uncentered correlation is:

```text
rho_uncentered = sum(L*R) / sqrt(sum(L^2) * sum(R^2))
```

Current ASFW code instead subtracts the channel means:

```text
C_LR = sum(L*R) - sum(L)*sum(R)/N
C_LL = sum(L^2) - sum(L)^2/N
C_RR = sum(R^2) - sum(R)^2/N
rho  = C_LR / sqrt(C_LL*C_RR)
```

Use this centered Pearson definition for v1 and expose invalid correlation when either variance is negligible. Correlation history uses measured audio timestamps. A rolling/exponential summary has an explicit time constant and must reset on routing or stream-generation changes.

A polarity indicator is only a heuristic inferred from a measurement window. Define thresholds, hysteresis, signal-level eligibility, and an unknown state; do not claim to diagnose wiring polarity from arbitrary music.

Optional future Mid/Side loudness must be labeled as an ASFW extension, with channel weighting and transform normalization specified. It must not be confused with programme loudness or Momentary/Short-term terminology.

## 14. Mono compatibility

Define one repeatable custom metric rather than an unexplained “mono safe” percentage:

```text
E_stereo = E_L + E_R = E_Mid + E_Side
Mono Energy Retention_dB = 10 log10(E_Mid / E_stereo)
Mono Energy Loss_dB      = -Mono Energy Retention_dB
```

Expected retention values:

| Signal | Retention |
|---|---:|
| L = R | 0 dB |
| L only | -3.0103 dB |
| L = -R | negative infinity |

This is energy retained in the orthonormal Mid component. It is not a claim about a physical playback chain's downmix gain. An arithmetic average `(L+R)/2` has a different absolute gain convention. Silence is undefined for this ratio; represent it as unavailable, not zero loss.

## 15. Spectrum calibration and presentation

Keep periodic Hann as the current default:

```text
w[n] = 0.5 * (1 - cos(2*pi*n/N)),  0 <= n < N
```

Symmetric Hann uses `N-1` in the denominator and is a distinct selectable definition, not a spelling change. For the unnormalized FFT, normalize using the actual sum of window samples:

```text
A[k] = 2*abs(X[k]) / sum(w)      interior positive-frequency bins
A[k] =   abs(X[k]) / sum(w)      DC and Nyquist
Spectrum_dBFS[k] = 20 log10(A[k])
```

For periodic Hann, sum(w) is N/2. The current factors 4/N and 2/N match this convention. A well-resolved interior exact-bin sine of peak amplitude 0.5 should measure approximately -6.0206 dBFS at its peak bin for N = 1024, 2048, 4096, and 8192, with explicit tolerances. Check edge cases separately; do not promise perfect invariance for off-bin tones or all windows.

Coherent-gain tone calibration is not noise-density calibration. If adding PSD/noise readings, use window energy and equivalent-noise-bandwidth normalization and label the units, such as dBFS/Hz, separately.

The Spectrum panel shares one pair of FFT results between overlay and split presentation. L/R and Mid/Side bases, power averaging, and peak hold are analysis settings. Peak-hold decay uses audio/measurement time rather than arbitrary UI refresh frequency. A logarithmic plot must disclose its bin aggregation/interpolation behavior when used for measurements.

Keep headroom for orthonormal Mid/Side: a full-scale in-phase pair produces a Mid amplitude of sqrt(2), or about +3.0103 dB relative to the single-channel reference. The current +6 dBFS top scale accommodates it.

Later spectrogram, cross-spectrum/coherence, and frequency-dependent phase/width are extensions. Coherence requires averaged complex spectra, not one magnitude FFT window.

## 16. Classes and ownership

Proposed responsibilities, using existing mechanisms where suitable:

| Type | Responsibility |
|---|---|
| `ASFWAudioObserverClient` | UserClient connection, selected endpoint, mapping, metadata queries. Split shader creation out of this transport-facing object. |
| `AudioAnalyzerSession` | Main-actor UI lifetime, selected device/routing/settings, connect/reconnect, publication of snapshots. |
| `AudioAnalysisEngine` | Serial cursor-driven processing, committed/provisional state, gap handling, scheduling derived outputs. Independent of view draw callbacks. |
| `AudioMappingLease` | Retain the mapped PCM resource until CPU reads and in-flight GPU work finish. Build on the existing retained-mapping lifetime mechanism. |
| `LoudnessProcessor` | Transparent CPU K-weighting, energy chunks, and live-window results. |
| `LoudnessMeasurementSession` | Start/Pause/Continue/Reset, gating records, LRA records, interval maxima, and completeness. |
| `TruePeakProcessor` | Continuous polyphase interpolation and peak aggregation. |
| `StereoAnalysisProcessor` | Raw channel/transform statistics, correlation, balance, mono retention, and histories. |
| `SpectrumAnalysisResources` | FFT pipelines, one selected-pair resource set, smoothing and peak state, validated frame tokens. |
| `AnalyzerHistoryStore` | Bounded display histories; no authority over full-session gating storage. |
| Panel views/renderers | Draw committed results and leased plot resources; never create independent observers or repeat measurement accumulation. |

Initially standard DSP runs as a double-precision CPU reference on a dedicated serial worker. Direct mapped PCM loads preserve the absence of a staging PCM copy, but remain subject to the observer read contract. FFT/goniometer/rendering remain GPU work. Profile before migrating standard DSP to GPU; every migrated stage must be compared against the reference and standard vectors.

UI work stays on the main actor; DSP and history scanning stay off it. Publish immutable Sendable summaries. GPU completion callbacks release resources/update a synchronized result store; they do not mutate SwiftUI state directly.

## 17. Structures and validity

Use named fields rather than untyped arrays with magic offsets. Define GPU wire layout separately from UI structures and verify field offsets/stride when implementing.

```text
AudioChannelPair
  leftIndex, rightIndex                  // internal indices are zero-based
  routingGeneration

AudioRingGeometry
  sampleRateHz, channels
  activeRingFrames, mappedFrames
  memoryGeneration

AudioFrameToken
  memoryGeneration, sessionEpoch, discontinuityEpoch, routingGeneration
  startFrame, endFrame

AudioMeasurementValue<T>
  value: optional T
  status: unavailable | warmingUp | valid | discontinuous

AudioLevelMetrics
  left, right, mid, side                 // each: samplePeak, rms, window span
  truePeakLeft, truePeakRight
  sampleOverRangeCounts

AudioStereoMetrics
  correlation, correlationAverage
  balance, sideEnergyRatio, monoEnergyRetention
  polarityEstimate

AudioLoudnessMetrics
  momentary, shortTerm, integrated, lra
  maxMomentary, maxShortTerm, maximumTruePeak, plr
  lraProvisional, measurementID, acceptedAudioFrames
  completeness

AudioDiagnosticsMetrics
  geometry, cursor, epochs
  timing per analysis/render stage
  droppedRanges, unsafeRanges, inFlight, wrapWindows

AudioAnalyzerSnapshot
  selectedDevice, selection, streamStatus
  frameToken, levels, stereo, loudness, diagnostics
```

Spectrum/point-cloud arrays stay in leased GPU resources rather than being copied into every SwiftUI snapshot. Metadata identifies which frame/token they represent. Treat unavailable, silent, warming-up, user-paused, disconnected, and discontinuous as different states.

## 18. Buffer sizes and storage policy

The following are **payload budgets**, not allocator overhead or guaranteed final ABI sizes.

| Resource | Proposed capacity | Payload |
|---|---|---:|
| Active source PCM | 12288 frames x C Float32 channels | `49152*C` bytes; 96 KiB for C=2 |
| Backing PCM mapping | 24576 frames x C Float32 channels | `98304*C` bytes; 192 KiB for C=2 |
| CPU K-weighting state | Four biquads in Double | Tens to hundreds of bytes, layout-dependent |
| Loudness energy history | 300 chunks at 10 ms; budget 32 bytes/chunk | 9600 bytes, approximately 9.38 KiB |
| Integrated history | 10 Double records/s | 281.25 KiB/hour |
| LRA history | 10 Double records/s | 281.25 KiB/hour |
| True-peak FIR state | Two channels, phase/filter-dependent | Hundreds of bytes; determine from validated filter |
| CPU/GPU result exchange | Three slots, provisional 256 bytes each | 768 bytes; verify final ABI |
| Spectrum raw amplitudes | 2 x 1025 Float, FFT=2048 | 8200 bytes |
| Spectrum smoothing state | 2 x 1025 float4 | 32800 bytes |
| Spectrum averaged + held traces | 2 traces x 2 channels x 1025 Float | 16400 bytes |
| Total persistent spectrum arrays | Previous three rows | 57400 bytes, approximately 56.05 KiB |
| FFT threadgroup scratch | 2048 float2 per active threadgroup | 16 KiB; not a persistent CPU buffer |
| Display metric history | 600 records x provisional 32 bytes | 19200 bytes, approximately 18.75 KiB |
| Optional goniometer point persistence | 32768 float2 points | 256 KiB; plus metadata |

For FFT length N, positive-frequency arrays contain N/2+1 bins; sizes scale accordingly. Larger N must fit both retained valid audio and a supported FFT implementation. The current one-threadgroup 2048 implementation cannot simply be resized to 8192: its scratch would grow to 64 KiB, so query hardware limits and use an appropriate alternative algorithm when needed.

The point-ring budget gives about one second only if appending roughly 1024 points at 30 updates/s. Store/derive timestamps and expire by time; fixed capacity alone is not a persistence-time guarantee. Share the same point data between compact and large goniometers.

Full-session histories grow separately from the 60-second display ring. At 10 Hz, two Double histories use approximately 13.18 MiB after 24 hours. Define a documented session limit, explicit user-visible stop/rotation, or file-backed storage before release. Never silently turn Integrated/LRA into a rolling 60-second measurement to cap memory.

## 19. Gaps and configuration boundaries

On a rejected/lost range, record its reason and estimated frame extent. Discard provisional updates and clear state that depends on continuous input. Existing accepted Integrated/LRA records may be retained for inspection, but the session remains marked incomplete until reset or an explicit new session.

| Measurement | Recovery |
|---|---|
| Sample peak | Valid for the next accepted range; lifetime maxima retain a completeness flag |
| RMS | Valid after its configured window is filled |
| FFT | Valid after a full valid FFT window |
| Correlation / mono energy | Valid after a full configured stereo window with eligible energy |
| True peak | Valid after sufficient FIR history; handle delay and tails |
| Momentary | Valid after 400 ms of accepted continuous audio |
| Short-term | Valid after 3 s of accepted continuous audio |
| Integrated / LRA | Prior session is incomplete; reset/new session required for an unbroken result |

Changing selected channels, sample rate, or mapping generation creates a measurement boundary. Do not mix filter state or gating records from different configurations. A cursor stall is not a sample gap by itself: distinguish no new audio from lost audio. Never synthesize silent frames for an unknown lost interval.

## 20. Calibration and conformance

Validation is part of each algorithm's implementation, not a final optional polish step:

```text
math and chunk-boundary checks
  -> deterministic ASFW synthetic signals
  -> EBU Tech 3341 loudness and true-peak signals
  -> EBU Tech 3342 LRA signals
  -> applicable ITU-R BS.2217 compliance material
  -> live mapped-ring lifecycle and overwrite validation
```

Retain the supplied proposal's representative expected results: an in-phase stereo 1 kHz sine at -23 dBFS peak/channel has approximately -23 LUFS Momentary, Short-term, and Integrated within the specified tolerance; gated low-level lead/tail material exercises Integrated; stepped loudness exercises LRA; inter-sample/high-frequency cases exercise true peak. Use the exact official signal definitions, fade envelopes, phases, and tolerances rather than loosely similar replacements.

Additional ASFW checks cover periodic Hann exact-bin gain across supported N, DC/Nyquist behavior, off-bin leakage, L/R and Mid/Side isolation, wrap boundaries, variable consumer chunk sizes, reset/pause/continue, format/routing changes, silence, non-finite data, and intentionally delayed reads.

Maximum Momentary/Short-term tests must vary the event offset relative to the analysis quantum. Long-duration and gate-boundary cases must expose precision and storage-policy errors. A CPU reference is a numerical oracle for future GPU migration, but agreement with the reference alone does not establish standards conformance.

User-facing Calibration/Test Signal tooling is a separate feature. An observer does not write test PCM into the driver ring; a test source must play through the normal selected CoreAudio output or use an explicitly labeled offline harness.

Reference test requirements: [EBU Tech 3341, section 2.9](https://tech.ebu.ch/docs/tech/tech3341.pdf), [EBU Tech 3342, minimum-requirements signals](https://tech.ebu.ch/docs/tech/tech3342.pdf), and [ITU-R BS.1770-5's BS.2217 reference](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.1770-5-202311-I!!PDF-E.pdf). Passing offline vectors validates algorithms; live claims additionally depend on accepted, continuous ring reads.

## 21. Implementation sequence

| Phase | Deliverable and acceptance |
|---|---|
| A | Typed frame tokens, mapping leases, continuous range consumption, and explicit gap/overwrite reporting. Establish the observation safety limits. |
| B | CPU K-weighting reference and fine energy chunks; verify coefficients and invariance to input chunk boundaries. |
| C | Momentary/Short-term and maxima; run the corresponding timing/offset vectors. |
| D | Measurement lifecycle established before Integrated/LRA accumulation; define pause, reset, maxima, and missing-data behavior. |
| E | 400 ms gating blocks and Integrated; validate absolute/relative gates and session boundaries. |
| F | LRA, percentiles, full history, and provisional status; validate the LRA vectors. |
| G | True peak, interpolation state, tails, and maximum TP; validate inter-sample vectors. |
| H | PLR, crest, sample-over-range counters, and their interval/validity semantics. |
| I | Stereo histories, shared goniometer persistence, and mono retention; keep these independent of standard loudness semantics. |
| J | Shared spectrum resources, calibrated configurable FFT/window choices, and overlay/split presentation. |
| K | Five-panel dashboard consumes one shared session and committed snapshots; common context and settings; no duplicate accumulation per view. |
| L | Combined conformance/lifecycle suite, Release build, and Instruments/performance characterization. |

UI restructuring can proceed alongside DSP stages using explicitly unavailable fields. All five panels are part of v1's dashboard scope; no placeholder numeric values or compliance claims are permitted before the respective implementation and validation are complete.

## 22. Open decisions before implementation

- What producer-side write-span/synchronization evidence can support accepted reads without blocking audio? The current observer metadata alone does not answer this.
- Which sample rates are validated in v1, and how are K-weighting/interpolation coefficients derived for each?
- Does the 10 ms maximum-search grid satisfy every required vector and tolerance, or is finer evaluation required?
- What explicit duration/storage policy applies to Integrated/LRA measurement sessions?
- Which exact pause/tail and true-peak interval semantics satisfy the selected meter requirements?
- What percentile convention and invalid/silent thresholds will be used consistently across CPU and GPU implementations?

These are implementation choices to resolve through reference documents and validation, not reasons to change the agreed output-pair scope or five-panel layout.
