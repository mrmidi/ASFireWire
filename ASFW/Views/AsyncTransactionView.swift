//
//  AsyncTransactionView.swift
//  ASFW
//
//  Created by ASFireWire Project on 11.10.2025.
//

import SwiftUI
import Foundation

struct AsyncTransactionView: View {
    @ObservedObject var viewModel: DebugViewModel

    // Transaction parameters
    @State private var operationType: OperationType = .read
    @State private var destinationID: String = "ffc0"
    @State private var addressHigh: String = "ffff"
    @State private var addressLow: String = "f0000400"
    @State private var length: String = "4"
    @State private var payloadHex: String = "12345678"

    // Transaction state
    @State private var isSending = false
    @State private var lastHandle: UInt16?
    @State private var lastError: String?
    @State private var lastTransactionDate: Date?

    enum OperationType: String, CaseIterable {
        case read = "Read"
        case write = "Write"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 24) {
            // Operation Type Selector
            GroupBox {
                Picker("Operation", selection: $operationType) {
                    ForEach(OperationType.allCases, id: \.self) { op in
                        Text(op.rawValue).tag(op)
                    }
                }
                .pickerStyle(.segmented)
                .padding(.bottom, 8)

                Text("Select transaction type: Read retrieves data, Write sends data")
                    .font(.caption)
                    .foregroundColor(.secondary)
            } label: {
                Label("Transaction Type", systemImage: "arrow.left.arrow.right")
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
                            TextField("f0000400", text: $addressLow)
                                .textFieldStyle(.roundedBorder)
                                .font(.system(.body, design: .monospaced))
                        }
                        Text("Common addresses: f0000400 (Config ROM)")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                    }

                    Divider()

                    // Length
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Length (bytes)")
                            .font(.caption)
                            .foregroundColor(.secondary)
                        TextField("4", text: $length)
                            .textFieldStyle(.roundedBorder)
                            .font(.system(.body, design: .monospaced))
                        Text("Use 4 for quadlet reads/writes")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                    }

                    // Payload (Write only)
                    if operationType == .write {
                        Divider()

                        VStack(alignment: .leading, spacing: 4) {
                            Text("Payload (hex)")
                                .font(.caption)
                                .foregroundColor(.secondary)
                            TextEditor(text: $payloadHex)
                                .font(.system(.body, design: .monospaced))
                                .frame(minHeight: 60)
                                .border(Color.gray.opacity(0.3), width: 1)
                            Text("Enter hex bytes (no 0x prefix): 12345678 or 00 11 22 33")
                                .font(.caption2)
                                .foregroundColor(.secondary)
                        }
                    }
                }
                .padding()
            } label: {
                Label("Parameters", systemImage: "slider.horizontal.3")
                    .font(.headline)
            }

            // Status Display
            GroupBox {
                VStack(alignment: .leading, spacing: 12) {
                    if viewModel.isConnected {
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
                sendTransaction()
            } label: {
                if isSending {
                    ProgressView()
                        .controlSize(.regular)
                        .padding(.trailing, 8)
                    Text("Sending…")
                } else {
                    Label("Send \(operationType.rawValue) Transaction", systemImage: "paperplane")
                }
            }
            .buttonStyle(.borderedProminent)
            .disabled(isSending || !viewModel.isConnected)

            Spacer()
        }
        .padding()
        .navigationTitle("Async Transactions")
    }

    // MARK: - Transaction Logic

    private func sendTransaction() {
        guard viewModel.isConnected else {
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

        guard let len = UInt32(length) else {
            lastError = "Invalid length (must be a number)"
            return
        }

        isSending = true
        lastError = nil

        switch operationType {
        case .read:
            // Use DebugViewModel method for consistency with logging
            viewModel.performAsyncRead(
                destinationID: destID,
                addressHigh: addrHi,
                addressLow: addrLo,
                length: len
            )

            // Update UI after async operation completes
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                self.isSending = false
                if viewModel.asyncErrorMessage == nil {
                    // Success - extract handle from status message if available
                    self.lastTransactionDate = Date()
                    self.lastError = nil
                } else {
                    self.lastError = viewModel.asyncErrorMessage
                }
            }

        case .write:
            guard let payloadData = parsePayloadHex(payloadHex) else {
                DispatchQueue.main.async {
                    self.isSending = false
                    self.lastError = "Invalid payload hex format"
                }
                return
            }

            // Use DebugViewModel method for consistency with logging
            viewModel.performAsyncWrite(
                destinationID: destID,
                addressHigh: addrHi,
                addressLow: addrLo,
                payload: payloadData
            )

            // Update UI after async operation completes
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                self.isSending = false
                if viewModel.asyncErrorMessage == nil {
                    // Success
                    self.lastTransactionDate = Date()
                    self.lastError = nil
                } else {
                    self.lastError = viewModel.asyncErrorMessage
                }
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

    private func parsePayloadHex(_ str: String) -> Data? {
        let cleaned = str.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "0x", with: "")
            .replacingOccurrences(of: " ", with: "")
            .replacingOccurrences(of: "\n", with: "")
            .replacingOccurrences(of: "\t", with: "")

        // Must have even number of hex digits
        guard cleaned.count % 2 == 0 else {
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
}

#if false
#Preview {
    AsyncTransactionView(viewModel: DebugViewModel())
}
#endif
