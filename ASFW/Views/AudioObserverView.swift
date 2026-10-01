import Combine
import SwiftUI

@MainActor
private final class AudioObserverPanelModel: ObservableObject {
    @Published private(set) var status = "Connecting to the ASFW output ring…"
    @Published private(set) var snapshot = AudioObserverSnapshot()
    @Published private(set) var metrics = AudioObserverMetrics()
    @Published var mode: AudioObserverDisplayMode = .phaseScope

    let client: ASFWAudioObserverClient
    private var connected = false
    private var engine: AudioAnalysisEngine?
    private var leftChannel: UInt32 = 0
    private var rightChannel: UInt32 = 1
    private var routingGeneration: UInt64 = 0
    private var lastWriteEndFrame: UInt64?
    private var lastWriteProgress = Date.distantPast
    private var lastMetricsPublish = Date.distantPast

    init(guid: UInt64) {
        client = ASFWAudioObserverClient(guid: guid)
    }

    func run() async {
        while !Task.isCancelled {
            do {
                if !connected {
                    try client.open()
                    guard let device = client.metalDevice,
                          let ringBuffer = client.ringBuffer,
                          let stateReader = client.stateReader else {
                        throw AudioObserverError.metalUnavailable
                    }
                    engine = try AudioAnalysisEngine(device: device,
                                                     ringBuffer: ringBuffer,
                                                     stateReader: stateReader,
                                                     metrics: client.metrics)
                    engine?.setPair(left: leftChannel, right: rightChannel,
                                    generation: routingGeneration)
                    connected = true
                    status = "Observing the live output ring"
                }
                var current = try await client.poll()
                if lastWriteEndFrame != current.writeEndFrame {
                    lastWriteEndFrame = current.writeEndFrame
                    lastWriteProgress = Date()
                }
                current.ioRunning = Date().timeIntervalSince(lastWriteProgress) < 0.5
                if !current.ioRunning {
                    // Do not leave the last successful write painted after
                    // CoreAudio stops advancing the shared ring.
                    current.validHistoryFrames = 0
                }
                snapshot = current
                engine?.consume(current)
                if !current.ioRunning {
                    status = "Waiting for playback samples…"
                    client.metrics.markIdle()
                } else {
                    status = "Observing the live output ring"
                }
                if Date().timeIntervalSince(lastMetricsPublish) >= 0.1 {
                    metrics = client.metrics.read()
                    lastMetricsPublish = Date()
                }
                try await Task.sleep(for: .milliseconds(10))
            } catch is CancellationError {
                break
            } catch {
                status = error.localizedDescription
                engine?.stop()
                engine = nil
                client.close()
                connected = false
                do {
                    try await Task.sleep(for: .milliseconds(500))
                } catch {
                    break
                }
            }
        }
        engine?.stop()
        engine = nil
        client.close()
        connected = false
    }

    func setChannels(left: UInt32, right: UInt32) {
        guard left != leftChannel || right != rightChannel else { return }
        leftChannel = left
        rightChannel = right
        routingGeneration &+= 1
        engine?.setPair(left: left, right: right, generation: routingGeneration)
    }

    func startLoudnessMeasurement() {
        client.metrics.startLoudnessMeasurement(sampleRateHz: snapshot.sampleRateHz)
    }

    func pauseLoudnessMeasurement() { client.metrics.pauseLoudnessMeasurement() }
    func resumeLoudnessMeasurement() { client.metrics.resumeLoudnessMeasurement() }
    func resetLoudnessMeasurement() { client.metrics.resetLoudnessMeasurement() }
}

struct AudioObserverPanel: View {
    @StateObject private var model: AudioObserverPanelModel
    @State private var leftChannel: UInt32 = 0
    @State private var rightChannel: UInt32 = 1
    @State private var stereoSpectrum = true
    @State private var midSide = false
    @State private var slowSpectrum = false
    @State private var peakHold = true
    private let deviceName: String

