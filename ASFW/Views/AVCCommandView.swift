//
//  AVCCommandView.swift
//  ASFW
//
//  AV/C command sender with preset builder and raw FCP hex mode.
//

import SwiftUI

struct AVCCommandView: View {
    @ObservedObject var viewModel: DebugViewModel

    @State private var avcUnits: [ASFWDriverConnector.AVCUnitInfo] = []
    @State private var selectedUnitGUID: UInt64?

    @State private var commandTab: CommandTab = .presetBuilder

    @State private var commandType: FCPCommandType = .status
    @State private var subunitType: String = "1F"
    @State private var subunitID: String = "7"
    @State private var opcode: String = "31"
    @State private var operands: String = "07FFFFFF"

    @State private var rawCommandHex: String = "01 FF 31 07 FF FF FF FF"

    @State private var isSending = false
    @State private var lastResponseData: Data?
    @State private var lastResponseSummary: String?
    @State private var lastError: String?
    @State private var lastSentTime: Date?

    enum CommandTab: String, CaseIterable {
        case presetBuilder = "Preset Builder"
        case rawHex = "Raw Hex"
    }

    enum FCPCommandType: String, CaseIterable {
        case control = "CONTROL (0x00)"
        case status = "STATUS (0x01)"
        case inquiry = "INQUIRY (0x02)"
        case notify = "NOTIFY (0x03)"

        var ctype: UInt8 {
            switch self {
            case .control: return 0x00
            case .status: return 0x01
            case .inquiry: return 0x02
            case .notify: return 0x03
            }
        }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                GroupBox {
                    HStack {
                        if viewModel.isConnected {
                            Label("Connected to driver", systemImage: "checkmark.circle.fill")
                                .foregroundColor(.green)
                        } else {
                            Label("Driver not connected", systemImage: "exclamationmark.triangle.fill")
                                .foregroundColor(.orange)
                        }

                        Spacer()

                        Button {
                            refreshUnits()
                        } label: {
                            Label("Refresh Units", systemImage: "arrow.clockwise")
                        }
                        .disabled(!viewModel.isConnected)
                    }
                } label: {
                    Label("Status", systemImage: "info.circle")
                        .font(.headline)
                }

                GroupBox {
                    VStack(alignment: .leading, spacing: 12) {
                        Text("Select AV/C Unit")
                            .font(.subheadline.bold())

                        if avcUnits.isEmpty {
                            HStack {
                                Image(systemName: "exclamationmark.triangle")
                                    .foregroundColor(.orange)
                                Text("No AV/C units detected")
                                    .foregroundColor(.secondary)
                            }
                        } else {
                            Picker("Unit", selection: $selectedUnitGUID) {
                                Text("Select unit...").tag(nil as UInt64?)
                                ForEach(avcUnits) { unit in
                                    Text("GUID: \(unit.guidHex) (Node: \(unit.nodeIDHex))")
                                        .tag(unit.guid as UInt64?)
                                }
                            }
                            .labelsHidden()
                        }
                    }
                } label: {
                    Label("Target Unit", systemImage: "target")
                        .font(.headline)
                }

                GroupBox {
                    VStack(alignment: .leading, spacing: 16) {
                        Picker("Mode", selection: $commandTab) {
                            ForEach(CommandTab.allCases, id: \.rawValue) { mode in
                                Text(mode.rawValue).tag(mode)
                            }
                        }
                        .pickerStyle(.segmented)

                        if commandTab == .presetBuilder {
                            presetBuilderSection
                        } else {
                            rawHexSection
                        }
                    }
                } label: {
                    Label("AV/C Command", systemImage: "list.bullet.rectangle")
                        .font(.headline)
                }

                if lastResponseData != nil || lastError != nil {
                    GroupBox {
                        VStack(alignment: .leading, spacing: 12) {
                            if let time = lastSentTime {
                                Text("Sent: \(time.formatted(date: .omitted, time: .standard))")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }

                            if let summary = lastResponseSummary {
                                Text(summary)
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }

                            if let responseData = lastResponseData {
                                VStack(alignment: .leading, spacing: 4) {
                                    Text("FCP Response (\(responseData.count) bytes):")
                                        .font(.caption.bold())
                                    Text(hexString(responseData))
                                        .font(.system(.caption, design: .monospaced))
                                        .foregroundColor(.green)
                                        .textSelection(.enabled)
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
                    } label: {
                        Label("Result", systemImage: "arrow.down.circle")
                            .font(.headline)
                    }
                }

                Button {
                    sendCommand()
                } label: {
                    if isSending {
                        ProgressView()
                            .controlSize(.regular)
                            .padding(.trailing, 8)
                        Text("Sending...")
                    } else {
                        Label("Send FCP Command", systemImage: "paperplane.fill")
                    }
                }
                .buttonStyle(.borderedProminent)
                .disabled(isSending || !viewModel.isConnected || selectedUnitGUID == nil)
                .controlSize(.large)

                Spacer()
            }
            .padding()
        }
        .navigationTitle("AV/C Commands")
        .onAppear {
            if viewModel.isConnected {
                refreshUnits()
            }
        }
    }

