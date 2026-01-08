//
//  AVCDebugView.swift
//  ASFW
//
//  AV/C debug interface - shows discovered AV/C units
//

import SwiftUI
import UniformTypeIdentifiers

struct AVCDebugView: View {
    @ObservedObject var viewModel: DebugViewModel
    @State private var isRefreshing = false
    @State private var lastRefresh: Date?
    
    var body: some View {
        ScrollView {
            VStack(spacing: 20) {
                // Header with connection status
                StatusHeaderView(
                    isConnected: viewModel.isConnected,
                    lastRefresh: lastRefresh,
                    isRefreshing: isRefreshing,
                    onRefresh: refreshAVCUnits,
                    onReScan: triggerReScan,
                    onTestIRM: triggerIRMTest,
                    onReleaseIRM: triggerIRMRelease,
                    onCMPConnectOPCR: triggerCMPConnectOPCR,
                    onCMPDisconnectOPCR: triggerCMPDisconnectOPCR,
                    onITDMAAllocate: triggerITDMAAllocate,
                    onITDMADeallocate: triggerITDMADeallocate
                )
                .padding(.horizontal)
                
                if viewModel.avcUnits.isEmpty {
                    EmptyStateView()
                } else {
                    LazyVStack(spacing: 16) {
                        ForEach(viewModel.avcUnits) { unit in
                            AVCUnitCard(unit: unit, viewModel: viewModel)
                        }
                    }
                    .padding(.horizontal)
                }
            }
            .padding(.vertical)
        }
        .navigationTitle("AV/C Units")
        .background(Color(NSColor.controlBackgroundColor))
        .onAppear {
            if viewModel.isConnected {
                refreshAVCUnits()
            }
        }
        .onChange(of: viewModel.isConnected) { connected in
            if connected {
                refreshAVCUnits()
            }
        }
    }
    
    private func refreshAVCUnits() {
        guard viewModel.isConnected else { return }
        isRefreshing = true
        viewModel.fetchAVCUnits()
        
        // Simulate refresh delay for UI feedback
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            self.lastRefresh = Date()
            self.isRefreshing = false
        }
    }
    
    private func triggerReScan() {
        guard viewModel.isConnected else { return }
        isRefreshing = true
        
        DispatchQueue.global(qos: .userInitiated).async {
            _ = viewModel.connector.reScanAVCUnits()
            Thread.sleep(forTimeInterval: 0.5)
            Task { @MainActor in
                viewModel.fetchAVCUnits()
                self.lastRefresh = Date()
                self.isRefreshing = false
            }
        }
    }
    
    // Phase 0.5: IRM allocation test
    private func triggerIRMTest() {
        guard viewModel.isConnected else { return }
        _ = viewModel.connector.testIRMAllocation()
    }
    
    // Phase 0.5: IRM release test
    private func triggerIRMRelease() {
        guard viewModel.isConnected else { return }
        _ = viewModel.connector.testIRMRelease()
    }
    
    // Phase 0.5: CMP connect oPCR test
    private func triggerCMPConnectOPCR() {
        guard viewModel.isConnected else { return }
        _ = viewModel.connector.testCMPConnectOPCR()
    }
    
    // Phase 0.5: CMP disconnect oPCR test
    private func triggerCMPDisconnectOPCR() {
        guard viewModel.isConnected else { return }
        _ = viewModel.connector.testCMPDisconnectOPCR()
    }
    
    // Phase 1.5: IT DMA allocation (no CMP)
    private func triggerITDMAAllocate() {
        guard viewModel.isConnected else { return }
        _ = viewModel.connector.allocateITDMA(channel: 1)
    }
    
    private func triggerITDMADeallocate() {
        guard viewModel.isConnected else { return }
        _ = viewModel.connector.deallocateITDMA()
    }
}


