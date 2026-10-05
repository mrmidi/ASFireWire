import Combine
import SwiftUI
import AppKit

@MainActor
final class AudioObserverPanelModel: ObservableObject {
    @Published private(set) var status = "Connecting to the ASFW output ring…"
    @Published private(set) var snapshot = AudioObserverSnapshot()
    private(set) var metrics = AudioObserverMetrics()
    // Numeric readouts are drawn by the panel canvases (AnalyzerMetalText);
    // only the loudness session controls still observe published metrics.
    let loudnessControlsUI = AnalyzerPanelUIState(section: .loudnessControls)

    let client: ASFWAudioObserverClient
    private var connected = false
    private var connectionGeneration: UInt64 = 0
    private var engine: AudioAnalysisEngine?
    private var leftChannel: UInt32 = 0
    private var rightChannel: UInt32 = 1
    private var routingGeneration: UInt64 = 0
    private var lastMetricsPublish = Date.distantPast
    private var lastScalarPublish = Date.distantPast
    private var publicationHz = 4.0
    private var lastPolicyRead = Date.distantPast
    private var lastCalibrationConfig: AnalyzerCalibrationConfig?

    init(guid: UInt64) {
        client = ASFWAudioObserverClient(guid: guid)
    }

    func run() async {
        while !Task.isCancelled {
            do {
                if !connected {
                    connectionGeneration &+= 1
                    let generation = connectionGeneration
                    try client.open()
                    guard let device = client.metalDevice,
                          let ringBuffer = client.ringBuffer,
                          let stateReader = client.stateReader else {
                        throw AudioObserverError.metalUnavailable
                    }
                    client.metrics.setCalibrationConfig(AnalyzerCalibrationState.shared.config)
                    engine = try await AudioAnalysisEngine.make(device: device,
                        ringBuffer: ringBuffer,
                        stateReader: stateReader,
                        metrics: client.metrics,
                        plotHistory: client.plotHistory)
                    await engine?.setCompletionHandler { [weak self] in
                        guard let self, self.connected, self.connectionGeneration == generation else { return }
                        self.publishCompletedAnalysis()
                    }
                    connected = true
                    status = "Observing the live output ring"
                }
                guard let engine else { throw AudioObserverError.pipelineFailed }
                await engine.setPair(AudioChannelPair(leftIndex: leftChannel, rightIndex: rightChannel,
                                                       generation: routingGeneration))
                let generation = connectionGeneration
                try await engine.run(renderState: client.renderState) { [weak self] _ in
                    guard let self, self.connected, self.connectionGeneration == generation else { return }
                    let current = self.client.renderState.read()
                    self.snapshot = current
                    self.status = current.ioRunning ? "Observing the live output ring" : "Waiting for playback samples…"
                    if !current.ioRunning {
                        let currentConfig = AnalyzerCalibrationState.shared.config
                        if self.lastCalibrationConfig != currentConfig {
                            self.lastCalibrationConfig = currentConfig
                            self.client.metrics.setCalibrationConfig(currentConfig)
                        }
                        self.metrics = self.client.metrics.read(includeHistory: false)
                        self.publishScalarPanels(self.metrics, snapshot: current)
                        self.client.renderSubmission.withFrame {
                            NotificationCenter.default.post(name: .asfwAnalysisCompleted, object: self.client.renderState)
                        }
                    }
                }
            } catch is CancellationError {
                break
            } catch {
                status = error.localizedDescription
                connected = false
                await engine?.stop()
                engine = nil
                client.close()
                do {
                    try await Task.sleep(for: .milliseconds(500))
                } catch {
                    break
                }
            }
        }
        connected = false
        await engine?.stop()
        engine = nil
        client.close()
        connected = false
    }

    private func publishCompletedAnalysis() {
        let now = Date()
        let currentConfig = AnalyzerCalibrationState.shared.config
        if lastCalibrationConfig != currentConfig {
            lastCalibrationConfig = currentConfig
            client.metrics.setCalibrationConfig(currentConfig)
        }
        client.renderSubmission.withFrame {
            NotificationCenter.default.post(name: .asfwAnalysisCompleted, object: client.renderState)
        }
        #if DEBUG
        if now.timeIntervalSince(lastPolicyRead) >= 1 {
            lastPolicyRead = now
            if let text = try? String(contentsOfFile: "/tmp/asfw-analyzer-ui-hz", encoding: .utf8),
               let hz = Double(text.trimmingCharacters(in: .whitespacesAndNewlines)), hz >= 0, hz <= 60 {
                publicationHz = hz
            } else { publicationHz = 4 }
        }
        #endif
        guard now.timeIntervalSince(lastMetricsPublish) >= (publicationHz > 0 ? 1 / publicationHz : 0.25) else { return }
        lastMetricsPublish = now
        metrics = client.metrics.read(includeHistory: false)
        if publicationHz > 0 && now.timeIntervalSince(lastScalarPublish) >= 1 / publicationHz {
            lastScalarPublish = now
            let current = client.renderState.read()
            loudnessControlsUI.publish(metrics, snapshot: current)
        }
    }

    func publishScalarPanels(_ metrics: AudioObserverMetrics, snapshot: AudioObserverSnapshot) {
        var scalars = metrics
        scalars.stereoHistory = []
        loudnessControlsUI.publish(scalars, snapshot: snapshot)
    }

