import SwiftUI

struct LoggingSettingsView: View {
    @ObservedObject var connector: ASFWDriverConnector
    var showAdvancedControls = false
    @State private var asyncVerbosity: UInt32 = 1
    @State private var isochVerbosity: UInt32 = 1
    @State private var hexDumpsEnabled = false
    @State private var isLoading = false
    @State private var showCustomSettings = false
    @State private var feedback: String?

    private let verbosityNames = ["Critical", "Compact", "Transitions", "Verbose", "Debug"]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                Text("Logging Settings").font(.title2.bold())
                Text("Use Normal for everyday use. Choose Detailed when investigating a problem, then copy or save a report from the Reports section.")
                    .foregroundStyle(.secondary)
                GroupBox("Logging level") {
                    VStack(alignment: .leading, spacing: 14) {
                        HStack {
                            presetButton("Normal", async: 1, isoch: 1, hex: false)
                            presetButton("Detailed", async: 4, isoch: 3, hex: true)
                        }
                        Text("Normal keeps logs concise. Detailed includes transport telemetry and packet contents.")
                            .font(.callout).foregroundStyle(.secondary)
                        Divider()
                        DisclosureGroup("Custom settings", isExpanded: $showCustomSettings) {
                            VStack(alignment: .leading, spacing: 14) {
                                Picker("Async logging", selection: $asyncVerbosity) {
                                    ForEach(0..<verbosityNames.count, id: \.self) { level in
                                        Text(verbosityNames[level]).tag(UInt32(level))
                                    }
                                }
                                .frame(maxWidth: 360)
                                Toggle("Transport telemetry", isOn: Binding(
                                    get: { isochVerbosity >= 3 },
                                    set: { isochVerbosity = $0 ? 3 : 1 }
                                ))
                                Text("Includes frequent audio and isochronous transport updates.")
                                    .font(.caption).foregroundStyle(.secondary)
                                Toggle("Packet hex dumps", isOn: $hexDumpsEnabled)
                            }
                            .padding(.top, 12)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
                }
                .disabled(!connector.isConnected || isLoading)
                HStack {
                    Button("Refresh", action: loadCurrentConfig)
                    Button("Apply", action: applySettings).buttonStyle(.borderedProminent)
                    if isLoading { ProgressView().controlSize(.small) }
                }
                .disabled(!connector.isConnected || isLoading)
                if !connector.isConnected {
                    Label("Connect to the driver to change logging settings.", systemImage: "info.circle")
                        .foregroundStyle(.secondary)
                } else if let feedback {
                    Text(feedback).font(.callout).foregroundStyle(.secondary)
                }
            }
            .frame(maxWidth: 760, alignment: .leading)
            .padding(24)
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .onAppear {
            showCustomSettings = showAdvancedControls
            loadCurrentConfig()
        }
        .onChange(of: connector.isConnected) { _, connected in
            if connected { loadCurrentConfig() }
        }
    }

    private func presetButton(_ title: String, async: UInt32, isoch: UInt32, hex: Bool) -> some View {
        Button {
            asyncVerbosity = async
            isochVerbosity = isoch
            hexDumpsEnabled = hex
            feedback = "Choose Apply to send these settings to the driver."
        } label: {
            Label(title, systemImage: asyncVerbosity == async && isochVerbosity == isoch && hexDumpsEnabled == hex ? "checkmark.circle.fill" : "circle")
                .frame(minWidth: 100)
        }
    }

    private func loadCurrentConfig() {
        guard connector.isConnected, !isLoading else { return }
        isLoading = true
        DispatchQueue.global(qos: .userInitiated).async {
            let config = connector.getLogConfig()
            DispatchQueue.main.async {
                isLoading = false
                if let config {
                    asyncVerbosity = config.asyncVerbosity
                    isochVerbosity = config.isochVerbosity
                    hexDumpsEnabled = config.hexDumpsEnabled
                    feedback = "Showing the driver's current settings."
                } else {
                    feedback = "Could not read logging settings. Try Refresh."
                }
            }
        }
    }

    private func applySettings() {
        guard connector.isConnected, !isLoading else { return }
        isLoading = true
        let requestedAsync = asyncVerbosity
        let requestedIsoch = isochVerbosity
        let requestedHex = hexDumpsEnabled
        DispatchQueue.global(qos: .userInitiated).async {
            let asyncOK = connector.setAsyncVerbosity(requestedAsync)
            let isochOK = connector.setIsochVerbosity(requestedIsoch)
            let hexOK = connector.setHexDumps(enabled: requestedHex)
            let actual = connector.getLogConfig()
            DispatchQueue.main.async {
                isLoading = false
                let matches = actual.map {
                    $0.asyncVerbosity == requestedAsync && $0.isochVerbosity == requestedIsoch && $0.hexDumpsEnabled == requestedHex
                } ?? false
                feedback = asyncOK && isochOK && hexOK && matches
                    ? "Logging settings applied."
                    : "Could not confirm all settings. Refresh and try again."
                if let actual {
                    asyncVerbosity = actual.asyncVerbosity
                    isochVerbosity = actual.isochVerbosity
                    hexDumpsEnabled = actual.hexDumpsEnabled
                }
            }
        }
    }
}
