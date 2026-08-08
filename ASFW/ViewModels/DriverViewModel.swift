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
    @Published var activationStatus: String = "Ready"
    @Published var isBusy: Bool = false
    @Published var logMessages: [LogEntry] = []
    @Published var driverVersion: DriverVersionInfo?
    @Published private(set) var isDriverConnected: Bool = false

    var displayedStatus: String {
        isDriverConnected ? "Driver is loaded and connected" : activationStatus
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

    func updateDriverConnection(_ connected: Bool) {
        let changed = connected != isDriverConnected
        isDriverConnected = connected

        if connected {
            activationStatus = "Driver is loaded and connected"
            if changed {
                log(activationStatus, source: .userClient, level: .success)
            }
        } else if changed {
            activationStatus = "Driver is disconnected"
            driverVersion = nil
            log(activationStatus, source: .userClient, level: .warning)
        }
    }
    
    func installDriver(requireNewerBuild: Bool = DriverInstallSettings.defaultRequireNewerBuild) {
        isBusy = true
        activationStatus = "Requesting driver installation…"
        log("Driver installation request submitted", source: .app, level: .info)
        
        DriverInstallManager.shared.activate(requireNewerBuild: requireNewerBuild,
                                             progress: { [weak self] message in
            Task { @MainActor in
                self?.activationStatus = message
                self?.log(message, source: .app, level: .info)
            }
        }) { [weak self] result in
            guard let self = self else { return }
            Task { @MainActor in
                self.isBusy = false
                switch result {
                case .success(let message):
                    self.activationStatus = message
                    self.log(message, source: .app, level: .success)
                case .failure(let error):
                    let message = self.installErrorMessage(error)
                    self.activationStatus = "Error: \(message)"
                    self.log(message, source: .app, level: .error)
                }
            }
        }
    }
    
    func uninstallDriver() {
        isBusy = true
        activationStatus = "Requesting driver removal…"
        log("Driver removal request submitted", source: .app, level: .info)
        
        DriverInstallManager.shared.deactivate(progress: { [weak self] message in
            Task { @MainActor in
                self?.activationStatus = message
                self?.log(message, source: .app, level: .info)
            }
        }) { [weak self] result in
            guard let self = self else { return }
            Task { @MainActor in
                self.isBusy = false
                switch result {
                case .success(let message):
                    self.activationStatus = message
                    self.log(message, source: .app, level: .success)
                case .failure(let error):
                    let message = self.installErrorMessage(error)
                    self.activationStatus = "Error: \(message)"
                    self.log(message, source: .app, level: .error)
                }
            }
        }
    }

    private func installErrorMessage(_ error: Error) -> String {
        let nsError = error as NSError
        guard let recovery = nsError.localizedRecoverySuggestion,
              !recovery.isEmpty else {
            return nsError.localizedDescription
        }
        return "\(nsError.localizedDescription) \(recovery)"
    }
}
