//
//  ModernContentView.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI
import Foundation

struct ModernContentView: View {
    @StateObject private var driverVM = DriverViewModel()
    @StateObject private var debugVM = DebugViewModel()
    @StateObject private var topologyVM: TopologyViewModel
    @StateObject private var romExplorerVM: RomExplorerViewModel
    @State private var selectedSection: SidebarSection? = .overview

    init() {
        let driverViewModel = DriverViewModel()
        let debugViewModel = DebugViewModel()
        let topologyViewModel = TopologyViewModel(connector: debugViewModel.connector)
        _driverVM = StateObject(wrappedValue: driverViewModel)
        _debugVM = StateObject(wrappedValue: debugViewModel)
        _topologyVM = StateObject(wrappedValue: topologyViewModel)
        _romExplorerVM = StateObject(wrappedValue: RomExplorerViewModel(
            connector: debugViewModel.connector,
            topologyViewModel: topologyViewModel
        ))
    }

    enum SidebarSection: String, CaseIterable, Identifiable {
        case overview = "Overview"
        case devices = "Device Discovery"
        case ping = "Ping"
        case controller = "Controller Status"
        case async = "Async Commands"
        case topology = "Topology & Self-ID"
        case romExplorer = "ROM Explorer"
        case busReset = "Bus Reset History"
        case logs = "System Logs"

        var id: String { rawValue }

        var systemImage: String {
            switch self {
            case .overview: return "info.circle"
            case .devices: return "externaldrive.connected.to.line.below"
            case .ping: return "waveform.path"
            case .controller: return "cpu"
            case .async: return "bolt.horizontal.circle"
            case .topology: return "network"
            case .romExplorer: return "memorychip"
            case .busReset: return "bolt.horizontal.circle"
            case .logs: return "doc.text"
            }
        }

        var isEnabled: Bool { true }
    }
    
    var body: some View {
        NavigationSplitView {
            // Sidebar
            List(SidebarSection.allCases, selection: $selectedSection) { section in
                Label(section.rawValue, systemImage: section.systemImage)
                    .tag(section)
                    .foregroundColor(.primary)
            }
            .navigationTitle("ASFW Driver")
            .listStyle(.sidebar)
        } detail: {
            // Detail view
            Group {
                switch selectedSection {
                case .overview:
                    OverviewView(viewModel: driverVM)
                case .devices:
                    DeviceDiscoveryView(viewModel: debugVM)
                case .ping:
                    PingView(viewModel: debugVM)
                case .controller:
                    ControllerDetailView(viewModel: debugVM)
                case .async:
                    CommandsView(viewModel: debugVM)
                case .topology:
                    TopologyView(viewModel: topologyVM)
                case .romExplorer:
                    ROMExplorerView(viewModel: debugVM)
                case .busReset:
                    BusResetHistoryView(viewModel: debugVM)
                case .logs:
                    SystemLogsView(viewModel: driverVM)
                case .none:
                    Text("Select a section")
                        .foregroundStyle(.secondary)
                }
            }
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    HStack(spacing: 12) {
                        if driverVM.isBusy {
                            ProgressView()
                                .controlSize(.small)
                        }
                        
                        Button {
                            driverVM.installDriver()
                        } label: {
                            Label("Install", systemImage: "arrow.down.circle.fill")
                        }
                        .disabled(driverVM.isBusy)
                        .keyboardShortcut("i", modifiers: .command)
                        
                        Button {
                            driverVM.uninstallDriver()
                        } label: {
                            Label("Uninstall", systemImage: "trash.fill")
                        }
                        .disabled(driverVM.isBusy)
                        .tint(.red)
                        .keyboardShortcut("u", modifiers: .command)
                    }
                }
            }
        }
        .onAppear {
            debugVM.setDriverViewModel(driverVM)
            debugVM.connect()
            topologyVM.startAutoRefresh()
            romExplorerVM.setConnector(debugVM.connector, topologyViewModel: topologyVM)
        }
        .onDisappear {
            debugVM.disconnect()
            topologyVM.stopAutoRefresh()
        }
        .onChange(of: topologyVM.topology?.generation) {
            // Update available nodes when topology generation changes
            romExplorerVM.refreshAvailableNodes()
        }
    }
}

struct AsyncCommandView: View {
    @ObservedObject var viewModel: DebugViewModel

    @State private var destinationID: String = "0x0000"
    @State private var addressHigh: String = "0x0000"
    @State private var addressLow: String = "0x00000000"
    @State private var readLength: String = "16"
    @State private var payloadHex: String = ""
    @State private var validationError: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                header

                if let error = validationError {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                        .font(.callout)
                }

                if let message = viewModel.asyncStatusMessage {
                    Label(message, systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                        .font(.callout)
                }

                if let message = viewModel.asyncErrorMessage {
                    Label(message, systemImage: "xmark.octagon.fill")
                        .foregroundStyle(.red)
                        .font(.callout)
                }

                GroupBox("Common Parameters") {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
                        GridRow {
                            Text("Destination ID")
                            TextField("0x0000", text: $destinationID)
                                .textFieldStyle(.roundedBorder)
                                .monospaced()
                                .frame(width: 160)
                        }
                        GridRow {
                            Text("Address High")
                            TextField("0x0000", text: $addressHigh)
                                .textFieldStyle(.roundedBorder)
                                .monospaced()
                                .frame(width: 160)
                        }
                        GridRow {
                            Text("Address Low")
                            TextField("0x00000000", text: $addressLow)
                                .textFieldStyle(.roundedBorder)
                                .monospaced()
                                .frame(width: 200)
                        }
                    }
                }

