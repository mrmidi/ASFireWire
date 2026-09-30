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
    private var lastWriteEndFrame: UInt64?
    private var lastWriteProgress = Date.distantPast

    init(guid: UInt64) {
        client = ASFWAudioObserverClient(guid: guid)
    }

    func run() async {
        while !Task.isCancelled {
            do {
                if !connected {
                    try client.open()
                    connected = true
                    status = "Observing the live output ring"
                }
                var current = try client.poll()
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
                if !current.ioRunning {
                    status = "Waiting for playback samples…"
                } else {
                    status = "Observing the live output ring"
                }
                metrics = client.metrics.read()
                try await Task.sleep(for: .milliseconds(17))
            } catch is CancellationError {
                break
            } catch {
                status = error.localizedDescription
                client.close()
                connected = false
                do {
                    try await Task.sleep(for: .milliseconds(500))
                } catch {
                    break
                }
            }
        }
        client.close()
        connected = false
    }
}

struct AudioObserverPanel: View {
    @StateObject private var model: AudioObserverPanelModel
    @State private var leftChannel: UInt32 = 0
    @State private var rightChannel: UInt32 = 1
    @State private var stereoSpectrum = true
    private let deviceName: String

    init(guid: UInt64, deviceName: String) {
        _model = StateObject(wrappedValue: AudioObserverPanelModel(guid: guid))
        self.deviceName = deviceName
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .firstTextBaseline) {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Audio Observer")
                        .font(.title2.bold())
                    Text(deviceName)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Picker("Display", selection: $model.mode) {
                    Text("Phase Scope").tag(AudioObserverDisplayMode.phaseScope)
                    Text("Waveform").tag(AudioObserverDisplayMode.waveform)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 230)
            }

            HStack {
                channelPicker("L / A", selection: $leftChannel)
                channelPicker("R / B", selection: $rightChannel)
                Spacer()
                Picker("Spectrum", selection: $stereoSpectrum) {
                    Text("Mono").tag(false)
                    Text("Stereo").tag(true)
                }
                .pickerStyle(.segmented)
                .frame(width: 160)
            }

            if model.mode == .phaseScope {
                HStack(alignment: .top, spacing: 16) {
                    VStack {
                        Text("Goniometer").font(.headline)
                        scopePlot.frame(width: 320, height: 320)
                    }
                    VStack {
                        Text("Spectrum · Hann · 2048 samples").font(.headline)
                        HStack(spacing: 12) {
                            spectrumPlot(channel: leftChannel)
                            if stereoSpectrum { spectrumPlot(channel: rightChannel) }
                        }
                        .frame(height: 320)
                    }
                    .frame(maxWidth: .infinity)
                }
            } else {
                scopePlot.frame(height: 280)
            }

            HStack(spacing: 8) {
                Circle()
                    .fill(model.snapshot.validHistoryFrames > 0 ? .green : .orange)
                    .frame(width: 7, height: 7)
                Text(model.status)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Text("L peak \(model.metrics.leftPeak, format: .percent.precision(.fractionLength(0)))")
                Text("R peak \(model.metrics.rightPeak, format: .percent.precision(.fractionLength(0)))")
                Text("Corr \(model.metrics.correlation, format: .number.precision(.fractionLength(2)))")
            }
            .font(.caption.monospacedDigit())

            HStack(spacing: 18) {
                metric("Active ring", "\(model.snapshot.activeRingFrames) frames")
                metric("Mapped", "\(model.snapshot.mappedFrames) frames")
                metric("Channels", "\(model.snapshot.channels)")
                metric("Write end", "\(model.snapshot.writeEndFrame)")
                metric("Epoch", "\(model.snapshot.sessionEpoch)/\(model.snapshot.discontinuityEpoch)")
            }

            Text("Goniometer timing / safety").font(.caption).foregroundStyle(.secondary)
            HStack(spacing: 18) {
                metric("CPU encode", milliseconds(model.metrics.cpuEncodeMilliseconds))
                metric("Queued → GPU", milliseconds(model.metrics.scheduledToStartMilliseconds))
                metric("GPU", milliseconds(model.metrics.gpuMilliseconds))
                metric("Completion", milliseconds(model.metrics.completionMilliseconds))
                metric("Sample age", milliseconds(model.metrics.sampleAgeMilliseconds))
                metric("Overwrite margin", milliseconds(model.metrics.overwriteMarginMilliseconds))
                metric("In flight", "\(model.metrics.inFlight)")
                metric("Wrap windows", "\(model.metrics.windowsCrossingWrap)")
                metric("Unsafe", "\(model.metrics.unsafeWindows)")
            }
            .font(.caption)
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

    private var scopePlot: some View {
        ZStack {
            Color(red: 0.025, green: 0.035, blue: 0.05)
            if model.snapshot.validHistoryFrames > 1 {
                MetalAudioObserverView(client: model.client, mode: model.mode,
                                       leftChannel: leftChannel, rightChannel: rightChannel)
                    .id("\(model.snapshot.memoryGeneration)-\(model.mode)-\(leftChannel)-\(rightChannel)")
                    .padding(model.mode == .phaseScope ? 30 : 0)
            } else {
                Text("Waiting for audio").foregroundStyle(.secondary)
            }
            if model.mode == .phaseScope {
                AnalyzerPlotAxes(kind: .goniometer)
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 10))
    }

    private func spectrumPlot(channel: UInt32) -> some View {
        VStack(spacing: 4) {
            Text("Output \(channel + 1)").font(.caption)
            ZStack {
                Color(red: 0.025, green: 0.035, blue: 0.05)
                if model.snapshot.validHistoryFrames >= 2048 {
                    MetalSpectrumView(client: model.client, channel: channel)
                        .id("\(model.snapshot.memoryGeneration)-\(channel)")
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

    private func metric(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).foregroundStyle(.secondary)
            Text(value).font(.system(.caption, design: .monospaced))
        }
    }

    private func milliseconds(_ value: Double?) -> String {
        guard let value else { return "—" }
        return String(format: "%.2f ms", value)
    }
}
