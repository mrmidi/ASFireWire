import Foundation
import IOKit

extension ASFWDriverConnector {
    /// On Apple Silicon the driver defers publishing its SBP-2 nub until it
    /// knows which DART the SCSI kernel shim should resolve DMA mappings
    /// through — the mapper id is only readable from user space (the live
    /// IODARTMapper object does not serialize across the DriverKit boundary).
    /// Look it up and hand it down. On Intel (no DART nodes) this finds
    /// nothing and returns; VT-d's system mapper covers that case.
    func provisionSCSIMapperID() {
        guard connection != 0 else { return }
        guard let mapperID = Self.fireWireDARTMapperID() else {
            log("No DART mapper found for the FireWire controller — skipping SCSI mapper provisioning",
                level: .info)
            return
        }
        var input = UInt64(mapperID)
        let kr = IOConnectCallScalarMethod(
            connection, Method.provisionSCSIMapperID.rawValue, &input, 1, nil, nil)
        if kr == KERN_SUCCESS {
            log(String(format: "Provisioned SCSI DART mapper id 0x%08x", mapperID), level: .success)
        } else {
            log("SCSI mapper provisioning failed: \(interpretIOReturn(kr))", level: .error)
        }
    }

    /// The IOMapperID of a DART serving the FireWire controller's PCIe
    /// complex. The HBA copies payloads via UserGetDataBuffer and never
    /// dereferences the IOVA, so any published DART satisfies the kernel
    /// shim — the tiers below just prefer the controller's own complex.
    static func fireWireDARTMapperID() -> UInt32? {
        let complex = fireWireControllerPath().flatMap { path in
            path.range(of: "apciec[0-9]+", options: .regularExpression).map { String(path[$0]) }
        }
        return selectDARTCandidate(complex: complex, candidates: dartMapperCandidates())
    }

    /// Pure selection logic, split out for testability: exact
    /// `mapper-<complex>-piodma` match first, then any mapper in the same
    /// complex, then any mapper at all. The trailing hyphen in tier two
    /// keeps `apciec1` from matching `mapper-apciec10-…`.
    static func selectDARTCandidate(
        complex: String?, candidates: [(name: String, id: UInt32)]
    ) -> UInt32? {
        if let complex {
            if let exact = candidates.first(where: { $0.name == "mapper-\(complex)-piodma" }) {
                return exact.id
            }
            if let sameComplex = candidates.first(where: { $0.name.hasPrefix("mapper-\(complex)-") })
            {
                return sameComplex.id
            }
        }
        return candidates.first?.id
    }

    /// IOService-plane path of the FireWire OHCI controller (PCI class-code
    /// 0x0C0010), or nil if none is present.
    private static func fireWireControllerPath() -> String? {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(
            kIOMainPortDefault, IOServiceMatching("IOPCIDevice"), &iterator) == KERN_SUCCESS
        else { return nil }
        defer { IOObjectRelease(iterator) }

        while true {
            let service = IOIteratorNext(iterator)
            guard service != 0 else { return nil }
            defer { IOObjectRelease(service) }

            guard
                let classCode = IORegistryEntryCreateCFProperty(
                    service, "class-code" as CFString, kCFAllocatorDefault, 0)?
                    .takeRetainedValue() as? Data,
                classCode.count >= 3,
                classCode[0] == 0x10, classCode[1] == 0x00, classCode[2] == 0x0C
            else { continue }

            var path = [CChar](repeating: 0, count: 512)
            if IORegistryEntryGetPath(service, "IOService", &path) == KERN_SUCCESS {
                return String(cString: path)
            }
        }
    }

    /// Every IODARTMapper publishing an IOMapperID, named by its
    /// IODARTMapperNub parent (e.g. `mapper-apciec0-piodma`). No registry
    /// entry named `dart-*` carries IOMapperID — the property lives on the
    /// IODARTMapper child of the mapper nub.
    private static func dartMapperCandidates() -> [(name: String, id: UInt32)] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(
            kIOMainPortDefault, IOServiceMatching("IODARTMapper"), &iterator) == KERN_SUCCESS
        else { return [] }
        defer { IOObjectRelease(iterator) }

        var candidates: [(name: String, id: UInt32)] = []
        while true {
            let entry = IOIteratorNext(iterator)
            guard entry != 0 else { break }
            defer { IOObjectRelease(entry) }

            guard
                let data = IORegistryEntryCreateCFProperty(
                    entry, "IOMapperID" as CFString, kCFAllocatorDefault, 0)?
                    .takeRetainedValue() as? Data,
                data.count >= 4
            else { continue }
            let id = data.withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }

            var parent: io_registry_entry_t = 0
            guard IORegistryEntryGetParentEntry(entry, "IOService", &parent) == KERN_SUCCESS
            else { continue }
            defer { IOObjectRelease(parent) }
            var nameBuffer = [CChar](repeating: 0, count: 128)
            guard IORegistryEntryGetName(parent, &nameBuffer) == KERN_SUCCESS else { continue }
            candidates.append((name: String(cString: nameBuffer), id: UInt32(littleEndian: id)))
        }
        return candidates
    }
}
