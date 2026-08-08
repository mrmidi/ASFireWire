//
//  OverviewView.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI

struct OverviewView: View {
    @ObservedObject var viewModel: DriverViewModel
    @Binding var requireNewerBuild: Bool
    
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
                
                // Driver Management Card
                VStack(alignment: .leading, spacing: 12) {
                    Label("Driver Management", systemImage: "circle.hexagonpath")
                        .font(.headline)
                    
                    HStack(spacing: 10) {
                        statusIndicator
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Current status")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                            Text(viewModel.displayedStatus)
                                .font(.system(.body, design: .monospaced))
                        }
                        Spacer()
                    }
                    .padding()
                    .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))

                    HStack(spacing: 12) {
                        Button {
                            viewModel.installDriver(requireNewerBuild: requireNewerBuild)
                        } label: {
                            Label("Install", systemImage: "arrow.down.circle.fill")
                        }
                        .labelStyle(.titleAndIcon)
                        .buttonStyle(.borderedProminent)
                        .disabled(viewModel.isBusy)

                        Button(role: .destructive) {
                            viewModel.uninstallDriver()
                        } label: {
                            Label("Delete", systemImage: "trash.fill")
                        }
                        .labelStyle(.titleAndIcon)
                        .buttonStyle(.bordered)
                        .disabled(viewModel.isBusy)
                    }

                    Divider()

                    Toggle(isOn: $requireNewerBuild) {
                        VStack(alignment: .leading, spacing: 3) {
                            Text("Require a newer build")
                            Text("Only replace an installed driver when the incoming build number is higher.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .toggleStyle(.switch)
                    .disabled(viewModel.isBusy)
                    .help("When enabled, ASFW rejects an equal or older build during installation.")
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
                
                // Build Metadata Card
                if let version = viewModel.driverVersion {
                    VStack(alignment: .leading, spacing: 12) {
                        Label("Build Metadata", systemImage: "hammer.fill")
                            .font(.headline)
                        
                        InfoRow(label: "Version", value: version.semanticVersion)
                        InfoRow(label: "Commit", value: "\(version.gitCommitShort) (\(version.gitBranch))")
                        InfoRow(label: "Built", value: version.buildTimestamp)
                        InfoRow(label: "Host", value: version.buildHost)
                        if version.gitDirty {
                            Text("⚠️ Built with uncommitted changes")
                                .font(.caption)
                                .foregroundStyle(.orange)
                        }
                    }
                    .padding()
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                }
                
                // Help Text
                Text("After the first installation, macOS may ask you to approve ASFW in System Settings > General > Login Items & Extensions > Driver Extensions.")
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
        let isActive = viewModel.isDriverConnected
        let isError = !isActive && viewModel.activationStatus.contains("Error")
        
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
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("Driver status indicator")
            .accessibilityValue(viewModel.displayedStatus)
    }
}