struct StatusHeaderView: View {
    let isConnected: Bool
    let lastRefresh: Date?
    let isRefreshing: Bool
    let onRefresh: () -> Void
    let onReScan: () -> Void
    let onTestIRM: () -> Void  // Phase 0.5 IRM allocation test
    let onReleaseIRM: () -> Void  // Phase 0.5 IRM release test
    let onCMPConnectOPCR: () -> Void  // Phase 0.5 CMP connect oPCR
    let onCMPDisconnectOPCR: () -> Void  // Phase 0.5 CMP disconnect oPCR
    let onITDMAAllocate: () -> Void  // Phase 1.5 IT DMA allocation (no CMP)
    let onITDMADeallocate: () -> Void  // Phase 1.5 IT DMA deallocation
    
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label(isConnected ? "Connected" : "Disconnected", 
                      systemImage: isConnected ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                    .foregroundColor(isConnected ? .green : .orange)
                    .font(.headline)
                
                Spacer()
                

                
                if isRefreshing {
                    ProgressView()
                        .controlSize(.small)
                }
                
                // Phase 0.5: IRM test buttons
                Button(action: onTestIRM) {
                    Label("Alloc IRM", systemImage: "plus.circle")
                }
                .disabled(!isConnected || isRefreshing)
                .help("Allocate IRM (channel 0, 84 BW units) - check Console.app")
                
                Button(action: onReleaseIRM) {
                    Label("Free IRM", systemImage: "minus.circle")
                }
                .disabled(!isConnected || isRefreshing)
                .help("Release IRM (channel 0, 84 BW units) - check Console.app")
                
                // Phase 0.5: CMP test buttons
                Button(action: onCMPConnectOPCR) {
                    Label("🔌 oPCR", systemImage: "arrow.right.circle")
                }
                .disabled(!isConnected || isRefreshing)
                .help("CMP Connect oPCR[0] - starts device→host stream")
                
                Button(action: onCMPDisconnectOPCR) {
                    Label("⏏ oPCR", systemImage: "arrow.left.circle")
                }
                .disabled(!isConnected || isRefreshing)
                .help("CMP Disconnect oPCR[0] - stops device→host stream")
                
                // Phase 1.5: IT DMA allocation buttons (no CMP)
                Button(action: onITDMAAllocate) {
                    Label("📤 IT DMA", systemImage: "square.and.arrow.up")
                }
                .disabled(!isConnected || isRefreshing)
                .help("Allocate IT DMA (~2MB) - DMA only, NO CMP iPCR")
                
                Button(action: onITDMADeallocate) {
                    Label("🗑️ IT DMA", systemImage: "trash")
                }
                .disabled(!isConnected || isRefreshing)
                .help("Deallocate IT DMA")
                
                Button(action: onReScan) {
                    Label("Re-scan Bus", systemImage: "arrow.triangle.2.circlepath")
                }
                .disabled(!isConnected || isRefreshing)
                
                Button(action: onRefresh) {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .disabled(!isConnected || isRefreshing)
            }
            
            if let lastRefresh = lastRefresh {
                Text("Last updated: \(lastRefresh.formatted(date: .omitted, time: .standard))")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
        .padding()
        .background(Color(NSColor.controlBackgroundColor))
        .cornerRadius(10)
        .overlay(
            RoundedRectangle(cornerRadius: 10)
                .stroke(Color.secondary.opacity(0.2), lineWidth: 1)
        )
    }
}


struct EmptyStateView: View {
    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "music.note.list")
                .font(.system(size: 48))
                .foregroundColor(.secondary.opacity(0.5))
            Text("No AV/C Units Found")
                .font(.headline)
                .foregroundColor(.secondary)
            Text("Connect a FireWire device to see it here.")
                .font(.caption)
                .foregroundColor(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 60)
    }
}


struct AVCUnitCard: View {
    let unit: ASFWDriverConnector.AVCUnitInfo
    @ObservedObject var viewModel: DebugViewModel
    
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Unit Header
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("GUID: \(unit.guidHex)")
                        .font(.system(.body, design: .monospaced))
                        .fontWeight(.bold)
                    
