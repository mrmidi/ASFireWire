//
//  DeviceDiscoveryView.swift
//  ASFW
//
//  Device Discovery GUI - displays FireWire devices and their units
//

import SwiftUI

struct DeviceDetailView: View {
    let device: ASFWDriverConnector.FWDeviceInfo

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
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
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .textSelection(.enabled)
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

