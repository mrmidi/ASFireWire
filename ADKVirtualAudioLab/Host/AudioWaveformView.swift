import Observation
import SwiftUI

@MainActor
@Observable
final class AudioWaveformModel {
    private(set) var client: AudioRingClient?
    private(set) var snapshot = AudioViewSnapshot()
    private(set) var status = "Waiting to open the virtual device…"
    private(set) var failure = false

    func observe() async {
        if client == nil {
            do {
                let newClient = AudioRingClient()
                try newClient.open()
                client = newClient
                status = "Zero-copy Metal import succeeded. Start playback to see the waveform."
                failure = false
            } catch {
                status = error.localizedDescription
                failure = true
                return
            }
        }

        guard let client else { return }
        while !Task.isCancelled {
            do {
                snapshot = try client.poll()
                status = snapshot.ioRunning
                    ? "Live output ring"
                    : "Connected — waiting for CoreAudio playback"
            } catch {
                status = error.localizedDescription
                failure = true
                return
            }

            do {
                try await Task.sleep(nanoseconds: 33_333_333)
            } catch {
                return
            }
        }
    }
}

struct AudioWaveformView: View {
    @State private var model = AudioWaveformModel()

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .firstTextBaseline) {
                Text("Virtual device output")
                    .font(.title3.weight(.semibold))
                Spacer()
                Label(model.snapshot.ioRunning ? "Running" : "Stopped",
                      systemImage: model.snapshot.ioRunning ? "waveform" : "pause.circle")
                    .foregroundStyle(model.snapshot.ioRunning ? .green : .secondary)
            }

            Text(model.status)
                .font(.callout)
                .foregroundStyle(model.failure ? .red : .secondary)
                .textSelection(.enabled)

            if let client = model.client,
               let buffer = client.metalBuffer,
               let pipeline = client.renderPipeline {
                MetalWaveformView(client: client, buffer: buffer, pipeline: pipeline)
                    .frame(minHeight: 240)
                    .clipShape(.rect(cornerRadius: 10))
                    .accessibilityLabel("Live waveform for virtual device output channel zero")
            } else {
                ContentUnavailableView(
                    model.failure ? "Waveform unavailable" : "Waiting for device",
                    systemImage: model.failure ? "exclamationmark.triangle" : "waveform",
                    description: Text(model.status))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(.black.opacity(0.12))
                    .clipShape(.rect(cornerRadius: 10))
            }

            HStack(spacing: 24) {
                metric("Mapped", value: "\(model.snapshot.mappedFrames) frames")
                metric("Active ring", value: "\(model.snapshot.activeRingFrames) frames")
                metric("Channels", value: "\(model.snapshot.channels)")
                metric("Write end", value: "\(model.snapshot.writeEndFrame)")
            }

            HStack(alignment: .firstTextBaseline, spacing: 10) {
                Text("Last ch 0 samples")
                    .foregroundStyle(.secondary)
                Text(sampleLine)
                    .font(.system(.callout, design: .monospaced))
                    .textSelection(.enabled)
            }
        }
        .padding(16)
        .task { await model.observe() }
    }

    private var sampleLine: String {
        guard !model.snapshot.lastLeftSamples.isEmpty else { return "—" }
        return model.snapshot.lastLeftSamples
            .map { String(format: "%+.3f", $0) }
            .joined(separator: "   ")
    }

    private func metric(_ title: String, value: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title.uppercased())
                .font(.caption2.weight(.semibold))
                .foregroundStyle(.secondary)
            Text(value)
                .font(.system(.caption, design: .monospaced))
                .textSelection(.enabled)
        }
    }
}
