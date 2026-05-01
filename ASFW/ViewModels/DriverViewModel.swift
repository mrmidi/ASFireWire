//
//  DriverViewModel.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import Foundation
import Combine
import SwiftUI
import AppKit

@MainActor
class DriverViewModel: ObservableObject {
    @Published var activationStatus: String = "Idle"
    @Published var helperStatus: MaintenanceHelperApprovalState = .unknown
    @Published var maintenanceStatus: String = "Maintenance helper has not been checked yet."
    @Published var lastMaintenanceSnapshotPath: String?
    @Published var isBusy: Bool = false
    @Published var logMessages: [LogEntry] = []
    @Published var driverVersion: DriverVersionInfo?
    @Published var lifecycleStatus: MaintenanceLifecycleStatus = .unknown
    @Published var userClientConnected: Bool = false
    @Published var repairAttemptedSinceLastHealthyCheck: Bool = false
    @Published var isLifecycleRefreshing: Bool = false
    @Published var isConfirmingDriverMismatch: Bool = false
    @Published var lastLifecycleCheckDate: Date?
    @Published var driverOperationProgress: DriverOperationProgress?
    @Published var driverOperationTick: Int = 0

    private let maintenanceHelper: MaintenanceHelperManaging
    private let lifecycleProbe: MaintenanceLifecycleProbe
    private let mismatchConfirmationDelay: TimeInterval
    private var disconnectDriverClient: (() -> Void)?
    private var reconnectDriverClient: (() -> Void)?
    private var lifecycleProbeGeneration = 0
    private var pendingMismatchSignature: String?
    private var confirmedMismatchSignature: String?
    private var operationProgressTimer: Timer?
    private let postActivationSettleInterval: TimeInterval = 2.0
    private let postActivationSettleMaxAttempts = 45
    private let postActivationHelperRefreshAttempt = 6

    typealias MaintenanceLifecycleProbe = (_ isRunningFromApplications: Bool,
                                           _ helperStatus: MaintenanceHelperApprovalState,
                                           _ userClientConnected: Bool,
                                           _ driverBundleID: String,
                                           _ expectedCDHash: String?) -> MaintenanceLifecycleStatus

    init(maintenanceHelper: MaintenanceHelperManaging = MaintenanceHelperManager.shared,
         lifecycleProbe: MaintenanceLifecycleProbe? = nil,
         mismatchConfirmationDelay: TimeInterval = 0.75) {
        self.maintenanceHelper = maintenanceHelper
        self.lifecycleProbe = lifecycleProbe ?? { runningFromApplications, helperStatus, userClientConnected, driverBundleID, expectedCDHash in
            let inputs = MaintenanceLocalProbe.collect(
                isRunningFromApplications: runningFromApplications,
                helperStatus: helperStatus,
                userClientConnected: userClientConnected,
                driverBundleID: driverBundleID,
                expectedCDHash: expectedCDHash
            )
            return ASFWMaintenanceLifecycleEvaluator.evaluate(inputs)
        }
        self.mismatchConfirmationDelay = mismatchConfirmationDelay
        self.helperStatus = maintenanceHelper.helperStatus
    }

    var driverBundleIdentifier: String {
        DriverInstallManager.shared.extensionBundleIdentifier
    }

    var appBundlePath: String {
        DriverInstallManager.shared.appBundlePath
    }

    var isRunningFromApplications: Bool {
        DriverInstallManager.shared.isRunningFromApplications
    }

    var canUseMaintenanceHelper: Bool {
        helperStatus == .enabled
    }

    var canInstallOrUpdateDriver: Bool {
        !isBusy
        && isRunningFromApplications
        && lifecycleStatus.health != .rebootRequired
    }

    var canUninstallDriver: Bool {
        !isBusy
    }

    var canRepairDriver: Bool {
        canUseMaintenanceHelper
        && lifecycleStatus.recommendedAction == .repairOnce
        && confirmedRepairStateMatchesCurrentStatus
        && !repairAttemptedSinceLastHealthyCheck
        && !isLifecycleRefreshing
        && !isConfirmingDriverMismatch
    }

    private var confirmedRepairStateMatchesCurrentStatus: Bool {
        guard lifecycleStatus.recommendedAction == .repairOnce else { return false }
        guard let signature = lifecycleStatus.driverMismatchSignature else { return true }
        return signature == confirmedMismatchSignature
    }

    var canPerformRecommendedAction: Bool {
        lifecycleStatus.recommendedAction.isDirectlyActionableInApp
        && !isBusy
        && !(lifecycleStatus.recommendedAction == .repairOnce && !canRepairDriver)
        && !(lifecycleStatus.recommendedAction == .captureDiagnostics && !canUseMaintenanceHelper)
        && !(lifecycleStatus.recommendedAction == .sendDiagnostics && !canUseMaintenanceHelper)
    }

