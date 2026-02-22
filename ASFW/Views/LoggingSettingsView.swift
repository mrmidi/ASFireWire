import SwiftUI

struct LoggingSettingsView: View {
    @ObservedObject var connector: ASFWDriverConnector
    @State private var asyncVerbosity: Double = 1.0
    @State private var isochTelemetryEnabled: Bool = false
    @State private var hexDumpsEnabled: Bool = false
    @State private var txVerifierEnabled: Bool = false
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
            
            if !connector.isConnected {
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
                        .disabled(!connector.isConnected || isLoading)
                    
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
                    .disabled(!connector.isConnected || isLoading)

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
                    .disabled(!connector.isConnected || isLoading)
                
                Text("Force enable/disable packet hex dumps independent of verbosity level")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .padding(.leading, 4)
            }
            .padding()
            .background(Color(nsColor: .controlBackgroundColor))
            .cornerRadius(8)

            // Dev TX Verifier / Recovery Toggle
            VStack(alignment: .leading, spacing: 8) {
                Toggle("Enable Isoch TX Verifier + Recovery (Dev)", isOn: $txVerifierEnabled)
                    .font(.headline)
                    .disabled(!connector.isConnected || isLoading)

                Text("Enables watchdog-driven TX verifier checks and recovery triggers. Intended for debugging, not production.")
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
                .disabled(!connector.isConnected || isLoading)
                
                Button("Apply") {
                    applySettings()
                }
                .disabled(!connector.isConnected || isLoading)
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
        guard connector.isConnected else { return }
        isLoading = true
        
        DispatchQueue.global(qos: .userInitiated).async {
            if let config = connector.getLogConfig() {
                DispatchQueue.main.async {
                    self.asyncVerbosity = Double(config.asyncVerbosity)
                    self.hexDumpsEnabled = config.hexDumpsEnabled
                    self.isochTelemetryEnabled = config.isochVerbosity >= 3
                    self.txVerifierEnabled = config.isochTxVerifierEnabled
                    self.isLoading = false
                }
            } else {
                DispatchQueue.main.async {
                    self.isLoading = false
                }
            }
        }
    }
    
    private func applySettings() {
        guard connector.isConnected else { return }
        isLoading = true
        
        DispatchQueue.global(qos: .userInitiated).async {
            _ = connector.setAsyncVerbosity(UInt32(asyncVerbosity))
            _ = connector.setIsochTelemetryLogging(enabled: isochTelemetryEnabled)
            _ = connector.setHexDumps(enabled: hexDumpsEnabled)
            _ = connector.setIsochTxVerifier(enabled: txVerifierEnabled)
            
            DispatchQueue.main.async {
                self.isLoading = false
                // Success/failure is already logged by the connector methods
            }
        }
    }
}

#Preview {
    LoggingSettingsView(connector: ASFWDriverConnector())
        .frame(width: 600, height: 500)
}
