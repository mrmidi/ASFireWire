import Foundation

enum MaintenanceRecommendedAction: String, Equatable {
    case none
    case moveAppToApplications
    case installOrUpdateDriver
    case enableHelper
    case approveHelper
    case repairOnce
    case reboot
    case reconnectDevice
    case captureDiagnostics
    case sendDiagnostics

    var displayName: String {
        switch self {
        case .none: return "No action needed"
        case .moveAppToApplications: return "Open the app from Applications"
        case .installOrUpdateDriver: return "Install / Update Driver"
        case .enableHelper: return "Enable Maintenance Helper"
        case .approveHelper: return "Approve Maintenance Helper"
        case .repairOnce: return "Repair Driver"
        case .reboot: return "Reboot"
        case .reconnectDevice: return "Reconnect FireWire device"
        case .captureDiagnostics: return "Capture Diagnostics"
        case .sendDiagnostics: return "Send Diagnostics"
        }
    }

    var buttonTitle: String {
        switch self {
        case .none: return "No Action Needed"
        case .moveAppToApplications: return "Open Applications"
        case .installOrUpdateDriver: return "Install / Update"
        case .enableHelper: return "Enable Helper"
        case .approveHelper: return "Approve Helper"
        case .repairOnce: return "Repair Once"
        case .reboot: return "Reboot Required"
        case .reconnectDevice: return "Reconnect Device"
        case .captureDiagnostics, .sendDiagnostics: return "Capture Diagnostics"
        }
    }

    var isDirectlyActionableInApp: Bool {
        switch self {
        case .moveAppToApplications,
             .installOrUpdateDriver,
             .enableHelper,
             .approveHelper,
             .repairOnce,
             .captureDiagnostics,
             .sendDiagnostics:
            return true
        case .none, .reboot, .reconnectDevice:
            return false
        }
    }
}

struct MaintenanceLifecycleInputs: Equatable {
    var isRunningFromApplications: Bool
    var helperStatus: MaintenanceHelperApprovalState
    var userClientConnected: Bool
    var systemExtensions: String
    var driverIoreg: String
    var audioNubIoreg: String
    var coreAudioOutput: String
    var expectedCDHash: String?
    var expectedCoreAudioDeviceName: String?
    var stagedDriverPresent: Bool
    var driverBundleID: String
}

struct MaintenanceLifecycleStatus: Equatable {
    var health: MaintenanceHealthState
    var recommendedAction: MaintenanceRecommendedAction
    var summary: String
    var detail: String
    var activeDriver: Bool
    var staleTerminatingDriver: Bool
    var coreAudioDeviceVisible: Bool
    var audioNubVisible: Bool
    var userClientConnected: Bool
    var stagedDriverPresent: Bool
    var activeCDHash: String?
    var expectedCDHash: String?
    var expectedCoreAudioDeviceName: String?

    static let unknown = MaintenanceLifecycleStatus(
        health: .unknown,
        recommendedAction: .captureDiagnostics,
        summary: "Driver state has not been checked yet.",
        detail: "Open the app from Applications and refresh status.",
        activeDriver: false,
        staleTerminatingDriver: false,
        coreAudioDeviceVisible: false,
        audioNubVisible: false,
        userClientConnected: false,
        stagedDriverPresent: false,
        activeCDHash: nil,
        expectedCDHash: nil,
        expectedCoreAudioDeviceName: "Alesis MultiMix Firewire"
    )

    var isCleanForAudio: Bool {
        health == .clean && activeDriver && coreAudioDeviceVisible && audioNubVisible
    }

    var copySummary: String {
        [
            "ASFW lifecycle: \(summary)",
            "Detail: \(detail)",
            "Recommended action: \(recommendedAction.displayName)",
            "Active driver: \(activeDriver ? "yes" : "no")",
            "ASFW audio nub: \(audioNubVisible ? "yes" : "no")",
            "Expected CoreAudio device: \(expectedCoreAudioDeviceName ?? "not required")",
            "CoreAudio expected device visible: \(expectedCoreAudioDeviceName == nil ? "not required" : (coreAudioDeviceVisible ? "yes" : "no"))",
            "Debug user-client: \(userClientConnected ? "connected" : "unavailable")",
            "Stale uninstall: \(staleTerminatingDriver ? "yes" : "no")",
            "Active CDHash: \(activeCDHash ?? "unknown")",
            "Expected CDHash: \(expectedCDHash ?? "unknown")"
        ].joined(separator: "\n")
    }
}

