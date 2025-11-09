//
//  SelfIDDetailView.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI

struct SelfIDDetailView: View {
    let capture: SelfIDCapture
    @Environment(\.dismiss) private var dismiss
    
    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Self-ID Capture")
                        .font(.title2)
                        .fontWeight(.semibold)
                    
                    Text("Generation \(capture.generation) • \(capture.packets.count) nodes")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                
                Spacer()
                
                Button("Done") {
                    dismiss()
                }
            }
            .padding()
            .background(Color(NSColor.controlBackgroundColor))
            
            // Content
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    // Status section
                    statusSection
                    
                    // Raw hex dump
                    hexDumpSection
                    
                    // Interpreted packets
                    packetsSection
                }
                .padding()
            }
        }
        .frame(minWidth: 600, minHeight: 320)
    }
    
    // MARK: - Status Section
    
    private var statusSection: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("Valid:")
                    Spacer()
                    if capture.valid {
                        Label("Yes", systemImage: "checkmark.circle.fill")
                            .foregroundColor(.green)
                    } else {
                        Label("No", systemImage: "xmark.circle.fill")
                            .foregroundColor(.red)
                    }
                }
                
                if capture.crcError {
                    HStack {
                        Text("CRC Error:")
                        Spacer()
                        Label("Detected", systemImage: "exclamationmark.triangle.fill")
                            .foregroundColor(.red)
                    }
                }
                
                if capture.timedOut {
                    HStack {
                        Text("Timeout:")
                        Spacer()
                        Label("Yes", systemImage: "clock.badge.exclamationmark.fill")
                            .foregroundColor(.orange)
                    }
                }
                
                if let error = capture.errorReason {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Error Reason:")
                            .fontWeight(.semibold)
                        Text(error)
                            .font(.caption)
                            .foregroundColor(.red)
                    }
                }
                
                HStack {
                    Text("Quadlet Count:")
                    Spacer()
                    Text("\(capture.rawQuadlets.count)")
                        .fontWeight(.semibold)
                }
                
                HStack {
                    Text("Timestamp:")
                    Spacer()
                    Text("\(capture.captureTimestamp)")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundColor(.secondary)
                }
            }
        } label: {
            Label("Capture Status", systemImage: "info.circle")
                .font(.headline)
        }
    }
    
    // MARK: - Hex Dump Section
    
    private var hexDumpSection: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 4) {
                // Header
                HStack(spacing: 0) {
                    Text("Offset")
                        .frame(width: 80, alignment: .leading)
                    Text("Hex Data")
                        .frame(width: 280, alignment: .leading)
                    Text("ASCII")
                        .frame(width: 120, alignment: .leading)
                }
                .font(.system(.caption, design: .monospaced))
                .fontWeight(.semibold)
                .foregroundColor(.secondary)
                .padding(.bottom, 4)
                
                Divider()
                
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 2) {
                        ForEach(Array(stride(from: 0, to: capture.rawQuadlets.count, by: 4)), id: \.self) { offset in
                            hexDumpRow(offset: offset)
                        }
                    }
                }
                .frame(height: 200)
            }
            .padding(8)
        } label: {
            Label("Raw Self-ID Quadlets (Hex Dump)", systemImage: "terminal")
                .font(.headline)
        }
    }
    
    private func hexDumpRow(offset: Int) -> some View {
        let quadlets = capture.rawQuadlets[offset..<min(offset + 4, capture.rawQuadlets.count)]
        let hexString = quadlets.map { String(format: "%08X ", $0) }.joined()
        let asciiString = quadlets.map { quadletToASCII($0) }.joined()
        
        return HStack(spacing: 0) {
            Text(String(format: "%04X", offset * 4))
                .frame(width: 80, alignment: .leading)
                .foregroundColor(.secondary)
            
            Text(hexString)
                .frame(width: 280, alignment: .leading)
            
            Text(asciiString)
                .frame(width: 120, alignment: .leading)
                .foregroundColor(.secondary)
        }
        .font(.system(.caption, design: .monospaced))
        .padding(.vertical, 1)
        .background(offset / 4 % 2 == 0 ? Color.clear : Color.gray.opacity(0.05))
    }
    
    private func quadletToASCII(_ quadlet: UInt32) -> String {
        let bytes = [
            UInt8((quadlet >> 24) & 0xFF),
            UInt8((quadlet >> 16) & 0xFF),
            UInt8((quadlet >> 8) & 0xFF),
            UInt8(quadlet & 0xFF)
        ]
        return bytes.map { byte in
            (byte >= 32 && byte < 127) ? String(Character(UnicodeScalar(byte))) : "."
        }.joined()
    }
    
    // MARK: - Packets Section
    
    private var packetsSection: some View {
        GroupBox {
            LazyVStack(alignment: .leading, spacing: 12) {
                ForEach(capture.packets) { packet in
                    packetCard(packet)
                }
            }
        } label: {
            Label("Interpreted Self-ID Packets", systemImage: "doc.text")
                .font(.headline)
        }
    }
    
    private func packetCard(_ packet: SelfIDPacket) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            // Header
            HStack {
                Text("Physical ID: \(packet.physicalID)")
                    .font(.system(.body, design: .monospaced))
                    .fontWeight(.semibold)
                
                Spacer()
                
                Text("\(packet.speedMbps) Mbps")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            
            // Flags
            HStack(spacing: 8) {
                flagBadge("Link", active: packet.linkActive, color: .green)
                flagBadge("Contender", active: packet.isContender, color: .blue)
                flagBadge("Reset Init", active: packet.initiatedReset, color: .orange)
            }
            
            // Properties
            VStack(alignment: .leading, spacing: 4) {
                propertyRow("Gap Count", value: "\(packet.gapCount)")
                propertyRow("Power Class", value: powerClassDescription(packet.powerClass))
                propertyRow("Port Count", value: "\(packet.portStates.count)")
            }
            .font(.caption)
            
            // Port states
            if !packet.portStates.isEmpty {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Port States:")
                        .font(.caption)
                        .fontWeight(.semibold)
                    
                    HStack(spacing: 6) {
                        ForEach(Array(packet.portStates.enumerated()), id: \.offset) { index, state in
                            VStack(spacing: 2) {
                                Text(state.icon)
                                    .font(.caption)
                                Text("P\(index)")
                                    .font(.system(size: 9))
                                    .foregroundColor(.secondary)
                            }
                            .help(state.description)
                        }
                    }
                }
            }
            
            // Raw quadlets
            VStack(alignment: .leading, spacing: 2) {
                Text("Raw Quadlets:")
                    .font(.caption)
                    .fontWeight(.semibold)
                
                ForEach(Array(packet.quadlets.enumerated()), id: \.offset) { index, quadlet in
                    Text(String(format: "Q%d: 0x%08X", index, quadlet))
                        .font(.system(.caption, design: .monospaced))
                        .foregroundColor(.secondary)
                }
            }
        }
        .padding()
        .background(Color(NSColor.controlBackgroundColor).opacity(0.5))
        .cornerRadius(8)
    }
    
    private func flagBadge(_ label: String, active: Bool, color: Color) -> some View {
        HStack(spacing: 4) {
            Circle()
                .fill(active ? color : Color.gray.opacity(0.3))
                .frame(width: 8, height: 8)
            Text(label)
                .font(.caption2)
                .foregroundColor(active ? .primary : .secondary)
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 3)
        .background(active ? color.opacity(0.1) : Color.clear)
        .cornerRadius(4)
    }
    
    private func propertyRow(_ label: String, value: String) -> some View {
        HStack {
            Text(label + ":")
                .foregroundColor(.secondary)
            Text(value)
                .fontWeight(.medium)
        }
    }
    
    private func powerClassDescription(_ powerClass: UInt8) -> String {
        let descriptions = [
            "No power", "Self-powered (15W)", "Bus-powered (1.5W)", "Bus-powered (3W)",
            "Bus-powered (6W)", "Self-powered (10W)", "Reserved", "Reserved"
        ]
        return descriptions[Int(min(powerClass, 7))]
    }
}