    var driverOperationElapsedText: String {
        _ = driverOperationTick
        guard let progress = driverOperationProgress else { return "" }
        let seconds = max(0, Int(Date().timeIntervalSince(progress.startedAt)))
        return "\(seconds)s"
    }

    deinit {
        operationProgressTimer?.invalidate()
    }
    
    enum DriverOperationKind: String, Equatable {
        case helperSetup
        case installUpdate
        case repair
        case uninstall
        case diagnostics
    }

    struct DriverOperationProgress: Identifiable, Equatable {
        let id: UUID
        var kind: DriverOperationKind
        var title: String
        var phase: String
        var detail: String
        var currentStep: Int
        var totalSteps: Int
        var startedAt: Date
        var updatedAt: Date
        var isComplete: Bool

        var fraction: Double {
            guard totalSteps > 0 else { return 0 }
            let clamped = min(max(currentStep, 0), totalSteps)
            return Double(clamped) / Double(totalSteps)
        }

        var stepText: String {
            "Step \(min(max(currentStep, 1), max(totalSteps, 1))) of \(max(totalSteps, 1))"
        }
    }

    struct LogEntry: Identifiable, Equatable {
        let id = UUID()
        let timestamp: Date
        let source: Source
        let level: Level
        let message: String
        
        enum Source: String {
            case driver = "Driver"
            case userClient = "Connection"
            case app = "App"
        }
        
        enum Level {
            case info, warning, error, success
            
            var color: Color {
                switch self {
                case .info: return .blue
                case .warning: return .orange
                case .error: return .red
                case .success: return .green
                }
            }
            
            var systemImage: String {
                switch self {
                case .info: return "info.circle.fill"
                case .warning: return "exclamationmark.triangle.fill"
                case .error: return "xmark.circle.fill"
                case .success: return "checkmark.circle.fill"
                }
            }
        }
        
        var formattedTime: String {
            let formatter = DateFormatter()
            formatter.dateFormat = "HH:mm:ss"
            return formatter.string(from: timestamp)
        }
    }