    init(guid: UInt64, deviceName: String) {
        _model = StateObject(wrappedValue: AudioObserverPanelModel(guid: guid))
        self.deviceName = deviceName
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .firstTextBaseline) {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Audio Analyzer").font(.title2.bold())
                    Text(deviceName).font(.subheadline).foregroundStyle(.secondary)
                }
                Spacer()
                channelPicker("L / A", selection: $leftChannel)
                    .onChange(of: leftChannel) { _, value in model.setChannels(left: value, right: rightChannel) }
                channelPicker("R / B", selection: $rightChannel)
                    .onChange(of: rightChannel) { _, value in model.setChannels(left: leftChannel, right: value) }
                Label(String(format: "%.1f kHz · %u ch", Double(model.snapshot.sampleRateHz) / 1_000,
                             model.snapshot.channels),
                      systemImage: model.snapshot.ioRunning ? "waveform" : "pause.circle")
                    .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            }

            HStack(alignment: .top, spacing: 12) {
                panel("Monitor", subtitle: "Live levels and stereo summary") {
                    StereoMetersView(metrics: model.metrics, active: model.snapshot.ioRunning)
                    HStack {
                        valueTile("L Peak", dbfs(model.metrics.leftPeak))
                        valueTile("R Peak", dbfs(model.metrics.rightPeak))
                        valueTile("Correlation", model.metrics.correlationValid
                                  ? String(format: "%+.2f", model.metrics.correlation) : "—")
                        valueTile("Side energy", String(format: "%.1f%%", 100 * model.metrics.meterValues[7]))
                    }
                }
                panel("Stereo", subtitle: "Goniometer · selected output pair") {
                    scopePlot(mode: .phaseScope).frame(maxWidth: .infinity).aspectRatio(1, contentMode: .fit)
                    HStack {
                        valueTile("Balance", String(format: "%+.2f", model.metrics.meterValues[6]))
                        valueTile("Mono retention", db(model.metrics.analysis.stereo.monoEnergyRetentionDB.value))
                        valueTile("Side energy", String(format: "%.1f%%", 100 * model.metrics.meterValues[7]))
                    }
                }
            }

            panel("Spectrum", subtitle: "2048-point periodic Hann · logarithmic frequency") {
                HStack {
                    Picker("Basis", selection: $midSide) {
                        Text("L/R").tag(false); Text("M/S").tag(true)
                    }.pickerStyle(.segmented).frame(width: 150)
                    Picker("Average", selection: $slowSpectrum) {
                        Text("Fast · 150 ms").tag(false); Text("Slow · 1 s").tag(true)
                    }.pickerStyle(.segmented).frame(width: 230)
                    Toggle("Peak hold", isOn: $peakHold).toggleStyle(.checkbox)
                    Picker("Layout", selection: $stereoSpectrum) {
                        Text("Mono").tag(false); Text("Stereo").tag(true)
                    }.pickerStyle(.segmented).frame(width: 140)
                    Spacer()
                }
                HStack(spacing: 12) {
                    spectrumPlot(channel: leftChannel, side: false)
                    if stereoSpectrum { spectrumPlot(channel: rightChannel, side: true) }
                }.frame(height: 270)
            }

            HStack(alignment: .top, spacing: 12) {
                panel("Loudness", subtitle: "EBU R128 / ITU-R BS.1770 · 48 kHz measurement path") {
                    HStack {
                        valueTile("Momentary", measurementText(model.metrics.analysis.loudness.momentaryLUFS))
                        valueTile("Short-term", measurementText(model.metrics.analysis.loudness.shortTermLUFS))
                        valueTile("Integrated", measurementText(model.metrics.analysis.loudness.integratedLUFS))
                        valueTile(model.metrics.analysis.loudness.loudnessRangeIsProvisional
                                  ? "LRA · provisional" : "LRA",
                                  measurementText(model.metrics.analysis.loudness.loudnessRangeLU))
                        valueTile("Max True Peak",
                                  dbtpText(model.metrics.analysis.loudness.maximumTruePeakDBTP))
                    }
                    HStack(spacing: 8) {
                        loudnessSessionControls
                        Spacer()
                        Text(String(format: "Included %.1f s · 24 h maximum",
                                    Double(model.metrics.analysis.loudness.includedAudioFrames) / 48_000))
                            .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                    }
                    Text("Momentary and Short-term are live. Integrated, LRA, and maximum True Peak accumulate only while the measurement session runs; a stream discontinuity freezes them until Reset.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                panel("Diagnostics", subtitle: "Ring state, observer quality, and timing") {
                    HStack {
                        Picker("View", selection: $model.mode) {
                            Text("Waveform").tag(AudioObserverDisplayMode.waveform)
                            Text("Performance").tag(AudioObserverDisplayMode.phaseScope)
                        }.pickerStyle(.segmented).frame(width: 200)
                        Text(model.status).font(.caption).foregroundStyle(.secondary)
                    }
                    if model.mode == .waveform { scopePlot(mode: .waveform).frame(height: 150) }
                    diagnosticsRow("Ring / mapped", "\(model.snapshot.activeRingFrames) / \(model.snapshot.mappedFrames) frames")
                    diagnosticsRow("Write end / epoch", "\(model.snapshot.writeEndFrame) · \(model.snapshot.sessionEpoch)/\(model.snapshot.discontinuityEpoch)")
                    diagnosticsRow("CPU / queued / GPU", "\(milliseconds(model.metrics.cpuEncodeMilliseconds)) / \(milliseconds(model.metrics.scheduledToStartMilliseconds)) / \(milliseconds(model.metrics.gpuMilliseconds))")
                    diagnosticsRow("Age / overwrite margin", "\(milliseconds(model.metrics.sampleAgeMilliseconds)) / \(milliseconds(model.metrics.overwriteMarginMilliseconds))")
                    diagnosticsRow("In flight / wrap / unsafe", "\(model.metrics.inFlight) / \(model.metrics.windowsCrossingWrap) / \(model.metrics.unsafeWindows)")
                    diagnosticsRow("Invalid samples / over-range L·R", "\(model.metrics.invalidSampleCount) / \(model.metrics.analysis.levels.left.overRangeSamples) · \(model.metrics.analysis.levels.right.overRangeSamples)")
                }
            }
        }
        .padding(16)
        .background(.thinMaterial)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .task { await model.run() }
    }

    private func channelPicker(_ title: String, selection: Binding<UInt32>) -> some View {
        Picker(title, selection: selection) {
            ForEach(0..<max(2, Int(model.snapshot.channels)), id: \.self) { channel in
                Text("Output \(channel + 1)").tag(UInt32(channel))
            }
        }
        .frame(width: 190)
    }

    private func scopePlot(mode: AudioObserverDisplayMode) -> some View {
        ZStack {
            Color(red: 0.025, green: 0.035, blue: 0.05)
            if model.snapshot.validHistoryFrames > 1 {
                MetalAudioObserverView(client: model.client, mode: mode,
                                       leftChannel: leftChannel, rightChannel: rightChannel)
                    .id("\(model.snapshot.memoryGeneration)-\(mode)-\(leftChannel)-\(rightChannel)")
                    .padding(mode == .phaseScope ? 30 : 0)
            } else {
                Text("Waiting for audio").foregroundStyle(.secondary)
            }
            if mode == .phaseScope {
                AnalyzerPlotAxes(kind: .goniometer)
            } else {
                AnalyzerPlotAxes(kind: .waveform)
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 10))
    }

    private func spectrumPlot(channel: UInt32, side: Bool) -> some View {
        VStack(spacing: 4) {
            Text(midSide ? (side ? "Side · (L−R)/√2" : "Mid · (L+R)/√2") : "Output \(channel + 1)").font(.caption)
            ZStack {
                Color(red: 0.025, green: 0.035, blue: 0.05)
                if model.snapshot.validHistoryFrames >= 2048 {
                    MetalSpectrumView(client: model.client, channel: midSide ? leftChannel : channel,
                                      otherChannel: rightChannel, transform: midSide ? (side ? 2 : 1) : 0,
                                      slow: slowSpectrum, peakHold: peakHold)
                        .id("\(model.snapshot.memoryGeneration)-\(channel)-\(leftChannel)-\(rightChannel)-\(midSide)-\(slowSpectrum)-\(peakHold)")
                        .padding(.leading, 38).padding(.trailing, 12)
                        .padding(.top, 12).padding(.bottom, 30)
                } else {
                    Text("Waiting for audio").foregroundStyle(.secondary)
                }
                AnalyzerPlotAxes(kind: .spectrum(sampleRate: model.snapshot.sampleRateHz))
            }
            .clipShape(RoundedRectangle(cornerRadius: 10))
        }
        .frame(maxWidth: .infinity)
    }

    private func panel<Content: View>(_ title: String, subtitle: String,
                                      @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.headline)
                Text(subtitle).font(.caption).foregroundStyle(.secondary)
            }
            content()
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.primary.opacity(0.035))
        .clipShape(RoundedRectangle(cornerRadius: 10))
    }

    private func valueTile(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.system(.callout, design: .monospaced).weight(.semibold))
                .lineLimit(1).minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(8)
        .background(.white.opacity(0.04))
        .clipShape(RoundedRectangle(cornerRadius: 7))
    }

    @ViewBuilder
    private var loudnessSessionControls: some View {
        let loudness = model.metrics.analysis.loudness
        switch loudness.sessionPhase {
        case .idle:
            Button("Start") { model.startLoudnessMeasurement() }
                .disabled(model.snapshot.sampleRateHz != AudioLoudnessMeasurementSession.supportedSampleRate)
        case .running:
            Button("Pause") { model.pauseLoudnessMeasurement() }
            Button("Reset", role: .destructive) { model.resetLoudnessMeasurement() }
        case .paused:
            Button("Continue") { model.resumeLoudnessMeasurement() }
            Button("Reset", role: .destructive) { model.resetLoudnessMeasurement() }
        case .discontinuous:
            Label("Discontinuous · Reset to restart", systemImage: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
            Button("Reset") { model.resetLoudnessMeasurement() }
        case .complete:
            Label("24-hour measurement complete", systemImage: "checkmark.circle.fill")
                .foregroundStyle(.green)
            Button("Reset") { model.resetLoudnessMeasurement() }
        }
    }

    private func diagnosticsRow(_ title: String, _ value: String) -> some View {
        HStack {
            Text(title).foregroundStyle(.secondary)
            Spacer(minLength: 12)
            Text(value).font(.system(.caption, design: .monospaced)).multilineTextAlignment(.trailing)
        }
    }

    private func dbfs(_ value: Float) -> String {
        value > 0.000001 ? String(format: "%.1f dBFS", 20 * log10(value)) : "−∞ dBFS"
    }

    private func db(_ value: Float?) -> String {
        value.map { String(format: "%+.1f dB", $0) } ?? "—"
    }

    private func dbtpText(_ measurement: AudioMeasurement<Float>) -> String {
        guard let value = measurement.value else {
            return measurementText(measurement)
        }
        if measurement.status == .discontinuous {
            return String(format: "Hold %.1f dBTP", value)
        }
        return String(format: "%.1f dBTP", value)
    }

    private func measurementText(_ measurement: AudioMeasurement<Float>) -> String {
        if measurement.status == .discontinuous {
            guard let value = measurement.value, value.isFinite else { return "Discontinuous" }
            return String(format: "Hold %.1f LUFS", value)
        }
        guard let value = measurement.value else {
            switch measurement.status {
            case .unsupported: return "Unsupported"
            case .warmingUp: return "Warming up"
            case .idle: return "Idle"
            case .discontinuous: return "Discontinuous"
            case .valid: return "—"
            }
        }
        return value.isFinite ? String(format: "%.1f LUFS", value) : "−∞ LUFS"
    }

    private func milliseconds(_ value: Double?) -> String {
        guard let value else { return "—" }
        return String(format: "%.2f ms", value)
    }
}
