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
    @Published var isBusy: Bool = false
    @Published var logMessages: [LogEntry] = []
    @Published var driverVersion: DriverVersionInfo?
    
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
    
    func installDriver() {
        isBusy = true
        activationStatus = "Requesting activation..."
        log("Activation request submitted", source: .app, level: .info)
        
        DriverInstallManager.shared.activate { [weak self] result in
            guard let self = self else { return }
            Task { @MainActor in
                self.isBusy = false
                switch result {
                case .success(let message):
                    self.activationStatus = message
                    self.log(message, source: .app, level: .success)
                case .failure(let error):
                    self.activationStatus = "Error: \(error.localizedDescription)"
                    self.log(error.localizedDescription, source: .app, level: .error)
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
                self.isBusy = false
                switch result {
                case .success(let message):
                    self.activationStatus = message
                    self.log(message, source: .app, level: .success)
                case .failure(let error):
                    self.activationStatus = "Error: \(error.localizedDescription)"
                    self.log(error.localizedDescription, source: .app, level: .error)
                }
            }
        }
    }
}
