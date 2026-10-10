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
    @StateObject private var diagnosticsStore: DiagnosticsStore
    @StateObject private var diceReportStore: DiceReportStore
    @StateObject private var avcReportStore: AvcReportStore
    @StateObject private var avcUnitsStore: AvcUnitsStore
    @StateObject private var mcpVM: ASFWMCPControlViewModel
    @State private var publishedAudioDevices: [AudioWrapperDevice] = []
    @State private var selectedSection: SidebarSection? = .overview
    @AppStorage(AdvancedToolsSettings.storageKey)
    private var showAdvancedTools = AdvancedToolsSettings.defaultEnabled
    @AppStorage(DriverInstallSettings.requireNewerBuildKey)
    private var requireNewerBuild = DriverInstallSettings.defaultRequireNewerBuild

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
        _diagnosticsStore = StateObject(wrappedValue: DiagnosticsStore(connector: debugViewModel.connector))
        _diceReportStore = StateObject(wrappedValue: DiceReportStore(connector: debugViewModel.connector))
        _avcReportStore = StateObject(wrappedValue: AvcReportStore(connector: debugViewModel.connector))
        _avcUnitsStore = StateObject(wrappedValue: AvcUnitsStore(connector: debugViewModel.connector))
        _mcpVM = StateObject(wrappedValue: ASFWMCPControlViewModel(connector: debugViewModel.connector))
    }

    enum SidebarSection: String, CaseIterable, Identifiable {
        case overview = "Overview"
        case busInspector = "Bus Inspector"
        case avcUnits = "AV/C Units"
        case avcCommands = "AV/C Commands"
        case controller = "Controller Status"
        case async = "Async Commands"
        case audioTelemetry = "Audio Telemetry"
        case audioGeometry = "Audio Geometry"
        case dvCapture = "DV Capture"
        case busReset = "Bus Reset History"
        case logs = "System Logs"
        case loggingSettings = "Logging Settings"
        case mcpSettings = "MCP Control"
        case audio = "Core Audio"
        case audioAnalyzer = "Audio Analyzer"
        case duet = "Duet"
        case diagnostics = "Driver Report"
        case diceReport = "DICE Report"
        case avcReport = "AV/C Report"

        var id: String { rawValue }

        var systemImage: String {
            switch self {
            case .overview: return "info.circle"
            case .busInspector: return "network"
            case .avcUnits: return "music.note"
            case .avcCommands: return "command"
            case .controller: return "cpu"
            case .async: return "bolt.horizontal.circle"
            case .audioTelemetry: return "waveform.path.ecg"
            case .audioGeometry: return "slider.horizontal.3"
            case .dvCapture: return "video.fill"
            case .busReset: return "bolt.horizontal.circle"
            case .logs: return "doc.text"
            case .loggingSettings: return "slider.horizontal.3"
            case .mcpSettings: return "point.3.connected.trianglepath.dotted"
            case .audio: return "hifispeaker.fill"
            case .audioAnalyzer: return "waveform"
            case .duet: return "slider.horizontal.below.square.filled.and.square"
            case .diagnostics: return "heart.text.square"
            case .diceReport: return "doc.text.magnifyingglass"
            case .avcReport: return "doc.text.magnifyingglass"
            }
        }

        var group: SidebarGroup {
            switch self {
            case .overview: .general
            case .busInspector, .avcUnits, .duet, .audioAnalyzer: .devices
            case .dvCapture: .video
            case .diagnostics, .diceReport, .avcReport: .reports
            case .logs, .loggingSettings: .support
            default: .advanced
            }
        }

        var requiresAdvancedTools: Bool { group == .advanced }
    }
    
    private var visibleSections: [SidebarSection] {
        SidebarSection.allCases.filter { section in
            guard showAdvancedTools || !section.requiresAdvancedTools else { return false }
            switch section {
            case .avcUnits, .avcCommands, .avcReport: return debugVM.hasAVCDevice
            case .diceReport: return debugVM.hasDICEDevice
            case .duet: return debugVM.hasDuetDevice
            case .audioAnalyzer, .audioTelemetry, .audioGeometry:
                return !publishedAudioDevices.isEmpty
            default: return true
            }
        }
    }

    private func refreshPublishedAudioDevices() {
        publishedAudioDevices = AudioSystem.shared.devices.filter {
            ASFWAudioObserverClient.guid(fromDeviceUID: $0.uid) != nil
        }
    }

    var body: some View {
        NavigationSplitView {
            List(selection: $selectedSection) {
                ForEach(SidebarGroup.allCases) { group in
                    let sections = visibleSections.filter { $0.group == group }
                    if !sections.isEmpty {
                        Section(group.rawValue) {
                            ForEach(sections) { section in
                                Label(section.rawValue, systemImage: section.systemImage)
                                    .tag(section)
                            }
                        }
                    }
                }
            }
            .navigationTitle("ASFW")
            .listStyle(.sidebar)
            .safeAreaInset(edge: .bottom) {
                Toggle("Show Advanced Tools", isOn: $showAdvancedTools)
                    .toggleStyle(.switch)
                    .controlSize(.small)
                    .padding()
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(.bar)
                    .help("Show technical inspection and command tools. Logging is configured separately.")
            }
        } detail: {
            // Detail view
            Group {
                switch selectedSection {
                case .overview:
                    OverviewView(viewModel: driverVM,
                                 requireNewerBuild: $requireNewerBuild)
                case .busInspector:
                    BusInspectorView(debugViewModel: debugVM, topologyViewModel: topologyVM, romViewModel: romExplorerVM)
                case .avcUnits:
                    AvcUnitsView(store: avcUnitsStore, connector: debugVM.connector, developerTools: showAdvancedTools)
                case .avcCommands:
                    AVCCommandView(viewModel: debugVM)
                case .controller:
                    ControllerDetailView(viewModel: debugVM)
                case .async:
                    CommandsView(viewModel: debugVM)
                case .audioTelemetry:
                    AudioTelemetryView(connector: debugVM.connector)
                case .audioGeometry:
                    AudioGeometryView(connector: debugVM.connector)
                case .dvCapture:
                    DVCaptureView(viewModel: debugVM)
                case .busReset:
                    BusResetHistoryView(viewModel: debugVM)
                case .logs:
                    SystemLogsView(connector: debugVM.connector)
                case .loggingSettings:
                    LoggingSettingsView(connector: debugVM.connector, showAdvancedControls: showAdvancedTools)
                case .mcpSettings:
                    MCPSettingsView(viewModel: mcpVM)
                case .audio:
                    AudioDebugView()
                case .audioAnalyzer:
                    AudioAnalyzerView(devices: publishedAudioDevices, refresh: refreshPublishedAudioDevices)
                case .duet:
                    DuetControlView(connector: debugVM.connector)
                case .diagnostics:
                    DiagnosticsView(store: diagnosticsStore, showAdvancedTools: showAdvancedTools)
                case .diceReport:
                    DiceReportView(store: diceReportStore)
                case .avcReport:
                    AvcReportView(store: avcReportStore)
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
                            driverVM.installDriver(requireNewerBuild: requireNewerBuild)
                        } label: {
                            Label("Install", systemImage: "arrow.down.circle.fill")
                        }
                        .labelStyle(.titleAndIcon)
                        .disabled(driverVM.isBusy)
                        .keyboardShortcut("i", modifiers: .command)
                        
                        Button {
                            driverVM.uninstallDriver()
                        } label: {
                            Label("Delete", systemImage: "trash.fill")
                        }
                        .labelStyle(.titleAndIcon)
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
            if mcpVM.isEnabled {
                Task { await mcpVM.start() }
            }
        }
        .onDisappear {
            Task { await mcpVM.stop() }
            debugVM.disconnect()
            topologyVM.stopAutoRefresh()
        }
        .task {
            // Discovery can finish after the bus status notification. Poll its cached
            // inventory globally so AV/C navigation also updates while on Overview.
            while !Task.isCancelled {
                debugVM.refreshDiscovery()
                refreshPublishedAudioDevices()
                do { try await Task.sleep(for: .seconds(2)) } catch { return }
            }
        }
        .onChange(of: debugVM.topologyCache?.generation) { _, _ in
            topologyVM.refresh()
        }
        .onChange(of: visibleSections) { _, sections in
            if let selectedSection, !sections.contains(selectedSection) {
                self.selectedSection = .overview
            }
        }
        .onChange(of: topologyVM.topology?.generation) { _, _ in
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
