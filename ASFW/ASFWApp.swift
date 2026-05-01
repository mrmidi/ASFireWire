//
//  ASFWApp.swift
//  ASFW
//
//  Created by Alexander Shabelnikov on 21.09.2025.
//

import SwiftUI
import Foundation

@main
struct ASFWApp: App {
    init() {
        ASFWCommandLine.handleIfRequested()
    }

    var body: some Scene {
        WindowGroup {
            ModernContentView()
        }
        .defaultSize(width: 1000, height: 700)
    }
}

private enum ASFWCommandLine {
    static func handleIfRequested() {
        let arguments = CommandLine.arguments
        if arguments.contains("--install-driver-once") {
            installDriverOnce()
        }

        if arguments.contains("--register-helper-once") {
            registerHelperOnce()
        }

        if let refreshIndex = arguments.firstIndex(of: "--refresh-driver-once") {
            let explicitCDHash = arguments.index(after: refreshIndex) < arguments.endIndex
                && !arguments[arguments.index(after: refreshIndex)].hasPrefix("--")
                ? arguments[arguments.index(after: refreshIndex)]
                : nil
            refreshDriverOnce(explicitCDHash: explicitCDHash)
        }
    }

    private static func installDriverOnce() {
        var isComplete = false
        DriverInstallManager.shared.activate { result in
            switch result {
            case .success(let message):
                FileHandle.standardOutput.write(Data((message + "\n").utf8))
                Foundation.exit(0)
            case .failure(let error):
                FileHandle.standardError.write(Data((error.localizedDescription + "\n").utf8))
                Foundation.exit(1)
            }
            isComplete = true
        }

        while !isComplete {
            RunLoop.main.run(mode: .default, before: Date(timeIntervalSinceNow: 0.1))
        }
    }

    private static func registerHelperOnce() {
        do {
            let status = try MaintenanceHelperManager.shared.registerHelper()
            let message = "Maintenance helper status: \(status.displayName)"
            FileHandle.standardOutput.write(Data((message + "\n").utf8))
            Foundation.exit(status == .enabled ? 0 : 2)
        } catch {
            FileHandle.standardError.write(Data((error.localizedDescription + "\n").utf8))
            Foundation.exit(1)
        }
    }

    private static func refreshDriverOnce(explicitCDHash: String?) {
        let appBundleID = Bundle.main.bundleIdentifier ?? "com.chrisizatt.ASFWLocal"
        let driverBundleID = "\(appBundleID).ASFWDriver"
        let helper = MaintenanceHelperManager.shared
        let expectedCDHash = explicitCDHash ?? helper.stagedDriverCDHash(driverBundleID: driverBundleID)

        var isComplete = false
        helper.refreshDriver(expectedCDHash: expectedCDHash, driverBundleID: driverBundleID) { outcome in
            let message = outcome.snapshotPath.map { "\(outcome.message)\nSnapshot: \($0)" } ?? outcome.message
            switch outcome {
            case .succeeded:
                FileHandle.standardOutput.write(Data((message + "\n").utf8))
                Foundation.exit(0)
            case .repairNeeded, .rebootRequired:
                FileHandle.standardError.write(Data((message + "\n").utf8))
                Foundation.exit(2)
            case .failed:
                FileHandle.standardError.write(Data((message + "\n").utf8))
                Foundation.exit(1)
            }
            isComplete = true
        }

        while !isComplete {
            RunLoop.main.run(mode: .default, before: Date(timeIntervalSinceNow: 0.1))
        }
    }
}
