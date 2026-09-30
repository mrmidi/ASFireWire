import Observation
import SwiftUI

@MainActor
@Observable
final class AudioPhaseScopeModel {
    private(set) var client: AudioRingClient?
    private(set) var snapshot = AudioViewSnapshot()
    private(set) var metrics = ScopeMetricsSnapshot()
    private(set) var status = "Waiting to open the virtual device…"
    private(set) var failure = false

    func observe() async {
        if client == nil {
            do {
                let newClient = AudioRingClient()
                try newClient.open()
                client = newClient
                status = "Zero-copy Metal import succeeded. Start playback to see the phase scope."
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
                metrics = client.diagnostics.read()
                status = snapshot.ioRunning
                    ? "Live output ring"
                    : "Connected — waiting for CoreAudio playback"
            } catch {
                status = error.localizedDescription
                failure = true
                return
            }
            do {
                try await Task.sleep(nanoseconds: 16_666_667)
            } catch {
                return
            }
        }
    }
}

struct AudioPhaseScopeView: View {
    @State private var model = AudioPhaseScopeModel()

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

            Text("In phase → vertical · polarity reversed → horizontal · wider stereo → broader trace")
                .font(.caption)
                .foregroundStyle(.secondary)

            if let client = model.client,
               let buffer = client.metalBuffer,
               let renderPipeline = client.renderPipeline,
               let analysisPipeline = client.analysisPipeline,
               let analysisBuffer = client.analysisBuffer {
                MetalPhaseScopeView(client: client,
                                    buffer: buffer,
                                    renderPipeline: renderPipeline,
                                    analysisPipeline: analysisPipeline,
                                    analysisBuffer: analysisBuffer)
                    .frame(minHeight: 280)
                    .clipShape(.rect(cornerRadius: 10))
                    .accessibilityLabel("Live stereo phase scope for virtual device output")
            } else {
                ContentUnavailableView(
                    model.failure ? "Phase scope unavailable" : "Waiting for device",
                    systemImage: model.failure ? "exclamationmark.triangle" : "waveform.path.ecg",
                    description: Text(model.status))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(.black.opacity(0.12))
                    .clipShape(.rect(cornerRadius: 10))
            }

            HStack(spacing: 24) {
                metric("L peak", value: String(format: "%.3f", model.metrics.leftPeak))
                metric("R peak", value: String(format: "%.3f", model.metrics.rightPeak))
                metric("Correlation", value: String(format: "%+.3f", model.metrics.correlation))
                metric("Valid history", value: "\(model.snapshot.validHistoryFrames) frames")
                metric("Epoch", value: "\(model.snapshot.epoch)")
            }

            Text("GPU \(milliseconds(model.metrics.gpuMs))   age \(milliseconds(model.metrics.sampleAgeMs))   overwrite margin \(milliseconds(model.metrics.overwriteMarginMs))   inflight \(model.metrics.inFlight)")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)

            Text("CPU encode \(milliseconds(model.metrics.cpuEncodeMs))   queued→GPU \(milliseconds(model.metrics.scheduledToStartMs))   completion \(milliseconds(model.metrics.completionMs))   wrap windows \(model.metrics.windowsCrossingWrap)/\(model.metrics.windowsRendered)   bad \(model.metrics.badWindows)")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)

            Text("CPU/GPU exact sample checks: \(model.metrics.cpuGpuValidationChecks), mismatches: \(model.metrics.cpuGpuValidationMismatches)")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(model.metrics.cpuGpuValidationMismatches == 0
                                 ? Color.secondary : Color.red)

            HStack(spacing: 18) {
                metric("Mapped", value: "\(model.snapshot.mappedFrames) frames")
                metric("Active ring", value: "\(model.snapshot.activeRingFrames) frames")
                metric("Channels", value: "\(model.snapshot.channels)")
                metric("Write end", value: "\(model.snapshot.writeEndFrame)")
            }

            HStack(alignment: .firstTextBaseline, spacing: 10) {
                Text("Recent L / R")
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
        return zip(model.snapshot.lastLeftSamples, model.snapshot.lastRightSamples)
            .map { String(format: "%+.3f / %+.3f", $0, $1) }
            .joined(separator: "    ")
    }

    private func milliseconds(_ value: Double?) -> String {
        value.map { String(format: "%.3f ms", $0) } ?? "—"
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
