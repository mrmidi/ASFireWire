import Foundation

// Read-only BridgeCo/BeBoB inventory. This intentionally exposes BootROM
// information only; starting the boot loader or flashing firmware remains
// outside MCP until there is an explicitly approved recovery workflow.

extension ASFWMCPToolCatalog {
    static let bebobTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(
            name: "asfw_bebob_read_bootrom_info",
            group: "bebob",
            visibility: .readOnly,
            readOnly: true,
            idempotent: false,
            summary: "Read and decode the BridgeCo BeBoB BootROM information block.",
            requiredProtocolHints: ["bebob"]
        )
    ]
}

/// Decoder for the BridgeCo BootROM information block. The device memory is
/// little-endian despite IEEE 1394 transport byte order. Wire layout is
/// cross-validated with the local ALSA BeBoB reference's bridgeco.rs:1787-1929;
/// this is a fresh Swift implementation.
struct ASFWMCPBeBoBBootRomInformation: Equatable {
    static let addressHigh: UInt16 = 0xffff
    static let addressLow: UInt32 = 0xc802_0000
    static let sizeWithoutDebugger = 80
    static let sizeWithDebugger = 104

    let protocolVersion: UInt32
    let bootloaderVersion: UInt32
    let guid: UInt64
    let hardwareModelId: UInt32
    let hardwareRevision: UInt32
    let softwareTimestamp: String
    let softwareId: UInt32
    let softwareVersion: UInt32
    let imageBaseAddress: UInt32
    let imageMaximumSize: UInt32
    let bootloaderTimestamp: String
    let debugger: Debugger?

    struct Debugger: Equatable {
        let timestamp: String
        let id: UInt32
        let version: UInt32
    }

    static func decode(_ bytes: [UInt8]) -> ASFWMCPBeBoBBootRomInformation? {
        guard bytes.count == sizeWithoutDebugger || bytes.count >= sizeWithDebugger,
              Array(bytes.prefix(8)) == Array("bridgeCo".utf8),
              let protocolVersion = littleEndian32(bytes, 8),
              let bootloaderVersion = littleEndian32(bytes, 12),
              let guid = littleEndian64(bytes, 16),
              let hardwareModelId = littleEndian32(bytes, 24),
              let hardwareRevision = littleEndian32(bytes, 28),
              let softwareTimestamp = timestamp(bytes, 32),
              let softwareId = littleEndian32(bytes, 48),
              let softwareVersion = littleEndian32(bytes, 52),
              let imageBaseAddress = littleEndian32(bytes, 56),
              let imageMaximumSize = littleEndian32(bytes, 60),
              let bootloaderTimestamp = timestamp(bytes, 64) else {
            return nil
        }

        let debugger: Debugger?
        if bytes.count >= sizeWithDebugger,
           !bytes[80..<96].allSatisfy({ $0 == 0 }),
           let timestamp = timestamp(bytes, 80),
           let id = littleEndian32(bytes, 96),
           let version = littleEndian32(bytes, 100) {
            debugger = Debugger(timestamp: timestamp, id: id, version: version)
        } else {
            debugger = nil
        }

        return ASFWMCPBeBoBBootRomInformation(
            protocolVersion: protocolVersion,
            bootloaderVersion: bootloaderVersion,
            guid: guid,
            hardwareModelId: hardwareModelId,
            hardwareRevision: hardwareRevision,
            softwareTimestamp: softwareTimestamp,
            softwareId: softwareId,
            softwareVersion: softwareVersion,
            imageBaseAddress: imageBaseAddress,
            imageMaximumSize: imageMaximumSize,
            bootloaderTimestamp: bootloaderTimestamp,
            debugger: debugger
        )
    }

    var mcpValue: ASFWMCPValue {
        var value: [String: ASFWMCPValue] = [
            "recognized": .bool(true),
            "protocolVersion": .uint64(UInt64(protocolVersion)),
            "bootloader": .object([
                "version": .string(versionString(bootloaderVersion)),
                "rawVersion": .uint64(UInt64(bootloaderVersion)),
                "timestamp": .string(bootloaderTimestamp)
            ]),
            "hardware": .object([
                "guid": .string(String(format: "0x%016llX", guid)),
                "modelId": .string(String(format: "0x%06X", hardwareModelId)),
                "revision": .string(versionString(hardwareRevision))
            ]),
            "software": .object([
                "timestamp": .string(softwareTimestamp),
                "id": .string(String(format: "0x%08X", softwareId)),
                "revision": .string(versionString(softwareVersion))
            ]),
            "image": .object([
                "baseAddress": .string(String(format: "0x%08X", imageBaseAddress)),
                "maximumSize": .uint64(UInt64(imageMaximumSize))
            ])
        ]
        if let debugger {
            value["debugger"] = .object([
                "timestamp": .string(debugger.timestamp),
                "id": .string(String(format: "0x%08X", debugger.id)),
                "revision": .string(versionString(debugger.version))
            ])
        }
        return .object(value)
    }

    private static func littleEndian32(_ bytes: [UInt8], _ offset: Int) -> UInt32? {
        guard bytes.count >= offset + 4 else { return nil }
        return UInt32(bytes[offset]) | UInt32(bytes[offset + 1]) << 8 |
            UInt32(bytes[offset + 2]) << 16 | UInt32(bytes[offset + 3]) << 24
    }

    private static func littleEndian64(_ bytes: [UInt8], _ offset: Int) -> UInt64? {
        guard bytes.count >= offset + 8 else { return nil }
        return (0..<8).reduce(UInt64(0)) { value, index in
            value | UInt64(bytes[offset + index]) << UInt64(index * 8)
        }
    }

    private static func timestamp(_ bytes: [UInt8], _ offset: Int) -> String? {
        guard bytes.count >= offset + 16,
              let date = String(bytes: bytes[offset..<(offset + 8)], encoding: .ascii),
              date.allSatisfy({ $0.isNumber }) else { return nil }
        let timeBytes = bytes[(offset + 8)..<(offset + 16)]
        if timeBytes.allSatisfy({ $0 == 0 }) { return "\(date)T000000Z" }
        guard let time = String(bytes: timeBytes, encoding: .ascii), time.allSatisfy({ $0.isNumber }) else {
            return nil
        }
        return "\(date)T\(time)Z"
    }

    private func versionString(_ value: UInt32) -> String {
        "\(value >> 24).\((value >> 16) & 0xff).\(value & 0xffff)"
    }
}
