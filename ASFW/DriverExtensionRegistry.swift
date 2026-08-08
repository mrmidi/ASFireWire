import Foundation
import IOKit

struct DriverExtensionRegistrySnapshot: Equatable, Sendable {
    var controllerPresent: Bool
    var driverAttached: Bool
    var serverPresent: Bool
    var associatedServiceCount: Int

    var hasOrphanedServer: Bool {
        serverPresent && !driverAttached && associatedServiceCount == 0
    }
}

enum DriverExtensionActivationDisposition: Equatable, Sendable {
    case attached
    case enabledWaitingForController
    case pending
    case orphanedServer
    case didNotAttach
}

enum DriverExtensionDeactivationDisposition: Equatable, Sendable {
    case detached
    case pending
    case orphanedServer
    case didNotDetach
}

struct DriverExtensionHealthPolicy {
    static func activationDisposition(
        for snapshot: DriverExtensionRegistrySnapshot,
        timedOut: Bool
    ) -> DriverExtensionActivationDisposition {
        if snapshot.driverAttached {
            return .attached
        }
        if !snapshot.controllerPresent {
            return .enabledWaitingForController
        }
        if !timedOut {
            return .pending
        }
        if snapshot.hasOrphanedServer {
            return .orphanedServer
        }
        return .didNotAttach
    }

    static func deactivationDisposition(
        for snapshot: DriverExtensionRegistrySnapshot,
        timedOut: Bool
    ) -> DriverExtensionDeactivationDisposition {
        if !snapshot.driverAttached && !snapshot.serverPresent {
            return .detached
        }
        if !timedOut {
            return .pending
        }
        if snapshot.hasOrphanedServer {
            return .orphanedServer
        }
        return .didNotDetach
    }
}

struct DriverExtensionVersionPolicy {
    static func replacementIsNewer(existing: String,
                                   replacement: String) -> Bool {
        replacement.compare(existing, options: [.numeric]) ==
            .orderedDescending
    }
}

enum DriverExtensionRegistryInspector {
    private static let driverServiceName = "ASFWDriver"
    private static let driverServerName = "net.mrmidi.ASFW.ASFWDriver"
    private static let pciMatch = "0x590111c1"

    static func snapshot() -> DriverExtensionRegistrySnapshot {
        let server = serverState()
        return DriverExtensionRegistrySnapshot(
            controllerPresent: controllerIsPresent(),
            driverAttached: driverIsAttached(),
            serverPresent: server.present,
            associatedServiceCount: server.associatedServiceCount
        )
    }

    private static func driverIsAttached() -> Bool {
        let service = IOServiceGetMatchingService(
            kIOMainPortDefault,
            IOServiceNameMatching(driverServiceName)
        )
        guard service != 0 else { return false }
        IOObjectRelease(service)
        return true
    }

    private static func controllerIsPresent() -> Bool {
        guard let matching = IOServiceMatching("IOPCIDevice") else {
            return false
        }
        let mutableMatching = matching as NSMutableDictionary
        mutableMatching["IOPCIMatch"] = pciMatch
        let service = IOServiceGetMatchingService(
            kIOMainPortDefault,
            matching
        )
        guard service != 0 else { return false }
        IOObjectRelease(service)
        return true
    }

    private static func serverState()
        -> (present: Bool, associatedServiceCount: Int) {
        guard let matching = IOServiceMatching("IOUserServer") else {
            return (false, 0)
        }

        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(
            kIOMainPortDefault,
            matching,
            &iterator
        ) == KERN_SUCCESS else {
            return (false, 0)
        }
        defer { IOObjectRelease(iterator) }

        while true {
            let entry = IOIteratorNext(iterator)
            if entry == 0 { break }
            defer { IOObjectRelease(entry) }

            guard registryString(entry, key: "IOUserServerName") ==
                    driverServerName else {
                continue
            }

            let associated = registryArrayCount(
                entry,
                key: "IOAssociatedServices"
            )
            return (true, associated)
        }
        return (false, 0)
    }

    private static func registryString(
        _ entry: io_registry_entry_t,
        key: String
    ) -> String? {
        guard let value = IORegistryEntryCreateCFProperty(
            entry,
            key as CFString,
            kCFAllocatorDefault,
            0
        )?.takeRetainedValue() else {
            return nil
        }
        return value as? String
    }

    private static func registryArrayCount(
        _ entry: io_registry_entry_t,
        key: String
    ) -> Int {
        guard let value = IORegistryEntryCreateCFProperty(
            entry,
            key as CFString,
            kCFAllocatorDefault,
            0
        )?.takeRetainedValue() else {
            return 0
        }
        return (value as? NSArray)?.count ?? 0
    }
}