                    HStack(spacing: 12) {
                        Label("Node: \(unit.nodeIDHex)", systemImage: "network")
                        Label("Vendor: \(String(format: "0x%04X", unit.vendorID))", systemImage: "building.2")
                        Label("Model: \(String(format: "0x%04X", unit.modelID))", systemImage: "tag")
                    }
                    .font(.caption)
                    .foregroundColor(.secondary)
                }
                
                Spacer()
                

                
                StatusBadge(isInitialized: unit.isInitialized)
            }
            .padding()
            .background(Color(NSColor.controlBackgroundColor).opacity(0.5))
            
            // Unit Plugs Section
            if unit.totalIsoPlugs > 0 || unit.totalExtPlugs > 0 {
                Divider()
                UnitPlugsSection(unit: unit)
                    .padding()
                    .background(Color(NSColor.controlBackgroundColor).opacity(0.3))
            }
            
            Divider()
            
            // Subunits
            if unit.subunits.isEmpty {
                Text("No Subunits")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .padding()
            } else {
                VStack(spacing: 1) {
                    ForEach(unit.subunits) { subunit in
                        SubunitRow(unit: unit, subunit: subunit, viewModel: viewModel)
                    }
                }
                .background(Color.secondary.opacity(0.1)) // Separator color
            }
        }
        .background(Color(NSColor.controlBackgroundColor))
        .cornerRadius(12)
        .shadow(color: .black.opacity(0.1), radius: 4, x: 0, y: 2)
    }
}


struct SubunitRow: View {
    let unit: ASFWDriverConnector.AVCUnitInfo
    let subunit: ASFWDriverConnector.AVCSubunitInfo
    @ObservedObject var viewModel: DebugViewModel
    
    @State private var isExpanded = false
    @State private var capabilities: ASFWDriverConnector.AVCMusicCapabilities?
    @State private var isLoading = false
    
    private var color: Color {
        switch subunit.accentColor {
        case "blue": return .blue
        case "purple": return .purple
        case "orange": return .orange
        default: return .gray
        }
    }
    
    var body: some View {
        VStack(spacing: 0) {
            Button {
                withAnimation(.spring(response: 0.3, dampingFraction: 0.7)) {
                    isExpanded.toggle()
                }
            } label: {
                HStack(spacing: 12) {
                    // Icon
                    ZStack {
                        RoundedRectangle(cornerRadius: 8)
                            .fill(color.opacity(0.15))
                            .frame(width: 36, height: 36)
                        Image(systemName: subunit.symbolName)
                            .foregroundColor(color)
                    }
                    
                    VStack(alignment: .leading, spacing: 2) {
                        Text(subunit.typeName)
                            .font(.subheadline.bold())
                        Text("ID: \(subunit.subunitID)")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                    }
                    
                    Spacer()
                

                    
                    // Plug Badges
                    HStack(spacing: 8) {
                        PlugBadge(count: subunit.numDestPlugs, isInput: true)
                        PlugBadge(count: subunit.numSrcPlugs, isInput: false)
                    }
                    
                    Image(systemName: "chevron.right")
                        .font(.caption.bold())
                        .foregroundColor(.secondary)
                        .rotationEffect(.degrees(isExpanded ? 90 : 0))
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 12)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .background(Color(NSColor.controlBackgroundColor))
            
            if isExpanded {
                Divider()
                    .padding(.leading, 64)
                
                if isMusicSubunit {
                    SubunitCapabilitiesView(
                        viewModel: viewModel,
                        unit: unit,
                        subunit: subunit,
                        capabilities: capabilities,
                        isLoading: isLoading
                    )
                    .padding(.leading, 64)
                    .padding(.trailing, 16)
                    .padding(.vertical, 16)
                    .background(Color(NSColor.controlBackgroundColor).opacity(0.5))
                    .transition(.opacity.combined(with: .move(edge: .top)))
                    .task {
                        if capabilities == nil {
                            await fetchCapabilities()
                        }
                    }
                } else {
                    Text("No detailed capabilities available for this subunit type.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .padding(16)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color(NSColor.controlBackgroundColor).opacity(0.5))
                }
            }
        }
    }
    
    private var isMusicSubunit: Bool {
        return subunit.type == 0x1C || subunit.type == 0x0C
    }
    
    private func fetchCapabilities() async {
        isLoading = true
        capabilities = await viewModel.getSubunitCapabilities(
            guid: unit.guid,
            type: subunit.type,
            id: subunit.subunitID
        )
        isLoading = false
    }
}



struct PlugBadge: View {
    let count: UInt8
    let isInput: Bool
    
    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: isInput ? "arrow.down.circle.fill" : "arrow.up.circle.fill")
                .font(.system(size: 10))
            Text("\(count)")
                .font(.system(size: 11, design: .monospaced).bold())
        }
        .foregroundColor(isInput ? .blue : .green)
        .padding(.horizontal, 6)
        .padding(.vertical, 3)
        .background(
            Capsule()
                .fill(isInput ? Color.blue.opacity(0.1) : Color.green.opacity(0.1))
        )
    }
}



