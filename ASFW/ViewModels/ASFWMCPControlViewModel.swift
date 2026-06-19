import Combine
import Foundation

@MainActor
final class ASFWMCPControlViewModel: ObservableObject {
    @Published var isEnabled: Bool
    @Published var portText: String
    @Published private(set) var status: ASFWMCPHostStatus = .stopped
    @Published private(set) var isChangingState = false
    @Published private(set) var lastError: String?

    private let connector: ASFWDriverConnector
    private let defaults: UserDefaults
    private var host: ASFWMCPHost<LiveASFWDriverControl>?

    private enum DefaultsKey {
        static let enabled = "asfw.mcp.enabled"
        static let port = "asfw.mcp.port"
    }

    init(connector: ASFWDriverConnector, defaults: UserDefaults = .standard) {
        self.connector = connector
        self.defaults = defaults
        let savedPort = defaults.integer(forKey: DefaultsKey.port)
        self.portText = savedPort > 0 ? "\(savedPort)" : "8765"
        self.isEnabled = defaults.bool(forKey: DefaultsKey.enabled)
    }

    var endpointText: String {
        status.endpointURL?.absoluteString ?? "Not running"
    }

    var sessionText: String {
        guard status.isRunning else { return "Stopped" }
        return status.activeHTTPConnections > 0 ? "Session active" : "Waiting for agent"
    }

    var canEditPort: Bool {
        status.isRunning == false && isChangingState == false
    }

    func applyEnabledState() {
        Task { await setEnabled(isEnabled) }
    }

    func setEnabled(_ enabled: Bool) async {
        defaults.set(enabled, forKey: DefaultsKey.enabled)
        isEnabled = enabled
        if enabled {
            await start()
        } else {
            await stop()
        }
    }

    func start() async {
        guard status.isRunning == false else { return }
        guard let port = UInt16(portText.trimmingCharacters(in: .whitespacesAndNewlines)) else {
            lastError = "Port must be a number from 0 to 65535."
            isEnabled = false
            defaults.set(false, forKey: DefaultsKey.enabled)
            return
        }

        isChangingState = true
        lastError = nil
        defaults.set(Int(port), forKey: DefaultsKey.port)

        let driver = LiveASFWDriverControl(backend: connector)
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)
        let nextHost = ASFWMCPHost(core: core)
        nextHost.onStatusChanged = { [weak self] status in
            self?.status = status
        }

        do {
            status = try await nextHost.start(configuration: ASFWMCPHostConfiguration(port: port))
            host = nextHost
        } catch {
            lastError = "Failed to start MCP host: \(error.localizedDescription)"
            status = .stopped
            host = nil
            isEnabled = false
            defaults.set(false, forKey: DefaultsKey.enabled)
        }

        isChangingState = false
    }

    func stop() async {
        isChangingState = true
        lastError = nil
        await host?.stop()
        host = nil
        status = .stopped
        isChangingState = false
    }
}
