import Combine
import SwiftUI

@MainActor
final class AudioObserverPanelModel: ObservableObject {
    @Published private(set) var status = "Connecting to the ASFW output ring…"
    @Published private(set) var snapshot = AudioObserverSnapshot()
    private(set) var metrics = AudioObserverMetrics()
    let monitorUI = AnalyzerPanelUIState(section: .monitor)
    let stereoUI = AnalyzerPanelUIState(section: .stereo)
    let loudnessUI = AnalyzerPanelUIState(section: .loudness)
    let diagnosticsUI = AnalyzerPanelUIState(section: .diagnostics)
    private(set) var loudnessHistory: [AnalyzerHistoryVertex] = []

    let client: ASFWAudioObserverClient
    private var connected = false
    private var engine: AudioAnalysisEngine?
    private var leftChannel: UInt32 = 0
    private var rightChannel: UInt32 = 1
    private var routingGeneration: UInt64 = 0
    private var lastWriteEndFrame: UInt64?
    private var lastWriteProgress = Date.distantPast
    private var lastMetricsPublish = Date.distantPast
    private var latestSnapshot = AudioObserverSnapshot()
    private var lastHistoryPublish = Date.distantPast
    private var lastScalarPublish = Date.distantPast
    private var publicationHz = 10.0
    private var lastPolicyRead = Date.distantPast
    private var lastPlotPublish = Date.distantPast

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
                    engine?.onCompletion = { [weak self] in self?.publishCompletedAnalysis() }
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
                latestSnapshot = current
                client.renderState.update(current)
                engine?.consume(current)
                if !current.ioRunning && snapshot.ioRunning {
                    client.metrics.markIdle()
                }
                // Acquisition updates only lifecycle/geometry. Meters and history
                // are published by completed GPU work, independently of this poll.
                if snapshot.ioRunning != current.ioRunning ||
                    snapshot.memoryGeneration != current.memoryGeneration ||
                    snapshot.sessionEpoch != current.sessionEpoch ||
                    snapshot.discontinuityEpoch != current.discontinuityEpoch ||
                    snapshot.sampleRateHz != current.sampleRateHz ||
                    snapshot.channels != current.channels ||
                    snapshot.activeRingFrames != current.activeRingFrames {
                    snapshot = current
                    let nextStatus = current.ioRunning
                        ? "Observing the live output ring" : "Waiting for playback samples…"
                    if status != nextStatus { status = nextStatus }
                    if !current.ioRunning { metrics = client.metrics.read(includeHistory: false) }
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

    private func publishCompletedAnalysis() {
        let now = Date()
        if now.timeIntervalSince(lastPlotPublish) >= 1.0 / 60.0 {
            lastPlotPublish = now
            NotificationCenter.default.post(name: .asfwAnalysisCompleted, object: client.renderState)
        }
        #if DEBUG
        if now.timeIntervalSince(lastPolicyRead) >= 1 {
            lastPolicyRead = now
            if let text = try? String(contentsOfFile: "/tmp/asfw-analyzer-ui-hz", encoding: .utf8),
               let hz = Double(text.trimmingCharacters(in: .whitespacesAndNewlines)), hz >= 0, hz <= 60 {
                publicationHz = hz
            } else { publicationHz = 10 }
        }
        #endif
        guard now.timeIntervalSince(lastMetricsPublish) >= min(0.1, publicationHz > 0 ? 1 / publicationHz : 0.1) else { return }
        lastMetricsPublish = now
        metrics = client.metrics.read(includeHistory: false)
        if now.timeIntervalSince(lastHistoryPublish) >= 0.1 {
            lastHistoryPublish = now
            if let token = metrics.analysis.token {
                let loudness = metrics.analysis.loudness
                let frame = token.endFrame
                if let last = loudnessHistory.last, frame < last.frame { loudnessHistory.removeAll() }
                if loudnessHistory.last?.frame != frame {
                    loudnessHistory.append(AnalyzerHistoryVertex(frame: frame,
                        correlation: loudness.momentaryLUFS.value ?? .nan,
                        sideEnergy: loudness.shortTermLUFS.value ?? .nan,
                        breakBefore: metrics.analysis.streamStatus == .discontinuous ? 1 : 0,
                        integrated: loudness.integratedLUFS.value ?? .nan))
                    let duration = UInt64(latestSnapshot.sampleRateHz) * 60
                    loudnessHistory.removeAll { frame > $0.frame && frame - $0.frame > duration }
                }
            }
            client.plotHistory.stereo = client.metrics.readStereoHistory()
            client.plotHistory.loudness = loudnessHistory
            client.plotHistory.revision &+= 1
        }
        if publicationHz > 0 && now.timeIntervalSince(lastScalarPublish) >= 1 / publicationHz {
            lastScalarPublish = now
            publishScalarPanels(metrics, snapshot: latestSnapshot)
        }
    }

    func publishScalarPanels(_ metrics: AudioObserverMetrics, snapshot: AudioObserverSnapshot) {
        var scalars = metrics
        scalars.stereoHistory = []
        monitorUI.publish(scalars, snapshot: snapshot)
        stereoUI.publish(scalars, snapshot: snapshot)
        loudnessUI.publish(scalars, snapshot: snapshot)
        diagnosticsUI.publish(scalars, snapshot: snapshot)
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
    @State private var fftSize: UInt32 = 2048
    @State private var spectrumWindow: UInt32 = 0
    @State private var diagnosticTab = "Performance"
    @State private var testSignal = "1 kHz Sine"
    @State private var testLevel = "−6 dBFS"
    @State private var testMode = "L = R (Mono)"
    @State private var midSide = false
    @State private var slowSpectrum = false
    @State private var peakHold = true
    private let deviceName: String

    init(guid: UInt64, deviceName: String) {
        _model = StateObject(wrappedValue: AudioObserverPanelModel(guid: guid))
        self.deviceName = deviceName
    }

    var body: some View {
        GeometryReader { geometry in
            let compact = geometry.size.width < 1_040 || geometry.size.height < 680
            Group {
                if compact {
                    ScrollView {
                        compactDashboard
                    }
                } else {
                    standardDashboard(in: geometry.size)
                }
            }
        }
        .background(.thinMaterial)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .task { await model.run() }
    }

    private var dashboardHeader: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Label(model.snapshot.ioRunning ? "Running" : "Waiting for audio", systemImage: "circle.fill")
                    .font(.caption.weight(.semibold)).foregroundStyle(model.snapshot.ioRunning ? .mint : .secondary)
                    .help(model.status)
                Text(deviceName).font(.caption).foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer(minLength: 8)
            channelPicker("L / A", selection: $leftChannel)
                .onChange(of: leftChannel) { _, value in model.setChannels(left: value, right: rightChannel) }
            channelPicker("R / B", selection: $rightChannel)
                .onChange(of: rightChannel) { _, value in model.setChannels(left: leftChannel, right: value) }
            Label(String(format: "%.1f kHz · %u ch", Double(model.snapshot.sampleRateHz) / 1_000,
                         model.snapshot.channels),
                  systemImage: model.snapshot.ioRunning ? "waveform" : "pause.circle")
                .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                .fixedSize()
        }
    }