struct ASFWMaintenanceLifecycleEvaluator {
    static func evaluate(_ inputs: MaintenanceLifecycleInputs) -> MaintenanceLifecycleStatus {
        let summary = ASFWMaintenanceParser.summarize(
            systemExtensions: inputs.systemExtensions,
            ioregOutput: inputs.driverIoreg,
            coreAudioOutput: inputs.coreAudioOutput,
            expectedCDHash: inputs.expectedCDHash,
            driverBundleID: inputs.driverBundleID,
            expectedCoreAudioDeviceName: inputs.expectedCoreAudioDeviceName
        )
        let audioNubVisible = ASFWMaintenanceParser.hasAudioNub(
            driverIoregOutput: inputs.driverIoreg,
            audioNubIoregOutput: inputs.audioNubIoreg
        )
        let expected = inputs.expectedCDHash?.isEmpty == false ? inputs.expectedCDHash : nil
        let cdHashMismatch = expected != nil && summary.activeCDHash != expected

        func status(_ health: MaintenanceHealthState,
                    _ action: MaintenanceRecommendedAction,
                    _ title: String,
                    _ detail: String) -> MaintenanceLifecycleStatus {
            MaintenanceLifecycleStatus(
                health: health,
                recommendedAction: action,
                summary: title,
                detail: detail,
                activeDriver: summary.activeDriver,
                staleTerminatingDriver: summary.staleTerminatingDriver,
                coreAudioDeviceVisible: summary.coreAudioDeviceVisible,
                audioNubVisible: audioNubVisible,
                userClientConnected: inputs.userClientConnected,
                stagedDriverPresent: inputs.stagedDriverPresent,
                activeCDHash: summary.activeCDHash,
                expectedCDHash: expected,
                expectedCoreAudioDeviceName: inputs.expectedCoreAudioDeviceName
            )
        }

        if !inputs.isRunningFromApplications {
            return status(.repairNeeded,
                          .moveAppToApplications,
                          "Use the Applications copy before maintenance.",
                          "Installing, updating, and repairing must be done from /Applications/ASFWLocal.app.")
        }

        if summary.staleTerminatingDriver {
            return status(.rebootRequired,
                          .reboot,
                          "A stale ASFW driver is still terminating.",
                          "Reboot before another install or repair attempt.")
        }

        if !summary.activeDriver {
            return status(.uninstalled,
                          .installOrUpdateDriver,
                          "ASFW driver is not active.",
                          inputs.stagedDriverPresent ? "Install or update the driver from this app." : "The staged driver is missing from the app bundle.")
        }

        if cdHashMismatch {
            return status(.repairNeeded,
                          helperAction(for: inputs.helperStatus, fallback: .repairOnce),
                          "Active driver does not match the staged driver.",
                          "Use one Repair Driver attempt. If the mismatch remains, reboot before another install.")
        }

        if !audioNubVisible && !summary.coreAudioDeviceVisible {
            return status(.repairNeeded,
                          .reconnectDevice,
                          "Driver is active, but no ASFW audio device is attached.",
                          "Reconnect or power-cycle the FireWire device once, then recheck status.")
        }

        if audioNubVisible && !summary.coreAudioDeviceVisible {
            return status(.repairNeeded,
                          helperAction(for: inputs.helperStatus, fallback: .repairOnce),
                          "ASFW audio nub exists, but CoreAudio is not publishing Alesis.",
                          "Close Logic or other audio apps, then use one Repair Driver attempt.")
        }

        if !inputs.userClientConnected {
            return status(.clean,
                          .none,
                          inputs.expectedCoreAudioDeviceName == nil ? "ASFW driver is active; debug connection is unavailable." : "Audio path is healthy; debug connection is unavailable.",
                          inputs.expectedCoreAudioDeviceName == nil ? "This tester build does not require a specific CoreAudio device for maintenance health. Use diagnostics/logs for device-specific Midas results." : "CoreAudio can see Alesis. Some debug tabs need the user-client entitlement/connection, but audio can continue without it.")
        }

        return status(.clean,
                      .none,
                      inputs.expectedCoreAudioDeviceName == nil ? "ASFW driver is active." : "ASFW audio path looks healthy.",
                      inputs.expectedCoreAudioDeviceName == nil ? "The active driver matches the staged driver. This tester build does not require CoreAudio publication before collecting device logs." : "The active driver, ASFW audio nub, and CoreAudio Alesis device are all visible.")
    }