// Reusing existing SubunitCapabilitiesView but adapted for inline
// (Assuming SubunitCapabilitiesView logic is mostly fine, just needs minor tweaks if any)
// I'll include the full definition to ensure it works with the new layout.

struct SubunitCapabilitiesView: View {
    @ObservedObject var viewModel: DebugViewModel
    let unit: ASFWDriverConnector.AVCUnitInfo
    let subunit: ASFWDriverConnector.AVCSubunitInfo
    let capabilities: ASFWDriverConnector.AVCMusicCapabilities?
    let isLoading: Bool
    
    @State private var isExporting = false
    @State private var descriptorData: Data?
    @State private var showFileExporter = false
    
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            if isLoading {
                HStack {
                    ProgressView().controlSize(.small)
                    Text("Fetching capabilities...")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            } else if let caps = capabilities {
                
                // 1. Global Clock / Sample Rate
                GlobalClockView(caps: caps)
                
                Divider()
                
                // 2. Plugs (Grouped by Type and Direction)
                VStack(alignment: .leading, spacing: 16) {
                    
                    // Audio Dest Plugs - Isoch stream → Device (for playback to outputs)
                    let audioInputs = caps.plugs.filter { $0.isInput && $0.type == 0x00 }
                    if !audioInputs.isEmpty {
                        PlugSection(title: "Audio: Isoch → Device (Dest Plugs)", icon: "arrow.right.circle.fill", color: .blue, plugs: audioInputs, channels: caps.channels)
                    }
                    
                    // Audio Source Plugs - Device → Isoch stream (for recording from inputs)
                    let audioOutputs = caps.plugs.filter { !$0.isInput && $0.type == 0x00 }
                    if !audioOutputs.isEmpty {
                        PlugSection(title: "Audio: Device → Isoch (Source Plugs)", icon: "arrow.left.circle.fill", color: .green, plugs: audioOutputs, channels: caps.channels)
                    }
                    
                    // MIDI Dest Plugs
                    let midiInputs = caps.plugs.filter { $0.isInput && $0.type == 0x01 }
                    if !midiInputs.isEmpty {
                        PlugSection(title: "MIDI: Isoch → Device", icon: "pianokeys", color: .orange, plugs: midiInputs, channels: caps.channels)
                    }
                    
                    // MIDI Source Plugs
                    let midiOutputs = caps.plugs.filter { !$0.isInput && $0.type == 0x01 }
                    if !midiOutputs.isEmpty {
                        PlugSection(title: "MIDI: Device → Isoch", icon: "pianokeys", color: .orange, plugs: midiOutputs, channels: caps.channels)
                    }
                    
                    // Sync Dest Plugs
                    let otherInputs = caps.plugs.filter { $0.isInput && $0.type != 0x00 && $0.type != 0x01 }
                    if !otherInputs.isEmpty {
                        PlugSection(title: "Sync: Isoch → Device", icon: "clock.arrow.circlepath", color: .purple, plugs: otherInputs, channels: caps.channels)
                    }

                    // Sync Source Plugs
                    let otherOutputs = caps.plugs.filter { !$0.isInput && $0.type != 0x00 && $0.type != 0x01 }
                    if !otherOutputs.isEmpty {
                        PlugSection(title: "Sync: Device → Isoch", icon: "clock.arrow.circlepath", color: .purple, plugs: otherOutputs, channels: caps.channels)
                    }
                }

                Divider()
                
                // Dump Button
                HStack {
                    Spacer()
                

                    Button(action: dumpDescriptor) {
                        if isExporting {
                            ProgressView().controlSize(.small)
                        } else {
                            Label("Dump Descriptor", systemImage: "arrow.down.doc")
                        }
                    }
                    .disabled(isExporting)
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                }
                
            } else {
                Text("Failed to load capabilities")
                    .font(.caption)
                    .foregroundColor(.red)
            }
        }
        .fileExporter(
            isPresented: $showFileExporter,
            document: BinaryFileDocument(initialData: descriptorData ?? Data()),
            contentType: .data,
            defaultFilename: "MusicSubunitDescriptor.bin"
        ) { result in
            if case .failure(let error) = result {
                print("Failed to save: \(error.localizedDescription)")
            }
        }
    }
    
    private func dumpDescriptor() {
        isExporting = true
        DispatchQueue.global(qos: .userInitiated).async {
            let data = viewModel.connector.getSubunitDescriptor(
                guid: unit.guid,
                type: subunit.type,
                id: subunit.subunitID
            )
            DispatchQueue.main.async {
                self.isExporting = false
                if let data = data, !data.isEmpty {
                    self.descriptorData = data
                    self.showFileExporter = true
                }
            }
        }
    }
}

