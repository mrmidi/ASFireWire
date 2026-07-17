import Foundation
import Testing
@testable import ASFW

@MainActor
struct MCPControlViewModelTests {
    @Test func viewModelStartsStopsAndPersistsEnabledSettings() async throws {
        let suiteName = "ASFWTests.MCPControl.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defaults.removePersistentDomain(forName: suiteName)

        let viewModel = ASFWMCPControlViewModel(
            connector: ASFWDriverConnector(),
            defaults: defaults
        )
        viewModel.setGuardedFCPExperimentsEnabled(true)
        #expect(defaults.bool(forKey: "asfw.mcp.guarded-fcp-experiments-enabled"))
        viewModel.setGuardedFCPExperimentsEnabled(false)
        viewModel.portText = "0"

        await viewModel.setEnabled(true)
        defer {
            Task { @MainActor in
                await viewModel.stop()
                defaults.removePersistentDomain(forName: suiteName)
            }
        }

        #expect(viewModel.isEnabled == true)
        #expect(viewModel.status.isRunning == true)
        #expect(viewModel.endpointText.contains("http://127.0.0.1:"))
        #expect(viewModel.sessionText == "Waiting for agent")
        #expect(defaults.bool(forKey: "asfw.mcp.enabled") == true)

        await viewModel.setEnabled(false)
        #expect(viewModel.isEnabled == false)
        #expect(viewModel.status == .stopped)
    }
}
