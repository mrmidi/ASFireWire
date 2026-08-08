import Foundation

// Symbolic DICE register addressing for the MCP control plane.
//
// The driver already owns the complete map (ASFWDriver/Audio/Protocols/DICE/Core/
// DICETypes.hpp: GeneralSections, GlobalOffset, TxOffset, RxOffset, ClockSelect,
// StatusBits, ExtStatusBits). Until now none of it was reachable from MCP:
// asfw_dice_read_register was a plain alias for asfw_read_quadlet, so a caller
// had to hand-compute absolute addresses, and asfw_dice_decode_status was a stub.
//
// SOURCE OF TRUTH: DICETypes.hpp. The offsets below mirror it because the driver
// and the control plane are different processes in different languages. The
// mirror is pinned by ASFWMCPDiceRegisterMapTests -- if the driver's layout
// changes, update both and the test will say so.
//
// Section bases are NOT constants: each device publishes its own section table
// at the DICE base, so resolving a symbolic register costs one extra block read.

/// DICE CSR base in IEEE 1394 private space (DICETypes.hpp kDICEBaseAddress).
enum ASFWMCPDiceSpace {
    static let baseAddressHigh: UInt16 = 0xFFFF
    static let baseAddressLow: UInt32 = 0xE000_0000

    /// Five (offset, size) pairs, each a quadlet count: global, tx, rx, extSync,
    /// reserved. GeneralSections::kWireSize.
    static let sectionTableBytes: UInt32 = 40
}

/// Which section a symbolic register lives in.
enum ASFWMCPDiceSection: String, CaseIterable {
    case global
    case txStreamFormat
    case rxStreamFormat
    case extSync

    /// Index of this section's (offset, size) pair in the section table.
    var tableIndex: Int {
        switch self {
        case .global: return 0
        case .txStreamFormat: return 1
        case .rxStreamFormat: return 2
        case .extSync: return 3
        }
    }
}

/// One section's byte offset and size, decoded from the device's section table.
struct ASFWMCPDiceSectionTable: Equatable {
    struct Entry: Equatable {
        let offsetBytes: UInt32
        let sizeBytes: UInt32
    }

    let entries: [Entry]

    /// Section descriptors store quadlet counts; Section::Deserialize multiplies
    /// by 4. Data is big-endian on the wire.
    static func decode(_ data: [UInt8]) -> ASFWMCPDiceSectionTable? {
        guard data.count >= Int(ASFWMCPDiceSpace.sectionTableBytes) else { return nil }
        var entries: [Entry] = []
        entries.reserveCapacity(5)
        for index in 0..<5 {
            let base = index * 8
            guard let offsetQuadlets = data.readBigEndianUInt32(at: base),
                  let sizeQuadlets = data.readBigEndianUInt32(at: base + 4) else {
                return nil
            }
            // A malformed or absent section table would otherwise produce a
            // plausible-looking absolute address pointing anywhere in the node.
            guard offsetQuadlets < 0x0400_0000, sizeQuadlets < 0x0400_0000 else {
                return nil
            }
            entries.append(Entry(offsetBytes: offsetQuadlets &* 4,
                                 sizeBytes: sizeQuadlets &* 4))
        }
        return ASFWMCPDiceSectionTable(entries: entries)
    }

    func entry(for section: ASFWMCPDiceSection) -> Entry? {
        guard section.tableIndex < entries.count else { return nil }
        return entries[section.tableIndex]
    }
}

/// A named DICE register.
///
/// `perStream` registers repeat once per isochronous stream; their address adds
/// `streamIndex * streamConfigSize`, where the size comes from the section's own
/// SIZE register (quadlets) rather than being assumed.
struct ASFWMCPDiceRegister {
    let name: String
    let section: ASFWMCPDiceSection
    let offset: UInt32
    let perStream: Bool
    let summary: String
}