    private var presetBuilderSection: some View {
        VStack(alignment: .leading, spacing: 16) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Command Type (ctype)")
                    .font(.subheadline.bold())
                Picker("", selection: $commandType) {
                    ForEach(FCPCommandType.allCases, id: \.rawValue) { type in
                        Text(type.rawValue).tag(type)
                    }
                }
                .pickerStyle(.segmented)
            }

            Divider()

            HStack(spacing: 16) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Subunit Type")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    HStack {
                        Text("0x")
                            .foregroundColor(.secondary)
                        TextField("1F", text: $subunitType)
                            .textFieldStyle(.roundedBorder)
                            .font(.system(.body, design: .monospaced))
                            .frame(width: 60)
                    }
                }

                VStack(alignment: .leading, spacing: 4) {
                    Text("Subunit ID")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    TextField("7", text: $subunitID)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(.body, design: .monospaced))
                        .frame(width: 80)
                }
            }

            Text("Common: 0x1F (unit) + ID 7 (broadcast)")
                .font(.caption2)
                .foregroundColor(.secondary)

            Divider()

            VStack(alignment: .leading, spacing: 4) {
                Text("Opcode")
                    .font(.subheadline.bold())
                HStack {
                    Text("0x")
                        .foregroundColor(.secondary)
                    TextField("31", text: $opcode)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(.body, design: .monospaced))
                        .frame(width: 80)
                }
                Text("Example: 0x31 (SUBUNIT_INFO), 0x30 (UNIT_INFO)")
                    .font(.caption2)
                    .foregroundColor(.secondary)
            }

            Divider()

            VStack(alignment: .leading, spacing: 4) {
                Text("Operands (hex bytes)")
                    .font(.subheadline.bold())
                TextEditor(text: $operands)
                    .font(.system(.body, design: .monospaced))
                    .frame(height: 80)
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.secondary.opacity(0.2)))
                Text("Enter hex bytes (optional spaces/newlines): 07 FF FF FF")
                    .font(.caption2)
                    .foregroundColor(.secondary)
            }

            VStack(alignment: .leading, spacing: 8) {
                Text("Quick Commands")
                    .font(.caption.bold())
                HStack(spacing: 8) {
                    Button("SUBUNIT_INFO") {
                        setSubunitInfo()
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)

                    Button("UNIT_INFO") {
                        setUnitInfo()
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                }
            }

            if let preview = buildFCPCommand() {
                Text("Frame Preview: \(hexString(preview))")
                    .font(.caption2)
                    .foregroundColor(.secondary)
                    .textSelection(.enabled)
            } else {
                Text("Frame Preview: invalid fields")
                    .font(.caption2)
                    .foregroundColor(.red)
            }
        }
    }

    private var rawHexSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Raw FCP frame bytes")
                .font(.subheadline.bold())
            TextEditor(text: $rawCommandHex)
                .font(.system(.body, design: .monospaced))
                .frame(height: 120)
                .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.secondary.opacity(0.2)))
            Text("Enter full FCP command frame (3-512 bytes). Example: 01 FF 31 07 FF FF FF FF")
                .font(.caption2)
                .foregroundColor(.secondary)
            HStack {
                Button("Load Preset Frame") {
                    if let preview = buildFCPCommand() {
                        rawCommandHex = hexString(preview)
                    }
                }
                .buttonStyle(.bordered)
                .controlSize(.small)

                Spacer()

                if let byteCount = parseHexBytes(rawCommandHex)?.count {
                    Text("\(byteCount) bytes")
                        .font(.caption)
                        .foregroundColor(.secondary)
                } else {
                    Text("Invalid hex")
                        .font(.caption)
                        .foregroundColor(.red)
                }
            }
        }
    }

    private func refreshUnits() {
        guard viewModel.isConnected else { return }

        DispatchQueue.global(qos: .userInitiated).async {
            let units = viewModel.connector.getAVCUnits() ?? []
            DispatchQueue.main.async {
                self.avcUnits = units
                if let first = units.first {
                    self.selectedUnitGUID = first.guid
                }
            }
        }
    }

    private func sendCommand() {
        guard let guid = selectedUnitGUID else { return }

        let commandData: Data?
        switch commandTab {
        case .presetBuilder:
            commandData = buildFCPCommand()
        case .rawHex:
            commandData = parseHexBytes(rawCommandHex)
        }

        guard let commandData else {
            lastError = "Invalid command format"
            lastResponseData = nil
            lastResponseSummary = nil
            return
        }

        isSending = true
        lastError = nil
        lastResponseData = nil
        lastResponseSummary = nil
        lastSentTime = Date()

        DispatchQueue.global(qos: .userInitiated).async {
            let response = viewModel.connector.sendRawFCPCommand(guid: guid, frame: commandData)

            DispatchQueue.main.async {
                self.isSending = false
                if let response {
                    self.lastResponseData = response
                    self.lastResponseSummary = self.responseSummary(response)
                    self.lastError = nil
                } else {
                    self.lastError = viewModel.connector.lastError ?? "Failed to send FCP command"
                    self.lastResponseData = nil
                    self.lastResponseSummary = nil
                }
            }
        }
    }

    private func buildFCPCommand() -> Data? {
        guard let subunitTypeVal = UInt8(subunitType.cleanedHexString, radix: 16) else { return nil }
        guard let subunitIDVal = parseUInt8(subunitID), subunitIDVal <= 0x07 else { return nil }
        guard let opcodeVal = UInt8(opcode.cleanedHexString, radix: 16) else { return nil }
        guard let operandBytes = parseHexBytes(operands, minBytes: 0, maxBytes: 509) else { return nil }

        var frame = Data()
        frame.append(commandType.ctype)
        frame.append((subunitTypeVal << 3) | (subunitIDVal & 0x7))
        frame.append(opcodeVal)
        frame.append(operandBytes)

        while frame.count < 8 {
            frame.append(0xFF)
        }

        return frame
    }

    private func parseUInt8(_ value: String) -> UInt8? {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.hasPrefix("0x") || trimmed.hasPrefix("0X") {
            return UInt8(trimmed.dropFirst(2), radix: 16)
        }
        if let decimal = UInt8(trimmed, radix: 10) {
            return decimal
        }
        return UInt8(trimmed, radix: 16)
    }

    private func parseHexBytes(_ value: String, minBytes: Int = 3, maxBytes: Int = 512) -> Data? {
        let cleaned = value.cleanedHexString
        guard cleaned.count % 2 == 0 else { return nil }

        var data = Data(capacity: cleaned.count / 2)
        var index = cleaned.startIndex
        while index < cleaned.endIndex {
            let nextIndex = cleaned.index(index, offsetBy: 2)
            let byteString = String(cleaned[index..<nextIndex])
            guard let byte = UInt8(byteString, radix: 16) else { return nil }
            data.append(byte)
            index = nextIndex
        }

        guard data.count >= minBytes && data.count <= maxBytes else { return nil }
        return data
    }

    private func responseSummary(_ response: Data) -> String {
        guard response.count >= 3 else {
            return "Response received (\(response.count) bytes)"
        }

        let ctype = response[0]
        let subunit = response[1]
        let opcodeValue = response[2]
        let operandCount = response.count - 3

        return String(
            format: "ctype=0x%02X (%@), subunit=0x%02X, opcode=0x%02X, operands=%u",
            ctype,
            responseTypeName(ctype),
            subunit,
            opcodeValue,
            UInt32(operandCount)
        )
    }

    private func responseTypeName(_ ctype: UInt8) -> String {
        switch ctype {
        case 0x08: return "NOT_IMPLEMENTED"
        case 0x09: return "ACCEPTED"
        case 0x0A: return "REJECTED"
        case 0x0B: return "IN_TRANSITION"
        case 0x0C: return "IMPLEMENTED/STABLE"
        case 0x0D: return "CHANGED"
        case 0x0F: return "INTERIM"
        default: return "UNKNOWN"
        }
    }

    private func setSubunitInfo() {
        commandType = .status
        subunitType = "1F"
        subunitID = "7"
        opcode = "31"
        operands = "07FFFFFF"
    }

    private func setUnitInfo() {
        commandType = .status
        subunitType = "1F"
        subunitID = "7"
        opcode = "30"
        operands = "07FFFFFF"
    }

    private func hexString(_ data: Data) -> String {
        data.map { String(format: "%02X", $0) }.joined(separator: " ")
    }
}

private extension String {
    var cleanedHexString: String {
        trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "0x", with: "")
            .replacingOccurrences(of: "0X", with: "")
            .replacingOccurrences(of: " ", with: "")
            .replacingOccurrences(of: "\n", with: "")
            .replacingOccurrences(of: "\t", with: "")
    }
}

#Preview {
    AVCCommandView(viewModel: DebugViewModel())
}
