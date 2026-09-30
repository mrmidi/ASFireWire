import SwiftUI
import SystemExtensions

// Activation host and observer for the lab dext. The Phase Scope tab maps the
// output ring read-only and renders directly from that mapping with Metal.

@main
struct ADKLabHostApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
                .frame(minWidth: 760, minHeight: 560)
        }
    }
}

struct ContentView: View {
    @StateObject private var manager = ExtensionManager()

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("ADKVirtualAudioLab Host")
                .font(.title2)
            Text("Dext: \(ExtensionManager.dextIdentifier)")
                .font(.caption)
                .textSelection(.enabled)

            HStack(spacing: 12) {
                Button("Activate") { manager.activate() }
                Button("Deactivate") { manager.deactivate() }
            }

            Text(manager.status)
                .font(.callout)
                .foregroundStyle(.secondary)
                .textSelection(.enabled)

            Text("After activation, the virtual device appears in Audio MIDI Setup. Choose it as an output and play a test tone to inspect its output ring.")
                .font(.caption)
                .foregroundStyle(.tertiary)

            Divider()

            TabView {
                Tab("Packets", systemImage: "waveform.path") {
                    PacketInspectorView()
                }
                Tab("Phase Scope", systemImage: "waveform.path.ecg") {
                    AudioWaveformView()
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .padding(20)
    }
}

final class ExtensionManager: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
    // Discovered from the embedded dext so signing lanes that override the
    // bundle identifier (BENCH.md Lane B) keep working without code edits.
    static let dextIdentifier: String = {
        let dir = Bundle.main.bundleURL
            .appendingPathComponent("Contents/Library/SystemExtensions")
        if let items = try? FileManager.default.contentsOfDirectory(
            at: dir, includingPropertiesForKeys: nil) {
            for url in items where url.pathExtension == "dext" {
                if let identifier = Bundle(url: url)?.bundleIdentifier {
                    return identifier
                }
            }
        }
        return "net.mrmidi.ASFW.ADKVirtualAudioLab"
    }()

    @Published var status = "Idle."

    func activate() {
        submit(OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: Self.dextIdentifier, queue: .main))
    }

    func deactivate() {
        submit(OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: Self.dextIdentifier, queue: .main))
    }

    private func submit(_ request: OSSystemExtensionRequest) {
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
        status = "Request submitted…"
    }

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties)
        -> OSSystemExtensionRequest.ReplacementAction {
        status = "Replacing \(existing.bundleShortVersion) with \(ext.bundleShortVersion)…"
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        status = "Needs approval in System Settings → General → Login Items & Extensions."
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        switch result {
        case .completed:
            status = "Completed. Check Audio MIDI Setup for the virtual device."
        case .willCompleteAfterReboot:
            status = "Will complete after reboot."
        @unknown default:
            status = "Finished with unknown result (\(result.rawValue))."
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFailWithError error: Error) {
        status = "Failed: \(error.localizedDescription)"
    }
}
