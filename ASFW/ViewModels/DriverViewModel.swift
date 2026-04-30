//
//  DriverViewModel.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import Foundation
import Combine
import SwiftUI

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

    private let maintenanceHelper: MaintenanceHelperManaging
    private var disconnectDriverClient: (() -> Void)?
    private var reconnectDriverClient: (() -> Void)?

    init(maintenanceHelper: MaintenanceHelperManaging = MaintenanceHelperManager.shared) {
        self.maintenanceHelper = maintenanceHelper
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

    var canRepairDriver: Bool {
        canUseMaintenanceHelper
        && lifecycleStatus.health != .clean
        && lifecycleStatus.health != .rebootRequired
        && !repairAttemptedSinceLastHealthyCheck
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
        refreshLifecycleStatus()
    }

    func refreshLifecycleStatus() {
        let driverID = driverBundleIdentifier
        let expectedCDHash = maintenanceHelper.stagedDriverCDHash(driverBundleID: driverID)
        let runningFromApplications = isRunningFromApplications
        let helper = helperStatus
        let userConnected = userClientConnected

        DispatchQueue.global(qos: .utility).async {
            let inputs = MaintenanceLocalProbe.collect(
                isRunningFromApplications: runningFromApplications,
                helperStatus: helper,
                userClientConnected: userConnected,
                driverBundleID: driverID,
                expectedCDHash: expectedCDHash
            )
            let status = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs)
            Task { @MainActor in
                self.lifecycleStatus = status
                if status.isCleanForAudio {
                    self.repairAttemptedSinceLastHealthyCheck = false
                }
            }
        }
    }

    func enableMaintenanceHelper() {
        isBusy = true
        activationStatus = "Registering maintenance helper..."
        log("Maintenance helper registration requested", source: .app, level: .info)

        do {
            helperStatus = try maintenanceHelper.registerHelper()
            refreshHelperStatus()
            isBusy = false
            if helperStatus == .requiresApproval {
                activationStatus = "Maintenance helper needs approval in System Settings."
                log("Maintenance helper registered; approval required in System Settings", source: .app, level: .warning)
            } else {
                activationStatus = "Maintenance helper \(helperStatus.displayName.lowercased())."
                log(maintenanceStatus, source: .app, level: helperStatus == .enabled ? .success : .info)
            }
        } catch {
            isBusy = false
            helperStatus = .failed
            let message = Self.errorMessage(for: error)
            activationStatus = "Error: \(message)"
            maintenanceStatus = message
            log(message, source: .app, level: .error)
        }
    }

    func openMaintenanceApprovalSettings() {
        maintenanceHelper.openApprovalSettings()
    }
    
    func installDriver() {
        isBusy = true
        activationStatus = "Requesting activation/update..."
        log("Activation/update request submitted", source: .app, level: .info)
        
        DriverInstallManager.shared.activate { [weak self] result in
            guard let self = self else { return }
            Task { @MainActor in
                switch result {
                case .success(let message):
                    self.activationStatus = message
                    self.log(message, source: .app, level: .success)
                    self.runPostActivationRefresh()
                case .failure(let error):
                    self.isBusy = false
                    let message = Self.errorMessage(for: error)
                    self.activationStatus = "Error: \(message)"
                    self.log(message, source: .app, level: .error)
                }
            }
        }
    }
    
    func uninstallDriver() {
        isBusy = true
        activationStatus = "Requesting deactivation..."
        log("Deactivation request submitted", source: .app, level: .info)
        
        DriverInstallManager.shared.deactivate { [weak self] result in
            guard let self = self else { return }
            Task { @MainActor in
                switch result {
                case .success(let message):
                    self.activationStatus = message
                    self.log(message, source: .app, level: .success)
                    self.runPostUninstallCleanup()
                case .failure(let error):
                    self.isBusy = false
                    let message = Self.errorMessage(for: error)
                    self.activationStatus = "Error: \(message)"
                    self.log(message, source: .app, level: .error)
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
                message = "Repair is not needed while the ASFW audio path looks healthy."
            } else if lifecycleStatus.health == .rebootRequired {
                message = "Reboot is required before another repair attempt."
            } else {
                message = "Repair was already attempted for this state. Reboot or capture diagnostics before trying again."
            }
            activationStatus = message
            maintenanceStatus = message
            log(message, source: .app, level: .warning)
            return
        }

        isBusy = true
        activationStatus = "Repairing driver state..."
        repairAttemptedSinceLastHealthyCheck = true
        log("Repair Driver requested. Close Logic or other audio apps before repair.", source: .app, level: .warning)
        let expectedCDHash = maintenanceHelper.stagedDriverCDHash(driverBundleID: driverBundleIdentifier)
        disconnectDriverClient?()

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
                self.refreshLifecycleStatus()
            }
        }
    }

    private func runPostActivationRefresh() {
        refreshHelperStatus()
        guard canUseMaintenanceHelper else {
            isBusy = false
            maintenanceStatus = "Activation was submitted. Enable the maintenance helper if the driver does not publish cleanly."
            refreshLifecycleStatus()
            return
        }

        activationStatus = "Refreshing driver after activation..."
        let expectedCDHash = maintenanceHelper.stagedDriverCDHash(driverBundleID: driverBundleIdentifier)
        disconnectDriverClient?()
        maintenanceHelper.refreshDriver(expectedCDHash: expectedCDHash, driverBundleID: driverBundleIdentifier) { [weak self] outcome in
            Task { @MainActor in
                guard let self = self else { return }
                self.reconnectDriverClient?()
                self.applyMaintenanceOutcome(outcome, successPrefix: "Driver activation/update refreshed")
            }
        }
    }

    private func runPostUninstallCleanup() {
        refreshHelperStatus()
        guard canUseMaintenanceHelper else {
            isBusy = false
            maintenanceStatus = "Deactivation was submitted. Enable the maintenance helper for reliable cleanup."
            refreshLifecycleStatus()
            return
        }

        activationStatus = "Cleaning up after deactivation..."
        disconnectDriverClient?()
        maintenanceHelper.cleanupAfterUninstall(driverBundleID: driverBundleIdentifier) { [weak self] outcome in
            Task { @MainActor in
                guard let self = self else { return }
                self.applyMaintenanceOutcome(outcome, successPrefix: "Driver uninstall cleanup completed")
            }
        }
    }

    private func applyMaintenanceOutcome(_ outcome: MaintenanceOperationOutcome, successPrefix: String) {
        isBusy = false
        lastMaintenanceSnapshotPath = outcome.snapshotPath

        switch outcome {
        case .succeeded(let message, _):
            activationStatus = "\(successPrefix): \(message)"
            maintenanceStatus = message
            log(message, source: .app, level: .success)
        case .repairNeeded(let message, _):
            activationStatus = "Repair needed: \(message)"
            maintenanceStatus = message
            log(message, source: .app, level: .warning)
        case .rebootRequired(let message, _):
            activationStatus = "Reboot required: \(message)"
            maintenanceStatus = message
            log(message, source: .app, level: .error)
        case .failed(let message, _):
            activationStatus = "Error: \(message)"
            maintenanceStatus = message
            log(message, source: .app, level: .error)
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
