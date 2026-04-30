//
//  OverviewView.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI

struct OverviewView: View {
    @ObservedObject var viewModel: DriverViewModel
    @State private var showUninstallConfirmation = false
    
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
                            .textSelection(.enabled)
                        Spacer()
                    }
                    .padding()
                    .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))
                }
                .padding()
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))

                // Install Card
                VStack(alignment: .leading, spacing: 12) {
                    Label("Driver Install & Repair", systemImage: "puzzlepiece.extension")
                        .font(.headline)

                    HStack(alignment: .top, spacing: 12) {
                        Image(systemName: viewModel.isRunningFromApplications ? "checkmark.seal.fill" : "exclamationmark.triangle.fill")
                            .foregroundStyle(viewModel.isRunningFromApplications ? .green : .orange)
                            .font(.title3)

                        VStack(alignment: .leading, spacing: 6) {
                            Text(viewModel.isRunningFromApplications ? "Ready to request activation" : "Launch ASFW from /Applications before installing")
                                .font(.subheadline.weight(.semibold))
                            Text(viewModel.appBundlePath)
                                .font(.caption.monospaced())
                                .foregroundStyle(.secondary)
                                .lineLimit(2)
                                .truncationMode(.middle)
                                .textSelection(.enabled)
                        }

                        Spacer(minLength: 16)

                        HStack(spacing: 8) {
                            Button {
                                viewModel.installDriver()
                            } label: {
                                Label("Install / Update Driver", systemImage: "arrow.down.circle.fill")
                            }
                            .buttonStyle(.borderedProminent)
                            .disabled(viewModel.isBusy || !viewModel.isRunningFromApplications)

                            Button {
                                viewModel.repairDriver()
                            } label: {
                                Label("Repair Driver", systemImage: "wrench.and.screwdriver.fill")
                            }
                            .buttonStyle(.bordered)
                            .disabled(viewModel.isBusy || !viewModel.canUseMaintenanceHelper)

                            Button(role: .destructive) {
                                showUninstallConfirmation = true
                            } label: {
                                Label("Uninstall", systemImage: "trash.fill")
                            }
                            .buttonStyle(.bordered)
                            .disabled(viewModel.isBusy)
                        }
                    }
                    .padding()
                    .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))

                    Text("Close Logic or other audio apps before Repair or Uninstall. Do one repair attempt before rebooting; do not repeatedly reinstall.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .padding()
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))

                // Maintenance Helper Card
                VStack(alignment: .leading, spacing: 12) {
                    Label("Maintenance Helper", systemImage: "lock.shield")
                        .font(.headline)

                    HStack(alignment: .top, spacing: 12) {
                        Image(systemName: maintenanceHelperIcon)
                            .foregroundStyle(maintenanceHelperColor)
                            .font(.title3)

                        VStack(alignment: .leading, spacing: 6) {
                            Text("Helper: \(viewModel.helperStatus.displayName)")
                                .font(.subheadline.weight(.semibold))
                            Text(viewModel.maintenanceStatus)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .textSelection(.enabled)
                            if let path = viewModel.lastMaintenanceSnapshotPath {
                                Text(path)
                                    .font(.caption.monospaced())
                                    .foregroundStyle(.secondary)
                                    .lineLimit(2)
                                    .truncationMode(.middle)
                                    .textSelection(.enabled)
                            }
                        }

                        Spacer(minLength: 16)

                        HStack(spacing: 8) {
                            Button {
                                viewModel.enableMaintenanceHelper()
                            } label: {
                                Label("Enable Helper", systemImage: "key.fill")
                            }
                            .buttonStyle(.bordered)
                            .disabled(viewModel.isBusy || viewModel.helperStatus == .enabled)

                            Button {
                                viewModel.openMaintenanceApprovalSettings()
                            } label: {
                                Label("Approve", systemImage: "gearshape.fill")
                            }
                            .buttonStyle(.bordered)
                            .disabled(viewModel.isBusy || viewModel.helperStatus != .requiresApproval)

                            Button {
                                viewModel.refreshHelperStatus()
                            } label: {
                                Label("Recheck", systemImage: "arrow.clockwise")
                            }
                            .buttonStyle(.bordered)
                            .disabled(viewModel.isBusy)
                        }
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
                    InfoRow(label: "Bundle ID", value: viewModel.driverBundleIdentifier)
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
                Text("💡 You may need to allow the system extension in **System Settings > Privacy & Security** after first activation.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .padding()
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
            }
            .padding()
        }
        .navigationTitle("Overview")
        .onAppear {
            viewModel.refreshHelperStatus()
        }
        .confirmationDialog("Uninstall ASFW Driver?",
                            isPresented: $showUninstallConfirmation,
                            titleVisibility: .visible) {
            Button("Uninstall Driver", role: .destructive) {
                viewModel.uninstallDriver()
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Close Logic or other audio apps first. If macOS reports the driver is terminating for uninstall, reboot before trying another install.")
        }
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

    private var maintenanceHelperIcon: String {
        switch viewModel.helperStatus {
        case .enabled: return "checkmark.shield.fill"
        case .requiresApproval: return "exclamationmark.triangle.fill"
        case .failed, .notFound: return "xmark.octagon.fill"
        case .notRegistered, .unknown: return "lock.shield"
        }
    }

    private var maintenanceHelperColor: Color {
        switch viewModel.helperStatus {
        case .enabled: return .green
        case .requiresApproval: return .orange
        case .failed, .notFound: return .red
        case .notRegistered, .unknown: return .secondary
        }
    }
}
