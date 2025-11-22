//
//  AVCCommandView.swift
//  ASFW
//
//  AV/C Command Sender - send raw FCP commands for debugging
//

import SwiftUI

struct AVCCommandView: View {
    @ObservedObject var viewModel: DebugViewModel
    
    // AV/C unit selection
    @State private var avcUnits: [ASFWDriverConnector.AVCUnitInfo] = []
    @State private var selectedUnitGUID: UInt64?
    
    // FCP command parameters
    @State private var commandType: FCPCommandType = .status
    @State private var subunitType: String = "1F"  // Unit (0x1F)
    @State private var subunitID: String = "7"     // ID 7 (broadcast)
    @State private var opcode: String = "31"       // SUBUNIT_INFO
    @State private var operands: String = "07FFFFFF"  // Page 0 + padding
    
    // Transaction state
    @State private var isSending = false
    @State private var lastResponse: String?
    @State private var lastError: String?
    @State private var lastSentTime: Date?
    
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
                // Connection status
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
                
                // Unit selection
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
                                    Text("GUID: \\(unit.guidHex) (Node: \\(unit.nodeIDHex))")
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
                
                // Command builder
                GroupBox {
                    VStack(alignment: .leading, spacing: 16) {
                        // Command type
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
                        
                        // Subunit address
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
                                    .frame(width: 60)
                            }
                        }
                        
                        Text("Common: 0x1F (unit) + ID 7 (broadcast)")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                        
                        Divider()
                        
                        // Opcode
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
                        
                        // Operands
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Operands (hex bytes)")
                                .font(.subheadline.bold())
                            TextEditor(text: $operands)
                                .font(.system(.body, design: .monospaced))
                                .frame(height: 80)
                                .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.secondary.opacity(0.2)))
                            Text("Enter hex bytes (no spaces or 0x): 07FFFFFF")
                                .font(.caption2)
                                .foregroundColor(.secondary)
                        }
                        
                        // Quick presets
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
                    }
                } label: {
                    Label("AV/C Command", systemImage: "list.bullet.rectangle")
                        .font(.headline)
                }
                
                // Response display
                if lastResponse != nil || lastError != nil {
                    GroupBox {
                        VStack(alignment: .leading, spacing: 12) {
                            if let time = lastSentTime {
                                Text("Sent: \\(time.formatted(date: .omitted, time: .standard))")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }
                            
                            if let response = lastResponse {
                                VStack(alignment: .leading, spacing: 4) {
                                    Text("Response:")
                                        .font(.caption.bold())
                                    Text(response)
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
                
                // Send button
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
    
    // MARK: - Actions
    
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
        guard let unit = avcUnits.first(where: { $0.guid == guid }) else { return }
        
        // Build FCP command frame
        guard let commandData = buildFCPCommand() else {
            lastError = "Invalid command format"
            return
        }
        
        // FCP Command register: 0xFFFFF0000B00
        let fcpCommandAddr: UInt64 = 0xFFFFF0000B00
        let nodeID = unit.nodeID
        
        isSending = true
        lastError = nil
        lastResponse = nil
        lastSentTime = Date()
        
        DispatchQueue.global(qos: .userInitiated).async {
            let handle = viewModel.connector.asyncWrite(
                destinationID: nodeID,
                addressHigh: UInt16((fcpCommandAddr >> 32) & 0xFFFF),
                addressLow: UInt32(fcpCommandAddr & 0xFFFFFFFF),
                payload: commandData
            )
            
            DispatchQueue.main.async {
                self.isSending = false
                if let handle = handle {
                    self.lastResponse = String(format: "Command sent (handle: 0x%04X) - Check logs for FCP response", handle)
                    self.lastError = nil
                } else {
                    self.lastError = viewModel.connector.lastError ?? "Failed to send command"
                    self.lastResponse = nil
                }
            }
        }
    }
    
    private func buildFCPCommand() -> Data? {
        // Parse hex inputs
        guard let subunitTypeVal = UInt8(subunitType, radix: 16) else { return nil }
        guard let subunitIDVal = UInt8(subunitID) else { return nil }
        guard let opcodeVal = UInt8(opcode, radix: 16) else { return nil }
        
        // Parse operands hex string
        let operandsClean = operands.replacingOccurrences(of: " ", with: "")
            .replacingOccurrences(of: "\\n", with: "")
        guard operandsClean.count % 2 == 0 else { return nil }
        
        var operandBytes = Data()
        var index = operandsClean.startIndex
        while index < operandsClean.endIndex {
            let nextIndex = operandsClean.index(index, offsetBy: 2)
            let byteStr = String(operandsClean[index..<nextIndex])
            guard let byte = UInt8(byteStr, radix: 16) else { return nil }
            operandBytes.append(byte)
            index = nextIndex
        }
        
        // Build FCP frame: [ctype/response | subunit_type+ID | opcode | operand[0] | operand[1] | ...]
        var frame = Data()
        frame.append(commandType.ctype)
        frame.append((subunitTypeVal << 3) | (subunitIDVal & 0x7))
        frame.append(opcodeVal)
        frame.append(operandBytes)
        
        // Pad to 8 bytes minimum (FCP requirement)
        while frame.count < 8 {
            frame.append(0xFF)
        }
        
        return frame
    }
    
    // MARK: - Presets
    
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
}

#Preview {
    AVCCommandView(viewModel: DebugViewModel())
}