    private func standardDashboard(in size: CGSize) -> some View {
        let inset: CGFloat = 12
        let gap: CGFloat = 10
        let headerHeight: CGFloat = 42
        let width = max(0, size.width - inset * 2)
        let height = max(0, min(size.height, 900) - inset * 2)
        let rowHeight = max(0, (height - headerHeight - gap * 2) / 2)
        let topPanelWidth = max(0, (width - gap * 2) * 0.28)
        let stereoPanelWidth = max(0, (width - gap * 2) * 0.31)
        let spectrumPanelWidth = max(0, width - gap * 2 - topPanelWidth - stereoPanelWidth)
        let bottomPanelWidth = max(0, width - gap)

        return VStack(spacing: gap) {
            dashboardHeader.frame(height: headerHeight)
            VStack(spacing: gap) {
                HStack(spacing: gap) {
                    monitorPanel.frame(width: topPanelWidth, height: rowHeight)
                    stereoPanel.frame(width: stereoPanelWidth, height: rowHeight)
                    spectrumPanel.frame(width: spectrumPanelWidth, height: rowHeight)
                }
                HStack(spacing: gap) {
                    loudnessPanel.frame(width: bottomPanelWidth * 0.51, height: rowHeight)
                    diagnosticsPanel.frame(width: bottomPanelWidth * 0.49, height: rowHeight)
                }
            }
            .frame(height: rowHeight * 2 + gap)
        }
        .padding(inset)
        .frame(width: size.width, height: size.height, alignment: .topLeading)
    }