    func setChannels(left: UInt32, right: UInt32) {
        guard left != leftChannel || right != rightChannel else { return }
        leftChannel = left
        rightChannel = right
        routingGeneration &+= 1
        let pair = AudioChannelPair(leftIndex: left, rightIndex: right, generation: routingGeneration)
        let engine = engine
        Task { await engine?.setPair(pair) }
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
    @ObservedObject private var themeState = AnalyzerThemeState.shared
    @ObservedObject private var calibrationState = AnalyzerCalibrationState.shared
    @State private var leftChannel: UInt32 = 0
    @State private var rightChannel: UInt32 = 1
    @State private var spectrumVisualization: SpectrumVisualization = .spectrum
    @State private var waterfallContours = false
    @State private var fftSize: UInt32 = 2048
    @State private var spectrumWindow: UInt32 = 0
    @State private var diagnosticTab = "GPU"
    @State private var gpuDetails = false
    @State private var gpuKernelTimings = false
    @State private var testSignal = "1 kHz Sine"
    @State private var testLevel = "−6 dBFS"
    @State private var testMode = "L = R (Mono)"
    @State private var midSide = false
    @State private var slowSpectrum = false
    @State private var peakHold = true
    @State private var isEditingOffset = false
    @State private var offsetInputText = ""
    @FocusState private var isOffsetFocused: Bool
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
        .background(themeState.mode.outerBackground)
        .preferredColorScheme(themeState.mode.isLight ? .light : .dark)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .task { await model.run() }
    }

