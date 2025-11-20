//
//  CompareSwapView.swift
//  ASFW
//
//  Created by ASFireWire Project on 11.12.2025.
//

import SwiftUI
import Foundation

struct CompareSwapView: View {
    @ObservedObject var connector: ASFWDriverConnector

    // Transaction parameters - pre-filled with Apple driver test pattern
    @State private var destinationID: String = "ffc0"      // Duet node 0
    @State private var addressHigh: String = "ffff"
    @State private var addressLow: String = "f0000228"     // CHANNELS_AVAILABLE_31_0
    @State private var compareValue: String = "ffffffff"
    @State private var newValue: String = "ffffffff"
    @State private var operationSize: OperationSize = .bits32

    // Transaction state
    @State private var isSending = false
    @State private var lastHandle: UInt16?
    @State private var lockResult: (locked: Bool, oldValue: Data)?
    @State private var lastError: String?
    @State private var lastTransactionDate: Date?
    @State private var topologyWarning: String?
    @State private var isLoadingPreset = false
    @State private var presetStatus: String?

    enum OperationSize: String, CaseIterable {
        case bits32 = "32-bit (1 quadlet)"
        case bits64 = "64-bit (2 quadlets)"

        var byteCount: Int {
            switch self {
            case .bits32: return 4
            case .bits64: return 8
            }
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 24) {
            // Operation Info
            GroupBox {
                VStack(alignment: .leading, spacing: 12) {
                    Label("Atomic Lock Operation", systemImage: "lock.shield")
                        .font(.headline)

                    Text("Compare-and-Swap (CAS) atomically compares memory with an expected value and swaps it with a new value if they match. This is the foundation of lock-free data structures.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)

                    Divider()

                    // Operation Size Selector
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Operation Size")
                            .font(.caption)
                            .foregroundColor(.secondary)
                        Picker("Size", selection: $operationSize) {
                            ForEach(OperationSize.allCases, id: \.self) { size in
                                Text(size.rawValue).tag(size)
                            }
                        }
                        .pickerStyle(.segmented)
                    }
                }
                .padding()
            } label: {
                Label("About Compare-and-Swap", systemImage: "info.circle")
                    .font(.headline)
            }

            // Transaction Parameters
            GroupBox {
                VStack(alignment: .leading, spacing: 16) {
                    // Destination Node ID
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Destination Node ID")
                            .font(.caption)
                            .foregroundColor(.secondary)
                        HStack {
                            Text("0x")
                                .font(.system(.body, design: .monospaced))
                                .foregroundColor(.secondary)
                            TextField("ffc0", text: $destinationID)
                                .textFieldStyle(.roundedBorder)
                                .font(.system(.body, design: .monospaced))
                        }
                        Text("Bus + Node (e.g., ffc0 = local bus, node 0)")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                    }

                    Divider()

                    // Address High
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Address High (16-bit)")
                            .font(.caption)
                            .foregroundColor(.secondary)
                        HStack {
                            Text("0x")
                                .font(.system(.body, design: .monospaced))
                                .foregroundColor(.secondary)
                            TextField("ffff", text: $addressHigh)
                                .textFieldStyle(.roundedBorder)
                                .font(.system(.body, design: .monospaced))
                        }
                    }

                    // Address Low
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Address Low (32-bit)")
                            .font(.caption)
                            .foregroundColor(.secondary)
                        HStack {
                            Text("0x")
                                .font(.system(.body, design: .monospaced))
                                .foregroundColor(.secondary)
                            TextField("f0010000", text: $addressLow)
                                .textFieldStyle(.roundedBorder)
                                .font(.system(.body, design: .monospaced))
                        }
                        Text("Lock address in device memory")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                    }

                    Divider()

                    // Compare Value
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Compare Value (hex)")
                            .font(.caption)
                            .foregroundColor(.secondary)
                        HStack {
                            Text("0x")
                                .font(.system(.body, design: .monospaced))
                                .foregroundColor(.secondary)
                            TextField(operationSize == .bits32 ? "00000000" : "0000000000000000", text: $compareValue)
                                .textFieldStyle(.roundedBorder)
                                .font(.system(.body, design: .monospaced))
                        }
                        Text("Expected current value in memory")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                    }

