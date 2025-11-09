//
//  OverviewView.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI

struct OverviewView: View {
    @ObservedObject var viewModel: DriverViewModel
    
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                // Header
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Image(systemName: "cpu.fill")
                            .font(.system(size: 40))
                            .foregroundStyle(.blue.gradient)
                        
                        VStack(alignment: .leading, spacing: 4) {
                            Text("ASFW DriverKit Extension")
                                .font(.title.bold())
                            Text("FireWire OHCI Host Controller Driver")
                                .font(.subheadline)
                                .foregroundStyle(.secondary)
                        }
                    }
                    
                    Divider()
                }
                .padding()
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                
                // Status Card
                VStack(alignment: .leading, spacing: 12) {
                    Label("Driver Status", systemImage: "circle.hexagonpath")
                        .font(.headline)
                    
                    HStack {
                        statusIndicator
                        Text(viewModel.activationStatus)
                            .font(.system(.body, design: .monospaced))
                        Spacer()
                    }
                    .padding()
                    .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))
                }
                .padding()
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                
                // Info Card
                VStack(alignment: .leading, spacing: 12) {
                    Label("Information", systemImage: "info.circle")
                        .font(.headline)
                    
                    InfoRow(label: "Target Hardware", value: "pci11c1,5901 (Agere FW800)")
                    InfoRow(label: "Protocol", value: "IEEE 1394 OHCI 1.1")
                    InfoRow(label: "Bundle ID", value: "net.mrmidi.ASFW.ASFWDriver")
                }
                .padding()
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                
                // Help Text
                Text("💡 You may need to allow the system extension in **System Settings > Privacy & Security** after first activation.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .padding()
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
            }
            .padding()
        }
        .navigationTitle("Overview")
    }
    
    @ViewBuilder
    private var statusIndicator: some View {
        let isActive = viewModel.activationStatus.contains("result: 0")
        let isError = viewModel.activationStatus.contains("Error")
        
        Circle()
            .fill(viewModel.isBusy ? Color.orange : (isError ? Color.red : (isActive ? Color.green : Color.gray)))
            .frame(width: 12, height: 12)
            .overlay(
                Circle()
                    .stroke(viewModel.isBusy ? Color.orange : (isError ? Color.red : (isActive ? Color.green : Color.gray)), lineWidth: 2)
                    .scaleEffect(viewModel.isBusy ? 1.3 : 1.0)
                    .opacity(viewModel.isBusy ? 0 : 1)
                    .animation(viewModel.isBusy ? .easeInOut(duration: 1.0).repeatForever(autoreverses: true) : .default, value: viewModel.isBusy)
            )
    }
}
