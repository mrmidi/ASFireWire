//
//  RoundTripLatencyResultViews.swift
//  ASFW
//
//  Presentation of a finished impulse measurement and a finished tone test.
//  Names and meanings follow tools/rtl/README.md; milliseconds are only a view
//  of the frame counts at the active sample rate.
//

import SwiftUI

// MARK: - Impulse measurement

struct RtlMeasurementResultView: View {
    let result: RtlMeasurementResult
    let canExport: Bool
    let export: () -> Void

    var body: some View {
        RtlCard(title: "Result", systemImage: "chart.xyaxis.line") {
            configuration
            Divider()
            acceptance
            if result.accepted > 0 {
                Divider()
                numbers
            }
        }
        RtlIOAuditCard(io: result.io)
        RtlTrialTable(trials: result.trials)
        HStack {
            Button {
                export()
            } label: {
                Label("Export JSON", systemImage: "square.and.arrow.up")
            }
            .disabled(!canExport)
            Text("Writes the asfw.rtl_loopback.v1 record: provenance, declarations, IO audit, every trial with its verdict, and the summary.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    // MARK: Configuration

    private var configuration: some View {
        VStack(alignment: .leading, spacing: 6) {
            InfoRow(label: "Device", value: result.deviceName)
            InfoRow(label: "Sample rate", value: String(format: "%.1f Hz", result.sampleRate))
            InfoRow(label: "Buffer frames", value: "\(result.bufferActual)")
            InfoRow(label: "Capture window",
                    value: "\(result.window) fr (longest measurable round trip \(RtlFormat.millisecondsText(frames: Double(RtlFormat.maximumMeasurableFrames(window: result.window, edgeGuard: RoundTripLatencyViewModel.edgeGuardFrames)), sampleRate: result.sampleRate, decimals: 1)))")
            InfoRow(label: "Trials requested", value: "\(result.trialsRequested)")
            if result.bufferWasCoerced, let requested = result.bufferRequested {
                Label("Requested \(requested) frames; the device is at \(result.bufferActual). This run is at \(result.bufferActual).",
                      systemImage: "exclamationmark.triangle.fill")
                    .font(.callout)
                    .foregroundStyle(.orange)
            }
            if result.outcome == .timedOut {
                Label("Timed out: IO may have stalled.", systemImage: "exclamationmark.triangle.fill")
                    .font(.callout)
                    .foregroundStyle(.orange)
            }
        }
    }

    // MARK: Acceptance

    private var acceptance: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Trials")
                .font(.subheadline.bold())
            // The rate is part of the measurement: 3 of 20 accepted is a
            // different claim from 20 of 20.
            InfoRow(label: "Acceptance",
                    value: RtlFormat.acceptanceText(accepted: result.accepted, run: Int(result.trialsRun)))
            ForEach(result.rejections) { rejection in
                InfoRow(label: "  rejected: \(rejection.name)", value: "\(rejection.count)")
            }
            if result.accepted == 0 {
                Label("No trial was admitted.", systemImage: "xmark.octagon.fill")
                    .foregroundStyle(.red)
                Text("If the rejections are \"no signal\", check the loopback cable, output volume and input gain; the per-channel peaks below show where signal actually landed. \"window edge\" means the round trip is longer than the window: use the automatic window or a larger one. Otherwise the timeline was not quiet enough: raise the buffer size or quiet the machine.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
        }
    }

    // MARK: Numbers

    @ViewBuilder
    private var numbers: some View {
        let rate = result.sampleRate
        VStack(alignment: .leading, spacing: 8) {
            signalRow
            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 6) {
                GridRow {
                    Text("")
                    header("median")
                    header("")
                    header("range")
                    header("sd")
                }
                if let raw = result.raw { statRow("RTL_raw", raw, rate, emphasised: true) }
                if let onset = result.onset { statRow("RTL_raw (onset)", onset, rate) }
                if let ts = result.ts { statRow("RTL_ts (hw latency)", ts, rate) }
            }
            if let roundTrip = result.declared.roundTripFrames {
                InfoRow(label: "Declared round trip",
                        value: RtlFormat.framesAndMilliseconds(Double(roundTrip), sampleRate: rate))
            } else {
                InfoRow(label: "Declared round trip", value: "unavailable: a declaration could not be read")
            }
            Text("RTL_raw counts delivered frames between writing the impulse and seeing it return, so no reporting-only latency property can move it. RTL_ts is the same event pair in the HAL sample-time domain: timestamps carry safety but not hardware latency, so a truthful device returns in_hw_latency + out_hw_latency, not zero. RTL_raw (onset) reads early by the pre-ring; trust the peak.")
                .font(.caption)
                .foregroundStyle(.secondary)

            Divider()
            if let scheduling = result.scheduling {
                InfoRow(label: "Scheduling distance",
                        value: "\(RtlFormat.frames(scheduling.median)) measured, \(result.declaredSchedulingFrames) fr declared (2 x io + safety)")
            }
            if let ts = result.ts {
                InfoRow(label: "Hardware latency",
                        value: "\(RtlFormat.frames(ts.median)) measured, \(result.declaredHardwareFrames) fr declared (device + stream)")
            }
            residual
        }
    }

    @ViewBuilder
    private var signalRow: some View {
        if let decibels = RtlFormat.signalToNoiseDecibels(peak: result.minPeak, noise: result.worstNoise) {
            InfoRow(label: "Signal",
                    value: String(format: "peak %.3f, noise %.5f (%.1f dB SNR)", result.minPeak, result.worstNoise, decibels))
        } else {
            InfoRow(label: "Signal",
                    value: String(format: "peak %.3f, no measurable noise", result.minPeak))
        }
        if result.isWeakSignal {
            Label("Weak signal: below \(Int(RtlFormat.weakSignalRatio))x signal-to-noise. Raise the input gain before trusting sub-frame precision.",
                  systemImage: "exclamationmark.triangle.fill")
                .font(.callout)
                .foregroundStyle(.orange)
        }
        if result.invertedTrials > 0 {
            Label("Polarity inverted in \(result.invertedTrials) trials (cable or device inverts).",
                  systemImage: "arrow.up.arrow.down")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private var residual: some View {
        if let residual = result.residualFrames {
            InfoRow(label: "RESIDUAL",
                    value: RtlFormat.framesAndMilliseconds(residual, sampleRate: result.sampleRate, signed: true))
            Text("The signed amount by which the declarations misstate the physical path: median(RTL_ts) minus the declared hardware latency. Positive means the device under-declares: audio really arrives later than it claims.")
                .font(.caption)
                .foregroundStyle(.secondary)
        } else {
            InfoRow(label: "RESIDUAL", value: "unavailable")
            Text("\(result.declared.missing.count) declared propert\(result.declared.missing.count == 1 ? "y" : "ies") could not be read. The measurements above stand on their own; the residual does not, because the declaration it subtracts is incomplete.")
                .font(.caption)
                .foregroundStyle(.orange)
        }
    }

    // MARK: Rows

    private func header(_ text: String) -> some View {
        Text(text).font(.caption).foregroundStyle(.secondary)
    }

    private func statRow(_ label: String, _ stat: RtlStat, _ rate: Double,
                         emphasised: Bool = false) -> some View {
        GridRow {
            Text(label)
                .font(emphasised ? .body.bold() : .body)
            Text(RtlFormat.frames(stat.median))
                .font(.system(.body, design: .monospaced).weight(emphasised ? .bold : .regular))
            Text(RtlFormat.millisecondsText(frames: stat.median, sampleRate: rate))
                .font(.system(.body, design: .monospaced).weight(emphasised ? .bold : .regular))
            Text(String(format: "[%.2f .. %.2f]", stat.minimum, stat.maximum))
                .font(.system(.callout, design: .monospaced))
                .foregroundStyle(.secondary)
            Text(String(format: "%.2f", stat.standardDeviation))
                .font(.system(.callout, design: .monospaced))
                .foregroundStyle(.secondary)
        }
    }
}

// MARK: - IO audit

struct RtlIOAuditCard: View {
    let io: RtlIOAudit

    var body: some View {
        RtlCard(title: "IO Audit", systemImage: "waveform.path.ecg") {
            InfoRow(label: "Callbacks", value: "\(io.callbacks)")
            InfoRow(label: "Distinct spans",
                    value: io.distinctSpans.map(String.init).joined(separator: " ")
                        + (io.distinctSpans.count > 1 ? "   spans vary" : ""))
            InfoRow(label: "Processor overloads", value: "\(io.processorOverloads)")
            InfoRow(label: "Delivered-frame gaps",
                    value: String(format: "%u events, %.0f frames lost", io.gapEvents, io.gapFrames))
            InfoRow(label: "Sample-time re-anchors",
                    value: String(format: "%u events (worst %+.0f fr)", io.anchorEvents, io.worstAnchorFrames))
            InfoRow(label: "Clocks disagreed", value: "\(io.clockDisagreements)")
            InfoRow(label: "Missing timestamps", value: "\(io.missingTimestamps)")
            if io.hasAnomaly {
                Label("Any of the above inside a trial rejects that whole trial.",
                      systemImage: "exclamationmark.triangle.fill")
                    .font(.callout)
                    .foregroundStyle(.orange)
            }
            if io.distinctSpans.count > 1 {
                Text("Callback size varies; any reasoning that assumed a fixed span is wrong.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            InfoRow(label: "Input channel peaks",
                    value: io.inputChannelPeaks.prefix(16)
                        .map { String(format: "%.3f", $0) }.joined(separator: " "))
        }
    }
}

// MARK: - Per-trial table

struct RtlTrialTable: View {
    let trials: [RtlTrialRow]

    var body: some View {
        RtlCard(title: "Trials", systemImage: "list.number") {
            if trials.isEmpty {
                Text("No trials were captured.").foregroundStyle(.secondary)
            } else {
                Table(trials) {
                    TableColumn("#") { row in
                        Text("\(row.index + 1)").monospacedDigit()
                    }
                    .width(min: 28, ideal: 34, max: 44)
                    TableColumn("Verdict") { row in
                        Text(row.verdictName)
                            .foregroundStyle(row.isAccepted ? Color.primary : Color.orange)
                    }
                    .width(min: 100, ideal: 130)
                    TableColumn("RTL_raw (fr)") { row in number(row.rawFrames) }
                    TableColumn("RTL_ts (fr)") { row in number(row.tsFrames) }
                    TableColumn("onset (fr)") { row in number(row.onsetFrames) }
                    TableColumn("peak") { row in
                        Text(String(format: "%.3f", row.peak)).monospacedDigit()
                    }
                    TableColumn("callbacks") { row in
                        Text("\(row.callbacks)").monospacedDigit()
                    }
                    TableColumn("overloads") { row in
                        Text("\(row.overloads)").monospacedDigit()
                    }
                }
                .frame(height: min(320, 36 + CGFloat(trials.count) * 24))
                Text("Only accepted trials reach any statistic. Rejected trials are listed with what disqualified them; their numbers are shown for diagnosis, not for use.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func number(_ value: Double?) -> some View {
        Text(value.map { String(format: "%.2f", $0) } ?? "-")
            .monospacedDigit()
    }
}

// MARK: - Tone

struct RtlToneResultView: View {
    let result: RtlToneResult

    var body: some View {
        RtlCard(title: "Tone Result", systemImage: "waveform") {
            InfoRow(label: "Device", value: result.deviceName)
            InfoRow(label: "Tone", value: String(format: "%.1f Hz, %.0f s capture at %.1f Hz", result.frequency, result.duration, result.sampleRate))
            InfoRow(label: "Buffer frames", value: result.bufferActual > 0 ? "\(result.bufferActual)" : "unavailable")
            if result.bufferWasCoerced, let requested = result.bufferRequested {
                Label("Requested \(requested) frames; the device is at \(result.bufferActual). This run is at \(result.bufferActual).",
                      systemImage: "exclamationmark.triangle.fill")
                    .font(.callout)
                    .foregroundStyle(.orange)
            }
            if result.outcome == .timedOut {
                Label("Timed out after \(String(format: "%.1f", Double(result.capturedFrames) / result.sampleRate)) s captured: IO may have stalled.",
                      systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
            }
            Divider()
            InfoRow(label: "Callbacks",
                    value: "\(result.callbacks)"
                        + (result.averageFramesPerCallback.map { String(format: " (%.1f frames each on average)", $0) } ?? ""))
            InfoRow(label: "Processor overloads", value: "\(result.processorOverloads)")
            InfoRow(label: "Input sample-time gaps", value: "\(result.sampleTimeGaps)")
            InfoRow(label: "Missing timestamps", value: "\(result.missingTimestamps)")
            Divider()
            verdict
        }
        if result.isValid, !result.events.isEmpty {
            eventTable
        }
    }

    @ViewBuilder
    private var verdict: some View {
        if !result.isValid {
            Label("No verdict: \(result.invalidReason ?? "the capture could not be judged")",
                  systemImage: "questionmark.circle.fill")
                .foregroundStyle(.orange)
        } else {
            let decibels = result.noiseRms > 0 ? 20.0 * log10(result.toneAmplitude / result.noiseRms) : nil
            InfoRow(label: "Looped-back tone",
                    value: String(format: "%.3f (%.1f dBFS)", result.toneAmplitude,
                                  result.toneAmplitude > 0 ? 20.0 * log10(result.toneAmplitude) : -.infinity)
                        + (decibels.map { String(format: ", noise %.5f rms (%.1f dB SNR)", result.noiseRms, $0) } ?? ""))
            InfoRow(label: "Analysed",
                    value: String(format: "%.1f s, tone arrived at capture frame %llu", result.analysedSeconds, result.onsetSample))
            InfoRow(label: "Glitches",
                    value: "\(result.dropouts) dropouts, \(result.slips) slips, \(result.clicks) clicks"
                        + (result.eventsPerMinute.map { String(format: "  (%.2f per minute)", $0) } ?? ""))
            if result.eventCount == 0 {
                Label("Clean: every analysed frame matches the tone.", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green)
            } else if Int(result.eventCount) > result.events.count {
                Text("\(Int(result.eventCount) - result.events.count) more events are not listed.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Text("A loopback cannot say whether a glitch happened on the way out or on the way in; it measures the sum of both. A glitch next to an overload, sample-time gap or missing timestamp may be CoreAudio's, not the driver's.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private var eventTable: some View {
        RtlCard(title: "Glitch Events", systemImage: "exclamationmark.triangle") {
            Table(result.events) {
                TableColumn("#") { row in Text("\(row.index)").monospacedDigit() }
                    .width(min: 28, ideal: 34, max: 44)
                TableColumn("Kind") { row in Text(row.kind) }
                    .width(min: 60, ideal: 80)
                TableColumn("Input sample") { row in
                    Text(row.inputSample.map { String(format: "%.0f", $0) } ?? "-").monospacedDigit()
                }
                TableColumn("t (s)") { row in
                    Text(String(format: "%.3f", row.seconds)).monospacedDigit()
                }
                TableColumn("Frames") { row in Text("\(row.frames)").monospacedDigit() }
                TableColumn("Silent") { row in Text("\(row.silentSamples)").monospacedDigit() }
                TableColumn("Slip (fr)") { row in
                    Text(row.slipFrames.map { String(format: "%+.2f", $0) } ?? "").monospacedDigit()
                }
                TableColumn("IO") { row in
                    Text(row.ioNote).foregroundStyle(row.ioNote.isEmpty ? Color.secondary : Color.orange)
                }
                .width(min: 120, ideal: 200)
            }
            .frame(height: min(320, 36 + CGFloat(result.events.count) * 24))
            Text("\"Input sample\" is the HAL input sample time of the event's first frame; it is the key for joining an event to a driver-side trace. A slip is measured modulo one tone period (\(String(format: "%.1f", result.sampleRate / result.frequency)) frames).")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }
}