enum ASFWMCPDiceRegisterMap {
    /// Mirrors GlobalOffset / TxOffset / RxOffset in DICETypes.hpp.
    static let all: [ASFWMCPDiceRegister] = [
        // --- GLOBAL (GlobalOffset) ---
        .init(name: "GLOBAL_OWNER_HI", section: .global, offset: 0x00, perStream: false, summary: "Owner node, high quadlet."),
        .init(name: "GLOBAL_OWNER_LO", section: .global, offset: 0x04, perStream: false, summary: "Owner node, low quadlet."),
        .init(name: "GLOBAL_NOTIFICATION", section: .global, offset: 0x08, perStream: false, summary: "Notification bits."),
        .init(name: "GLOBAL_NICKNAME", section: .global, offset: 0x0C, perStream: false, summary: "Device nickname (64 bytes)."),
        .init(name: "GLOBAL_CLOCK_SELECT", section: .global, offset: 0x4C, perStream: false, summary: "Clock source and rate index."),
        .init(name: "GLOBAL_ENABLE", section: .global, offset: 0x50, perStream: false, summary: "Streaming enable."),
        .init(name: "GLOBAL_STATUS", section: .global, offset: 0x54, perStream: false, summary: "Clock lock state and nominal rate."),
        .init(name: "GLOBAL_EXTENDED_STATUS", section: .global, offset: 0x58, perStream: false, summary: "Per-source lock and slip flags."),
        .init(name: "GLOBAL_SAMPLE_RATE", section: .global, offset: 0x5C, perStream: false, summary: "Measured sample rate."),
        .init(name: "GLOBAL_VERSION", section: .global, offset: 0x60, perStream: false, summary: "DICE interface version."),
        .init(name: "GLOBAL_CLOCK_CAPABILITIES", section: .global, offset: 0x64, perStream: false, summary: "Supported rate/source bitmask."),
        .init(name: "GLOBAL_CLOCK_SOURCE_NAMES", section: .global, offset: 0x68, perStream: false, summary: "Clock source name block."),

        // --- TX = device transmits = host capture (TxOffset) ---
        .init(name: "TX_NUMBER", section: .txStreamFormat, offset: 0x00, perStream: false, summary: "Number of device->host streams."),
        .init(name: "TX_SIZE", section: .txStreamFormat, offset: 0x04, perStream: false, summary: "Per-stream config size in quadlets."),
        .init(name: "TX_ISOCHRONOUS", section: .txStreamFormat, offset: 0x08, perStream: true, summary: "Isoch channel (-1 = disabled)."),
        .init(name: "TX_NUMBER_AUDIO", section: .txStreamFormat, offset: 0x0C, perStream: true, summary: "PCM channels in this capture stream."),
        .init(name: "TX_NUMBER_MIDI", section: .txStreamFormat, offset: 0x10, perStream: true, summary: "MIDI ports in this capture stream."),
        .init(name: "TX_SPEED", section: .txStreamFormat, offset: 0x14, perStream: true, summary: "Transmission speed (0=S100..2=S400)."),
        .init(name: "TX_NAMES", section: .txStreamFormat, offset: 0x18, perStream: true, summary: "Channel names (256 bytes)."),

        // --- RX = device receives = host playback (RxOffset) ---
        .init(name: "RX_NUMBER", section: .rxStreamFormat, offset: 0x00, perStream: false, summary: "Number of host->device streams."),
        .init(name: "RX_SIZE", section: .rxStreamFormat, offset: 0x04, perStream: false, summary: "Per-stream config size in quadlets."),
        .init(name: "RX_ISOCHRONOUS", section: .rxStreamFormat, offset: 0x08, perStream: true, summary: "Isoch channel (-1 = disabled)."),
        .init(name: "RX_SEQ_START", section: .rxStreamFormat, offset: 0x0C, perStream: true, summary: "Sequence start index."),
        .init(name: "RX_NUMBER_AUDIO", section: .rxStreamFormat, offset: 0x10, perStream: true, summary: "PCM channels in this playback stream."),
        .init(name: "RX_NUMBER_MIDI", section: .rxStreamFormat, offset: 0x14, perStream: true, summary: "MIDI ports in this playback stream."),
        .init(name: "RX_NAMES", section: .rxStreamFormat, offset: 0x18, perStream: true, summary: "Channel names (256 bytes).")
    ]

    static func register(named name: String) -> ASFWMCPDiceRegister? {
        let wanted = name.uppercased()
        return all.first { $0.name == wanted }
    }

    /// Absolute low address for a symbolic register.
    ///
    /// `streamConfigSizeBytes` is the section's SIZE register value (already in
    /// bytes) and is required only for per-stream registers.
    static func addressLow(for register: ASFWMCPDiceRegister,
                           sectionOffsetBytes: UInt32,
                           streamIndex: UInt32,
                           streamConfigSizeBytes: UInt32) -> UInt32? {
        var offset = register.offset
        if register.perStream {
            guard streamConfigSizeBytes > 0 else { return nil }
            let (stride, overflow) = streamConfigSizeBytes.multipliedReportingOverflow(by: streamIndex)
            guard !overflow else { return nil }
            let (withStride, strideOverflow) = offset.addingReportingOverflow(stride)
            guard !strideOverflow else { return nil }
            offset = withStride
        } else if streamIndex != 0 {
            return nil // streamIndex is meaningless for a section-scalar register
        }
        let (sectionRelative, sectionOverflow) = sectionOffsetBytes.addingReportingOverflow(offset)
        guard !sectionOverflow else { return nil }
        let (absolute, absoluteOverflow) =
            ASFWMCPDiceSpace.baseAddressLow.addingReportingOverflow(sectionRelative)
        guard !absoluteOverflow else { return nil }
        return absolute
    }
}