    private func beginDriverOperation(_ kind: DriverOperationKind,
                                      title: String,
                                      totalSteps: Int,
                                      phase: String,
                                      detail: String) {
        operationProgressTimer?.invalidate()
        driverOperationTick = 0
        driverOperationProgress = DriverOperationProgress(
            id: UUID(),
            kind: kind,
            title: title,
            phase: phase,
            detail: detail,
            currentStep: 1,
            totalSteps: totalSteps,
            startedAt: Date(),
            updatedAt: Date(),
            isComplete: false
        )
        operationProgressTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.driverOperationTick += 1
            }
        }
    }

    private func updateDriverOperation(step: Int,
                                       phase: String,
                                       detail: String) {
        guard var progress = driverOperationProgress else { return }
        progress.currentStep = min(max(step, 1), max(progress.totalSteps, 1))
        progress.phase = phase
        progress.detail = detail
        progress.updatedAt = Date()
        driverOperationProgress = progress
    }

    private func finishDriverOperation(phase: String,
                                       detail: String,
                                       clearAfter delay: TimeInterval = 1.25) {
        guard var progress = driverOperationProgress else { return }
        let id = progress.id
        progress.currentStep = progress.totalSteps
        progress.phase = phase
        progress.detail = detail
        progress.updatedAt = Date()
        progress.isComplete = true
        driverOperationProgress = progress

        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self else { return }
            Task { @MainActor in
                guard self.driverOperationProgress?.id == id else { return }
                self.clearDriverOperation()
            }
        }
    }

    private func clearDriverOperation() {
        operationProgressTimer?.invalidate()
        operationProgressTimer = nil
        driverOperationProgress = nil
    }
    
    func log(_ message: String, source: LogEntry.Source, level: LogEntry.Level = .info) {
        let entry = LogEntry(timestamp: Date(), source: source, level: level, message: message)
        logMessages.append(entry)
        
        // Keep last 200 messages
        if logMessages.count > 200 {
            logMessages.removeFirst(logMessages.count - 200)
        }
    }

    func setMaintenanceConnectionHandlers(disconnect: @escaping () -> Void,
                                          reconnect: @escaping () -> Void) {
        disconnectDriverClient = disconnect
        reconnectDriverClient = reconnect
    }

    func setUserClientConnected(_ connected: Bool) {
        userClientConnected = connected
        lifecycleStatus.userClientConnected = connected
    }

    func refreshHelperStatus() {
        updateHelperStatusOnly()
        refreshLifecycleStatus()
    }

    private func updateHelperStatusOnly() {
        helperStatus = maintenanceHelper.helperStatus
        switch helperStatus {
        case .enabled:
            maintenanceStatus = "Maintenance helper is enabled."
        case .requiresApproval:
            maintenanceStatus = "Maintenance helper is registered but needs approval in System Settings."
        case .notRegistered:
            maintenanceStatus = "Maintenance helper is not registered."
        case .notFound:
            maintenanceStatus = "Maintenance helper is missing from the app bundle."
        case .failed:
            maintenanceStatus = "Maintenance helper failed."
        case .unknown:
            maintenanceStatus = "Maintenance helper state is unknown."
        }
    }

    func refreshLifecycleStatus() {
        startLifecycleProbe(confirmingDriverMismatch: false)
    }

    private func startLifecycleProbe(confirmingDriverMismatch: Bool) {
        lifecycleProbeGeneration += 1
        let generation = lifecycleProbeGeneration
        let driverID = driverBundleIdentifier
        let expectedCDHash = maintenanceHelper.stagedDriverCDHash(driverBundleID: driverID)
        let runningFromApplications = isRunningFromApplications
        let helper = helperStatus
        let userConnected = userClientConnected
        let probe = lifecycleProbe

        isLifecycleRefreshing = true

        DispatchQueue.global(qos: .utility).async {
            let status = probe(runningFromApplications,
                               helper,
                               userConnected,
                               driverID,
                               expectedCDHash)
            Task { @MainActor in
                self.applyLifecycleProbeResult(status,
                                               generation: generation,
                                               confirmingDriverMismatch: confirmingDriverMismatch)
            }
        }
    }

    private func applyLifecycleProbeResult(_ status: MaintenanceLifecycleStatus,
                                           generation: Int,
                                           confirmingDriverMismatch: Bool) {
        guard generation == lifecycleProbeGeneration else {
            log("Ignored stale lifecycle probe generation \(generation).", source: .app, level: .info)
            return
        }

        lastLifecycleCheckDate = Date()

        if status.isDriverMismatchRepairCandidate,
           !confirmingDriverMismatch,
           status.driverMismatchSignature != confirmedMismatchSignature {
            pendingMismatchSignature = status.driverMismatchSignature
            isConfirmingDriverMismatch = true

            if lifecycleStatus.health == .clean {
                log("Driver mismatch observed during refresh; keeping healthy status while ASFW rechecks.", source: .app, level: .info)
            } else {
                lifecycleStatus = status.unconfirmedDriverMismatchStatus
            }

            scheduleDriverMismatchConfirmation(for: generation, signature: status.driverMismatchSignature)
            return
        }

        isLifecycleRefreshing = false
        isConfirmingDriverMismatch = false
        pendingMismatchSignature = nil

        if status.isDriverMismatchRepairCandidate {
            confirmedMismatchSignature = status.driverMismatchSignature
        } else {
            if confirmingDriverMismatch {
                log("Transient driver mismatch cleared on confirmation probe.", source: .app, level: .success)
            }
            confirmedMismatchSignature = nil
        }

        lifecycleStatus = status
        if status.isCleanForAudio {
            repairAttemptedSinceLastHealthyCheck = false
            softenRecoveredMaintenanceErrorIfNeeded()
        }
    }

    private func softenRecoveredMaintenanceErrorIfNeeded() {
        let lower = activationStatus.lowercased()
        guard lower.hasPrefix("error:"),
              lower.contains("helper") else {
            return
        }
        activationStatus = "Driver is active."
        maintenanceStatus = "The driver and audio path look usable. Helper-only maintenance actions can be retried later if diagnostics or repair are needed."
        log(maintenanceStatus, source: .app, level: .warning)
    }

    private func scheduleDriverMismatchConfirmation(for generation: Int, signature: String?) {
        DispatchQueue.main.asyncAfter(deadline: .now() + mismatchConfirmationDelay) { [weak self] in
            guard let self else { return }
            Task { @MainActor in
                guard self.lifecycleProbeGeneration == generation,
                      self.pendingMismatchSignature == signature else {
                    return
                }
                self.log("Rechecking driver match before enabling Repair.", source: .app, level: .info)
                self.startLifecycleProbe(confirmingDriverMismatch: true)
            }
        }
    }

    func enableMaintenanceHelper() {
        isBusy = true
        beginDriverOperation(.helperSetup,
                             title: "Maintenance Helper",
                             totalSteps: 3,
                             phase: "Registering helper",
                             detail: "macOS is registering the app-bundled privileged helper.")
        activationStatus = "Registering maintenance helper..."
        log("Maintenance helper registration requested", source: .app, level: .info)

        do {
            helperStatus = try maintenanceHelper.registerHelper()
            updateDriverOperation(step: 2,
                                  phase: "Checking approval",
                                  detail: "ASFW is checking whether System Settings approval is still needed.")
            refreshHelperStatus()
            isBusy = false
            if helperStatus == .requiresApproval {
                activationStatus = "Maintenance helper needs approval in System Settings."
                log("Maintenance helper registered; approval required in System Settings", source: .app, level: .warning)
                finishDriverOperation(phase: "Approval needed",
                                      detail: "Open System Settings and approve the helper before repair/cleanup actions.")
            } else {
                activationStatus = "Maintenance helper \(helperStatus.displayName.lowercased())."
                log(maintenanceStatus, source: .app, level: helperStatus == .enabled ? .success : .info)
                finishDriverOperation(phase: "Helper checked",
                                      detail: maintenanceStatus)
            }
        } catch {
            isBusy = false
            helperStatus = .failed
            let message = Self.errorMessage(for: error)
            activationStatus = "Error: \(message)"
            maintenanceStatus = message
            log(message, source: .app, level: .error)
            finishDriverOperation(phase: "Helper setup failed",
                                  detail: message)
        }
    }

    func openMaintenanceApprovalSettings() {
        maintenanceHelper.openApprovalSettings()
    }
    
    func installDriver() {
        guard canInstallOrUpdateDriver else {
            let message: String
            if !isRunningFromApplications {
                message = "Open ASFW from /Applications before installing or updating the driver."
            } else if lifecycleStatus.health == .rebootRequired {
                message = "Reboot is required before another install or update attempt."
            } else {
                message = "ASFW is busy. Wait for the current operation to finish."
            }
            activationStatus = message
            log(message, source: .app, level: .warning)
            return
        }
        isBusy = true
        beginDriverOperation(.installUpdate,
                             title: "Install / Update Driver",
                             totalSteps: 6,
                             phase: "Submitting request",
                             detail: "ASFW is asking macOS to activate or replace the DriverKit extension.")
        activationStatus = "Requesting activation/update..."
        log("Activation/update request submitted", source: .app, level: .info)
        
        DriverInstallManager.shared.activate { [weak self] result in
            guard let self = self else { return }
            Task { @MainActor in
                switch result {
                case .success(let message):
                    self.updateDriverOperation(step: 2,
                                               phase: "Request accepted",
                                               detail: "macOS accepted the System Extension request. Approval or a short settle period can still be needed.")
                    self.activationStatus = message
                    self.log(message, source: .app, level: .success)
                    self.runPostActivationRefresh()
                case .failure(let error):
                    self.isBusy = false
                    let message = Self.errorMessage(for: error)
                    if DriverInstallManager.isUserApprovalRequired(error) {
                        self.activationStatus = "Approval required: \(message)"
                        self.log(message, source: .app, level: .warning)
                        self.finishDriverOperation(phase: "Approval needed",
                                                   detail: "Approve ASFW in System Settings, then return to the app.")
                        self.refreshLifecycleStatus()
                    } else {
                        self.activationStatus = "Error: \(message)"
                        self.log(message, source: .app, level: .error)
                        self.finishDriverOperation(phase: "Install/update failed",
                                                   detail: message)
                    }
                }
            }
        }
    }
    
    func uninstallDriver() {
        guard canUninstallDriver else {
            activationStatus = "ASFW is busy. Wait for the current operation to finish."
            log(activationStatus, source: .app, level: .warning)
            return
        }
        isBusy = true
        beginDriverOperation(.uninstall,
                             title: "Uninstall Driver",
                             totalSteps: 5,
                             phase: "Submitting uninstall",
                             detail: "ASFW is asking macOS to deactivate the System Extension.")
        activationStatus = "Requesting deactivation..."
        log("Deactivation request submitted", source: .app, level: .info)
        
        DriverInstallManager.shared.deactivate { [weak self] result in
            guard let self = self else { return }
            Task { @MainActor in
                switch result {
                case .success(let message):
                    self.updateDriverOperation(step: 2,
                                               phase: "Uninstall accepted",
                                               detail: "macOS accepted the deactivation request. ASFW is checking cleanup state next.")
                    self.activationStatus = message
                    self.log(message, source: .app, level: .success)
                    self.runPostUninstallCleanup()
                case .failure(let error):
                    self.isBusy = false
                    let message = Self.errorMessage(for: error)
                    self.activationStatus = "Error: \(message)"
                    self.log(message, source: .app, level: .error)
                    self.finishDriverOperation(phase: "Uninstall failed",
                                               detail: message)
                }
            }
        }
    }

    func repairDriver() {
        guard canUseMaintenanceHelper else {
            refreshHelperStatus()
            activationStatus = "Enable the maintenance helper before repairing the driver."
            log(activationStatus, source: .app, level: .warning)
            return
        }
        guard canRepairDriver else {
            let message: String
            if lifecycleStatus.health == .clean {
                message = lifecycleStatus.expectedCoreAudioDeviceName == nil
                    ? "Repair is not needed while the ASFW driver lifecycle looks healthy. Capture diagnostics if the test device is missing."
                    : "Repair is not needed while the ASFW audio path looks healthy."
            } else if lifecycleStatus.health == .rebootRequired {
                message = "Reboot is required before another repair attempt."
            } else if lifecycleStatus.recommendedAction == .reconnectDevice {
                message = "Wait for the bus to settle, reconnect the FireWire device once, then Recheck. Capture diagnostics if it stays missing."
            } else {
                message = "Repair was already attempted for this state. Reboot or capture diagnostics before trying again."
            }
            activationStatus = message
            maintenanceStatus = message
            log(message, source: .app, level: .warning)
            return
        }

        isBusy = true
        isLifecycleRefreshing = true
        beginDriverOperation(.repair,
                             title: "Repair Driver",
                             totalSteps: 5,
                             phase: "Rechecking state",
                             detail: "ASFW is confirming repair is still needed before touching DriverKit or CoreAudio.")
        activationStatus = "Rechecking before repair..."
        log("Repair Driver requested. ASFW is rechecking before touching the driver.", source: .app, level: .info)
        let expectedCDHash = maintenanceHelper.stagedDriverCDHash(driverBundleID: driverBundleIdentifier)
        let runningFromApplications = isRunningFromApplications
        let helper = helperStatus
        let userConnected = userClientConnected
        let driverID = driverBundleIdentifier
        let probe = lifecycleProbe
        lifecycleProbeGeneration += 1
        let generation = lifecycleProbeGeneration

        DispatchQueue.global(qos: .utility).async {
            let status = probe(runningFromApplications,
                               helper,
                               userConnected,
                               driverID,
                               expectedCDHash)
            Task { @MainActor in
                self.handleRepairPreflight(status,
                                           expectedCDHash: expectedCDHash,
                                           generation: generation)
            }
        }
    }

    private func handleRepairPreflight(_ status: MaintenanceLifecycleStatus,
                                       expectedCDHash: String?,
                                       generation: Int) {
        guard generation == lifecycleProbeGeneration else {
            log("Ignored stale repair preflight generation \(generation).", source: .app, level: .info)
            return
        }

        lastLifecycleCheckDate = Date()
        isLifecycleRefreshing = false
        isConfirmingDriverMismatch = false
        pendingMismatchSignature = nil

        if status.isCleanForAudio || status.health == .clean {
            lifecycleStatus = status
            confirmedMismatchSignature = nil
            repairAttemptedSinceLastHealthyCheck = false
            isBusy = false
            activationStatus = "Audio path is already healthy."
            maintenanceStatus = activationStatus
            log(activationStatus, source: .app, level: .success)
            finishDriverOperation(phase: "Repair cancelled",
                                  detail: "The recheck is clean. ASFW did not restart any driver services.")
            return
        }

        lifecycleStatus = status
        if status.isDriverMismatchRepairCandidate {
            confirmedMismatchSignature = status.driverMismatchSignature
        }

        guard status.recommendedAction == .repairOnce else {
            isBusy = false
            let message: String
            if status.health == .rebootRequired {
                message = "Reboot is required before repair."
            } else {
                message = "Repair is not the recommended action after recheck."
            }
            activationStatus = message
            maintenanceStatus = message
            log(message, source: .app, level: .warning)
            finishDriverOperation(phase: "Repair cancelled",
                                  detail: message)
            return
        }

        repairAttemptedSinceLastHealthyCheck = true
        updateDriverOperation(step: 2,
                              phase: "Repair confirmed",
                              detail: "ASFW will disconnect debug tools, refresh ASFW services, then reconnect.")
        activationStatus = "Repairing driver state..."
        log("Repair confirmed. Close Logic or other audio apps before repair.", source: .app, level: .warning)
        disconnectDriverClient?()
        updateDriverOperation(step: 3,
                              phase: "Refreshing ASFW services",
                              detail: "ASFW is refreshing only its own driver services, then it will recheck before asking for action.")

        maintenanceHelper.refreshDriver(expectedCDHash: expectedCDHash, driverBundleID: driverBundleIdentifier) { [weak self] outcome in
            Task { @MainActor in
                guard let self = self else { return }
                self.reconnectDriverClient?()
                self.applyMaintenanceOutcome(outcome, successPrefix: "Repair completed")
            }
        }
    }

    func captureDiagnostics() {
        guard canUseMaintenanceHelper else {
            refreshHelperStatus()
            activationStatus = "Enable the maintenance helper before capturing diagnostics."
            log(activationStatus, source: .app, level: .warning)
            return
        }

        isBusy = true
        beginDriverOperation(.diagnostics,
                             title: "Capture Diagnostics",
                             totalSteps: 3,
                             phase: "Collecting snapshot",
                             detail: "ASFW is collecting logs and system state under /Users/Shared/ASFW/Diagnostics.")
        activationStatus = "Capturing diagnostics..."
        maintenanceHelper.captureHygieneSnapshot { [weak self] outcome in
            Task { @MainActor in
                guard let self = self else { return }
                self.isBusy = false
                self.lastMaintenanceSnapshotPath = outcome.snapshotPath
                self.maintenanceStatus = outcome.message
                self.activationStatus = outcome.message
                self.log(outcome.message,
                         source: .app,
                         level: outcome.snapshotPath == nil ? .warning : .success)
                self.finishDriverOperation(phase: outcome.snapshotPath == nil ? "Diagnostics incomplete" : "Diagnostics captured",
                                           detail: outcome.message)
                self.refreshLifecycleStatus()
            }
        }
    }

    func performRecommendedAction() {
        switch lifecycleStatus.recommendedAction {
        case .none:
            activationStatus = "No action needed."
        case .moveAppToApplications:
            NSWorkspace.shared.open(URL(fileURLWithPath: "/Applications", isDirectory: true))
        case .installOrUpdateDriver:
            installDriver()
        case .enableHelper:
            enableMaintenanceHelper()
        case .approveHelper:
            openMaintenanceApprovalSettings()
        case .repairOnce:
            repairDriver()
        case .reboot:
            activationStatus = "Reboot is required before another repair or install attempt."
            log(activationStatus, source: .app, level: .warning)
        case .reconnectDevice:
            activationStatus = "Wait for the bus to settle, reconnect the FireWire device once, then Recheck."
            log(activationStatus, source: .app, level: .warning)
        case .captureDiagnostics, .sendDiagnostics:
            captureDiagnostics()
        }
    }

    func copyLifecycleSummary() {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(lifecycleStatus.copySummary, forType: .string)
        activationStatus = "Lifecycle summary copied."
        log(activationStatus, source: .app, level: .info)
    }

    func copyLastMaintenanceSnapshotPath() {
        guard let path = lastMaintenanceSnapshotPath else {
            activationStatus = "No diagnostics snapshot path to copy."
            return
        }
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(path, forType: .string)
        activationStatus = "Diagnostics path copied."
        log(activationStatus, source: .app, level: .info)
    }

    func openLastMaintenanceSnapshot() {
        guard let path = lastMaintenanceSnapshotPath else {
            activationStatus = "No diagnostics snapshot to open."
            return
        }
        NSWorkspace.shared.open(URL(fileURLWithPath: path, isDirectory: true))
    }

    func openAudioMIDISetup() {
        let url = URL(fileURLWithPath: "/System/Applications/Utilities/Audio MIDI Setup.app", isDirectory: true)
        NSWorkspace.shared.open(url)
    }

    private func runPostActivationRefresh() {
        updateHelperStatusOnly()

        updateDriverOperation(step: 3,
                              phase: "Waiting for macOS",
                              detail: "ASFW is watching the System Extension and CoreAudio settle. If an older ASFW service stays attached, ASFW will refresh only ASFW services.")
        activationStatus = "Waiting for driver to settle..."
        let expectedCDHash = maintenanceHelper.stagedDriverCDHash(driverBundleID: driverBundleIdentifier)
        disconnectDriverClient?()
        pollPostActivationSettle(expectedCDHash: expectedCDHash,
                                 attempt: 1,
                                 helperRefreshAttempted: false)
    }

    private func pollPostActivationSettle(expectedCDHash: String?,
                                          attempt: Int,
                                          helperRefreshAttempted: Bool) {
        lifecycleProbeGeneration += 1
        let generation = lifecycleProbeGeneration
        let driverID = driverBundleIdentifier
        let runningFromApplications = isRunningFromApplications
        let helper = helperStatus
        let userConnected = userClientConnected
        let probe = lifecycleProbe

        isLifecycleRefreshing = true

        DispatchQueue.global(qos: .utility).async {
            let status = probe(runningFromApplications,
                               helper,
                               userConnected,
                               driverID,
                               expectedCDHash)
            Task { @MainActor in
                self.handlePostActivationSettleProbe(status,
                                                     expectedCDHash: expectedCDHash,
                                                     attempt: attempt,
                                                     helperRefreshAttempted: helperRefreshAttempted,
                                                     generation: generation)
            }
        }
    }

    private func handlePostActivationSettleProbe(_ status: MaintenanceLifecycleStatus,
                                                 expectedCDHash: String?,
                                                 attempt: Int,
                                                 helperRefreshAttempted: Bool,
                                                 generation: Int) {
        guard generation == lifecycleProbeGeneration else {
            log("Ignored stale post-activation probe generation \(generation).", source: .app, level: .info)
            return
        }

        lastLifecycleCheckDate = Date()

        if status.isCleanForAudio || status.health == .clean {
            lifecycleStatus = status
            isLifecycleRefreshing = false
            isConfirmingDriverMismatch = false
            pendingMismatchSignature = nil
            confirmedMismatchSignature = nil
            repairAttemptedSinceLastHealthyCheck = false
            isBusy = false
            reconnectDriverClient?()
            activationStatus = "Driver activation/update completed."
            maintenanceStatus = status.detail
            log(activationStatus, source: .app, level: .success)
            finishDriverOperation(phase: "Driver active",
                                  detail: status.detail)
            return
        }

        if status.health == .rebootRequired {
            lifecycleStatus = status
            isLifecycleRefreshing = false
            isBusy = false
            reconnectDriverClient?()
            activationStatus = "Reboot required: \(status.summary)"
            maintenanceStatus = status.detail
            log(status.detail, source: .app, level: .error)
            finishDriverOperation(phase: "Reboot required",
                                  detail: status.detail)
            return
        }

        if shouldRefreshServicesAfterActivation(status: status,
                                                attempt: attempt,
                                                helperRefreshAttempted: helperRefreshAttempted) {
            runPostActivationServiceRefresh(expectedCDHash: expectedCDHash,
                                            attempt: attempt)
            return
        }

        if attempt < postActivationSettleMaxAttempts {
            let elapsed = Int(Double(attempt) * postActivationSettleInterval)
            let detail = status.activeDriver
                ? "The System Extension is active; ASFW is waiting for the driver service/audio device to settle. Elapsed \(elapsed)s."
                : "macOS is still activating or replacing the System Extension. Elapsed \(elapsed)s."
            updateDriverOperation(step: min(3 + attempt / 15, 5),
                                  phase: "Waiting for macOS",
                                  detail: detail)
            activationStatus = "Waiting for driver to settle..."

            DispatchQueue.main.asyncAfter(deadline: .now() + postActivationSettleInterval) { [weak self] in
                Task { @MainActor in
                    guard let self,
                          self.isBusy,
                          self.driverOperationProgress?.kind == .installUpdate else {
                        return
                    }
                    self.pollPostActivationSettle(expectedCDHash: expectedCDHash,
                                                  attempt: attempt + 1,
                                                  helperRefreshAttempted: helperRefreshAttempted)
                }
            }
            return
        }

        lifecycleStatus = status
        isLifecycleRefreshing = false
        isBusy = false
        reconnectDriverClient?()
        activationStatus = "Driver update needs attention after settling."
        maintenanceStatus = status.detail
        log(status.detail, source: .app, level: .warning)
        finishDriverOperation(phase: "Needs attention",
                              detail: "macOS accepted the install/update request, but ASFW could not confirm a clean audio lifecycle before the timeout. Use Recheck once before trying Repair.")
    }

    private func shouldRefreshServicesAfterActivation(status: MaintenanceLifecycleStatus,
                                                      attempt: Int,
                                                      helperRefreshAttempted: Bool) -> Bool {
        guard canUseMaintenanceHelper,
              !helperRefreshAttempted,
              attempt >= postActivationHelperRefreshAttempt else {
            return false
        }

        return status.isDriverMismatchRepairCandidate
    }

    private func runPostActivationServiceRefresh(expectedCDHash: String?,
                                                 attempt: Int) {
        updateDriverOperation(step: 4,
                              phase: "Refreshing ASFW services",
                              detail: "An older ASFW driver service is still attached. ASFW is stopping only ASFW services so macOS can load the updated driver.")
        activationStatus = "Refreshing ASFW driver services..."
        maintenanceStatus = "ASFW is stopping the older driver service and checking again. This usually takes under a minute."
        log("Post-update service refresh started for stale ASFW driver service.", source: .app, level: .info)

        maintenanceHelper.refreshDriver(expectedCDHash: expectedCDHash,
                                        driverBundleID: driverBundleIdentifier) { [weak self] outcome in
            Task { @MainActor in
                guard let self else { return }

                if let snapshotPath = outcome.snapshotPath {
                    self.lastMaintenanceSnapshotPath = snapshotPath
                }

                switch outcome {
                case .rebootRequired(let message, _):
                    self.isLifecycleRefreshing = false
                    self.isBusy = false
                    self.reconnectDriverClient?()
                    self.activationStatus = "Reboot required: \(message)"
                    self.maintenanceStatus = message
                    self.log(message, source: .app, level: .error)
                    self.finishDriverOperation(phase: "Reboot required",
                                               detail: message)

                case .failed(let message, _):
                    self.log("ASFW service refresh did not complete: \(message)", source: .app, level: .warning)
                    self.updateDriverOperation(step: 5,
                                               phase: "Still checking",
                                               detail: "ASFW could not complete the service refresh, but it is continuing to check before asking for action.")
                    self.activationStatus = "Still checking driver state..."
                    self.continuePostActivationPolling(expectedCDHash: expectedCDHash,
                                                       nextAttempt: attempt + 1)

                case .succeeded(let message, _),
                     .repairNeeded(let message, _):
                    self.log("ASFW service refresh finished: \(message)", source: .app, level: .info)
                    self.updateDriverOperation(step: 5,
                                               phase: "Confirming driver",
                                               detail: "ASFW refreshed its own services and is checking that the updated driver is attached.")
                    self.activationStatus = "Confirming updated driver..."
                    self.continuePostActivationPolling(expectedCDHash: expectedCDHash,
                                                       nextAttempt: attempt + 1)
                }
            }
        }
    }

    private func continuePostActivationPolling(expectedCDHash: String?,
                                               nextAttempt: Int) {
        DispatchQueue.main.asyncAfter(deadline: .now() + postActivationSettleInterval) { [weak self] in
            Task { @MainActor in
                guard let self,
                      self.isBusy,
                      self.driverOperationProgress?.kind == .installUpdate else {
                    return
                }
                self.pollPostActivationSettle(expectedCDHash: expectedCDHash,
                                              attempt: nextAttempt,
                                              helperRefreshAttempted: true)
            }
        }
    }

    private func runPostUninstallCleanup() {
        refreshHelperStatus()
        guard canUseMaintenanceHelper else {
            isBusy = false
            maintenanceStatus = "Deactivation was submitted. Enable the maintenance helper for reliable cleanup."
            refreshLifecycleStatus()
            finishDriverOperation(phase: "Uninstall submitted",
                                  detail: "Enable the helper for reliable cleanup verification.")
            return
        }

        updateDriverOperation(step: 3,
                              phase: "Cleaning up",
                              detail: "ASFW is checking for stale ASFW DriverKit/CoreAudio state after uninstall.")
        activationStatus = "Cleaning up after deactivation..."
        disconnectDriverClient?()
        maintenanceHelper.cleanupAfterUninstall(driverBundleID: driverBundleIdentifier) { [weak self] outcome in
            Task { @MainActor in
                guard let self = self else { return }
                if case .succeeded = outcome {
                    // Leave the debug user-client disconnected after a clean uninstall.
                } else {
                    self.reconnectDriverClient?()
                }
                self.applyMaintenanceOutcome(outcome, successPrefix: "Driver uninstall cleanup completed")
            }
        }
    }

    private func applyMaintenanceOutcome(_ outcome: MaintenanceOperationOutcome, successPrefix: String) {
        isBusy = false
        lastMaintenanceSnapshotPath = outcome.snapshotPath

        switch outcome {
        case .succeeded(let message, _):
            updateDriverOperation(step: max(driverOperationProgress?.totalSteps ?? 1, 1) - 1,
                                  phase: "Verifying",
                                  detail: "ASFW is doing one final lifecycle check.")
            activationStatus = "\(successPrefix): \(message)"
            maintenanceStatus = message
            log(message, source: .app, level: .success)
            finishDriverOperation(phase: "Done",
                                  detail: message)
        case .repairNeeded(let message, _):
            activationStatus = "Repair needed: \(message)"
            maintenanceStatus = message
            log(message, source: .app, level: .warning)
            finishDriverOperation(phase: "Needs attention",
                                  detail: message)
        case .rebootRequired(let message, _):
            activationStatus = "Reboot required: \(message)"
            maintenanceStatus = message
            log(message, source: .app, level: .error)
            finishDriverOperation(phase: "Reboot required",
                                  detail: message)
        case .failed(let message, _):
            activationStatus = "Error: \(message)"
            maintenanceStatus = message
            log(message, source: .app, level: .error)
            finishDriverOperation(phase: "Failed",
                                  detail: message)
        }
        refreshLifecycleStatus()
    }

    private static func errorMessage(for error: Error) -> String {
        if let localized = error as? LocalizedError {
            let description = localized.errorDescription ?? error.localizedDescription
            if let suggestion = localized.recoverySuggestion {
                return "\(description) \(suggestion)"
            }
            return description
        }
        return error.localizedDescription
    }
}
