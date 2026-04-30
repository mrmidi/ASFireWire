import Foundation

enum MaintenanceHelperApprovalState: String, Equatable {
    case unknown
    case notRegistered
    case requiresApproval
    case enabled
    case notFound
    case failed

    var displayName: String {
        switch self {
        case .unknown: return "Unknown"
        case .notRegistered: return "Not registered"
        case .requiresApproval: return "Needs approval"
        case .enabled: return "Enabled"
        case .notFound: return "Not found"
        case .failed: return "Failed"
        }
    }
}

enum MaintenanceHealthState: String, Equatable {
    case unknown
    case clean
    case repairNeeded
    case rebootRequired
    case uninstalled
}

struct MaintenanceStateSummary: Equatable {
    var health: MaintenanceHealthState
    var activeDriver: Bool
    var staleTerminatingDriver: Bool
    var coreAudioDeviceVisible: Bool
    var activeCDHash: String?
    var expectedCDHash: String?
    var message: String

    static let unknown = MaintenanceStateSummary(
        health: .unknown,
        activeDriver: false,
        staleTerminatingDriver: false,
        coreAudioDeviceVisible: false,
        activeCDHash: nil,
        expectedCDHash: nil,
        message: "Driver state has not been checked yet."
    )
}

enum MaintenanceOperationOutcome: Equatable {
    case succeeded(message: String, snapshotPath: String?)
    case repairNeeded(message: String, snapshotPath: String?)
    case rebootRequired(message: String, snapshotPath: String?)
    case failed(message: String, snapshotPath: String?)

    var message: String {
        switch self {
        case .succeeded(let message, _),
             .repairNeeded(let message, _),
             .rebootRequired(let message, _),
             .failed(let message, _):
            return message
        }
    }

    var snapshotPath: String? {
        switch self {
        case .succeeded(_, let path),
             .repairNeeded(_, let path),
             .rebootRequired(_, let path),
             .failed(_, let path):
            return path
        }
    }
}

struct ASFWMaintenanceParser {
    static func hasActiveDriver(systemExtensions: String, driverBundleID: String) -> Bool {
        systemExtensions
            .split(separator: "\n")
            .contains { line in
                line.contains(driverBundleID)
                && line.contains("[activated enabled]")
                && !line.contains("terminating for uninstall")
            }
    }

    static func hasStaleTerminatingDriver(systemExtensions: String, driverBundleID: String) -> Bool {
        systemExtensions
            .split(separator: "\n")
            .contains { line in
                line.contains(driverBundleID)
                && line.localizedCaseInsensitiveContains("terminating for uninstall")
            }
    }

    static func activeCDHash(ioregOutput: String) -> String? {
        let pattern = #""IOUserServerCDHash"\s*=\s*"([^"]+)""#
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return nil }
        let range = NSRange(ioregOutput.startIndex..<ioregOutput.endIndex, in: ioregOutput)
        guard let match = regex.firstMatch(in: ioregOutput, range: range),
              match.numberOfRanges > 1,
              let hashRange = Range(match.range(at: 1), in: ioregOutput) else {
            return nil
        }
        return String(ioregOutput[hashRange])
    }

    static func coreAudioContainsDevice(systemProfilerOutput: String,
                                        deviceName: String = "Alesis MultiMix Firewire") -> Bool {
        systemProfilerOutput.localizedCaseInsensitiveContains(deviceName)
    }

    static func summarize(systemExtensions: String,
                          ioregOutput: String,
                          coreAudioOutput: String,
                          expectedCDHash: String?,
                          driverBundleID: String) -> MaintenanceStateSummary {
        let active = hasActiveDriver(systemExtensions: systemExtensions, driverBundleID: driverBundleID)
        let stale = hasStaleTerminatingDriver(systemExtensions: systemExtensions, driverBundleID: driverBundleID)
        let cdHash = activeCDHash(ioregOutput: ioregOutput)
        let coreAudioVisible = coreAudioContainsDevice(systemProfilerOutput: coreAudioOutput)
        let expected = expectedCDHash?.isEmpty == false ? expectedCDHash : nil
        let cdHashMismatch = expected != nil && cdHash != expected

        if stale {
            return MaintenanceStateSummary(
                health: .rebootRequired,
                activeDriver: active,
                staleTerminatingDriver: true,
                coreAudioDeviceVisible: coreAudioVisible,
                activeCDHash: cdHash,
                expectedCDHash: expected,
                message: "macOS still has an ASFW driver terminating for uninstall. Reboot before another install attempt."
            )
        }

        if active && !cdHashMismatch && coreAudioVisible {
            return MaintenanceStateSummary(
                health: .clean,
                activeDriver: true,
                staleTerminatingDriver: false,
                coreAudioDeviceVisible: true,
                activeCDHash: cdHash,
                expectedCDHash: expected,
                message: "ASFW driver and CoreAudio device look healthy."
            )
        }

        if !active && cdHash == nil && !coreAudioVisible {
            return MaintenanceStateSummary(
                health: .uninstalled,
                activeDriver: false,
                staleTerminatingDriver: false,
                coreAudioDeviceVisible: false,
                activeCDHash: nil,
                expectedCDHash: expected,
                message: "ASFW driver is not active."
            )
        }

        return MaintenanceStateSummary(
            health: .repairNeeded,
            activeDriver: active,
            staleTerminatingDriver: false,
            coreAudioDeviceVisible: coreAudioVisible,
            activeCDHash: cdHash,
            expectedCDHash: expected,
            message: "ASFW driver state needs repair or refresh."
        )
    }
}

