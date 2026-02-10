//
//  AudioDebugView.swift
//  ASFW
//
//  Created for ASFireWire Audio Debugging.
//

import SwiftUI
import CoreAudio

struct AudioDebugView: View {
    @StateObject private var viewModel = AudioDebugViewModel()
    
    var body: some View {
        HSplitView {
            // Left Pane: Device List
            VStack(spacing: 0) {
                List(viewModel.devices, id: \.id, selection: $viewModel.selectedDevice) { device in
                    HStack(spacing: 8) {
                        Image(systemName: device.transportType.iconName)
                            .foregroundStyle(.blue)
                            .frame(width: 20)
                        VStack(alignment: .leading, spacing: 2) {
                            HStack(spacing: 4) {
                                Text(device.name)
                                    .font(.headline)
                                    .lineLimit(1)
                                if device.isDefaultInput {
                                    Image(systemName: "mic.fill")
                                        .font(.caption2)
                                        .foregroundStyle(.green)
                                }
                                if device.isDefaultOutput {
                                    Image(systemName: "speaker.fill")
                                        .font(.caption2)
                                        .foregroundStyle(.blue)
                                }
                            }
                            Text(device.uid)
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                        }
                    }
                    .tag(device)
                    .listRowInsets(EdgeInsets(top: 6, leading: 8, bottom: 6, trailing: 8))
                }
                .listStyle(.sidebar)
                
                Divider()
                
                HStack {
                    Button(action: viewModel.refreshDevices) {
                        Label("Refresh", systemImage: "arrow.clockwise")
                    }
                    .buttonStyle(.borderless)
                    .padding(8)
                    Spacer()
                }
                .background(Color(nsColor: .controlBackgroundColor))
            }
            .frame(minWidth: 200, maxWidth: 320)
            
            // Right Pane: Main Content
            if let device = viewModel.selectedDevice {
                ScrollView {
                    VStack(alignment: .leading, spacing: 24) {
                        DeviceHeaderView(device: device)
                        
                        Divider()
                        
                        // Input Streams Section
                        if !device.inputStreams.isEmpty {
                            Text("Input Streams (\(device.inputStreams.count))")
                                .font(.title2)
                                .bold()
                            
                            ForEach(device.inputStreams, id: \.id) { stream in
                                StreamCardView(stream: stream, isInput: true)
                            }
                        }
                        
                        // Output Streams Section
                        if !device.outputStreams.isEmpty {
                            Text("Output Streams (\(device.outputStreams.count))")
                                .font(.title2)
                                .bold()
                            
                            ForEach(device.outputStreams, id: \.id) { stream in
                                StreamCardView(stream: stream, isInput: false)
                            }
                        }
                        
                        // Available Formats (for first stream, if any)
                        if let firstStream = device.allStreams.first, !firstStream.availablePhysicalFormats.isEmpty {
                            AvailableFormatsSection(formats: firstStream.availablePhysicalFormats)
                        }
                    }
                    .padding()
                }
                .frame(minWidth: 500, maxWidth: .infinity)
                .background(Color(nsColor: .windowBackgroundColor))
            } else {
                ContentUnavailableView(
                    "Select an Audio Device",
                    systemImage: "waveform",
                    description: Text("Choose a device from the sidebar to inspect its properties and streams.")
                )
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .onAppear {
            if viewModel.selectedDevice == nil {
                viewModel.refreshDevices()
            }
        }
    }
}

// MARK: - Subviews

struct DeviceHeaderView: View {
    let device: AudioWrapperDevice
    
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 12) {
                Text(device.name)
                    .font(.system(size: 28, weight: .bold))
                
                if device.isRunning {
                    Badge(text: "Running", color: .green)
                } else {
                    Badge(text: "Idle", color: .gray)
                }
                
                if device.isDefaultInput {
                    Badge(text: "Default Input", color: .green, icon: "mic.fill")
                }
                if device.isDefaultOutput {
                    Badge(text: "Default Output", color: .blue, icon: "speaker.fill")
                }
            }
            
            // Metadata Grid
            Grid(horizontalSpacing: 32, verticalSpacing: 12) {
                GridRow {
                    InfoGridItem(title: "Manufacturer", icon: "building.2", value: device.manufacturer.isEmpty ? "—" : device.manufacturer)
                    InfoGridItem(title: "Transport", icon: device.transportType.iconName, value: device.transportType.name)
                    InfoGridItem(title: "Sample Rate", icon: "waveform.path.ecg", value: "\(Int(device.sampleRate)) Hz")
                }
                GridRow {
                    InfoGridItem(title: "UID", icon: "fingerprint", value: device.uid)
                    InfoGridItem(title: "Model UID", icon: "tag", value: device.modelUID.isEmpty ? "—" : device.modelUID)
                    InfoGridItem(title: "Channels", icon: "cable.connector", value: "\(device.inputChannelCount) In / \(device.outputChannelCount) Out")
                }
                GridRow {
                    let range = device.bufferFrameSizeRange
                    InfoGridItem(title: "Buffer Size", icon: "memorychip", value: "\(device.bufferFrameSize) frames (\(range.min)–\(range.max))")
                    InfoGridItem(title: "Device Latency (Out/In)", icon: "clock", value: "\(device.outputDeviceLatency) / \(device.inputDeviceLatency) frames")
                    InfoGridItem(title: "Safety Offset (Out/In)", icon: "shield", value: "\(device.outputSafetyOffset) / \(device.inputSafetyOffset) frames")
                }
            }
            .padding(16)
            .background(.thinMaterial)
            .clipShape(RoundedRectangle(cornerRadius: 12))
        }
    }
}

struct Badge: View {
    let text: String
    let color: Color
    var icon: String? = nil
    