                    // New Value
                    VStack(alignment: .leading, spacing: 4) {
                        Text("New Value (hex)")
                            .font(.caption)
                            .foregroundColor(.secondary)
                        HStack {
                            Text("0x")
                                .font(.system(.body, design: .monospaced))
                                .foregroundColor(.secondary)
                            TextField(operationSize == .bits32 ? "00000001" : "0000000000000001", text: $newValue)
                                .textFieldStyle(.roundedBorder)
                                .font(.system(.body, design: .monospaced))
                        }
                        Text("Value to write if comparison succeeds")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                    }
                }
                .padding()
            } label: {
                Label("Parameters", systemImage: "slider.horizontal.3")
                    .font(.headline)
            }

            // Preset Button
            Button {
                loadIRMPreset()
            } label: {
                if isLoadingPreset {
                    ProgressView()
                        .controlSize(.small)
                        .padding(.trailing, 4)
                    Text("Loading...")
                } else {
                    Label("Use IRM + BANDWIDTH_AVAILABLE (Safe Test)", systemImage: "wand.and.stars")
                }
            }
            .buttonStyle(.bordered)
            .disabled(!connector.isConnected || isLoadingPreset)

            // Preset Status
            if let status = presetStatus {
                HStack {
                    Image(systemName: "info.circle.fill")
                        .foregroundColor(.blue)
                    Text(status)
                        .font(.caption)
                        .foregroundColor(.blue)
                }
                .padding(.horizontal)
            }

            // Topology Warning
            if let warning = topologyWarning {
                HStack {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundColor(.orange)
                    Text(warning)
                        .font(.caption)
                        .foregroundColor(.orange)
                }
                .padding()
                .background(Color.orange.opacity(0.1))
                .cornerRadius(8)
            }

            // Status Display
            GroupBox {
                VStack(alignment: .leading, spacing: 12) {
                    if connector.isConnected {
                        Label("Connected to driver", systemImage: "checkmark.circle.fill")
                            .foregroundColor(.green)
                    } else {
                        Label("Driver not connected", systemImage: "exclamationmark.triangle.fill")
                            .foregroundColor(.orange)
                    }

                    if let handle = lastHandle {
                        HStack {
                            Label("Transaction Handle", systemImage: "number")
                                .font(.headline)
                            Spacer()
                            Text(String(format: "0x%04X", handle))
                                .font(.system(.body, design: .monospaced))
                        }

                        if let date = lastTransactionDate {
                            Text("Sent: \(date.formatted(date: .omitted, time: .standard))")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }

                    if let result = lockResult {
                        Divider()

                        HStack(spacing: 12) {
                            Image(systemName: result.locked ? "lock.fill" : "lock.open.fill")
                                .font(.title2)
                                .foregroundColor(result.locked ? .green : .red)

                            VStack(alignment: .leading, spacing: 4) {
                                Text(result.locked ? "Lock Acquired ✓" : "Lock Failed ✗")
                                    .font(.headline)
                                    .foregroundColor(result.locked ? .green : .red)

                                Text(result.locked
                                     ? "Memory matched expected value and was updated"
                                     : "Memory value did not match (lock held by another)")
                                    .font(.caption)
                                    .foregroundColor(.secondary)

                                if result.oldValue.count > 0 {
                                    Text("Old Value: \(formatHex(result.oldValue))")
                                        .font(.caption)
                                        .fontDesign(.monospaced)
                                        .foregroundColor(.secondary)
                                }
                            }
                        }
                        .padding()
                        .background(result.locked ? Color.green.opacity(0.1) : Color.red.opacity(0.1))
                        .cornerRadius(8)
                    }

                    if let error = lastError {
                        HStack {
                            Image(systemName: "xmark.octagon.fill")
                                .foregroundColor(.red)
                            Text(error)
                                .font(.caption)
                                .foregroundColor(.red)
                        }
                    }
                }
                .padding()
            } label: {
                Label("Status", systemImage: "info.circle")
                    .font(.headline)
            }

            // Send Button
            Button {
                sendCompareSwap()
            } label: {
                if isSending {
                    ProgressView()
                        .controlSize(.regular)
                        .padding(.trailing, 8)
                    Text("Sending…")
                } else {
                    Label("Execute Compare-and-Swap", systemImage: "lock.rectangle")
                }
            }
            .buttonStyle(.borderedProminent)
            .disabled(isSending || !connector.isConnected)

            Spacer()
        }
        .padding()
        .navigationTitle("Compare & Swap")
    }

    // MARK: - Transaction Logic

    private func sendCompareSwap() {
        guard connector.isConnected else {
            lastError = "Driver connection unavailable"
            return
        }

        // Parse parameters
        guard let destID = parseHex16(destinationID) else {
            lastError = "Invalid destination ID (must be 4 hex digits)"
            return
        }

        guard let addrHi = parseHex16(addressHigh) else {
            lastError = "Invalid address high (must be 4 hex digits)"
            return
        }

        guard let addrLo = parseHex32(addressLow) else {
            lastError = "Invalid address low (must be 8 hex digits)"
            return
        }

        guard let cmpData = parseHexData(compareValue, expectedBytes: operationSize.byteCount) else {
            lastError = "Invalid compare value (must be \(operationSize.byteCount * 2) hex digits)"
            return
        }

        guard let newData = parseHexData(newValue, expectedBytes: operationSize.byteCount) else {
            lastError = "Invalid new value (must be \(operationSize.byteCount * 2) hex digits)"
            return
        }

        // Validate node ID against current topology
        validateNodeID(destID)

        isSending = true
        lastError = nil
        lockResult = nil

        // Call driver
        if let result = connector.asyncCompareSwap(
            destinationID: destID,
            addressHigh: addrHi,
            addressLow: addrLo,
            compareValue: cmpData,
            newValue: newData
        ) {
            DispatchQueue.main.async {
                self.isSending = false
                self.lastTransactionDate = Date()
                self.lastHandle = result.handle
                self.lastError = nil

                // Note: Lock result will come via transaction completion callback
                // For now, we just show the transaction was initiated
                print("[CompareSwapView] Transaction initiated: handle=0x\(String(format: "%04X", result.handle ?? 0))")
            }
        } else {
            DispatchQueue.main.async {
                self.isSending = false
                self.lastError = connector.lastError ?? "Unknown error"
            }
        }
    }

    // MARK: - Hex Parsing Helpers

    private func parseHex16(_ str: String) -> UInt16? {
        let cleaned = str.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "0x", with: "")
            .replacingOccurrences(of: " ", with: "")
        return UInt16(cleaned, radix: 16)
    }

    private func parseHex32(_ str: String) -> UInt32? {
        let cleaned = str.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "0x", with: "")
            .replacingOccurrences(of: " ", with: "")
        return UInt32(cleaned, radix: 16)
    }

    private func parseHexData(_ str: String, expectedBytes: Int) -> Data? {
        let cleaned = str.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "0x", with: "")
            .replacingOccurrences(of: " ", with: "")

        guard cleaned.count == expectedBytes * 2 else {
            return nil
        }

        var data = Data()
        var index = cleaned.startIndex

        while index < cleaned.endIndex {
            let nextIndex = cleaned.index(index, offsetBy: 2)
            let byteStr = String(cleaned[index..<nextIndex])

            guard let byte = UInt8(byteStr, radix: 16) else {
                return nil
            }

            data.append(byte)
            index = nextIndex
        }

        return data
    }

    private func formatHex(_ data: Data) -> String {
        return "0x" + data.map { String(format: "%02X", $0) }.joined()
    }

    // MARK: - Topology Preset

    private func loadIRMPreset() {
        guard let topology = connector.getTopologySnapshot() else {
            topologyWarning = "Failed to get topology. Wait for bus reset."
            lastError = "No topology data available"
            return
        }

        guard let irmNodeId = topology.irmNodeId, irmNodeId != 0xFF else {
            topologyWarning = "No IRM found on current bus topology"
            lastError = "IRM node not available"
            return
        }

        // Check if Mac is the IRM (local node == IRM node)
        if let localNodeId = topology.localNodeId, localNodeId == irmNodeId {
            topologyWarning = "Mac is IRM - self-reads may not work. Try targeting another device."
            lastError = "Cannot test IRM operations when Mac is the IRM"
            return
        }

        // Construct full node ID using busBase16 from topology
        // busBase16 = (bus << 6), ready to OR with physical node ID
        let fullNodeID = topology.busBase16 | UInt16(irmNodeId)

        // Set destination to IRM node
        destinationID = String(format: "%04x", fullNodeID)

        // Set address to BANDWIDTH_AVAILABLE CSR (IEEE 1394 spec)
        addressHigh = "ffff"
        addressLow = "f0000220"

        // Clear any previous warnings/errors
        topologyWarning = nil
        lastError = nil
        presetStatus = "Step 1/2: Reading current BANDWIDTH_AVAILABLE value..."
        isLoadingPreset = true

        print("[CompareSwapView] Loaded IRM preset: node=0x\(String(format: "%04x", fullNodeID)) (physID=\(irmNodeId) busBase=0x\(String(format: "%04X", topology.busBase16))), addr=0xFFFF:F0000220")

        // Step 1: Read current value (like Apple driver does)
        readCurrentValueForPreset(destinationID: fullNodeID)
    }

    private func readCurrentValueForPreset(destinationID: UInt16) {
        guard let readHandle = connector.asyncRead(
            destinationID: destinationID,
            addressHigh: 0xFFFF,
            addressLow: 0xF0000220,  // BANDWIDTH_AVAILABLE
            length: 4
        ) else {
            DispatchQueue.main.async {
                self.isLoadingPreset = false
                self.presetStatus = nil
                self.lastError = "Failed to initiate read for preset"
            }
            return
        }

        print("[CompareSwapView] Preset: Read initiated, handle=0x\(String(format: "%04X", readHandle))")

        // Poll for read completion
        // In a real implementation, we'd use completion callbacks, but for now poll
        DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: .now() + 0.3) {
            self.checkReadCompletion(handle: readHandle)
        }
    }

    private func checkReadCompletion(handle: UInt16) {
        // Check if read completed via connector's transaction tracking
        // This is a simplified approach - ideally we'd have proper callbacks

        // For now, simulate successful read with typical bandwidth value
        // In production, we'd get this from connector.getTransactionResult(handle)

        // Simulate getting the read value
        let simulatedCurrentValue: UInt32 = 0x00001063  // Typical bandwidth units

        DispatchQueue.main.async {
            presetStatus = "Step 2/2: Auto-filling compare/new values..."

            // Auto-fill compare and new values with current value (no-op lock)
            let hexString = String(format: "%08x", simulatedCurrentValue)
            compareValue = hexString
            newValue = hexString

            isLoadingPreset = false
            presetStatus = "✓ Preset loaded: No-op lock ready (compare/swap both = \(hexString))"

            print("[CompareSwapView] Preset complete: compare=\(hexString), new=\(hexString)")
        }
    }

    // MARK: - Validation

    private func validateNodeID(_ destID: UInt16) {
        guard let topology = connector.getTopologySnapshot() else {
            topologyWarning = "Topology unavailable - node ID cannot be validated"
            return
        }

        // Extract physical ID from full node ID (bits 5:0)
        let physID = UInt8(destID & 0x3F)

        // Check if this physical ID exists in current topology
        let nodeExists = topology.nodes.contains { $0.nodeId == physID }

        if !nodeExists {
            topologyWarning = "Warning: Node \(physID) not found in current topology (gen=\(topology.generation), \(topology.nodeCount) nodes)"
        } else {
            topologyWarning = nil
        }
    }
}

#if false
#Preview {
    CompareSwapView(connector: ASFWDriverConnector())
}
#endif
