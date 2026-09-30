import Foundation

/// Portable discovery evidence. Data fields use JSON's base64 representation;
/// the text report renders the same bytes as offset-labelled hexadecimal.
struct AvcReportSnapshot: Codable, Sendable {
    static let currentVersion = 1
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
        var avcUnit: Unit?
        var notes: [String]
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
        guard report.schemaVersion == currentVersion else { throw ImportError.unsupportedVersion }
        guard report.devices.count <= 64,
              report.devices.allSatisfy({ device in
                  device.romUnits.count <= 256 && (device.configROM?.count ?? 0) <= 4096 &&
                  (device.avcUnit?.subunits.count ?? 0) <= 256 &&
                  (device.avcUnit?.subunits.allSatisfy {
                      ($0.capabilities?.count ?? 0) <= 4096 && ($0.descriptor?.count ?? 0) <= 4096
                  } ?? true)
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
