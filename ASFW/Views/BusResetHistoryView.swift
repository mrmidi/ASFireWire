//
//  BusResetHistoryView.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI

struct BusResetHistoryView: View {
    @ObservedObject var viewModel: DebugViewModel
    @State private var selectedPacket: BusResetPacketSnapshot?
    
    var body: some View {
        VStack(spacing: 0) {
            if viewModel.isConnected {
                if viewModel.busResetHistory.isEmpty {
                    ContentUnavailableView(
                        "No Bus Resets Captured",
                        systemImage: "bolt.slash",
                        description: Text("Bus reset packets will appear here as they occur")
                    )
                } else {
                    List(viewModel.busResetHistory, id: \.captureTimestamp, selection: $selectedPacket) { packet in
                        BusResetPacketRow(packet: packet)
                            .tag(packet)
                    }
                }
            } else {
                ContentUnavailableView(
                    "Not Connected",
                    systemImage: "cable.connector.slash",
                    description: Text("Connect to the driver to view bus reset history")
                )
            }
        }
        .navigationTitle("Bus Reset History")
    }
}

struct BusResetPacketRow: View {
    let packet: BusResetPacketSnapshot
    
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            // Header
            HStack {
                Image(systemName: "bolt.fill")
                    .foregroundStyle(.orange)
                    .imageScale(.small)
                
                Text("Generation \(packet.generation)")
                    .font(.headline)
                
                Spacer()
                
                Text(formatTimestamp(packet.captureTimestamp))
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
            }
            
            // OHCI Spec-compliant packet diagram (always visible)
            BusResetPacketDiagram(packet: packet)
        }
        .padding(.vertical, 8)
    }
    
    private func formatTimestamp(_ ts: UInt64) -> String {
        let seconds = Double(ts) / 1_000_000_000.0
        return String(format: "%.3fs", seconds)
    }
}
