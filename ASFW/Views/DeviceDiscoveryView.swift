//
//  DeviceDiscoveryView.swift
//  ASFW
//
//  Device Discovery GUI - displays FireWire devices and their units
//

import SwiftUI

struct DeviceDiscoveryView: View {
    @ObservedObject var viewModel: DebugViewModel
    @State private var devices: [ASFWDriverConnector.FWDeviceInfo] = []
    @State private var selectedDeviceId: UInt64?
    @State private var autoRefreshEnabled = true
    @State private var lastRefresh: Date?
    @State private var refreshTimer: Timer?

    var body: some View {
        VStack(spacing: 0) {
            // Header with refresh controls
            HStack {
                Text("Discovered Devices")
                    .font(.title2.bold())

                Spacer()

                Toggle("Auto-refresh", isOn: $autoRefreshEnabled)
                    .toggleStyle(.switch)
                    .controlSize(.small)

                if let lastRefresh = lastRefresh {
                    Text("Updated: \(lastRefresh, style: .time)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Button {
                    refreshDevices()
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .buttonStyle(.borderedProminent)
                .disabled(!viewModel.isConnected)
            }
            .padding()

            Divider()

            if !viewModel.isConnected {
                ContentUnavailableView(
                    "Driver Not Connected",
                    systemImage: "cable.connector.slash",
                    description: Text("Connect to the driver to see discovered devices")
                )
            } else if devices.isEmpty {
                ContentUnavailableView(
                    "No Devices Discovered",
                    systemImage: "magnifyingglass",
                    description: Text("No FireWire devices have been detected yet")
                )
            } else {
                // Device list
                HSplitView {
                    // Left: Device list
                    List(devices, selection: $selectedDeviceId) { device in
                        DeviceRowView(device: device)
                            .tag(device.id)
                    }
                    .listStyle(.inset)
                    .frame(minWidth: 220, idealWidth: 250, maxWidth: 280)

                    // Right: Device details
                    if let selectedDevice = devices.first(where: { $0.id == selectedDeviceId }) {
                        DeviceDetailView(device: selectedDevice)
                    } else {
                        ContentUnavailableView(
                            "Select a Device",
                            systemImage: "sidebar.left",
                            description: Text("Choose a device from the list to see details")
                        )
                    }
                }
            }
        }
        .onAppear {
            refreshDevices()
            startAutoRefresh()
        }
        .onDisappear {
            stopAutoRefresh()
        }
    }

    private func refreshDevices() {
        guard viewModel.isConnected else { return }

        if let newDevices = viewModel.connector.getDiscoveredDevices() {
            devices = newDevices
            lastRefresh = Date()
        }
    }

    private func startAutoRefresh() {
        guard autoRefreshEnabled else { return }

        refreshTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { _ in
            if autoRefreshEnabled {
                refreshDevices()
            }
        }
    }

    private func stopAutoRefresh() {
        refreshTimer?.invalidate()
        refreshTimer = nil
    }
}

struct DeviceRowView: View {
    let device: ASFWDriverConnector.FWDeviceInfo

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            // Device name or fallback
            if !device.vendorName.isEmpty || !device.modelName.isEmpty {
                Text("\(device.vendorName) \(device.modelName)")
                    .font(.headline)
            } else {
                Text(String(format: "Device 0x%016llX", device.guid))
                    .font(.headline)
            }

            // GUID and node info
            HStack(spacing: 8) {
                Label(String(format: "Node %d", device.nodeId), systemImage: "circle.fill")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Text("•")
                    .foregroundStyle(.secondary)

                Text(String(format: "Gen %d", device.generation))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Text("•")
                    .foregroundStyle(.secondary)

                StateLabel(state: device.stateString)
            }

            // Unit count
            Text("\(device.units.count) unit\(device.units.count == 1 ? "" : "s")")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 4)
    }
}

struct DeviceDetailView: View {
    let device: ASFWDriverConnector.FWDeviceInfo

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                // Device Header
                VStack(alignment: .leading, spacing: 8) {
                    if !device.vendorName.isEmpty || !device.modelName.isEmpty {
                        Text("\(device.vendorName) \(device.modelName)")
                            .font(.title.bold())
                    }

                    HStack {
                        StateLabel(state: device.stateString)
                        Text(String(format: "Node %d", device.nodeId))
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                        Text("•")
                            .foregroundStyle(.secondary)
                        Text(String(format: "Generation %d", device.generation))
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                }

                Divider()

                // Device Properties
                GroupBox("Device Properties") {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                        GridRow {
                            Text("GUID:")
                                .fontWeight(.medium)
                            Text(String(format: "0x%016llX", device.guid))
                                .monospaced()
                        }
                        GridRow {
                            Text("Vendor ID:")
                                .fontWeight(.medium)
                            Text(String(format: "0x%06X", device.vendorId))
                                .monospaced()
                        }
                        GridRow {
                            Text("Model ID:")
                                .fontWeight(.medium)
                            Text(String(format: "0x%06X", device.modelId))
                                .monospaced()
                        }
                    }
                    .padding()
                }

                // Units Section
                if !device.units.isEmpty {
                    GroupBox("Unit Directories") {
                        VStack(alignment: .leading, spacing: 12) {
                            ForEach(device.units) { unit in
                                UnitCardView(unit: unit)
                                if unit.id != device.units.last?.id {
                                    Divider()
                                }
                            }
                        }
                        .padding()
                    }
                } else {
                    GroupBox("Unit Directories") {
                        Text("No unit directories found")
                            .foregroundStyle(.secondary)
                            .padding()
                    }
                }
            }
            .padding()
        }
    }
}

struct UnitCardView: View {
    let unit: ASFWDriverConnector.FWUnitInfo

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Unit")
                    .font(.headline)

                Spacer()

                StateLabel(state: unit.stateString)
            }

            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 6) {
                GridRow {
                    Text("Spec ID:")
                        .foregroundStyle(.secondary)
                    Text(unit.specIdHex)
                        .monospaced()
                }
                GridRow {
                    Text("SW Version:")
                        .foregroundStyle(.secondary)
                    Text(unit.swVersionHex)
                        .monospaced()
                }
                GridRow {
                    Text("ROM Offset:")
                        .foregroundStyle(.secondary)
                    Text(String(format: "%d quadlets", unit.romOffset))
                        .monospaced()
                }

                if let vendorName = unit.vendorName, !vendorName.isEmpty {
                    GridRow {
                        Text("Vendor:")
                            .foregroundStyle(.secondary)
                        Text(vendorName)
                    }
                }

                if let productName = unit.productName, !productName.isEmpty {
                    GridRow {
                        Text("Product:")
                            .foregroundStyle(.secondary)
                        Text(productName)
                    }
                }
            }
            .font(.callout)
        }
        .padding(12)
        .background(Color.secondary.opacity(0.05))
        .cornerRadius(8)
    }
}

struct StateLabel: View {
    let state: String

    var color: Color {
        switch state {
        case "Ready": return .green
        case "Suspended": return .orange
        case "Terminated": return .red
        case "Created": return .blue
        default: return .gray
        }
    }

    var body: some View {
        Text(state)
            .font(.caption)
            .fontWeight(.semibold)
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(color.opacity(0.15))
            .foregroundColor(color)
            .cornerRadius(4)
    }
}

#if DEBUG
struct DeviceDiscoveryView_Previews: PreviewProvider {
    static var previews: some View {
        DeviceDiscoveryView(viewModel: DebugViewModel())
            .frame(width: 900, height: 600)
    }
}
#endif