    var body: some View {
        HStack(spacing: 4) {
            if let icon = icon {
                Image(systemName: icon)
                    .font(.caption2)
            }
            Text(text)
                .font(.caption.bold())
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(color.opacity(0.2))
        .foregroundStyle(color)
        .clipShape(Capsule())
    }
}

struct InfoGridItem: View {
    let title: String
    let icon: String
    let value: String
    
    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: icon)
                .font(.title3)
                .foregroundStyle(.secondary)
                .frame(width: 24)
            
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(value)
                    .font(.subheadline)
                    .bold()
                    .lineLimit(2)
            }
        }
        .frame(minWidth: 150, alignment: .leading)
    }
}

struct StreamCardView: View {
    let stream: AudioStream
    let isInput: Bool
    
    var accentColor: Color { isInput ? .green : .blue }
    
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            // Header
            HStack {
                Label(isInput ? "Input Stream" : "Output Stream",
                      systemImage: isInput ? "mic.fill" : "speaker.wave.2.fill")
                    .font(.headline)
                    .foregroundStyle(accentColor)
                
                Spacer()
                
                Text(stream.terminalType)
                    .font(.caption.bold())
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(Color.secondary.opacity(0.1))
                    .clipShape(Capsule())
                
                Text("ID: \(stream.id)")
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
            }
            
            Divider()
            
            // Formats
            HStack(alignment: .top, spacing: 20) {
                if let phys = stream.physicalFormat {
                    FormatDetailView(title: "Physical Format", asbd: phys, highlight: true)
                }
                if let virt = stream.virtualFormat {
                    FormatDetailView(title: "Virtual Format", asbd: virt, highlight: false)
                }
            }
        }
        .padding()
        .background(
            RoundedRectangle(cornerRadius: 12)
                .fill(Color(nsColor: .controlBackgroundColor))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12)
                .stroke(accentColor.opacity(0.3), lineWidth: 1)
        )
    }
}

struct FormatDetailView: View {
    let title: String
    let asbd: AudioStreamBasicDescription
    let highlight: Bool
    
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.caption)
                .foregroundStyle(.secondary)
                .textCase(.uppercase)
            
            VStack(alignment: .leading, spacing: 4) {
                // Format Name & Data Type
                HStack {
                    Text(asbd.formatName)
                        .font(.subheadline.bold())
                    Text("•")
                        .foregroundStyle(.secondary)
                    Text(asbd.formatFlags.dataTypeString)
                        .font(.subheadline)
                }
                
                // Sample Rate & Bit Depth
                HStack(spacing: 16) {
                    Label("\(Int(asbd.mSampleRate)) Hz", systemImage: "waveform.path.ecg")
                    Label(asbd.mBitsPerChannel > 0 ? "\(asbd.mBitsPerChannel)-bit" : "–", systemImage: "number")
                    Label("\(asbd.mChannelsPerFrame) ch", systemImage: "speaker.wave.2")
                }
                .font(.caption)
                .foregroundStyle(.secondary)
                
                // Flags Badges
                HStack(spacing: 6) {
                    if asbd.isInterleaved {
                        FlagBadge(text: "Interleaved", color: .orange)
                    } else {
                        FlagBadge(text: "Non-Interleaved", color: .purple)
                    }
                    
                    if asbd.formatFlags.isPacked {
                        FlagBadge(text: "Packed", color: .cyan)
                    }
                    
                    FlagBadge(text: asbd.formatFlags.endiannessString, color: .gray)
                }
                
                // Raw Format ID
                Text("Format ID: \(String(format: "0x%08x", asbd.mFormatID))")
                    .font(.caption2.monospaced())
                    .foregroundStyle(.tertiary)
            }
        }
        .padding(12)
        .frame(minWidth: 220, alignment: .leading)
        .background(highlight ? Color.blue.opacity(0.05) : Color.clear)
        .cornerRadius(8)
    }
}

struct FlagBadge: View {
    let text: String
    let color: Color
    
    var body: some View {
        Text(text)
            .font(.caption2)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(color.opacity(0.15))
            .foregroundStyle(color)
            .cornerRadius(4)
    }
}

struct AvailableFormatsSection: View {
    let formats: [AudioStreamRangedDescription]
    @State private var isExpanded = false
    
    // Get unique sample rates for summary
    var uniqueRates: [Float64] {
        Array(Set(formats.map { $0.mFormat.mSampleRate })).sorted()
    }
    
    var body: some View {
        DisclosureGroup(isExpanded: $isExpanded) {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 300))], spacing: 8) {
                ForEach(formats.indices, id: \.self) { idx in
                    let fmt = formats[idx].mFormat
                    HStack {
                        Text(fmt.summary)
                            .font(.caption.monospaced())
                        Spacer()
                    }
                    .padding(8)
                    .background(Color.secondary.opacity(0.05))
                    .cornerRadius(6)
                }
            }
        } label: {
            HStack {
                Text("Available Formats")
                    .font(.title2)
                    .bold()
                Text("(\(formats.count))")
                    .foregroundStyle(.secondary)
                Spacer()
                Text("Rates: \(uniqueRates.map { "\(Int($0))" }.joined(separator: ", "))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

struct ChannelVisualizer: View {
    let count: Int
    let color: Color
    
    var displayCount: Int { min(count, 16) }
    
    var body: some View {
        HStack(spacing: 2) {
            ForEach(0..<displayCount, id: \.self) { i in
                Rectangle()
                    .fill(color.gradient)
                    .frame(width: 20, height: 14)
                    .overlay(
                        Text("\(i+1)")
                            .font(.system(size: 8))
                            .foregroundStyle(.white.opacity(0.8))
                    )
                    .cornerRadius(3)
            }
            if count > 16 {
                Text("+\(count - 16)")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }
}
