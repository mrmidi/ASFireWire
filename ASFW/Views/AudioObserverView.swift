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
                .frame(width: 230)
            }

            ZStack {
                RoundedRectangle(cornerRadius: 10)
                    .fill(Color.black.opacity(0.88))
                grid
                if model.snapshot.validHistoryFrames > 1 {
                    MetalAudioObserverView(client: model.client, mode: model.mode)
                        .clipShape(RoundedRectangle(cornerRadius: 10))
                } else {
                    ContentUnavailableView("Waiting for audio",
                                           systemImage: "waveform",
                                           description: Text("Start playback to see live samples."))
                }
            }
            .frame(height: 280)
            .accessibilityLabel(model.mode == .phaseScope ? "Stereo phase scope" : "Output waveform")

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

    private var grid: some View {
        Canvas { context, size in
            var lines = Path()
            for fraction in [0.25, 0.5, 0.75] {
                let x = size.width * fraction
                let y = size.height * fraction
                lines.move(to: CGPoint(x: x, y: 0))
                lines.addLine(to: CGPoint(x: x, y: size.height))
                lines.move(to: CGPoint(x: 0, y: y))
                lines.addLine(to: CGPoint(x: size.width, y: y))
            }
            context.stroke(lines, with: .color(.white.opacity(0.12)), lineWidth: 1)
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
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