struct PlugSection: View {
    let title: String
    var icon: String = "circle.fill"
    var color: Color = .secondary
    let plugs: [ASFWDriverConnector.AVCMusicCapabilities.PlugInfo]
    let channels: [ASFWDriverConnector.AVCMusicCapabilities.MusicChannel] // Kept for backward compat
    
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: icon)
                    .font(.caption)
                    .foregroundColor(color)
                Text(title)
                    .font(.caption.bold())
                    .foregroundColor(.secondary)
            }
            
            ForEach(plugs) { plug in
                PlugTreeRow(plug: plug)
            }
        }
    }
}

struct GlobalClockView: View {
    let caps: ASFWDriverConnector.AVCMusicCapabilities
    
    private var currentRateString: String {
        formatRate(caps.currentRate)
    }
    
    private func formatRate(_ rate: UInt8) -> String {
        // Matches driver SampleRate enum in StreamFormatTypes.hpp
        switch rate {
        case 0x00: return "22.05"
        case 0x01: return "24"
        case 0x02: return "32"
        case 0x03: return "44.1"
        case 0x04: return "48"
        case 0x05: return "96"
        case 0x06: return "176.4"
        case 0x07: return "192"
        case 0x0A: return "88.2"
        case 0x0F: return "Don't Care"
        case 0xFF: return "Unknown"
        default: return String(format: "0x%02X", rate)
        }
    }
    
    var body: some View {
        HStack(alignment: .top, spacing: 20) {
            // Current Rate (Large)
            VStack(alignment: .leading, spacing: 4) {
                Text("Sample Rate")
                    .font(.caption)
                    .foregroundColor(.secondary)
                Text("\(currentRateString) kHz")
                    .font(.title2)
                    .fontWeight(.bold)
                    .monospacedDigit()
            }
            .padding(12)
            .background(Color.secondary.opacity(0.1))
            .cornerRadius(8)
            
            // Supported Rates (List)
            VStack(alignment: .leading, spacing: 4) {
                Text("Supported Rates").font(.caption).foregroundColor(.secondary)
                
                HStack(spacing: 8) {
                    // Iterate over known rate codes sorted by frequency (not enum order)
                    // 0x00=22.05, 0x01=24, 0x02=32, 0x03=44.1, 0x04=48, 0x0A=88.2, 0x05=96, 0x06=176.4, 0x07=192
                    let rateCodes: [UInt8] = [0x00, 0x01, 0x02, 0x03, 0x04, 0x0A, 0x05, 0x06, 0x07]
                    ForEach(rateCodes, id: \.self) { rateVal in
                        let isSupported = (caps.supportedRatesMask & (UInt32(1) << UInt32(rateVal))) != 0
                        if isSupported {
                            Text(formatRate(rateVal))
                                .font(.caption.monospaced())
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(caps.currentRate == rateVal ? Color.blue : Color.secondary.opacity(0.1))
                                .foregroundColor(caps.currentRate == rateVal ? .white : .primary)
                                .cornerRadius(4)
                        }
                    }
                }
            }
            .padding(.top, 4)
            
            Spacer()
        }
    }
}



struct PlugTreeRow: View {
    let plug: ASFWDriverConnector.AVCMusicCapabilities.PlugInfo
    
    @State private var isExpanded = true
    
