import SwiftUI
import SystemExtensions

// Minimal activation host for the lab dext (Milestone 3). A DriverKit
// extension can only be activated by an app that embeds it in
// Contents/Library/SystemExtensions — this app does exactly that and nothing
// else. All observation happens via the dext's IOLog output:
//   log stream --predicate 'sender == "net.mrmidi.ASFW.ADKVirtualAudioLab"'

@main
struct ADKLabHostApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
                .frame(minWidth: 420, minHeight: 220)
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

            Text("After activation, the virtual device appears in Audio MIDI Setup. Start playback at it, then stop — the dext dumps verifier and O/C counters at StopIO (see BENCH.md).")
                .font(.caption)
                .foregroundStyle(.tertiary)
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
