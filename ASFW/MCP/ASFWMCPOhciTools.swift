import Foundation

// Read-only host OHCI register surface.
//
// Both tools are backed by the diagnostics ABI snapshot (`ASFWDiagOHCI`), which the
// driver already exports. That snapshot covers a fixed set of named registers — it
// is NOT arbitrary MMIO offset access. `asfw_read_ohci_register` therefore answers
// only for offsets in `coveredRegisters` and refuses anything else rather than
// implying a read it did not perform. Arbitrary-offset reads would need a new
// user-client selector in the dext.
//
// Offsets mirror `ASFWDriver/Hardware/RegisterMap.hpp` (`ASFW::Driver::Register32`),
// which is the single source of truth and cites OHCI 1.1 Table 5-1. That header is a
// C++ `enum class` in a namespace and cannot be bridged to Swift, so the values are
// restated here. Keep the two in sync when RegisterMap.hpp changes.

nonisolated struct ASFWMCPOhciRegister: Equatable {
    let name: String
    let offset: UInt32
    let value: UInt32
}

nonisolated struct ASFWMCPOhciSnapshot: Equatable {
    let generation: UInt32
    let registers: [ASFWMCPOhciRegister]
}

nonisolated enum ASFWMCPOhciRegisterMap {
    /// One table drives both the snapshot and the single-register read so the two
    /// tools can never disagree about which registers are covered.
    static let covered: [(name: String, offset: UInt32, field: KeyPath<ASFWDiagOHCI, UInt32>)] = [
        ("Version", 0x000, \.version),
        ("GUIDROM", 0x004, \.guidROM),
        ("ATRetries", 0x008, \.atRetries),
        ("CSRData", 0x00C, \.csrData),
        ("CSRCompareData", 0x010, \.csrCompareData),
        ("CSRControl", 0x014, \.csrControl),
        ("ConfigROMHeader", 0x018, \.configROMHeader),
        ("BusID", 0x01C, \.busIdRegister),
        ("BusOptions", 0x020, \.busOptions),
        ("GUIDHi", 0x024, \.guidHi),
        ("GUIDLo", 0x028, \.guidLo),
        ("ConfigROMMap", 0x034, \.configROMMap),
        ("PostedWriteAddressLo", 0x038, \.postedWriteAddressLo),
        ("PostedWriteAddressHi", 0x03C, \.postedWriteAddressHi),
        ("VendorId", 0x040, \.vendorId),
        // 0x050/0x054 are write-only set/clear pairs; on read both return the same
        // latched value. Exposed separately because the driver samples each.
        ("HCControlSet", 0x050, \.hcControlSet),
        ("HCControlClear", 0x054, \.hcControlClear),
        ("SelfIDBuffer", 0x064, \.selfIdBuffer),
        ("SelfIDCount", 0x068, \.selfIdCount),
        ("IntEventSet", 0x080, \.intEventSet),
        ("IntMaskSet", 0x088, \.intMaskSet),
        ("LinkControlSet", 0x0E0, \.linkControlSet),
        ("LinkControlClear", 0x0E4, \.linkControlClear),
        ("NodeID", 0x0E8, \.nodeId),
        ("PhyControl", 0x0EC, \.phyControl),
        ("CycleTimer", 0x0F0, \.isochronousCycleTimer)
    ]

    static var coveredOffsetList: String {
        covered.map { String(format: "0x%03X", $0.offset) }.joined(separator: ", ")
    }
}

extension ASFWMCPOhciRegister {
    var mcpValue: ASFWMCPValue {
        .object([
            "name": .string(name),
            "offset": .string(String(format: "0x%03X", offset)),
            "value": .string(String(format: "0x%08X", value))
        ])
    }
}

extension ASFWMCPOhciSnapshot {
    var mcpValue: ASFWMCPValue {
        .object([
            "generation": .int(Int(generation)),
            "source": .string("diagnosticsSnapshot"),
            "registerCount": .int(registers.count),
            "registers": .array(registers.map(\.mcpValue))
        ])
    }
}
