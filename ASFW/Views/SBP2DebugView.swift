import SwiftUI

struct SBP2DebugView: View {
    @ObservedObject var viewModel: SBP2DebugViewModel

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            content
        }
        .navigationTitle("SBP-2 Debug")
        .onAppear {
            viewModel.refreshDevices()
        }
    }

    private var header: some View {
        HStack {
            Text("SBP-2 Debug")
                .font(.title2.bold())

            Spacer()

            if let lastRefresh = viewModel.lastDeviceRefresh {
                Text("Devices: \(lastRefresh, style: .time)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if let lastStateRefresh = viewModel.lastStateRefresh {
                Text("State: \(lastStateRefresh, style: .time)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Button {
                viewModel.refreshDevices()
            } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
            }
            .buttonStyle(.borderedProminent)
            .disabled(!viewModel.isConnected || viewModel.isLoadingDevices)
        }
        .padding()
    }

    @ViewBuilder
    private var content: some View {
        if !viewModel.isConnected {
            ContentUnavailableView(
                "Driver Not Connected",
                systemImage: "cable.connector.slash",
                description: Text("Connect to the driver to debug SBP-2 sessions.")
            )
        } else if viewModel.sbp2Devices.isEmpty && !viewModel.isLoadingDevices {
            ContentUnavailableView(
                "No SBP-2 Devices",
                systemImage: "externaldrive.badge.questionmark",
                description: Text("Refresh after attaching a FireWire SBP-2 target.")
            )
        } else {
            HSplitView {
                List(viewModel.sbp2Devices, selection: $viewModel.selectedDeviceID) { device in
                    SBP2DeviceRow(device: device)
                        .tag(device.guid)
                }
                .frame(minWidth: 240, idealWidth: 280, maxWidth: 320)

                if let device = viewModel.selectedDevice, let unit = viewModel.selectedUnit {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 16) {
                            devicePanel(device: device, unit: unit)
                            sessionPanel
                            probePanel
                            rawCommandPanel
                            resultPanel
                            statusPanel
                        }
                        .padding()
                    }
                } else {
                    ContentUnavailableView(
                        "Select an SBP-2 Device",
                        systemImage: "sidebar.left",
                        description: Text("Choose an SBP-2 device and unit to create a session.")
                    )
                }
            }
        }
    }

    private func devicePanel(device: ASFWDriverConnector.FWDeviceInfo,
                             unit: ASFWDriverConnector.FWUnitInfo) -> some View {
        GroupBox("Selected Device") {
            VStack(alignment: .leading, spacing: 12) {
                Text(deviceTitle(device))
                    .font(.title3.weight(.semibold))

                Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                    GridRow {
                        Text("GUID:")
                            .foregroundStyle(.secondary)
                        Text(String(format: "0x%016llX", device.guid))
                            .monospaced()
                    }
                    GridRow {
                        Text("Node:")
                            .foregroundStyle(.secondary)
                        Text(String(format: "%u", device.nodeId))
                            .monospaced()
                    }
                    GridRow {
                        Text("Generation:")
                            .foregroundStyle(.secondary)
                        Text(String(format: "%u", device.generation))
                            .monospaced()
                    }
                    GridRow {
                        Text("Units:")
                            .foregroundStyle(.secondary)
                        Text("\(device.sbp2Units.count)")
                    }
                }

                Picker("SBP-2 Unit", selection: $viewModel.selectedUnitROMOffset) {
                    ForEach(device.sbp2Units) { listedUnit in
                        Text(unitLabel(listedUnit))
                            .tag(Optional(listedUnit.romOffset))
                    }
                }
                .pickerStyle(.menu)
                .frame(maxWidth: 420, alignment: .leading)

                Divider()

                Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                    GridRow {
                        Text("ROM Offset:")
                            .foregroundStyle(.secondary)
                        Text(String(format: "%u", unit.romOffset))
                            .monospaced()
                    }
                    GridRow {
                        Text("Spec ID:")
                            .foregroundStyle(.secondary)
                        Text(unit.specIdHex)
                            .monospaced()
                    }
                    GridRow {
                        Text("LUN:")
                            .foregroundStyle(.secondary)
                        Text(unit.lun.map { String(format: "0x%02X", $0) } ?? "n/a")
                            .monospaced()
                    }
                    GridRow {
                        Text("Mgmt Agent:")
                            .foregroundStyle(.secondary)
                        Text(unit.managementAgentOffset.map { String(format: "0x%08X", $0) } ?? "n/a")
                            .monospaced()
                    }
                    GridRow {
                        Text("Unit Chars:")
                            .foregroundStyle(.secondary)
                        Text(unit.unitCharacteristics.map { String(format: "0x%08X", $0) } ?? "n/a")
                            .monospaced()
                    }
                    GridRow {
                        Text("Fast Start:")
                            .foregroundStyle(.secondary)
                        Text(unit.fastStart.map { String(format: "0x%08X", $0) } ?? "n/a")
                            .monospaced()
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding()
        }
    }

    private var sessionPanel: some View {
        GroupBox("Session") {
            VStack(alignment: .leading, spacing: 12) {
                HStack(spacing: 12) {
                    Button("Create Session") {
                        viewModel.createSession()
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!viewModel.hasSelection || viewModel.isBusy)

                    Button("Start Login") {
                        viewModel.startLogin()
                    }
                    .buttonStyle(.bordered)
                    .disabled(viewModel.sessionHandle == nil || viewModel.isBusy)

                    Button("Release") {
                        viewModel.releaseSession()
                    }
                    .buttonStyle(.bordered)
                    .disabled(viewModel.sessionHandle == nil)
                }

                if let handle = viewModel.sessionHandle {
                    Text(String(format: "Handle: 0x%llX", handle))
                        .font(.callout)
                        .monospaced()
                }

                if let state = viewModel.sessionState {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                        GridRow {
                            Text("State:")
                                .foregroundStyle(.secondary)
                            Text(state.loginStateDescription)
                        }
                        GridRow {
                            Text("Login ID:")
                                .foregroundStyle(.secondary)
                            Text(String(format: "0x%04X", state.loginID))
                                .monospaced()
                        }
                        GridRow {
                            Text("Generation:")
                                .foregroundStyle(.secondary)
                            Text(String(format: "%u", state.generation))
                                .monospaced()
                        }
                        GridRow {
                            Text("Last Error:")
                                .foregroundStyle(.secondary)
                            Text("\(state.lastError)")
                                .monospaced()
                        }
                        GridRow {
                            Text("Reconnect Pending:")
                                .foregroundStyle(.secondary)
                            Text(state.reconnectPending ? "Yes" : "No")
                        }
                    }
                } else {
                    Text("No active session.")
                        .foregroundStyle(.secondary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding()
        }
    }

    private var probePanel: some View {
        GroupBox("Standard Probes") {
            VStack(alignment: .leading, spacing: 12) {
                HStack(spacing: 12) {
                    Button("INQUIRY") {
                        viewModel.runInquiry()
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(viewModel.sessionState?.isLoggedIn != true || viewModel.isBusy)

                    Button("TEST UNIT READY") {
                        viewModel.runTestUnitReady()
                    }
                    .buttonStyle(.bordered)
                    .disabled(viewModel.sessionState?.isLoggedIn != true || viewModel.isBusy)

                    Button("REQUEST SENSE") {
                        viewModel.runRequestSense()
                    }
                    .buttonStyle(.bordered)
                    .disabled(viewModel.sessionState?.isLoggedIn != true || viewModel.isBusy)
                }

                if let summary = viewModel.inquirySummary {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                        GridRow {
                            Text("Vendor:")
                                .foregroundStyle(.secondary)
                            Text(summary.vendor.isEmpty ? "?" : summary.vendor)
                        }
                        GridRow {
                            Text("Product:")
                                .foregroundStyle(.secondary)
                            Text(summary.product.isEmpty ? "?" : summary.product)
                        }
                        GridRow {
                            Text("Revision:")
                                .foregroundStyle(.secondary)
                            Text(summary.revision.isEmpty ? "?" : summary.revision)
                        }
                    }
                }

                if let sense = viewModel.senseSummary {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                        GridRow {
                            Text("Sense Key:")
                                .foregroundStyle(.secondary)
                            Text(String(format: "0x%02X", sense.senseKey))
                                .monospaced()
                        }
                        GridRow {
                            Text("ASC/ASCQ:")
                                .foregroundStyle(.secondary)
                            Text(String(format: "0x%02X / 0x%02X", sense.asc, sense.ascq))
                                .monospaced()
                        }
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding()
        }
    }

    private var rawCommandPanel: some View {
        GroupBox("Raw CDB") {
            VStack(alignment: .leading, spacing: 12) {
                TextField("CDB hex", text: $viewModel.rawCDBHex)
                    .textFieldStyle(.roundedBorder)

                HStack(spacing: 12) {
                    Picker("Direction", selection: $viewModel.rawDirection) {
                        ForEach(SBP2CommandDataDirection.allCases) { direction in
                            Text(direction.displayName).tag(direction)
                        }
                    }
                    .pickerStyle(.segmented)

                    TextField("Transfer", text: $viewModel.rawTransferLength)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 100)
                }

                TextField("Outgoing payload hex", text: $viewModel.rawOutgoingHex)
                    .textFieldStyle(.roundedBorder)

                Button("Send Raw CDB") {
                    viewModel.runRawCommand()
                }
                .buttonStyle(.borderedProminent)
                .disabled(viewModel.sessionState?.isLoggedIn != true || viewModel.isBusy)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding()
        }
    }

    private var resultPanel: some View {
        GroupBox("Last Result") {
            VStack(alignment: .leading, spacing: 12) {
                if let commandName = viewModel.lastCommandName,
                   let result = viewModel.commandResult {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                        GridRow {
                            Text("Command:")
                                .foregroundStyle(.secondary)
                            Text(commandName)
                        }
                        GridRow {
                            Text("Transport:")
                                .foregroundStyle(.secondary)
                            Text("\(result.transportStatus)")
                                .monospaced()
                        }
                        GridRow {
                            Text("SBP Status:")
                                .foregroundStyle(.secondary)
                            Text(String(format: "0x%02X", result.sbpStatus))
                                .monospaced()
                        }
                    }

                    if !result.payload.isEmpty {
                        VStack(alignment: .leading, spacing: 6) {
                            Text("Payload")
                                .font(.headline)
                            Text(hexDump(result.payload))
                                .font(.system(.caption, design: .monospaced))
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(12)
                                .background(Color.secondary.opacity(0.08))
                                .cornerRadius(8)
                        }
                    }

                    if !result.senseData.isEmpty {
                        VStack(alignment: .leading, spacing: 6) {
                            Text("Sense Data")
                                .font(.headline)
                            Text(hexDump(result.senseData))
                                .font(.system(.caption, design: .monospaced))
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(12)
                                .background(Color.secondary.opacity(0.08))
                                .cornerRadius(8)
                        }
                    }
                } else {
                    Text("No command result available yet.")
                        .foregroundStyle(.secondary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding()
        }
    }

    private var statusPanel: some View {
        GroupBox("Status") {
            VStack(alignment: .leading, spacing: 8) {
                if let statusMessage = viewModel.statusMessage {
                    Text(statusMessage)
                }
                if let errorMessage = viewModel.errorMessage {
                    Text(errorMessage)
                        .foregroundStyle(.red)
                }
                if viewModel.isBusy || viewModel.isLoadingDevices {
                    ProgressView()
                        .controlSize(.small)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding()
        }
    }

    private func deviceTitle(_ device: ASFWDriverConnector.FWDeviceInfo) -> String {
        let title = "\(device.vendorName) \(device.modelName)".trimmingCharacters(in: .whitespaces)
        return title.isEmpty ? String(format: "Device 0x%016llX", device.guid) : title
    }

    private func unitLabel(_ unit: ASFWDriverConnector.FWUnitInfo) -> String {
        if let productName = unit.productName, !productName.isEmpty {
            return "\(productName) (\(unit.specIdHex), ROM \(unit.romOffset))"
        }
        return "\(unit.specIdHex) • ROM \(unit.romOffset)"
    }

    private func hexDump(_ data: Data) -> String {
        data.map { String(format: "%02X", $0) }
            .chunked(into: 16)
            .map { $0.joined(separator: " ") }
            .joined(separator: "\n")
    }
}

private struct SBP2DeviceRow: View {
    let device: ASFWDriverConnector.FWDeviceInfo

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title)
                .font(.headline)

            HStack(spacing: 8) {
                Label(String(format: "Node %u", device.nodeId), systemImage: "externaldrive")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Text("•")
                    .foregroundStyle(.secondary)

                Text(String(format: "Gen %u", device.generation))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Text("\(device.sbp2Units.count) SBP-2 unit\(device.sbp2Units.count == 1 ? "" : "s")")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 4)
    }

    private var title: String {
        let combined = "\(device.vendorName) \(device.modelName)".trimmingCharacters(in: .whitespaces)
        return combined.isEmpty ? String(format: "SBP-2 0x%016llX", device.guid) : combined
    }
}

private extension Array {
    func chunked(into size: Int) -> [[Element]] {
        stride(from: 0, to: count, by: size).map { index in
            Array(self[index..<Swift.min(index + size, count)])
        }
    }
}