    /// Get all channels from all signal blocks (flattened)
    private var allChannels: [ASFWDriverConnector.AVCMusicCapabilities.ChannelDetail] {
        plug.signalBlocks.flatMap { $0.channels }
    }
    
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Plug Header
            HStack {
                Button {
                    withAnimation { isExpanded.toggle() }
                } label: {
                    Image(systemName: "chevron.right")
                        .rotationEffect(.degrees(isExpanded ? 90 : 0))
                        .font(.caption2)
                        .foregroundColor(.secondary)
                        .frame(width: 16)
                }
                .buttonStyle(.plain)
                
                Text(plug.name.isEmpty ? "Plug \(plug.plugID)" : plug.name)
                    .font(.body)
                    .fontWeight(.medium)
                
                if !plug.name.isEmpty {
                    Text("Plug \(plug.plugID)")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .monospacedDigit()
                }
                
                Spacer()
                
                // Type Badge
                Text(plug.typeName)
                    .font(.system(size: 10, weight: .bold))
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(Color.secondary.opacity(0.1))
                    .cornerRadius(4)
                    .foregroundColor(.secondary)
                
                // Structure Badges (signal blocks)
                HStack(spacing: 4) {
                    if plug.signalBlocks.isEmpty {
                        Text("No Format Info")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Color.secondary.opacity(0.05))
                            .cornerRadius(4)
                    } else {
                        ForEach(plug.signalBlocks) { block in
                            Text("\(block.channelCount)x \(block.formatCodeName)")
                                .font(.caption2.bold())
                                .foregroundColor(.white)
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(Color.blue.opacity(0.6))
                                .cornerRadius(4)
                        }
                    }
                }
            }
            .padding(8)
            .background(Color.secondary.opacity(0.05))
            .cornerRadius(6)
            
            // Channel List (from nested signal blocks)
            if isExpanded && !allChannels.isEmpty {
                VStack(alignment: .leading, spacing: 1) {
                    ForEach(allChannels) { channel in
                        HStack {
                            // Indentation line
                            Rectangle()
                                .fill(Color.secondary.opacity(0.2))
                                .frame(width: 1, height: 24)
                                .padding(.leading, 15)
                                .padding(.trailing, 8)
                            
                            Text(channel.name.isEmpty ? "Channel \(channel.position)" : channel.name)
                                .font(.caption)
                            
                            Spacer()
                            
                            // Position badge
                            Text("Pos \(channel.position)")
                                .font(.system(size: 9, weight: .medium))
                                .foregroundColor(.secondary)
                                .padding(.horizontal, 4)
                                .padding(.vertical, 2)
                                .background(Color.secondary.opacity(0.1))
                                .cornerRadius(3)
                            
                                // Music Plug ID
                            Text(String(format: "ID 0x%04X", channel.musicPlugID))
                                .font(.system(size: 9, weight: .medium, design: .monospaced))
                                .foregroundColor(.secondary)
                                .padding(.horizontal, 4)
                                .padding(.vertical, 2)
                                .background(Color.purple.opacity(0.1))
                                .cornerRadius(3)
                        }
                        .frame(height: 28)
                    }
                }
                .padding(.bottom, 8)
            }
            
            // Supported Formats (from 0xBF queries)
            // Deduplicate formats with same rate/format/channels
            let uniqueFormats = plug.supportedFormats.reduce(into: [ASFWDriverConnector.AVCMusicCapabilities.SupportedFormat]()) { result, fmt in
                let isDuplicate = result.contains { existing in
                    existing.sampleRateCode == fmt.sampleRateCode &&
                    existing.formatCode == fmt.formatCode &&
                    existing.channelCount == fmt.channelCount
                }
                if !isDuplicate {
                    result.append(fmt)
                }
            }
            
            if !uniqueFormats.isEmpty {
                DisclosureGroup {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(uniqueFormats) { fmt in
                            HStack(spacing: 8) {
                                Text(fmt.sampleRateName)
                                    .font(.system(size: 11))
                                    .foregroundColor(.primary)
                                Spacer()
                                Text("\(fmt.channelCount)ch")
                                    .font(.system(size: 10, weight: .medium, design: .monospaced))
                                    .foregroundColor(.secondary)
                                Text(fmt.formatCodeName)
                                    .font(.system(size: 10, weight: .medium))
                                    .foregroundColor(.blue)
                            }
                            .padding(.vertical, 2)
                        }
                    }
                } label: {
                    HStack {
                        Text("Supported Formats")
                            .font(.system(size: 11, weight: .medium))
                            .foregroundColor(.secondary)
                        Spacer()
                        Text("\(uniqueFormats.count)")
                            .font(.system(size: 10, weight: .medium))
                            .foregroundColor(.secondary)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Color.secondary.opacity(0.2))
                            .cornerRadius(4)
                    }
                }
                .padding(.top, 4)
            }
        }
    }
}