                commandSection
            }
            .padding()
        }
        .navigationTitle("Async Commands")
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Asynchronous Request Helpers")
                .font(.title2.bold())
            Text("Send raw async read/write transactions directly through the debug user client. Values accept decimal, hex (0x), octal (0o) or binary (0b).")
                .font(.callout)
                .foregroundStyle(.secondary)

            if !viewModel.isConnected {
                Label("Driver not connected", systemImage: "cable.connector.slash")
                    .foregroundStyle(.orange)
            }
        }
    }

    private var commandSection: some View {
        VStack(alignment: .leading, spacing: 16) {
            GroupBox("Async Read") {
                VStack(alignment: .leading, spacing: 12) {
                    HStack(spacing: 12) {
                        Text("Length")
                        TextField("16", text: $readLength)
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 120)
                            .monospaced()
                        Spacer()
                        Button {
                            submitRead()
                        } label: {
                            Label("Issue Read", systemImage: "arrow.down.circle")
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(!viewModel.isConnected || viewModel.asyncInProgress)
                    }
                }
            }

            GroupBox("Async Write") {
                VStack(alignment: .leading, spacing: 12) {
                    Text("Payload (hex bytes)")
                        .font(.subheadline)
                    TextEditor(text: $payloadHex)
                        .font(.system(.body, design: .monospaced))
                        .frame(minHeight: 100)
                        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.secondary.opacity(0.2)))

                    HStack {
                        Button("Clear") { payloadHex.removeAll() }
                        Spacer()
                        Button {
                            submitWrite()
                        } label: {
                            Label("Issue Write", systemImage: "arrow.up.circle")
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(!viewModel.isConnected || viewModel.asyncInProgress)
                    }
                }
            }

            if viewModel.asyncInProgress {
                ProgressView()
                    .progressViewStyle(.linear)
            }
        }
    }

    private func submitRead() {
        validationError = nil
        guard let destination = parseUInt16(destinationID) else {
            validationError = "Invalid destination ID"
            return
        }
        guard let high = parseUInt16(addressHigh) else {
            validationError = "Invalid address high"
            return
        }
        guard let low = parseUInt32(addressLow) else {
            validationError = "Invalid address low"
            return
        }
        guard let length = parseUInt32(readLength), length > 0 else {
            validationError = "Invalid length"
            return
        }

        viewModel.performAsyncRead(destinationID: destination,
                                   addressHigh: high,
                                   addressLow: low,
                                   length: length)
    }

    private func submitWrite() {
        validationError = nil
        guard let destination = parseUInt16(destinationID) else {
            validationError = "Invalid destination ID"
            return
        }
        guard let high = parseUInt16(addressHigh) else {
            validationError = "Invalid address high"
            return
        }
        guard let low = parseUInt32(addressLow) else {
            validationError = "Invalid address low"
            return
        }
        guard let data = dataFromHex(payloadHex), !data.isEmpty else {
            validationError = "Payload must contain at least one byte"
            return
        }

        viewModel.performAsyncWrite(destinationID: destination,
                                    addressHigh: high,
                                    addressLow: low,
                                    payload: data)
    }

    private func parseUInt16(_ value: String) -> UInt16? {
        parseUnsigned(value)
    }

    private func parseUInt32(_ value: String) -> UInt32? {
        parseUnsigned(value)
    }

    private func parseUnsigned<T: FixedWidthInteger & UnsignedInteger>(_ text: String) -> T? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }

        let value: String
        let radix: Int
        if trimmed.hasPrefix("0x") || trimmed.hasPrefix("0X") {
            value = String(trimmed.dropFirst(2))
            radix = 16
        } else if trimmed.hasPrefix("0b") || trimmed.hasPrefix("0B") {
            value = String(trimmed.dropFirst(2))
            radix = 2
        } else if trimmed.hasPrefix("0o") || trimmed.hasPrefix("0O") {
            value = String(trimmed.dropFirst(2))
            radix = 8
        } else {
            value = trimmed
            radix = 10
        }

        return T(value, radix: radix)
    }

    private func dataFromHex(_ text: String) -> Data? {
        let cleaned = text.replacingOccurrences(of: "\\s", with: "", options: .regularExpression)
        guard !cleaned.isEmpty, cleaned.count % 2 == 0 else { return nil }

        var data = Data(capacity: cleaned.count / 2)
        var index = cleaned.startIndex
        while index < cleaned.endIndex {
            let nextIndex = cleaned.index(index, offsetBy: 2)
            let byteString = cleaned[index..<nextIndex]
            guard let byte = UInt8(byteString, radix: 16) else { return nil }
            data.append(byte)
            index = nextIndex
        }
        return data
    }
}

#if DEBUG
struct AsyncCommandView_Previews: PreviewProvider {
    static var previews: some View {
        AsyncCommandView(viewModel: DebugViewModel())
            .frame(width: 600, height: 600)
    }
}
#endif