    private var compactDashboard: some View {
        VStack(spacing: 10) {
            dashboardHeader
            monitorPanel.frame(height: 260)
            stereoPanel.frame(height: 470)
            spectrumPanel.frame(height: 330)
            loudnessPanel.frame(height: 420)
            diagnosticsPanel.frame(height: 430)
        }
        .padding(12)
    }

    private var monitorPanel: some View {
        AnalyzerLivePanel(state: model.monitorUI) { metrics, snapshot in
            panel("Monitor", subtitle: "Live levels and stereo summary") {
                StereoMetersView(client: model.client, metrics: metrics, active: model.snapshot.ioRunning)
                    .frame(maxHeight: .infinity, alignment: .center)
                HStack(spacing: 8) {
                    valueTile("L True Peak", dbtpValue(metrics.analysis.levels.left.truePeak))
                    valueTile("R True Peak", dbtpValue(metrics.analysis.levels.right.truePeak))
                    valueTile("Correlation", metrics.correlationValid
                              ? String(format: "%+.2f", metrics.correlation) : "—")
                    valueTile("Side energy", String(format: "%.1f%%", 100 * metrics.meterValues[7]))
                }
                .frame(height: 48)
            }
        }
    }

    private var stereoPanel: some View {
        AnalyzerLivePanel(state: model.stereoUI) { metrics, snapshot in
            panel("Stereo", subtitle: "Goniometer · \(phasePersistenceText) · selected pair") {
                VStack(spacing: 8) {
                    GeometryReader { geometry in
                        let scopeSide = min(CGFloat(230),
                                            min(geometry.size.height, geometry.size.width * 0.62))
                        HStack(spacing: 8) {
                            scopePlot(mode: .phaseScope)
                                .frame(width: scopeSide, height: scopeSide)
                            StereoHistoryView(client: model.client, points: Array(model.client.plotHistory.stereo.suffix(1)),
                                              sampleRateHz: model.snapshot.sampleRateHz,
                                              active: model.snapshot.ioRunning)
                                .frame(maxWidth: .infinity, maxHeight: .infinity)
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                    .frame(maxHeight: .infinity)
                    HStack(spacing: 8) {
                        valueTile("Balance", String(format: "%+.2f", metrics.meterValues[6]))
                        valueTile("Mono retention", db(metrics.analysis.stereo.monoEnergyRetentionDB.value))
                        valueTile("Side energy", String(format: "%.1f%%", 100 * metrics.meterValues[7]))
                        valueTile("Mono cancellation", cancellationRiskText)
                    }
                    .frame(height: 48)
                }
            }
        }
    }

    private var spectrumPanel: some View {
        panel("Spectrum", subtitle: "Frequency domain analysis · stereo power / mid–side") {
            VStack(spacing: 7) {
                HStack(spacing: 8) {
                    Text("Basis")
                    Picker("Basis", selection: $midSide) {
                        Text("L/R").tag(false); Text("M/S").tag(true)
                    }
                    .labelsHidden().pickerStyle(.segmented).frame(width: 90)
                    Text("FFT Size")
                    Picker("FFT Size", selection: $fftSize) {
                        ForEach([UInt32(1024), 2048, 4096], id: \.self) { Text(String($0)).tag($0) }
                    }.labelsHidden().frame(width: 78)
                    Text("Window")
                    Picker("Window", selection: $spectrumWindow) {
                        Text("Hann").tag(UInt32(0)); Text("Hamming").tag(UInt32(1)); Text("Blackman").tag(UInt32(2))
                    }.labelsHidden().frame(width: 100)
                    Spacer(minLength: 0)
                }
                HStack(spacing: 8) {
                    Text("Average")
                    Picker("Average", selection: $slowSpectrum) {
                        Text("Fast · 150 ms").tag(false); Text("Slow · 1 s").tag(true)
                    }.labelsHidden().pickerStyle(.segmented).frame(width: 190)
                    Toggle("Peak hold · 2 s / 12 dB/s", isOn: $peakHold).toggleStyle(.checkbox)
                    Spacer(minLength: 0)
                }
                HStack(spacing: 8) {
                    spectrumPlot(channel: leftChannel, side: false)
                    if midSide { spectrumPlot(channel: rightChannel, side: true) }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                HStack(spacing: 14) {
                    spectrumLegend(.mint, midSide ? "Mid" : "Stereo power")
                    if midSide {
                        spectrumLegend(.orange, midSide ? "Side" : "Output \(rightChannel + 1)")
                    }
                    if peakHold { spectrumLegend(Color(red: 0.75, green: 0.55, blue: 0.22), "Peak hold") }
                    Spacer(minLength: 0)
                }
                .font(.caption2)
            }.font(.caption)
        }
    }

    private var loudnessPanel: some View {
        AnalyzerLivePanel(state: model.loudnessUI) { metrics, snapshot in
            let loudness = metrics.analysis.loudness
            panel("Loudness", subtitle: "Perceived loudness and dynamics · EBU R128 / ITU-R BS.1770") {
                HStack(spacing: 10) {
                    loudnessCard("Momentary", loudness.momentaryLUFS)
                    loudnessCard("Short-term", loudness.shortTermLUFS)
                    loudnessCard("Integrated", loudness.integratedLUFS)
                    VStack(spacing: 8) {
                        HStack(spacing: 8) {
                            valueTile(loudness.loudnessRangeIsProvisional ? "LRA · provisional" : "Loudness Range",
                                      dbValue(loudness.loudnessRangeLU, unit: "LU"))
                            valueTile("True Peak", dbtpText(loudness.maximumTruePeakDBTP))
                        }
                        HStack(spacing: 8) {
                            valueTile("PLR", dbValue(loudness.plrDB, unit: "dB"))
                            valueTile("Crest Factor", dbValue(loudness.crestFactorDB, unit: "dB"))
                        }
                    }.frame(maxWidth: .infinity)
                }.frame(height: 125)
                Text("Loudness History").font(.caption.weight(.medium))
                LoudnessHistoryView(client: model.client, hasHistory: !model.client.plotHistory.loudness.isEmpty)
                    .frame(maxHeight: .infinity)
                HStack(spacing: 14) {
                    spectrumLegend(.green, "Momentary")
                    spectrumLegend(.blue, "Short-term")
                    spectrumLegend(.purple, "Integrated")
                    Spacer()
                    loudnessSessionControls(loudness)
                    Text(String(format: "%.1f s", Double(loudness.includedAudioFrames) / 48_000))
                        .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                }.font(.caption)
            }
        }
    }

    private func loudnessCard(_ title: String, _ measurement: AudioMeasurement<Float>) -> some View {
        VStack(spacing: 7) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            Text(measurement.value.map { $0.isFinite ? String(format: "%.1f", $0) : "−∞" } ?? "—")
                .font(.system(size: 28, weight: .semibold, design: .rounded)).monospacedDigit()
            Text(measurement.value == nil ? measurementText(measurement) : "LUFS")
                .font(.caption2).foregroundStyle(.secondary)
            MetalAnalyzerPlotView(client: model.client, mode: 4,
                                  index: title == "Momentary" ? 0 : title == "Short-term" ? 1 : 2)
                .frame(height: 5).background(.white.opacity(0.08)).clipShape(Capsule())
        }
        .padding(12).frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(.white.opacity(0.035)).clipShape(RoundedRectangle(cornerRadius: 8))
    }

    private var diagnosticsPanel: some View {
        AnalyzerLivePanel(state: model.diagnosticsUI) { metrics, snapshot in
            panel("Diagnostics", subtitle: "Signal, performance and development tools") {
                Picker("Diagnostics", selection: $diagnosticTab) {
                    ForEach(["Waveform", "Ring Buffer", "Performance", "Calibration", "Test Signal", "Log"], id: \.self) {
                        Text($0).tag($0)
                    }
                }.labelsHidden().pickerStyle(.segmented)
                if diagnosticTab == "Performance" || diagnosticTab == "Ring Buffer" || diagnosticTab == "Waveform" {
                    HStack(alignment: .top, spacing: 10) {
                        diagnosticCard("Ring Buffer") {
                            diagnosticsRow("Active", "\(snapshot.activeRingFrames) frames")
                            diagnosticsRow("Mapped", "\(snapshot.mappedFrames) frames")
                            diagnosticsRow("Channels", "\(snapshot.channels)")
                            diagnosticsRow("Write end", "\(snapshot.writeEndFrame)")
                            diagnosticsRow("Epoch", "\(snapshot.sessionEpoch) / \(snapshot.discontinuityEpoch)")
                        }
                        if diagnosticTab != "Waveform" {
                            diagnosticCard("Performance · last frame") {
                                diagnosticsRow("CPU encode", milliseconds(metrics.cpuEncodeMilliseconds))
                                diagnosticsRow("Queued → GPU", milliseconds(metrics.scheduledToStartMilliseconds))
                                diagnosticsRow("GPU", milliseconds(metrics.gpuMilliseconds))
                                Divider()
                                diagnosticsRow("Sample age", milliseconds(metrics.sampleAgeMilliseconds))
                                diagnosticsRow("Overwrite margin", milliseconds(metrics.overwriteMarginMilliseconds))
                                diagnosticsRow("In flight", "\(metrics.inFlight)")
                                diagnosticsRow("Wrap / unsafe", "\(metrics.windowsCrossingWrap) / \(metrics.unsafeWindows)")
                            }
                        }
                        diagnosticCard("Output Waveform · L/R") {
                            scopePlot(mode: .waveform).frame(maxHeight: .infinity).frame(minHeight: 100)
                            HStack {
                                spectrumLegend(.mint, "Left"); spectrumLegend(.orange, "Right")
                            }
                        }
                    }.frame(maxHeight: .infinity)
                } else if diagnosticTab == "Calibration" {
                    ContentUnavailableView("Calibration", systemImage: "slider.horizontal.3",
                        description: Text("Calibration tools are planned. No correction is applied to the signal."))
                        .frame(maxHeight: .infinity)
                } else if diagnosticTab == "Log" {
                    diagnosticCard("Observer status") {
                        Text(model.status).font(.caption)
                        diagnosticsRow("Invalid samples", "\(metrics.invalidSampleCount)")
                        diagnosticsRow("Over-range L / R", "\(metrics.analysis.levels.left.overRangeSamples) / \(metrics.analysis.levels.right.overRangeSamples)")
                        Text("Full driver logs are available in System Logs.").font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer(minLength: 0)
                } else {
                    Text("Generator controls are a preview. Audio generation is not implemented.")
                        .font(.caption).foregroundStyle(.secondary)
                    Spacer(minLength: 0)
                }
                HStack(alignment: .top, spacing: 10) {
                    diagnosticCard("Test Signal Generator · preview") {
                        HStack {
                            Picker("Signal", selection: $testSignal) {
                                Text("1 kHz Sine").tag("1 kHz Sine"); Text("Pink Noise").tag("Pink Noise"); Text("White Noise").tag("White Noise")
                            }
                            Picker("Level", selection: $testLevel) {
                                ForEach(["−6 dBFS", "−12 dBFS", "−18 dBFS"], id: \.self) { Text($0).tag($0) }
                            }
                        }
                        HStack {
                            Picker("Mode", selection: $testMode) {
                                ForEach(["L = R (Mono)", "L = −R", "Left only", "Right only"], id: \.self) { Text($0).tag($0) }
                            }
                            Button("Play", systemImage: "play.fill") {}.disabled(true)
                                .help("Preview only — no audio is generated")
                        }
                    }
                    diagnosticCard("Validation · unavailable") {
                        diagnosticsRow("Peak / RMS", "— / —")
                        diagnosticsRow("Correlation", "—")
                        diagnosticsRow("Side Energy", "—")
                    }.frame(maxWidth: 180)
                }.frame(height: 105)
            }
        }
    }

    private func diagnosticCard<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            Text(title).font(.caption.weight(.semibold))
            content()
        }.padding(10).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .background(.black.opacity(0.12))
            .overlay { RoundedRectangle(cornerRadius: 8).strokeBorder(.white.opacity(0.06)) }
            .clipShape(RoundedRectangle(cornerRadius: 8))
    }

    private func spectrumLegend(_ color: Color, _ title: String) -> some View {
        HStack(spacing: 4) {
            Capsule().fill(color).frame(width: 10, height: 4)
            Text(title).foregroundStyle(.secondary)
        }
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
            if model.snapshot.ioRunning {
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
            Text(midSide ? (side ? "Side · (L−R)/√2" : "Mid · (L+R)/√2") : "L/R · averaged channel power")
                .font(.caption2)
            ZStack {
                Color(red: 0.025, green: 0.035, blue: 0.05)
                if model.snapshot.ioRunning {
                    MetalSpectrumView(client: model.client, channel: leftChannel,
                                      otherChannel: rightChannel, transform: midSide ? (side ? 2 : 1) : 3,
                                      slow: slowSpectrum, peakHold: peakHold, fftSize: fftSize, window: spectrumWindow)
                        .id("\(model.snapshot.memoryGeneration)-\(channel)-\(leftChannel)-\(rightChannel)-\(midSide)-\(slowSpectrum)-\(peakHold)-\(fftSize)-\(spectrumWindow)")
                        .padding(.leading, 38).padding(.trailing, 12)
                        .padding(.top, 12).padding(.bottom, 30)
                } else {
                    Text("Waiting for audio").foregroundStyle(.secondary)
                }
                AnalyzerPlotAxes(kind: .spectrum(sampleRate: model.snapshot.sampleRateHz))
            }
            .clipShape(RoundedRectangle(cornerRadius: 10))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func panel<Content: View>(_ title: String, subtitle: String,
                                      @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.title3.weight(.semibold))
                Text(subtitle).font(.caption).foregroundStyle(.secondary)
                    .lineLimit(1).minimumScaleFactor(0.85)
            }
            content()
        }
        .padding(14)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(LinearGradient(colors: [Color(red: 0.105, green: 0.14, blue: 0.165), Color(red: 0.065, green: 0.085, blue: 0.10)], startPoint: .topLeading, endPoint: .bottomTrailing))
        .overlay { RoundedRectangle(cornerRadius: 10).strokeBorder(.white.opacity(0.09), lineWidth: 1).allowsHitTesting(false) }
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
    private func loudnessSessionControls(_ loudness: AudioLoudnessMetrics) -> some View {
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
                .lineLimit(1).minimumScaleFactor(0.75)
            Spacer(minLength: 12)
            Text(value).font(.system(.caption, design: .monospaced))
                .multilineTextAlignment(.trailing).lineLimit(1).minimumScaleFactor(0.65)
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

    private func dbtpValue(_ measurement: AudioMeasurement<Float>) -> String {
        guard let value = measurement.value, value.isFinite else {
            return measurement.status == .unsupported ? "Unsupported" : "—"
        }
        return String(format: "%.1f dBTP", 20 * log10(max(value, 1.0e-12)))
    }

    private func dbValue(_ measurement: AudioMeasurement<Float>, unit: String) -> String {
        guard let value = measurement.value, value.isFinite else {
            switch measurement.status {
            case .unsupported: return "Unsupported"
            case .warmingUp: return "Warming up"
            case .idle: return "Idle"
            case .discontinuous: return "Discontinuous"
            case .valid: return "—"
            }
        }
        let prefix = measurement.status == .discontinuous ? "Hold " : ""
        return String(format: "%@%.1f %@", prefix, value, unit)
    }

    private var cancellationRiskText: String {
        switch model.metrics.analysis.stereo.cancellationRisk {
        case .insufficientSignal: "—"
        case .normal: "Low"
        case .risk: "Potential"
        }
    }

    private var phasePersistenceText: String {
        guard model.snapshot.sampleRateHz > 0,
              model.snapshot.activeRingFrames > 0 else { return "ring persistence" }
        let frames = AudioAnalyzerGeometry.goniometerWindowFrames(
            activeRingFrames: model.snapshot.activeRingFrames)
        let milliseconds = Double(frames)
            / Double(model.snapshot.sampleRateHz) * 1_000
        return String(format: "%.0f ms persistence", milliseconds)
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
