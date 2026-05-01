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
                
                // Last action card
                VStack(alignment: .leading, spacing: 12) {
                    Label("Last Driver Action", systemImage: "clock.badge.checkmark")
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

                    if let progress = viewModel.driverOperationProgress {
                        driverOperationProgressView(progress)
                    }
                }
                .padding()
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))

                // Lifecycle Card
                VStack(alignment: .leading, spacing: 12) {
                    Label(lifecycleTitle, systemImage: "checklist")
                        .font(.headline)

                    HStack(alignment: .top, spacing: 12) {
                        Image(systemName: lifecycleIcon)
                            .foregroundStyle(lifecycleColor)
                            .font(.title3)
                            .frame(width: 28)

                        VStack(alignment: .leading, spacing: 6) {
                            Text(viewModel.lifecycleStatus.summary)
                                .font(.subheadline.weight(.semibold))
                            Text(viewModel.lifecycleStatus.detail)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .textSelection(.enabled)

                            Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 6) {
                                lifecycleRow("Status", lifecyclePlainStatus)
                                lifecycleRow("Driver", yesNo(viewModel.lifecycleStatus.activeDriver))
                                lifecycleRow("Driver Service", viewModel.lifecycleStatus.driverServiceLoaded ? "Loaded" : "Not loaded")
                                lifecycleRow("ASFW Audio Nub", yesNo(viewModel.lifecycleStatus.audioNubVisible))
                                lifecycleRow(coreAudioLifecycleLabel, coreAudioLifecycleValue)
                                lifecycleRow("Debug Tools", viewModel.userClientConnected ? "Connected" : "Not connected")
                                lifecycleRow("Action", viewModel.lifecycleStatus.recommendedAction.displayName)
                                lifecycleRow("Installed driver matches app", installedDriverMatchesAppText)
                                lifecycleRow("Checked", lifecycleCheckedText)
                            }
                            .padding(.top, 4)
                        }

                        Spacer(minLength: 16)

                        VStack(alignment: .trailing, spacing: 8) {
                            if viewModel.isLifecycleRefreshing {
                                HStack(spacing: 6) {
                                    ProgressView()
                                        .controlSize(.small)
                                    Text(viewModel.isConfirmingDriverMismatch ? "Confirming driver state..." : "Checking driver state...")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                            }

                            recommendedActionControl

                            HStack(spacing: 8) {
                                Button {
                                    viewModel.refreshLifecycleStatus()
                                } label: {
                                    Label("Recheck", systemImage: "arrow.clockwise")
                                }
                                .buttonStyle(.bordered)
                                .disabled(viewModel.isBusy || viewModel.isLifecycleRefreshing)

                                Button {
                                    viewModel.copyLifecycleSummary()
                                } label: {
                                    Label("Copy", systemImage: "doc.on.doc")
                                }
                                .buttonStyle(.bordered)
                                .disabled(viewModel.lifecycleStatus.health == .unknown)
                            }
                        }
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
                            Text(driverInstallPurposeText)
                                .font(.caption)
                                .foregroundStyle(.secondary)
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
                            .disabled(!viewModel.canInstallOrUpdateDriver)

                            Button {
                                viewModel.repairDriver()
                            } label: {
                                Label("Repair Driver", systemImage: "wrench.and.screwdriver.fill")
                            }
                            .buttonStyle(.bordered)
                            .disabled(viewModel.isBusy || viewModel.isLifecycleRefreshing || !viewModel.canRepairDriver)

                            Button(role: .destructive) {
                                showUninstallConfirmation = true
                            } label: {
                                Label("Uninstall", systemImage: "trash.fill")
                            }
                            .buttonStyle(.bordered)
                            .disabled(!viewModel.canUninstallDriver)
                        }
                    }
                    .padding()
                    .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))

                    Text(repairGuidance)
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

                        VStack(alignment: .trailing, spacing: 8) {
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

                            HStack(spacing: 8) {
                                Button {
                                    viewModel.captureDiagnostics()
                                } label: {
                                    Label("Diagnostics", systemImage: "doc.text.magnifyingglass")
                                }
                                .buttonStyle(.bordered)
                                .disabled(viewModel.isBusy || !viewModel.canUseMaintenanceHelper)

                                Button {
                                    viewModel.openLastMaintenanceSnapshot()
                                } label: {
                                    Label("Open Snapshot", systemImage: "folder")
                                }
                                .buttonStyle(.bordered)
                                .disabled(viewModel.lastMaintenanceSnapshotPath == nil)

                                Button {
                                    viewModel.copyLastMaintenanceSnapshotPath()
                                } label: {
                                    Label("Copy Path", systemImage: "doc.on.doc")
                                }
                                .buttonStyle(.bordered)
                                .disabled(viewModel.lastMaintenanceSnapshotPath == nil)
                            }
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
                    
                    InfoRow(label: "Target Hardware", value: "FireWire OHCI controller")
                    InfoRow(label: "Protocol", value: "IEEE 1394 OHCI 1.1")
                    InfoRow(label: "Bundle ID", value: viewModel.driverBundleIdentifier)

                    Button {
                        viewModel.openAudioMIDISetup()
                    } label: {
                        Label("Open Audio MIDI Setup", systemImage: "speaker.wave.2.fill")
                    }
                    .buttonStyle(.bordered)
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
                            Text("Built with uncommitted changes")
                                .font(.caption)
                                .foregroundStyle(.orange)
                        }
                    }
                    .padding()
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                }
                
                // Help Text
                Text("You may need to allow the system extension in **System Settings > Privacy & Security** after first activation. After the driver is installed, ASFW can be closed; audio does not require the app to stay open. Closing the app only disconnects debug tabs.")
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
        let isError = viewModel.activationStatus.contains("Error")
            || viewModel.activationStatus.localizedCaseInsensitiveContains("reboot required")
        let isWarning = viewModel.activationStatus.localizedCaseInsensitiveContains("approval required")
            || viewModel.activationStatus.localizedCaseInsensitiveContains("repair needed")
            || viewModel.activationStatus.localizedCaseInsensitiveContains("reconnect")
            || viewModel.activationStatus.localizedCaseInsensitiveContains("required")
        let isComplete = viewModel.activationStatus.localizedCaseInsensitiveContains("completed")
            || viewModel.activationStatus.localizedCaseInsensitiveContains("accepted")
            || viewModel.activationStatus.localizedCaseInsensitiveContains("active")
            || viewModel.activationStatus.localizedCaseInsensitiveContains("working")
            || viewModel.activationStatus.localizedCaseInsensitiveContains("copied")
            || viewModel.activationStatus.localizedCaseInsensitiveContains("captured")
            || viewModel.activationStatus.localizedCaseInsensitiveContains("refreshed")
        let color: Color = viewModel.isBusy ? .orange : (isError ? .red : (isWarning ? .orange : (isComplete ? .green : .gray)))
        
        Circle()
            .fill(color)
            .frame(width: 12, height: 12)
            .overlay(
                Circle()
                    .stroke(color, lineWidth: 2)
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

    private var lifecycleIcon: String {
        if viewModel.isLifecycleRefreshing {
            return "arrow.triangle.2.circlepath"
        }
        switch viewModel.lifecycleStatus.health {
        case .clean: return "checkmark.circle.fill"
        case .repairNeeded: return "wrench.and.screwdriver.fill"
        case .rebootRequired: return "restart.circle.fill"
        case .uninstalled: return "arrow.down.circle.fill"
        case .unknown: return "questionmark.circle.fill"
        }
    }

    private var lifecycleColor: Color {
        if viewModel.isLifecycleRefreshing && viewModel.lifecycleStatus.health != .clean {
            return .blue
        }
        switch viewModel.lifecycleStatus.health {
        case .clean: return .green
        case .repairNeeded: return .orange
        case .rebootRequired: return .red
        case .uninstalled, .unknown: return .secondary
        }
    }

    private var recommendedActionIcon: String {
        switch viewModel.lifecycleStatus.recommendedAction {
        case .none: return "checkmark.circle"
        case .moveAppToApplications: return "folder"
        case .installOrUpdateDriver: return "arrow.down.circle.fill"
        case .enableHelper: return "key.fill"
        case .approveHelper: return "gearshape.fill"
        case .repairOnce: return "wrench.and.screwdriver.fill"
        case .reboot: return "restart.circle.fill"
        case .reconnectDevice: return "cable.connector"
        case .captureDiagnostics, .sendDiagnostics: return "doc.text.magnifyingglass"
        }
    }

    @ViewBuilder
    private var recommendedActionControl: some View {
        if viewModel.isConfirmingDriverMismatch {
            Label("Checking", systemImage: "arrow.clockwise")
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(.secondary)
        } else if viewModel.lifecycleStatus.recommendedAction.isDirectlyActionableInApp {
            Button {
                viewModel.performRecommendedAction()
            } label: {
                Label(viewModel.lifecycleStatus.recommendedAction.buttonTitle,
                      systemImage: recommendedActionIcon)
            }
            .buttonStyle(.borderedProminent)
            .disabled(!viewModel.canPerformRecommendedAction)
        } else if viewModel.lifecycleStatus.recommendedAction != .none {
            VStack(alignment: .trailing, spacing: 6) {
                Label(viewModel.lifecycleStatus.recommendedAction.displayName,
                      systemImage: recommendedActionIcon)
                    .font(.subheadline.weight(.semibold))
                Text(manualActionText)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.trailing)
                    .frame(maxWidth: 240, alignment: .trailing)
            }
        } else {
            Label(viewModel.lifecycleStatus.health == .clean ? "Working" : "No Action Needed",
                  systemImage: recommendedActionIcon)
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(.green)
        }
    }

    private var manualActionText: String {
        switch viewModel.lifecycleStatus.recommendedAction {
        case .reboot:
            return "Restart the Mac, reopen ASFW from Applications, then Recheck."
        case .reconnectDevice:
            return "Wait for the bus to settle, reconnect the FireWire device once, then Recheck."
        case .none,
             .moveAppToApplications,
             .installOrUpdateDriver,
             .enableHelper,
             .approveHelper,
             .repairOnce,
             .captureDiagnostics,
             .sendDiagnostics:
            return ""
        }
    }

    private var lifecycleTitle: String {
        viewModel.lifecycleStatus.expectedCoreAudioDeviceName == nil ? "Driver Lifecycle" : "Audio Lifecycle"
    }

    private var coreAudioLifecycleLabel: String {
        if let deviceName = viewModel.lifecycleStatus.expectedCoreAudioDeviceName,
           !deviceName.isEmpty {
            return "CoreAudio \(deviceName)"
        }
        return "CoreAudio device check"
    }

    private var coreAudioLifecycleValue: String {
        if viewModel.lifecycleStatus.coreAudioProbeUnavailable {
            return "Probe delayed"
        }
        return viewModel.lifecycleStatus.expectedCoreAudioDeviceName == nil
            ? "Not required"
            : yesNo(viewModel.lifecycleStatus.coreAudioDeviceVisible)
    }

    private var lifecyclePlainStatus: String {
        if viewModel.isLifecycleRefreshing && viewModel.lifecycleStatus.health != .clean {
            return "Checking"
        }
        switch viewModel.lifecycleStatus.health {
        case .clean:
            return "Working"
        case .uninstalled:
            return "Not installed"
        case .repairNeeded:
            return "Needs attention"
        case .rebootRequired:
            return "Reboot required"
        case .unknown:
            return "Not checked"
        }
    }

    private var installedDriverMatchesAppText: String {
        if viewModel.isLifecycleRefreshing || viewModel.isConfirmingDriverMismatch {
            return "Checking"
        }
        switch viewModel.lifecycleStatus.installedDriverMatchesApp {
        case .some(true):
            return "Yes"
        case .some(false):
            return "No"
        case .none:
            return "Unknown"
        }
    }

    private var lifecycleCheckedText: String {
        guard let date = viewModel.lastLifecycleCheckDate else {
            return viewModel.isLifecycleRefreshing ? "Checking" : "Not yet"
        }
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        let suffix = viewModel.isLifecycleRefreshing ? " (checking)" : ""
        return "\(formatter.string(from: date))\(suffix)"
    }

    private var driverInstallPurposeText: String {
        "Install / Update is for first install or version changes. Repair is only enabled when ASFW asks for it after Recheck."
    }

    private var repairGuidance: String {
        if viewModel.lifecycleStatus.expectedCoreAudioDeviceName == nil {
            return "Repair only fixes stale ASFW/CoreAudio lifecycle problems. If a test device is missing in Audio MIDI Setup, capture diagnostics instead of repeatedly repairing or reinstalling."
        }
        return "Close Logic or other audio apps before disruptive actions. Use Repair once only if ASFW asks for it after Recheck. Uninstall removes the driver and is not normal troubleshooting."
    }

    private func lifecycleRow(_ label: String, _ value: String) -> some View {
        GridRow {
            Text(label)
                .font(.caption)
                .foregroundStyle(.secondary)
            Text(value)
                .font(.caption.monospaced())
                .textSelection(.enabled)
        }
    }

    private func yesNo(_ value: Bool) -> String {
        value ? "Yes" : "No"
    }

    private func driverOperationProgressView(_ progress: DriverViewModel.DriverOperationProgress) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                if progress.isComplete {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                } else {
                    ProgressView()
                        .controlSize(.small)
                }

                VStack(alignment: .leading, spacing: 2) {
                    Text(progress.phase)
                        .font(.subheadline.weight(.semibold))
                    Text(progress.detail)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }

                Spacer(minLength: 12)

                VStack(alignment: .trailing, spacing: 2) {
                    Text(progress.stepText)
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                    Text(viewModel.driverOperationElapsedText)
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                }
            }

            ProgressView(value: progress.fraction)
                .progressViewStyle(.linear)
                .accessibilityLabel(progress.title)
                .accessibilityValue("\(Int(progress.fraction * 100)) percent")
        }
        .padding()
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

}
