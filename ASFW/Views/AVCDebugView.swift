//
//  AVCDebugView.swift
//  ASFW
//
//  AV/C debug interface - shows discovered AV/C units
//

import SwiftUI

struct AVCDebugView: View {
    @ObservedObject var viewModel: DebugViewModel
    @State private var avcUnits: [ASFWDriverConnector.AVCUnitInfo] = []
    @State private var isRefreshing = false
    @State private var lastRefresh: Date?
    
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            // Header with connection status
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
                        refreshAVCUnits()
                    } label: {
                        Label("Refresh", systemImage: "arrow.clockwise")
                    }
                    .disabled(!viewModel.isConnected || isRefreshing)
                }
                
                if let lastRefresh = lastRefresh {
                    Text("Last refreshed: \\(lastRefresh.formatted(date: .omitted, time: .standard))")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            } label: {
                Label("Status", systemImage: "info.circle")
                    .font(.headline)
            }
            
            // AV/C Units List
            GroupBox {
                if avcUnits.isEmpty {
                    VStack(spacing: 12) {
                        Image(systemName: "music.note.list")
                            .font(.system(size: 48))
                            .foregroundColor(.secondary)
                        Text("No AV/C units detected")
                            .font(.headline)
                            .foregroundColor(.secondary)
                        Text("Connect an AV/C device to see it here")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 40)
                } else {
                    List(avcUnits) { unit in
                        VStack(alignment: .leading, spacing: 8) {
                            HStack {
                                Text("GUID: \\(unit.guidHex)")
                                    .font(.system(.body, design: .monospaced))
                                    .fontWeight(.semibold)
                                Spacer()
                                StatusBadge(isInitialized: unit.isInitialized)
                            }
                            
                            HStack(spacing: 16) {
                                HStack {
                                    Image(systemName: "network")
                                        .foregroundColor(.secondary)
                                    Text("Node ID: \\(unit.nodeIDHex)")
                                        .font(.system(.caption, design: .monospaced))
                                }
                                
                                HStack {
                                    Image(systemName: "list.bullet")
                                        .foregroundColor(.secondary)
                                    Text("\\(unit.subunitCount) subunit(s)")
                                        .font(.caption)
                                }
                            }
                            .foregroundColor(.secondary)
                        }
                        .padding(.vertical, 4)
                    }
                }
            } label: {
                HStack {
                    Label("AV/C Units", systemImage: "music.note")
                        .font(.headline)
                    Spacer()
                    if !avcUnits.isEmpty {
                        Text("\\(avcUnits.count)")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }
            }
            
            Spacer()
        }
        .padding()
        .navigationTitle("AV/C Units")
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
        DispatchQueue.global(qos: .userInitiated).async {
            let units = viewModel.connector.getAVCUnits() ?? []
            DispatchQueue.main.async {
                self.avcUnits = units
                self.lastRefresh = Date()
                self.isRefreshing = false
            }
        }
    }
}

struct StatusBadge: View {
    let isInitialized: Bool
    
    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: isInitialized ? "checkmark.circle.fill" : "clock.fill")
            Text(isInitialized ? "Initialized" : "Pending")
        }
        .font(.caption)
        .foregroundColor(isInitialized ? .green : .orange)
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(isInitialized ? Color.green.opacity(0.15) : Color.orange.opacity(0.15))
        .cornerRadius(8)
    }
}

#Preview {
    AVCDebugView(viewModel: DebugViewModel())
}
