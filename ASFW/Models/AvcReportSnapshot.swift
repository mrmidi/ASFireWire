import Foundation

/// Portable discovery evidence. Data fields use JSON's base64 representation;
/// the text report renders the same bytes as offset-labelled hexadecimal.
struct AvcReportSnapshot: Codable, Sendable {
    /// 2 adds each device's FCP exchange log; 3 adds the driver's discovery
    /// document. Versions 1 and 2 still open.
    static let currentVersion = 3
    var schemaVersion = currentVersion
    var capturedAt = Date()
    var appVersion: String
    var driverVersion: String?
    var devices: [Device]

    struct Device: Codable, Sendable {
        var guid: UInt64
        var nodeID: UInt8
        var generation: UInt32
        var vendorID: UInt32
        var modelID: UInt32
        var vendorName: String
        var modelName: String
        var state: String
        var romUnits: [ROMUnit]
        var configROM: Data?
        /// Why `configROM` is absent, when it is.
        var configROMMissing: String? = nil
        var avcUnit: Unit?
        var notes: [String]
        /// Every FCP command the driver sent this unit and the reply, since attach
        /// or the last refresh. This is the raw discovery: tools/avc/avc_discover.py
        /// --replay rebuilds the capability graph from it.
        var exchanges: ExchangeLog? = nil
        /// The driver's discovery document for this unit, unchanged
        /// (AvcDiscoveryDocument): snapshot, graph and timed exchanges.
        var discovery: JSONValue? = nil
    }

    struct ExchangeLog: Codable, Sendable, Equatable {
        /// 1 = attach; increments with each refresh.
        var session: UInt32
        /// Exchanges the driver had no room to keep.
        var dropped: UInt32
        var records: [Exchange]
    }

    /// Byte arrays, not base64, so the dump is readable by the Python tools as-is.
    struct Exchange: Codable, Sendable, Equatable {
        var sequence: UInt32
        var generation: UInt32
        /// response, timeout, busReset, transportError, responseMismatch,
        /// refusedByFilter, busy, invalid. Only "response" carries reply bytes.
        var outcome: String
        var interim: Bool
        var retries: UInt8
        var command: [UInt8]
        var response: [UInt8]
    }

    struct ROMUnit: Codable, Sendable {
        var offset: UInt32
        var specifierID: UInt32
        var version: UInt32
    }

    struct Unit: Codable, Sendable {
        var isoInputPlugs: UInt8
        var isoOutputPlugs: UInt8
        var externalInputPlugs: UInt8
        var externalOutputPlugs: UInt8
        var subunits: [Subunit]
    }

    struct Subunit: Codable, Sendable {
        var type: UInt8
        var id: UInt8
        var sourcePlugs: UInt8
        var destinationPlugs: UInt8
        var capabilitySummary: String?
        var capabilities: Data?
        var descriptor: Data?
        /// Why `capabilities` / `descriptor` are absent, when they are.
        var capabilitiesMissing: String? = nil
        var descriptorMissing: String? = nil
    }

    func jsonData() throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        return try encoder.encode(self)
    }

    static func load(_ data: Data) throws -> Self {
        guard data.count <= 8 * 1024 * 1024 else { throw ImportError.invalidSize }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let report = try decoder.decode(Self.self, from: data)
        guard (1...currentVersion).contains(report.schemaVersion) else { throw ImportError.unsupportedVersion }
        guard report.devices.count <= 64,
              report.devices.allSatisfy({ device in
                  device.romUnits.count <= 256 && (device.configROM?.count ?? 0) <= 4096 &&
                  (device.avcUnit?.subunits.count ?? 0) <= 256 &&
                  (device.avcUnit?.subunits.allSatisfy {
                      ($0.capabilities?.count ?? 0) <= 4096 && ($0.descriptor?.count ?? 0) <= 4096
                  } ?? true) &&
                  (device.exchanges?.records.count ?? 0) <= 4096 &&
                  ((try? JSONEncoder().encode(device.discovery))?.count ?? 0) <= AvcDiscoveryDocument.maxBytes &&
                  (device.exchanges?.records.allSatisfy { $0.command.count <= 512 && $0.response.count <= 512 } ?? true)
              }) else { throw ImportError.invalidSize }
        return report
    }

    enum ImportError: LocalizedError {
        case invalidSize, unsupportedVersion
        var errorDescription: String? {
            switch self {
            case .invalidSize: "The AV/C dump exceeds the supported report size."
            case .unsupportedVersion: "This AV/C dump uses an unsupported report version."
            }
        }
    }
}

extension AvcReportSnapshot.ExchangeLog {
    struct Page {
        var session: UInt32
        var dropped: UInt32
        var totalRecords: UInt32
        var firstIndex: UInt32
        var records: [AvcReportSnapshot.Exchange]
    }

    /// Driver outcome codes (Protocols::AVC::FcpExchangeOutcome).
    static let outcomeNames = ["response", "timeout", "busReset", "transportError",
                               "responseMismatch", "refusedByFilter", "busy", "invalid"]

    /// Decode one user-client page (UserClient/WireFormats/AVCExchangeLogWire.hpp):
    /// a 24-byte header, then per record a 16-byte header, the command and
    /// response bytes, and padding to 4 bytes. Little-endian.
    static func parsePage(_ data: Data) -> Page? {
        let bytes = [UInt8](data)
        func u16(_ at: Int) -> UInt16 { UInt16(bytes[at]) | UInt16(bytes[at + 1]) << 8 }
        func u32(_ at: Int) -> UInt32 { UInt32(u16(at)) | UInt32(u16(at + 2)) << 16 }
        guard bytes.count >= 24 else { return nil }
        let count = Int(u32(16))
        var records: [AvcReportSnapshot.Exchange] = []
        var offset = 24
        for _ in 0..<count {
            guard offset + 16 <= bytes.count else { return nil }
            let commandLength = Int(u16(offset + 12))
            let responseLength = Int(u16(offset + 14))
            let body = offset + 16
            guard body + commandLength + responseLength <= bytes.count else { return nil }
            let code = Int(bytes[offset + 8])
            records.append(.init(
                sequence: u32(offset), generation: u32(offset + 4),
                outcome: code < outcomeNames.count ? outcomeNames[code] : "unknown(\(code))",
                interim: bytes[offset + 9] != 0, retries: bytes[offset + 10],
                command: Array(bytes[body..<(body + commandLength)]),
                response: Array(bytes[(body + commandLength)..<(body + commandLength + responseLength)])))
            offset += (16 + commandLength + responseLength + 3) & ~3
        }
        return Page(session: u32(0), dropped: u32(4), totalRecords: u32(8), firstIndex: u32(12), records: records)
    }
}
