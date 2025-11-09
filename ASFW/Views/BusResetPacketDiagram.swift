//
//  BusResetPacketDiagram.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI

struct BusResetPacketDiagram: View {
    let packet: BusResetPacketSnapshot
    
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            // Packet format diagram header
            Text("AR Request Context Bus Reset Packet Format (OHCI 1.1 § 8.4.2.3)")
                .font(.caption.bold())
                .foregroundStyle(.secondary)
            
            VStack(spacing: 2) {
                // Quadlet 0: tCode and selfIDGeneration
                QuadletRow(
                    quadletNumber: 0,
                    value: packet.wireQuadlets[0],
                    fields: [
                        BitField(name: "tCode", bits: "31-28", value: String(format: "0x%X", packet.tCode), highlight: .blue),
                        BitField(name: "reserved", bits: "27-8", value: "—", highlight: nil),
                        BitField(name: "selfIDGeneration", bits: "7-0", value: String(format: "0x%02X (%d)", packet.generation, packet.generation), highlight: .green)
                    ]
                )
                
                // Quadlet 1: reserved/undefined
                QuadletRow(
                    quadletNumber: 1,
                    value: packet.wireQuadlets[1],
                    fields: [
                        BitField(name: "reserved undefined", bits: "31-0", value: String(format: "0x%08X", packet.wireQuadlets[1]), highlight: nil)
                    ]
                )
                
                // Quadlet 2: reserved + eventCode
                QuadletRow(
                    quadletNumber: 2,
                    value: packet.wireQuadlets[2],
                    fields: [
                        BitField(name: "reserved", bits: "31-5", value: "—", highlight: nil),
                        BitField(name: "eventCode", bits: "4-0", value: String(format: "0x%02X (evt_bus_reset)", packet.eventCode), highlight: .orange)
                    ]
                )
                
                // Quadlet 3: reserved/undefined
                QuadletRow(
                    quadletNumber: 3,
                    value: packet.wireQuadlets[3],
                    fields: [
                        BitField(name: "reserved undefined", bits: "31-0", value: String(format: "0x%08X", packet.wireQuadlets[3]), highlight: nil)
                    ]
                )
            }
            .overlay(
                RoundedRectangle(cornerRadius: 6)
                    .stroke(Color.secondary.opacity(0.3), lineWidth: 1)
            )
            
            // Validation indicators
            VStack(alignment: .leading, spacing: 8) {
                ValidationRow(
                    label: "tCode == 0xE (PHY packet)",
                    isValid: packet.tCode == 0xE,
                    value: String(format: "0x%X", packet.tCode)
                )
                
                ValidationRow(
                    label: "eventCode == 0x09 (evt_bus_reset)",
                    isValid: packet.eventCode == 0x09,
                    value: String(format: "0x%02X", packet.eventCode)
                )
                
                if !packet.contextInfo.isEmpty {
                    HStack {
                        Image(systemName: "info.circle")
                            .foregroundStyle(.blue)
                            .imageScale(.small)
                        Text("Context: " + packet.contextInfo)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .padding(.vertical, 8)
            
            // Raw packet data
            RawPacketView(wireQuadlets: packet.wireQuadlets)
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }
}

struct RawPacketView: View {
    let wireQuadlets: [UInt32]
    
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("Raw Packet Data", systemImage: "doc.text.fill")
                .font(.caption.bold())
                .foregroundStyle(.secondary)
            
            // Hex dump with ASCII
            VStack(alignment: .leading, spacing: 2) {
                ForEach(0..<wireQuadlets.count, id: \.self) { index in
                    HexDumpRow(quadlet: wireQuadlets[index], offset: index * 4, index: index)
                }
            }
            .overlay(
                RoundedRectangle(cornerRadius: 4)
                    .stroke(Color.secondary.opacity(0.2), lineWidth: 1)
            )
        }
        .padding(.top, 8)
    }
}

struct HexDumpRow: View {
    let quadlet: UInt32
    let offset: Int
    let index: Int
    
    var body: some View {
        HStack(spacing: 12) {
            // Offset
            Text(String(format: "%04X:", offset))
                .font(.caption2.monospaced())
                .foregroundStyle(.tertiary)
                .frame(width: 45, alignment: .leading)
            
            // Hex bytes
            hexBytes
            
            // Separator
            Text("|")
                .font(.caption2.monospaced())
                .foregroundStyle(.tertiary)
            
            // ASCII representation
            asciiRepresentation
            
            Spacer()
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 2)
        .background(index % 2 == 0 ? Color.clear : Color.secondary.opacity(0.05))
    }
    
    @ViewBuilder
    private var hexBytes: some View {
        HStack(spacing: 4) {
            ForEach(0..<4, id: \.self) { byteIndex in
                let byte = UInt8((quadlet >> (24 - byteIndex * 8)) & 0xFF)
                Text(String(format: "%02X", byte))
                    .font(.caption.monospaced())
                    .foregroundStyle(.primary)
            }
        }
    }
    
    @ViewBuilder
    private var asciiRepresentation: some View {
        HStack(spacing: 0) {
            ForEach(0..<4, id: \.self) { byteIndex in
                let byte = UInt8((quadlet >> (24 - byteIndex * 8)) & 0xFF)
                let char = (byte >= 32 && byte <= 126) ? String(UnicodeScalar(byte)) : "."
                Text(char)
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
            }
        }
    }
}

struct BitField {
    let name: String
    let bits: String
    let value: String
    let highlight: Color?
}

struct QuadletRow: View {
    let quadletNumber: Int
    let value: UInt32
    let fields: [BitField]
    
    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Text("Quadlet \(quadletNumber)")
                    .font(.caption2.bold().monospaced())
                    .foregroundStyle(.secondary)
                Spacer()
                Text(String(format: "0x%08X", value))
                    .font(.caption2.monospaced())
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(.quaternary.opacity(0.5))
            
            // Bit fields
            VStack(spacing: 1) {
                ForEach(fields.indices, id: \.self) { index in
                    BitFieldRow(field: fields[index])
                }
            }
        }
    }
}

struct BitFieldRow: View {
    let field: BitField
    
    var body: some View {
        HStack(spacing: 8) {
            // Field name
            Text(field.name)
                .font(.caption.monospaced())
                .frame(width: 150, alignment: .leading)
            
            // Bit range
            Text(field.bits)
                .font(.caption2.monospaced())
                .foregroundStyle(.secondary)
                .frame(width: 60, alignment: .center)
            
            Spacer()
            
            // Value
            Text(field.value)
                .font(.caption.monospaced())
                .foregroundStyle(field.highlight ?? .primary)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(field.highlight?.opacity(0.1) ?? Color.clear)
    }
}

struct ValidationRow: View {
    let label: String
    let isValid: Bool
    let value: String
    
    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: isValid ? "checkmark.circle.fill" : "xmark.circle.fill")
                .foregroundStyle(isValid ? .green : .red)
                .imageScale(.small)
            
            Text(label)
                .font(.caption)
            
            Spacer()
            
            Text(value)
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
        }
    }
}