    private var dashboardHeader: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Label(model.snapshot.ioRunning ? "Running" : "Waiting for audio", systemImage: "circle.fill")
                    .font(.caption.weight(.semibold)).foregroundStyle(model.snapshot.ioRunning ? .mint : .secondary)
                    .help(model.status)
                Text(deviceName).font(.caption).foregroundStyle(themeState.mode.secondaryTextColor)
                    .lineLimit(1)
            }
            Spacer(minLength: 8)
            if calibrationState.config.effectiveOffsetDB != 0.0 {
                calibrationBadge
            }
            themePicker
            channelPicker("L / A", selection: $leftChannel)
                .onChange(of: leftChannel) { _, value in model.setChannels(left: value, right: rightChannel) }
            channelPicker("R / B", selection: $rightChannel)
                .onChange(of: rightChannel) { _, value in model.setChannels(left: leftChannel, right: value) }
            Label(String(format: "%.1f kHz · %u ch", Double(model.snapshot.sampleRateHz) / 1_000,
                         model.snapshot.channels),
                  systemImage: model.snapshot.ioRunning ? "waveform" : "pause.circle")
                .font(.caption.monospacedDigit()).foregroundStyle(themeState.mode.secondaryTextColor)
                .fixedSize()
        }
    }

    private var calibrationBadge: some View {
        Button {
            diagnosticTab = "Calibration"
        } label: {
            HStack(spacing: 4) {
                Image(systemName: "tuningfork")
                Text(String(format: "Calibrated %+.1f dB", calibrationState.config.effectiveOffsetDB))
                    .fontWeight(.medium)
            }
            .font(.caption2)
            .padding(.horizontal, 6)
            .padding(.vertical, 3)
            .background(Color.orange.opacity(0.18))
            .foregroundStyle(Color.orange)
            .clipShape(Capsule())
            .overlay(Capsule().strokeBorder(Color.orange.opacity(0.4), lineWidth: 1))
        }
        .buttonStyle(.plain)
        .help("Metering calibrated by \(String(format: "%+.1f dB", calibrationState.config.effectiveOffsetDB)). Click to configure.")
    }

    private var themePicker: some View {
        Menu {
            ForEach(AnalyzerThemeMode.allCases) { mode in
                Button {
                    themeState.setMode(mode)
                } label: {
                    HStack {
                        Text(mode.rawValue)
                        if themeState.mode == mode {
                            Image(systemName: "checkmark")
                        }
                    }
                }
            }
        } label: {
            Label(themeState.mode.rawValue, systemImage: "paintpalette")
                .font(.caption)
                .foregroundStyle(themeState.mode.secondaryTextColor)
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
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
        panel("Monitor", subtitle: "Live levels and stereo summary") {
            StereoMetersView(client: model.client, active: model.snapshot.ioRunning)
                .frame(maxHeight: .infinity, alignment: .center)
            HStack(spacing: 8) {
                liveTile("L True Peak") { dbtpValue($0.analysis.levels.left.truePeak) }
                liveTile("R True Peak") { dbtpValue($0.analysis.levels.right.truePeak) }
                liveTile("Correlation") {
                    $0.correlationValid ? String(format: "%+.2f", $0.correlation) : "—"
                }
                liveTile("Side energy") { String(format: "%.1f%%", 100 * $0.meterValues[7]) }
            }.frame(height: 48)
        }
    }

    private var stereoPanel: some View {
        panel("Stereo", subtitle: "Goniometer · \(phasePersistenceText) · selected pair") {
            VStack(spacing: 8) {
                GeometryReader { geometry in
                    let scopeSide = min(CGFloat(230), min(geometry.size.height, geometry.size.width * 0.62))
                    HStack(spacing: 8) {
                        scopePlot(mode: .phaseScope).frame(width: scopeSide, height: scopeSide)
                        StereoHistoryView(client: model.client,
                            sampleRateHz: model.snapshot.sampleRateHz, active: model.snapshot.ioRunning)
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }.frame(maxWidth: .infinity, maxHeight: .infinity)
                }.frame(maxHeight: .infinity)
                HStack(spacing: 8) {
                    liveTile("Balance") { String(format: "%+.2f", $0.meterValues[6]) }
                    liveTile("Mono retention") { db($0.analysis.stereo.monoEnergyRetentionDB.value) }
                    liveTile("Side energy") { String(format: "%.1f%%", 100 * $0.meterValues[7]) }
                    liveTile("Mono cancellation") { metrics in
                        switch metrics.analysis.stereo.cancellationRisk {
                        case .risk: "Risk"
                        case .normal: "Low"
                        case .insufficientSignal: "—"
                        }
                    }
                }.frame(height: 48)
            }
        }
    }

    private func liveTile(_ title: String,
                          value: @escaping (AudioObserverMetrics) -> String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.caption).foregroundStyle(themeState.mode.secondaryTextColor)
            AnalyzerMetalText("tile.\(title)", style: .tileValue, template: "-00.0 dBTP",
                              alignment: .leading) { metrics, _ in value(metrics) }
        }
        .frame(maxWidth: .infinity, alignment: .leading).padding(8)
        .background(themeState.mode.isLight ? Color.black.opacity(0.04) : Color.white.opacity(0.04)).clipShape(RoundedRectangle(cornerRadius: 7))
    }

    private var spectrumPanel: some View {
        panel("Spectrum", subtitle: "Frequency domain analysis · stereo power / mid–side", spectrumSelector: true) {
            VStack(spacing: 7) {
                HStack(spacing: 8) {
                    Text("Basis")
                    Picker("Basis", selection: $midSide) {
                        Text("L/R").tag(false); Text("M/S").tag(true)
                    }
                    .labelsHidden().pickerStyle(.segmented).frame(width: 90)
                    Text("FFT Size")
                    Picker("FFT Size", selection: $fftSize) {
                        ForEach(SpectrumFFTLayout.sizes, id: \.self) { Text(String($0)).tag($0) }
                    }.labelsHidden().frame(width: 78)
                    Text("Window")
                    Picker("Window", selection: $spectrumWindow) {
                        Text("Hann").tag(UInt32(0)); Text("Hamming").tag(UInt32(1)); Text("Blackman").tag(UInt32(2))
                    }.labelsHidden().frame(width: 100)
                    Spacer(minLength: 0)
                }
                if !spectrumVisualization.usesHistory {
                    HStack(spacing: 8) {
                        Text("Average")
                        Picker("Average", selection: $slowSpectrum) {
                            Text("Fast · 150 ms").tag(false); Text("Slow · 1 s").tag(true)
                        }.labelsHidden().pickerStyle(.segmented).frame(width: 190)
                        Toggle("Peak hold · 2 s / 12 dB/s", isOn: $peakHold).toggleStyle(.checkbox)
                        Spacer(minLength: 0)
                    }
                }
                if spectrumVisualization == .waterfall {
                    HStack {
                        Text("Frequency → · Level ↑ · History ↗").foregroundStyle(.secondary)
                        Spacer(minLength: 0)
                        Toggle("Contours", isOn: $waterfallContours).toggleStyle(.checkbox)
                    }
                }
                HStack(spacing: 8) {
                    spectrumPlot(channel: leftChannel, side: false)
                    if midSide { spectrumPlot(channel: rightChannel, side: true) }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .overlayPreferenceValue(SpectrumPlotAnchors.self) { anchors in
                    GeometryReader { geometry in
                        if !anchors.isEmpty {
                            let regions = anchors.keys.sorted().compactMap { transform -> SpectrumPlotRegion? in
                                guard let anchor = anchors[transform] else { return nil }
                                return SpectrumPlotRegion(transform: transform, rect: geometry[anchor])
                            }
                            if spectrumVisualization.usesHistory {
                                MetalSpectrogramView(client: model.client, channel: leftChannel, otherChannel: rightChannel,
                                    fftSize: fftSize, window: spectrumWindow, regions: regions, waterfall: spectrumVisualization == .waterfall, wireOverlay: waterfallContours)
                                    .id("spectrogram-\(model.snapshot.memoryGeneration)-\(leftChannel)-\(rightChannel)-\(midSide)-\(fftSize)-\(spectrumWindow)")
                                    .allowsHitTesting(false).accessibilityHidden(true)
                            } else {
                                MetalSpectrumView(client: model.client, channel: leftChannel, otherChannel: rightChannel,
                                    slow: slowSpectrum, peakHold: peakHold, fftSize: fftSize, window: spectrumWindow, regions: regions)
                                    .id("\(model.snapshot.memoryGeneration)-\(leftChannel)-\(rightChannel)-\(midSide)-\(slowSpectrum)-\(peakHold)-\(fftSize)-\(spectrumWindow)")
                                    .allowsHitTesting(false).accessibilityHidden(true)
                            }
                        }
                    }
                }
                if spectrumVisualization.usesHistory {
                    HStack(spacing: 8) {
                        Text("−100 dBFS")
                        LinearGradient(stops: [
                            .init(color: Color(red: 0.025, green: 0.035, blue: 0.05), location: 0),
                            .init(color: Color(red: 0.08, green: 0.2, blue: 0.55), location: 0.33),
                            .init(color: Color(red: 0.1, green: 0.85, blue: 0.7), location: 0.66),
                            .init(color: Color(red: 1, green: 0.8, blue: 0.2), location: 0.85),
                            .init(color: Color(red: 1, green: 0.4, blue: 0.08), location: 0.93),
                            .init(color: Color(red: 1, green: 0.08, blue: 0.04), location: 1)
                        ], startPoint: .leading, endPoint: .trailing)
                            .frame(width: 100, height: 6).clipShape(Capsule())
                        Text("0 dBFS")
                        Spacer(minLength: 0)
                        Text(spectrumVisualization == .waterfall ? "Newest at front" : "New audio →")
                    }.font(.caption2).foregroundStyle(.secondary)
                } else {
                    HStack(spacing: 14) {
                        let isLight = themeState.mode.isLight
                        let mainColor: Color = isLight ? Color(red: 0.04, green: 0.48, blue: 0.68) : .mint
                        let sideColor: Color = isLight ? Color(red: 0.85, green: 0.28, blue: 0.05) : .orange
                        let peakColor: Color = isLight ? Color(red: 0.82, green: 0.45, blue: 0.05) : Color(red: 0.75, green: 0.55, blue: 0.22)
                        spectrumLegend(mainColor, midSide ? "Mid" : "Stereo power")
                        if midSide {
                            spectrumLegend(sideColor, midSide ? "Side" : "Output \(rightChannel + 1)")
                        }
                        if peakHold { spectrumLegend(peakColor, "Peak hold") }
                        Spacer(minLength: 0)
                    }
                    .font(.caption2)
                }
            }.font(.caption)
        }
    }

    private var loudnessPanel: some View {
        panel("Loudness", subtitle: "Perceived loudness and dynamics · EBU R128 / ITU-R BS.1770") {
            HStack(spacing: 10) {
                loudnessCard("Momentary", keyPath: \.momentaryLUFS)
                loudnessCard("Short-term", keyPath: \.shortTermLUFS)
                loudnessCard("Integrated", keyPath: \.integratedLUFS)
                VStack(spacing: 8) {
                    HStack(spacing: 8) {
                        VStack(alignment: .leading, spacing: 4) {
                            AnalyzerMetalText("lra.title", style: .caption, template: "LRA · provisional",
                                              tone: .secondary, alignment: .leading) { metrics, _ in
                                metrics.analysis.loudness.loudnessRangeIsProvisional ? "LRA · provisional" : "Loudness Range"
                            }
                            AnalyzerMetalText("lra.value", style: .tileValue, template: "00.0 LU",
                                              alignment: .leading) { metrics, _ in
                                dbValue(metrics.analysis.loudness.loudnessRangeLU, unit: "LU")
                            }
                        }.frame(maxWidth: .infinity, alignment: .leading).padding(8)
                            .background(themeState.mode.isLight ? Color.black.opacity(0.04) : Color.white.opacity(0.04)).clipShape(RoundedRectangle(cornerRadius: 7))
                        liveTile("True Peak") { dbtpText($0.analysis.loudness.maximumTruePeakDBTP) }
                    }
                    HStack(spacing: 8) {
                        liveTile("PLR") { dbValue($0.analysis.loudness.plrDB, unit: "dB") }
                        liveTile("Crest Factor") { dbValue($0.analysis.loudness.crestFactorDB, unit: "dB") }
                    }
                }.frame(maxWidth: .infinity)
            }.frame(height: 125)
            Text("Loudness History").font(.caption.weight(.medium))
                .foregroundStyle(themeState.mode.primaryTextColor)
            LoudnessHistoryView(client: model.client).frame(maxHeight: .infinity)
            HStack(spacing: 14) {
                let isLight = themeState.mode.isLight
                spectrumLegend(isLight ? Color(red: 0.08, green: 0.60, blue: 0.22) : .green, "Momentary")
                spectrumLegend(isLight ? Color(red: 0.08, green: 0.42, blue: 0.88) : .blue, "Short-term")
                spectrumLegend(isLight ? Color(red: 0.55, green: 0.15, blue: 0.85) : .purple, "Integrated")
                Spacer()
                AnalyzerLivePanel(state: model.loudnessControlsUI) { metrics, _ in
                    loudnessSessionControls(metrics.analysis.loudness)
                }
                AnalyzerMetalText("loudness.seconds", style: .caption, template: "0000.0 s",
                                  tone: .secondary, alignment: .trailing) { metrics, _ in
                    String(format: "%.1f s", Double(metrics.analysis.loudness.includedAudioFrames) / 48_000)
                }
            }.font(.caption)
        }
    }

    private func loudnessCard(_ title: String, keyPath: KeyPath<AudioLoudnessMetrics, AudioMeasurement<Float>>) -> some View {
        VStack(spacing: 7) {
            Text(title).font(.caption).foregroundStyle(themeState.mode.secondaryTextColor)
            AnalyzerMetalText("card.\(title)", style: .loudnessHero, template: "-00.0",
                              alignment: .center) { metrics, _ in
                let measurement = metrics.analysis.loudness[keyPath: keyPath]
                return measurement.value.map { $0.isFinite ? String(format: "%.1f", $0) : "−∞" } ?? "—"
            }
            AnalyzerMetalText("card.\(title).unit", style: .caption2, template: "Hold -00.0 LUFS",
                              tone: .secondary, alignment: .center) { metrics, _ in
                let measurement = metrics.analysis.loudness[keyPath: keyPath]
                return measurement.value == nil ? measurementText(measurement) : "LUFS"
            }
            AnalyzerCanvasSlot(mode: 4, index: title == "Momentary" ? 0 : title == "Short-term" ? 1 : 2)
                .frame(height: 5).background(themeState.mode.isLight ? Color.black.opacity(0.08) : Color.white.opacity(0.08)).clipShape(Capsule())
        }
        .padding(12).frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(themeState.mode.isLight ? Color.black.opacity(0.04) : Color.white.opacity(0.035)).clipShape(RoundedRectangle(cornerRadius: 8))
    }

    private var diagnosticsPanel: some View {
        panel("Diagnostics", subtitle: "Signal, performance and development tools") {
            Picker("Diagnostics", selection: $diagnosticTab) {
                ForEach(["Waveform", "Ring Buffer", "GPU", "Calibration", "Test Signal", "Log"], id: \.self) {
                    Text($0).tag($0)
                }
            }.labelsHidden().pickerStyle(.segmented)
            if diagnosticTab == "GPU" {
                gpuDiagnostics
            } else if diagnosticTab == "Ring Buffer" || diagnosticTab == "Waveform" {
                HStack(alignment: .top, spacing: 10) {
                    diagnosticCard("Ring Buffer") {
                        liveDiagnosticsRow("Active") { metrics, snapshot in "\(snapshot.activeRingFrames) frames" }
                        liveDiagnosticsRow("Mapped") { metrics, snapshot in "\(snapshot.mappedFrames) frames" }
                        liveDiagnosticsRow("Channels") { metrics, snapshot in "\(snapshot.channels)" }
                        liveDiagnosticsRow("Write end") { metrics, snapshot in "\(snapshot.writeEndFrame)" }
                        liveDiagnosticsRow("Epoch") { metrics, snapshot in "\(snapshot.sessionEpoch) / \(snapshot.discontinuityEpoch)" }
                    }
                    if diagnosticTab != "Waveform" {
                        diagnosticCard("Buffer safety") {
                            liveDiagnosticsRow("Sample age") { metrics, snapshot in milliseconds(metrics.sampleAgeMilliseconds) }
                            liveDiagnosticsRow("Overwrite margin") { metrics, snapshot in milliseconds(metrics.overwriteMarginMilliseconds) }
                            liveDiagnosticsRow("In flight") { metrics, snapshot in "\(metrics.inFlight)" }
                            liveDiagnosticsRow("Wrap / unsafe") { metrics, snapshot in "\(metrics.windowsCrossingWrap) / \(metrics.unsafeWindows)" }
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
                calibrationDiagnostics
            } else if diagnosticTab == "Log" {
                diagnosticCard("Observer status") {
                    Text(model.status).font(.caption)
                    liveDiagnosticsRow("Invalid samples") { metrics, snapshot in "\(metrics.invalidSampleCount)" }
                    liveDiagnosticsRow("Over-range L / R") { metrics, snapshot in "\(metrics.analysis.levels.left.overRangeSamples) / \(metrics.analysis.levels.right.overRangeSamples)" }
                    Text("Full driver logs are available in System Logs.").font(.caption).foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
            } else {
                Text("Generator controls are a preview. Audio generation is not implemented.")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer(minLength: 0)
            }
            if diagnosticTab != "GPU" && diagnosticTab != "Calibration" {
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

    private var calibrationDiagnostics: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .center, spacing: 12) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Observer Gain Calibration")
                        .font(.headline)
                        .foregroundStyle(themeState.mode.primaryTextColor)
                    Text("Compensates for post-DAW room correction attenuation (e.g. Sonarworks SoundID Reference) so true programme loudness is measured.")
                        .font(.caption)
                        .foregroundStyle(themeState.mode.secondaryTextColor)
                }
                Spacer()
                Toggle("Active", isOn: Binding(
                    get: { calibrationState.config.isEnabled },
                    set: { _ in calibrationState.toggleEnabled() }
                ))
                .toggleStyle(.switch)
            }
            .padding(.bottom, 2)

            HStack(spacing: 12) {
                diagnosticCard("Offset Control") {
                    HStack(spacing: 10) {
                        if isEditingOffset {
                            TextField("0.0", text: $offsetInputText)
                                .textFieldStyle(.plain)
                                .font(.system(.title2, design: .monospaced).weight(.bold))
                                .focused($isOffsetFocused)
                                .padding(.horizontal, 8)
                                .padding(.vertical, 4)
                                .frame(width: 100)
                                .background(themeState.mode.isLight ? Color.white : Color.black.opacity(0.4))
                                .clipShape(RoundedRectangle(cornerRadius: 6))
                                .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color.orange, lineWidth: 1.5))
                                .onSubmit { commitOffsetEdit() }
                                .onExitCommand { isEditingOffset = false; isOffsetFocused = false }
                                .onChange(of: isOffsetFocused) { _, focused in
                                    if !focused && isEditingOffset {
                                        commitOffsetEdit()
                                    }
                                }

                            Button("Done") { commitOffsetEdit() }
                                .buttonStyle(.borderedProminent)
                                .tint(.orange)
                                .controlSize(.small)
                        } else {
                            Button {
                                offsetInputText = String(format: "%+.1f", calibrationState.config.offsetDB)
                                isEditingOffset = true
                                isOffsetFocused = true
                            } label: {
                                HStack(spacing: 4) {
                                    Text(String(format: "%+.1f", calibrationState.config.offsetDB))
                                        .font(.system(size: 24, weight: .bold, design: .monospaced))
                                    Text("dB")
                                        .font(.system(.body, design: .monospaced).weight(.semibold))
                                        .foregroundStyle(themeState.mode.secondaryTextColor)
                                }
                                .foregroundStyle(calibrationState.config.isEnabled ? (calibrationState.config.effectiveOffsetDB != 0 ? Color.orange : themeState.mode.primaryTextColor) : themeState.mode.secondaryTextColor)
                                .padding(.horizontal, 10)
                                .padding(.vertical, 4)
                                .background(themeState.mode.isLight ? Color.black.opacity(0.05) : Color.white.opacity(0.06))
                                .clipShape(RoundedRectangle(cornerRadius: 6))
                            }
                            .buttonStyle(.plain)
                            .help("Click or double-click to enter calibration offset directly")

                            Stepper("", value: Binding(
                                get: { calibrationState.config.offsetDB },
                                set: { calibrationState.setOffset($0) }
                            ), in: AnalyzerCalibrationConfig.minimumOffsetDB...AnalyzerCalibrationConfig.maximumOffsetDB, step: 0.1)
                            .labelsHidden()
                            .disabled(!calibrationState.config.isEnabled)
                        }

                        Spacer()

                        Button("Reset (0 dB)") {
                            calibrationState.reset()
                        }
                        .disabled(calibrationState.config.offsetDB == 0.0)
                    }

                    HStack {
                        Text("Click number for manual entry · Stepper adjusts ±0.1 dB")
                            .font(.caption2)
                            .foregroundStyle(themeState.mode.secondaryTextColor)
                        Spacer()
                        Text(String(format: "Linear: ×%.3f · Energy: ×%.3f",
                                    calibrationState.config.linearGain,
                                    calibrationState.config.energyScale))
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(themeState.mode.secondaryTextColor)
                    }
                }

                diagnosticCard("Presets") {
                    LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 6) {
                        ForEach(AnalyzerCalibrationConfig.presets) { preset in
                            Button {
                                calibrationState.setOffset(preset.offsetDB)
                            } label: {
                                Text(preset.label)
                                    .font(.caption)
                                    .frame(maxWidth: .infinity)
                            }
                            .buttonStyle(.bordered)
                            .tint(calibrationState.config.offsetDB == preset.offsetDB && calibrationState.config.isEnabled ? .orange : nil)
                        }
                    }
                }
                .frame(maxWidth: 240)
            }

            HStack(spacing: 6) {
                Image(systemName: "info.circle")
                    .foregroundStyle(.secondary)
                Text("Non-destructive: audio samples passing through the driver to hardware are untouched.")
                    .font(.caption2)
                    .foregroundStyle(themeState.mode.secondaryTextColor)
            }
            Spacer(minLength: 0)
        }
        .padding(.top, 4)
        .frame(maxHeight: .infinity, alignment: .top)
    }

    private func commitOffsetEdit() {
        let cleaned = offsetInputText.replacingOccurrences(of: "dB", with: "", options: .caseInsensitive)
                                     .replacingOccurrences(of: "+", with: "")
                                     .trimmingCharacters(in: .whitespacesAndNewlines)
        if let val = Double(cleaned) {
            calibrationState.setOffset(val)
        }
        isEditingOffset = false
        isOffsetFocused = false
    }


    private var gpuDiagnostics: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .top, spacing: 10) {
                gpuTimingCard("Audio analysis", keyPath: \.analysisGPU) {
                    liveDiagnosticsRow("CPU load", interval: 1) { m, _ in
                        guard let a = m.analysisCPU.spanPercent, let v = m.visualEncode.spanPercent else { return "—" }
                        return String(format: "%.2f%% / core", a + v)
                    }
                    liveDiagnosticsRow("Frame budget", interval: 1) { m, _ in milliseconds(m.analysisGPU.frameBudgetMilliseconds) }
                    if gpuKernelTimings {
                        liveDiagnosticsRow("Reduction GPU", interval: 1) { m, _ in milliseconds(m.reductionGPU.mean) }
                        liveDiagnosticsRow("True peak GPU", interval: 1) { m, _ in milliseconds(m.truePeakGPU.mean) }
                        liveDiagnosticsRow("K-weight GPU", interval: 1) { m, _ in milliseconds(m.kWeightGPU.mean) }
                    }
                    if gpuDetails {
                        liveDiagnosticsRow("CPU encode avg", interval: 1) { m, _ in milliseconds(m.analysisCPU.mean) }
                        liveDiagnosticsRow("Scheduled → start", interval: 1) { m, _ in milliseconds(m.analysisQueue.mean) }
                        liveDiagnosticsRow("Batch", interval: 1) { m, _ in
                            guard m.analysisBatchSampleRate > 0 else { return "—" }
                            return String(format: "%llu / %.1f ms", m.analysisBatchFrames,
                                Double(m.analysisBatchFrames) / Double(m.analysisBatchSampleRate) * 1_000)
                    }
                    }
                }
                gpuTimingCard("FFT/STFT + plots", keyPath: \.visualGPU) {
                    liveDiagnosticsRow("CPU prep / encode", interval: 1) { m, _ in milliseconds(m.visualEncode.mean) }
                    liveDiagnosticsRow("Drawable acquire", interval: 1) { m, _ in milliseconds(m.visualDrawable.mean) }
                    if gpuDetails {
                        liveDiagnosticsRow("Frame prep elapsed", interval: 1) { m, _ in milliseconds(m.visualCPU.mean) }
                        liveDiagnosticsRow("CPU prep load", interval: 1) { m, _ in
                            m.visualEncode.spanPercent.map { String(format: "%.2f%% / core", $0) } ?? "—"
                    }
                    liveDiagnosticsRow("Commit → start", interval: 1) { m, _ in milliseconds(m.visualQueue.mean) }
                    }
                    Text("\(spectrumVisualization.rawValue) · FFT \(fftSize)").font(.caption2).foregroundStyle(.secondary)
                        .help("GPU timing includes FFT/STFT and all visible Metal panels.")
                }
                gpuTimingCard("Visuals ready", keyPath: \.analysisToVisual) {
                    liveDiagnosticsRow("GPU timeline load", interval: 1) { m, _ in
                        guard let a = m.analysisGPU.spanPercent, let v = m.visualGPU.spanPercent else { return "—" }
                        return String(format: "%.2f%% spans", a + v)
                    }
                    if gpuDetails {
                        liveDiagnosticsRow("GPU work / s", interval: 1) { m, _ in
                            guard let a = m.analysisGPU.millisecondsPerSecond,
                                  let v = m.visualGPU.millisecondsPerSecond else { return "—" }
                            return String(format: "%.1f ms/s", a + v)
                    }
                    liveDiagnosticsRow("FFT window", interval: 1) { [fftSize] m, _ in
                        guard m.analysisBatchSampleRate > 0 else { return "—" }
                        return milliseconds(Double(fftSize) / Double(m.analysisBatchSampleRate) * 1_000)
                    }
                    liveDiagnosticsRow("FFT center offset", interval: 1) { [fftSize] m, _ in
                        guard m.analysisBatchSampleRate > 0 else { return "—" }
                        return milliseconds(Double(fftSize) / Double(m.analysisBatchSampleRate) * 500)
                    }
                    }
                    Text("Encode → GPU finish").font(.caption2).foregroundStyle(.secondary)
                        .help("Analysis encode start → visual GPU finish. Excludes screen presentation and audio-window latency.")
                }
            }
            HStack {
                Button("Reset measurements") { model.client.metrics.resetGPUTiming() }
                Button("Copy measurements") { copyGPUMeasurements() }
                Spacer()
                Toggle("Kernel timings", isOn: $gpuKernelTimings).toggleStyle(.checkbox)
                    .onChange(of: gpuKernelTimings) { _, enabled in model.client.metrics.setKernelTimingEnabled(enabled) }
                    .help("Apple GPU hardware timestamps for compute passes. Adds profiling overhead; unsupported counters show —. Values average the previous second.")
                Toggle("Details", isOn: $gpuDetails).toggleStyle(.checkbox)
                Image(systemName: "info.circle").foregroundStyle(.secondary)
                    .help("Avg / fastest / longest: previous second. Run: since reset. CPU load estimates encode/prep wall time, excluding drawable acquisition; not process CPU utilization. GPU load sums overlapping spans, not utilization. FFT window is not playback latency.")
            }
        }.frame(maxHeight: .infinity, alignment: .top)
    }

    private func copyGPUMeasurements() {
        let m = model.client.metrics.read(includeHistory: false)
        let stages: [(String, AnalyzerTimingStatistics)] = [
            ("Audio GPU", m.analysisGPU), ("Reduction GPU", m.reductionGPU),
            ("True peak GPU", m.truePeakGPU), ("K-weighting GPU", m.kWeightGPU), ("Visual GPU (FFT/STFT + plots)", m.visualGPU),
            ("Analysis encode → visual GPU finish", m.analysisToVisual),
            ("Analysis CPU encode", m.analysisCPU), ("Visual frame elapsed", m.visualCPU),
            ("Visual drawable acquisition", m.visualDrawable), ("Visual prep/encode excluding drawable", m.visualEncode)]
        let lines = stages.map { name, stats in
            "\(name): run avg \(milliseconds(stats.runMean)), fastest \(milliseconds(stats.runFastest)), longest \(milliseconds(stats.runPeak)); \(stats.count) completions; latest 1s avg \(milliseconds(stats.mean))"
        }
        let report = (["ASFW GPU measurements · \(spectrumVisualization.rawValue) · FFT \(fftSize) · \(m.analysisBatchSampleRate) Hz",
                       "Batch: \(m.analysisBatchFrames) audio frames. Run statistics are since reset."] + lines +
                      ["GPU spans may overlap. Chain excludes display presentation and audio-window latency."]).joined(separator: "\n")
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(report, forType: .string)
    }

    private func gpuTimingCard<Content: View>(_ title: String,
        keyPath: KeyPath<AudioObserverMetrics, AnalyzerTimingStatistics>,
        @ViewBuilder content: () -> Content) -> some View {
        diagnosticCard(title) {
            liveDiagnosticsRow("Avg · 1 s", id: "\(title).mean", interval: 1) { m, _ in milliseconds(m[keyPath: keyPath].mean) }
            liveDiagnosticsRow("Fastest · 1 s", id: "\(title).fastest", interval: 1) { m, _ in milliseconds(m[keyPath: keyPath].fastest) }
            liveDiagnosticsRow("Longest · 1 s", id: "\(title).peak", interval: 1) { m, _ in milliseconds(m[keyPath: keyPath].peak) }
            Divider()
            if gpuDetails {
                liveDiagnosticsRow("Run avg", id: "\(title).runMean", interval: 1) { m, _ in milliseconds(m[keyPath: keyPath].runMean) }
                liveDiagnosticsRow("Run fastest", id: "\(title).runFastest", interval: 1) { m, _ in milliseconds(m[keyPath: keyPath].runFastest) }
                liveDiagnosticsRow("Run longest", id: "\(title).runPeak", interval: 1) { m, _ in milliseconds(m[keyPath: keyPath].runPeak) }
                liveDiagnosticsRow("Completed", id: "\(title).count", interval: 1) { m, _ in "\(m[keyPath: keyPath].count)" }
                liveDiagnosticsRow("Rate · 1 s", id: "\(title).rate", interval: 1) { m, _ in
                    m[keyPath: keyPath].rate.map { String(format: "%.0f Hz", $0) } ?? "—"
            }
            Divider()
            }
            content()
        }
    }

    private func liveDiagnosticsRow(_ title: String, id: String? = nil,
        interval: Double = AnalyzerTextSpec.diagnostics,
        value: @escaping (AudioObserverMetrics, AudioObserverSnapshot) -> String) -> some View {
        HStack {
            Text(title).foregroundStyle(themeState.mode.secondaryTextColor).lineLimit(1).minimumScaleFactor(0.75)
            Spacer(minLength: 12)
            AnalyzerMetalText("diag.\(id ?? title)", style: .captionMono, template: "000000000000",
                              alignment: .trailing, interval: interval, format: value)
        }
    }

    private func diagnosticCard<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            Text(title).font(.caption.weight(.semibold))
                .foregroundStyle(themeState.mode.primaryTextColor)
            content()
        }.padding(10).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .background(themeState.mode.isLight ? Color.black.opacity(0.04) : Color.black.opacity(0.12))
            .overlay { RoundedRectangle(cornerRadius: 8).strokeBorder(themeState.mode.cardBorder) }
            .clipShape(RoundedRectangle(cornerRadius: 8))
    }

    private func spectrumLegend(_ color: Color, _ title: String) -> some View {
        HStack(spacing: 4) {
            Capsule().fill(color).frame(width: 10, height: 4)
            Text(title).foregroundStyle(themeState.mode.secondaryTextColor)
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
        let isPhaseScope = mode == .phaseScope
        return ZStack {
            if isPhaseScope {
                Color(red: 0.025, green: 0.035, blue: 0.05)
            } else {
                themeState.mode.plotBackground
            }
            if model.snapshot.ioRunning {
                if isPhaseScope {
                    AnalyzerCanvasSlot(mode: 5, index: leftChannel, otherChannel: rightChannel).padding(30)
                } else {
                    MetalAudioObserverView(client: model.client, mode: mode,
                                           leftChannel: leftChannel, rightChannel: rightChannel)
                        .id("\(model.snapshot.memoryGeneration)-\(mode)-\(leftChannel)-\(rightChannel)-\(themeState.mode.rawValue)")
                }
            } else {
                Text("Waiting for audio").foregroundStyle(isPhaseScope ? Color.gray : themeState.mode.secondaryTextColor)
            }
            if isPhaseScope {
                AnalyzerPlotAxes(kind: .goniometer)
            } else {
                AnalyzerPlotAxes(kind: .waveform)
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(isPhaseScope ? Color.white.opacity(0.12) : themeState.mode.plotBorder, lineWidth: 1))
    }

    private var historyPlotAxes: AnalyzerPlotAxes.Kind {
        let rate = model.snapshot.sampleRateHz
        let seconds = SpectrogramTimeline.duration(sampleRate: rate == 0 ? 48000 : rate)
        switch spectrumVisualization {
        case .spectrum: return .spectrum(sampleRate: rate)
        case .spectrogram: return .spectrogram(sampleRate: rate, seconds: seconds)
        case .waterfall: return .waterfall(sampleRate: rate, seconds: seconds)
        }
    }

    private func spectrumPlot(channel: UInt32, side: Bool) -> some View {
        let usesHistory3D = spectrumVisualization.usesHistory
        return VStack(spacing: 4) {
            Text(midSide ? (side ? "Side · (L−R)/√2" : "Mid · (L+R)/√2") : "L/R · averaged channel power")
                .font(.caption2)
                .foregroundStyle(themeState.mode.secondaryTextColor)
            ZStack {
                if usesHistory3D {
                    Color(red: 0.025, green: 0.035, blue: 0.05)
                } else {
                    themeState.mode.plotBackground
                }
                if model.snapshot.ioRunning {
                    SpectrumCanvasSlot(transform: midSide ? (side ? 2 : 1) : 3)
                        .padding(.leading, 38).padding(.trailing, 12)
                        .padding(.top, 12).padding(.bottom, 30)
                } else {
                    Text("Waiting for audio").foregroundStyle(usesHistory3D ? Color.gray : themeState.mode.secondaryTextColor)
                }
                AnalyzerPlotAxes(kind: historyPlotAxes)
            }
            .clipShape(RoundedRectangle(cornerRadius: 10))
            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(usesHistory3D ? Color.white.opacity(0.12) : themeState.mode.plotBorder, lineWidth: 1))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func panel<Content: View>(_ title: String, subtitle: String, spectrumSelector: Bool = false,
                                      @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.title3.weight(.semibold))
                        .foregroundStyle(themeState.mode.primaryTextColor)
                    Text(subtitle).font(.caption).foregroundStyle(themeState.mode.secondaryTextColor)
                        .lineLimit(1).minimumScaleFactor(0.85)
                }
                if spectrumSelector {
                    Spacer(minLength: 8)
                    Picker("Visualization", selection: $spectrumVisualization) {
                        ForEach(SpectrumVisualization.allCases, id: \.self) { mode in
                            Text(mode.rawValue).tag(mode)
                        }
                    }.labelsHidden().pickerStyle(.menu).fixedSize()
                }
            }
            content()
        }
        .analyzerCanvas(client: model.client)
        .padding(14)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(LinearGradient(colors: [themeState.mode.cardBackgroundTop, themeState.mode.cardBackgroundBottom], startPoint: .topLeading, endPoint: .bottomTrailing))
        .overlay { RoundedRectangle(cornerRadius: 10).strokeBorder(themeState.mode.cardBorder, lineWidth: 1).allowsHitTesting(false) }
        .clipShape(RoundedRectangle(cornerRadius: 10))
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
            Text(title).foregroundStyle(themeState.mode.secondaryTextColor)
                .lineLimit(1).minimumScaleFactor(0.75)
            Spacer(minLength: 12)
            Text(value).font(.system(.caption, design: .monospaced))
                .foregroundStyle(themeState.mode.primaryTextColor)
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
