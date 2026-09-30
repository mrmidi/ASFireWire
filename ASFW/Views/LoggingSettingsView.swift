import SwiftUI

struct LoggingSettingsView: View {
    @ObservedObject var connectorObservable: ASFWDriverConnector.Observable
    @State private var asyncVerbosity: Double = 1.0
    @State private var isochTelemetryEnabled: Bool = false
    @State private var hexDumpsEnabled: Bool = false
    @State private var isLoading: Bool = false
    
    let verbosityLevels: [(Int, String, String)] = [
        (0, "Critical", "Errors, failures, timeouts only"),
        (1, "Compact", "One-line summaries, aggregate stats"),
        (2, "Transitions", "Key state changes"),
        (3, "Verbose", "All transitions, detailed flow"),
        (4, "Debug", "Hex dumps, buffer dumps, full diagnostics")
    ]
    
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            Text("Runtime Logging Configuration")
                .font(.title2)
                .fontWeight(.bold)
            
            if !connectorObservable.isConnected {
                Text("⚠️ Not connected to driver")
                    .foregroundColor(.orange)
                    .padding()
            }
            
            // Async Verbosity Slider
            VStack(alignment: .leading, spacing: 8) {
                Text("Async Subsystem Verbosity")
                    .font(.headline)
                
                HStack {
                    Slider(value: $asyncVerbosity, in: 0...4, step: 1)
                        .disabled(!connectorObservable.isConnected || isLoading)
                    
                    Text("\(Int(asyncVerbosity))")
                        .frame(width: 30)
                        .font(.system(.body, design: .monospaced))
                }
                
                if let level = verbosityLevels.first(where: { $0.0 == Int(asyncVerbosity) }) {
                    Text("**\(level.1):** \(level.2)")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .padding(.leading, 4)
                }
            }
            .padding()
            .background(Color(nsColor: .controlBackgroundColor))
            .cornerRadius(8)

            // Isoch Telemetry Toggle
            VStack(alignment: .leading, spacing: 8) {
                Toggle("Enable Isoch Telemetry Logs", isOn: $isochTelemetryEnabled)
                    .font(.headline)
                    .disabled(!connectorObservable.isConnected || isLoading)

                Text("Temporarily show/hide high-frequency Isoch logs (CycleCorr, RxStats, IT Poll, Audio IO/CLK).")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .padding(.leading, 4)
            }
            .padding()
            .background(Color(nsColor: .controlBackgroundColor))
            .cornerRadius(8)
            
            // Hex Dumps Toggle
            VStack(alignment: .leading, spacing: 8) {
                Toggle("Enable Hex Dumps", isOn: $hexDumpsEnabled)
                    .font(.headline)
                    .disabled(!connectorObservable.isConnected || isLoading)
                
                Text("Force enable/disable packet hex dumps independent of verbosity level")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .padding(.leading, 4)
            }
            .padding()
            .background(Color(nsColor: .controlBackgroundColor))
            .cornerRadius(8)

            // Action Buttons
            HStack(spacing: 12) {
                Button("Refresh") {
                    loadCurrentConfig()
                }
                .disabled(!connectorObservable.isConnected || isLoading)
                
                Button("Apply") {
                    applySettings()
                }
                .disabled(!connectorObservable.isConnected || isLoading)
                .buttonStyle(.borderedProminent)
            }
            
            Spacer()
        }
        .padding()
        .onAppear {
            loadCurrentConfig()
        }
    }
    
    private func loadCurrentConfig() {
        guard connectorObservable.isConnected else { return }
        isLoading = true
        
        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) {
            if let config = await connector.getLogConfig() {
                Task { @MainActor in
                    self.asyncVerbosity = Double(config.asyncVerbosity)
                    self.hexDumpsEnabled = config.hexDumpsEnabled
                    self.isochTelemetryEnabled = config.isochVerbosity >= 3
                    self.isLoading = false
                }
            } else {
                Task { @MainActor in
                    self.isLoading = false
                }
            }
        }
    }
    
    private func applySettings() {
        guard connectorObservable.isConnected else { return }
        isLoading = true
        
        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak connector] in
            if let connector {
                _ = await connector.setAsyncVerbosity(UInt32(asyncVerbosity))
                _ = await connector.setIsochTelemetryLogging(enabled: isochTelemetryEnabled)
                _ = await connector.setHexDumps(enabled: hexDumpsEnabled)
            }
            Task { @MainActor in
                self.isLoading = false
                // Success/failure is already logged by the connector methods
            }
        }
    }
}

#Preview {
    LoggingSettingsView(connectorObservable: ASFWDriverConnector.Observable())
        .frame(width: 600, height: 500)
}
