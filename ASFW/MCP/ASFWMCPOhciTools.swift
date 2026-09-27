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

struct ASFWMCPOhciRegister: Equatable, Sendable {
    let name: String
    let offset: UInt32
    let value: UInt32
}

struct ASFWMCPOhciSnapshot: Equatable, Sendable {
    let generation: UInt32
    let registers: [ASFWMCPOhciRegister]
}

/// KeyPath wrapper that is marked @unchecked Sendable.
/// KeyPath itself is a static reference and does not capture mutable state,
/// so it is safe to share across threads. The actual value is retrieved
/// from the `ohci` snapshot at runtime, not from the KeyPath.
struct SendableKeyPath<Root, Value> {
    let keyPath: KeyPath<Root, Value>

    init(_ keyPath: KeyPath<Root, Value>) {
        self.keyPath = keyPath
    }
}

extension SendableKeyPath: @unchecked Sendable {}

/// A single OHCI register entry with a KeyPath to retrieve the value from a snapshot.
struct OhciRegisterInfo: Sendable {
    let name: String
    let offset: UInt32
    let field: SendableKeyPath<ASFWDiagOHCI, UInt32>
}

enum ASFWMCPOhciRegisterMap {
    /// One table drives both the snapshot and the single-register read so the two
    /// tools can never disagree about which registers are covered.
    static var covered: [OhciRegisterInfo] { [
        OhciRegisterInfo("Version", 0x000, SendableKeyPath(\.version)),
        OhciRegisterInfo("GUIDROM", 0x004, SendableKeyPath(\.guidROM)),
        OhciRegisterInfo("ATRetries", 0x008, SendableKeyPath(\.atRetries)),
        OhciRegisterInfo("CSRData", 0x00C, SendableKeyPath(\.csrData)),
        OhciRegisterInfo("CSRCompareData", 0x010, SendableKeyPath(\.csrCompareData)),
        OhciRegisterInfo("CSRControl", 0x014, SendableKeyPath(\.csrControl)),
        OhciRegisterInfo("ConfigROMHeader", 0x018, SendableKeyPath(\.configROMHeader)),
        OhciRegisterInfo("BusID", 0x01C, SendableKeyPath(\.busIdRegister)),
        OhciRegisterInfo("BusOptions", 0x020, SendableKeyPath(\.busOptions)),
        OhciRegisterInfo("GUIDHi", 0x024, SendableKeyPath(\.guidHi)),
        OhciRegisterInfo("GUIDLo", 0x028, SendableKeyPath(\.guidLo)),
        OhciRegisterInfo("ConfigROMMap", 0x034, SendableKeyPath(\.configROMMap)),
        OhciRegisterInfo("PostedWriteAddressLo", 0x038, SendableKeyPath(\.postedWriteAddressLo)),
        OhciRegisterInfo("PostedWriteAddressHi", 0x03C, SendableKeyPath(\.postedWriteAddressHi)),
        OhciRegisterInfo("VendorId", 0x040, SendableKeyPath(\.vendorId)),
        // 0x050/0x054 are write-only set/clear pairs; on read both return the same
        // latched value. Exposed separately because the driver samples each.
        OhciRegisterInfo("HCControlSet", 0x050, SendableKeyPath(\.hcControlSet)),
        OhciRegisterInfo("HCControlClear", 0x054, SendableKeyPath(\.hcControlClear)),
        OhciRegisterInfo("SelfIDBuffer", 0x064, SendableKeyPath(\.selfIdBuffer)),
        OhciRegisterInfo("SelfIDCount", 0x068, SendableKeyPath(\.selfIdCount)),
        OhciRegisterInfo("IntEventSet", 0x080, SendableKeyPath(\.intEventSet)),
        OhciRegisterInfo("IntMaskSet", 0x088, SendableKeyPath(\.intMaskSet)),
        OhciRegisterInfo("LinkControlSet", 0x0E0, SendableKeyPath(\.linkControlSet)),
        OhciRegisterInfo("LinkControlClear", 0x0E4, SendableKeyPath(\.linkControlClear)),
        OhciRegisterInfo("NodeID", 0x0E8, SendableKeyPath(\.nodeId)),
        OhciRegisterInfo("PhyControl", 0x0EC, SendableKeyPath(\.phyControl)),
        OhciRegisterInfo("CycleTimer", 0x0F0, SendableKeyPath(\.isochronousCycleTimer))
    ] }

    static var coveredOffsetList: String {
        covered.map { String(format: "0x%03X", $0.offset) }.joined(separator: ", ")
    }
}

// MARK: - Register mapping with offsets

extension OhciRegisterInfo {
    /// Initialize with a KeyPath and automatically derive the offset from the field name.
    /// This is a convenience initializer for creating the covered array.
    init(_ name: String, _ offset: UInt32, _ field: SendableKeyPath<ASFWDiagOHCI, UInt32>) {
        self.init(name: name, offset: offset, field: field)
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