struct PortCountColumn: View {
    let title: String
    let inCount: UInt8
    let outCount: UInt8
    
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.caption.bold())
            HStack(spacing: 12) {
                VStack(alignment: .leading) {
                    Text("In").font(.caption2).foregroundColor(.secondary)
                    Text("\(inCount)").font(.caption.monospaced())
                }
                VStack(alignment: .leading) {
                    Text("Out").font(.caption2).foregroundColor(.secondary)
                    Text("\(outCount)").font(.caption.monospaced())
                }
            }
        }
    }
}



// Re-using BinaryFileDocument, CapabilityBadge, StatusBadge from previous implementation
// (Assuming they are defined here or I should include them)

struct BinaryFileDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.data] }
    var data: Data
    init(initialData: Data = Data()) { self.data = initialData }
    init(configuration: ReadConfiguration) throws {
        if let data = configuration.file.regularFileContents { self.data = data } else { throw CocoaError(.fileReadCorruptFile) }
    }
    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        return FileWrapper(regularFileWithContents: data)
    }
}



struct CapabilityBadge: View {
    let name: String
    let isSupported: Bool
    var body: some View {
        Text(name)
            .font(.caption.bold())
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(isSupported ? Color.green.opacity(0.1) : Color.secondary.opacity(0.1))
            .foregroundColor(isSupported ? .green : .secondary)
            .cornerRadius(4)
    }
}







struct UnitPlugsSection: View {
    let unit: ASFWDriverConnector.AVCUnitInfo
    
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: "cable.connector")
                    .font(.caption)
                    .foregroundColor(.blue)
                Text("Unit Plugs")
                    .font(.caption.bold())
                    .foregroundColor(.secondary)
            }
            
            HStack(spacing: 16) {
                // Isochronous Plugs
                if unit.totalIsoPlugs > 0 {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Isochronous")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                        
                        HStack(spacing: 12) {
                            UnitPlugBadge(
                                count: unit.isoInputPlugs,
                                label: "In",
                                icon: "arrow.down.circle.fill",
                                color: .blue
                            )
                            
                            UnitPlugBadge(
                                count: unit.isoOutputPlugs,
                                label: "Out",
                                icon: "arrow.up.circle.fill",
                                color: .green
                            )
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(Color.blue.opacity(0.05))
                    .cornerRadius(8)
                }
                
                // External Plugs
                if unit.totalExtPlugs > 0 {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("External")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                        
                        HStack(spacing: 12) {
                            UnitPlugBadge(
                                count: unit.extInputPlugs,
                                label: "In",
                                icon: "arrow.down.circle",
                                color: .purple
                            )
                            
                            UnitPlugBadge(
                                count: unit.extOutputPlugs,
                                label: "Out",
                                icon: "arrow.up.circle",
                                color: .orange
                            )
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(Color.purple.opacity(0.05))
                    .cornerRadius(8)
                }
                
                Spacer()
            }
        }
    }
}

struct UnitPlugBadge: View {
    let count: UInt8
    let label: String
    let icon: String
    let color: Color
    
    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: icon)
                .font(.system(size: 11))
                .foregroundColor(color)
            VStack(alignment: .leading, spacing: 0) {
                Text(label)
                    .font(.system(size: 9))
                    .foregroundColor(.secondary)
                Text("\(count)")
                    .font(.system(size: 13, design: .monospaced).bold())
                    .foregroundColor(color)
            }
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 4)
        .background(color.opacity(0.1))
        .cornerRadius(6)
    }
}

struct StatusBadge: View {
    let isInitialized: Bool
    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: isInitialized ? "checkmark.circle.fill" : "clock.fill")
            Text(isInitialized ? "Ready" : "Pending")
        }
        .font(.caption.bold())
        .foregroundColor(isInitialized ? .green : .orange)
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(isInitialized ? Color.green.opacity(0.1) : Color.orange.opacity(0.1))
        .cornerRadius(12)
    }
}



#Preview {
    AVCDebugView(viewModel: DebugViewModel())
}