// MARK: - Decoding

/// Decoded GLOBAL_STATUS / GLOBAL_EXTENDED_STATUS / GLOBAL_CLOCK_SELECT.
enum ASFWMCPDiceDecoder {
    /// StatusBits in DICETypes.hpp.
    static func decodeStatus(_ value: UInt32) -> ASFWMCPValue {
        let rateIndex = (value & 0x0000_FF00) >> 8
        return .object([
            "raw": .string(String(format: "0x%08X", value)),
            "sourceLocked": .bool((value & 0x0000_0001) != 0),
            "nominalRateIndex": .int(Int(rateIndex)),
            "nominalRateHz": rateHz(forIndex: rateIndex).map { .int(Int($0)) } ?? .null
        ])
    }

    /// ClockSelect in DICETypes.hpp: [rate index << 8 | source].
    static func decodeClockSelect(_ value: UInt32) -> ASFWMCPValue {
        let source = value & 0x0000_00FF
        let rateIndex = (value & 0x0000_FF00) >> 8
        return .object([
            "raw": .string(String(format: "0x%08X", value)),
            "sourceIndex": .int(Int(source)),
            "sourceName": .string(clockSourceName(source)),
            "rateIndex": .int(Int(rateIndex)),
            "rateHz": rateHz(forIndex: rateIndex).map { .int(Int($0)) } ?? .null
        ])
    }

    /// ExtStatusBits in DICETypes.hpp. Lock flags occupy the low half; the
    /// matching slip flags sit in the high half.
    ///
    /// ARX1..ARX4 are the device's *audio receive* streams, i.e. the streams it
    /// receives from us -- ARX1 lock reflects our host->device transmit.
    static let extendedStatusFlags: [(name: String, mask: UInt32)] = [
        ("aes0", 0x0000_0001), ("aes1", 0x0000_0002),
        ("aes2", 0x0000_0004), ("aes3", 0x0000_0008),
        ("adat", 0x0000_0010), ("tdif", 0x0000_0020),
        ("arx1", 0x0000_0040), ("arx2", 0x0000_0080),
        ("arx3", 0x0000_0100), ("arx4", 0x0000_0200),
        ("wordClock", 0x0000_0400)
    ]

    static func decodeExtendedStatus(_ value: UInt32) -> ASFWMCPValue {
        var locked: [String: ASFWMCPValue] = [:]
        var slipping: [String: ASFWMCPValue] = [:]
        for flag in extendedStatusFlags {
            locked[flag.name] = .bool((value & flag.mask) != 0)
            slipping[flag.name] = .bool((value & (flag.mask << 16)) != 0)
        }
        return .object([
            "raw": .string(String(format: "0x%08X", value)),
            "locked": .object(locked),
            "slipping": .object(slipping),
            "note": .string("arx1..arx4 are the device's receive streams; arx1 reflects the host->device transmit.")
        ])
    }

    /// ClockRateIndex in DICETypes.hpp.
    static func rateHz(forIndex index: UInt32) -> UInt32? {
        switch index {
        case 0x00: return 32_000
        case 0x01: return 44_100
        case 0x02: return 48_000
        case 0x03: return 88_200
        case 0x04: return 96_000
        case 0x05: return 176_400
        case 0x06: return 192_000
        default: return nil
        }
    }

    /// Matches ClockSourceIndexToDefaultName in the TCAT reference driver.
    static func clockSourceName(_ index: UInt32) -> String {
        switch index {
        case 0: return "AES 1"
        case 1: return "AES 2"
        case 2: return "AES 3"
        case 3: return "AES 4"
        case 4: return "AES Any"
        case 5: return "ADAT"
        case 6: return "TDIF"
        case 7: return "Word Clock"
        case 8: return "ARX 1"
        case 9: return "ARX 2"
        case 10: return "ARX 3"
        case 11: return "ARX 4"
        case 12: return "Internal"
        default: return "Unknown"
        }
    }

    /// Decode by register name; nil when the register has no defined decoding.
    static func decode(registerName: String, value: UInt32) -> ASFWMCPValue? {
        switch registerName.uppercased() {
        case "GLOBAL_STATUS": return decodeStatus(value)
        case "GLOBAL_EXTENDED_STATUS": return decodeExtendedStatus(value)
        case "GLOBAL_CLOCK_SELECT": return decodeClockSelect(value)
        default: return nil
        }
    }
}

extension Array where Element == UInt8 {
    /// FireWire payloads are delivered in bus order (big-endian).
    func readBigEndianUInt32(at offset: Int) -> UInt32? {
        guard offset >= 0, count >= offset + 4 else { return nil }
        return (UInt32(self[offset]) << 24)
            | (UInt32(self[offset + 1]) << 16)
            | (UInt32(self[offset + 2]) << 8)
            | UInt32(self[offset + 3])
    }
}