    private static func helperAction(for helperStatus: MaintenanceHelperApprovalState,
                                     fallback: MaintenanceRecommendedAction) -> MaintenanceRecommendedAction {
        switch helperStatus {
        case .enabled:
            return fallback
        case .requiresApproval:
            return .approveHelper
        case .notRegistered, .unknown:
            return .enableHelper
        case .notFound, .failed:
            return .captureDiagnostics
        }
    }
}

extension ASFWMaintenanceParser {
    static func hasAudioNub(driverIoregOutput: String, audioNubIoregOutput: String) -> Bool {
        driverIoregOutput.contains("ASFWAudioNub") || audioNubIoregOutput.contains("ASFWAudioNub")
    }
}

struct MaintenanceLocalProbe {
    static func collect(isRunningFromApplications: Bool,
                        helperStatus: MaintenanceHelperApprovalState,
                        userClientConnected: Bool,
                        driverBundleID: String,
                        expectedCDHash: String?) -> MaintenanceLifecycleInputs {
        let stagedDextPath = Bundle.main.bundleURL
            .appendingPathComponent("Contents/Library/SystemExtensions", isDirectory: true)
            .appendingPathComponent("\(driverBundleID).dext", isDirectory: true)
            .path
        let stagedDriverPresent = FileManager.default.fileExists(atPath: stagedDextPath)

        return MaintenanceLifecycleInputs(
            isRunningFromApplications: isRunningFromApplications,
            helperStatus: helperStatus,
            userClientConnected: userClientConnected,
            systemExtensions: run("/usr/bin/systemextensionsctl", ["list"], timeout: 10),
            driverIoreg: run("/usr/sbin/ioreg", ["-p", "IOService", "-l", "-w0", "-r", "-c", "ASFWDriver"], timeout: 10),
            audioNubIoreg: run("/usr/sbin/ioreg", ["-p", "IOService", "-l", "-w0", "-r", "-c", "ASFWAudioNub"], timeout: 10),
            coreAudioOutput: run("/usr/sbin/system_profiler", ["SPAudioDataType"], timeout: 20),
            expectedCDHash: expectedCDHash,
            expectedCoreAudioDeviceName: MaintenanceCoreAudioExpectation.currentDeviceName,
            stagedDriverPresent: stagedDriverPresent,
            driverBundleID: driverBundleID
        )
    }

    private static func run(_ executable: String, _ arguments: [String], timeout: TimeInterval) -> String {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments

        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = Pipe()

        do {
            try process.run()
        } catch {
            return ""
        }

        let finished = DispatchSemaphore(value: 0)
        process.terminationHandler = { _ in finished.signal() }
        if finished.wait(timeout: .now() + timeout) == .timedOut {
            process.terminate()
            _ = finished.wait(timeout: .now() + 1)
        }

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        return String(data: data, encoding: .utf8) ?? ""
    }
}

enum MaintenanceCoreAudioExpectation {
    static var currentDeviceName: String? {
        let bundle = Bundle.main
        if let required = bundle.object(forInfoDictionaryKey: "ASFWRequireCoreAudioDevice") as? Bool,
           required == false {
            return nil
        }
        if let value = bundle.object(forInfoDictionaryKey: "ASFWExpectedCoreAudioDeviceName") as? String {
            let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty && !trimmed.hasPrefix("$(") {
                return trimmed
            }
        }
        return "Alesis MultiMix Firewire"
    }
}
