import Foundation

/// Writes a reviewable text report, the portable snapshot, and each available
/// binary evidence blob as its original bytes.
enum AvcReportExporter {
    struct Manifest: Codable, Sendable {
        var schemaVersion: Int
        var capturedAt: Date
        var artifacts: [Artifact]
    }

    struct Artifact: Codable, Sendable {
        var guid: String
        var kind: String
        var file: String
        var byteCount: Int
        var subunitType: UInt8?
        var subunitID: UInt8?
    }

    /// Exports into a new directory. Returns the manifest URL.
    @discardableResult
    static func export(_ snapshot: AvcReportSnapshot, to directory: URL) throws -> URL {
        let manager = FileManager.default
        guard !manager.fileExists(atPath: directory.path) else { throw CocoaError(.fileWriteFileExists) }
        try manager.createDirectory(at: directory, withIntermediateDirectories: false)
        try snapshot.jsonData().write(to: directory.appendingPathComponent("snapshot.json"), options: .atomic)
        let reportURL = directory.appendingPathComponent("report.txt", isDirectory: false)
        let manifestURL = directory.appendingPathComponent("manifest.json", isDirectory: false)
        try Data(AvcReportTextFormatter.format(snapshot).utf8).write(to: reportURL, options: .atomic)

        var artifacts: [Artifact] = []
        for device in snapshot.devices {
            let guid = String(format: "%016llX", device.guid)
            if let data = device.configROM {
                let file = "\(guid)-config-rom.bin"
                try data.write(to: directory.appendingPathComponent(file), options: .atomic)
                artifacts.append(.init(guid: guid, kind: "configROM", file: file, byteCount: data.count,
                                       subunitType: nil, subunitID: nil))
            }
            for subunit in device.avcUnit?.subunits ?? [] {
                let prefix = String(format: "%@-subunit-%02X-%02u", guid, subunit.type, subunit.id)
                if let data = subunit.capabilities {
                    let file = "\(prefix)-capabilities.bin"
                    try data.write(to: directory.appendingPathComponent(file), options: .atomic)
                    artifacts.append(.init(guid: guid, kind: "capabilities", file: file, byteCount: data.count,
                                           subunitType: subunit.type, subunitID: subunit.id))
                }
                if let data = subunit.descriptor {
                    let file = "\(prefix)-descriptor.bin"
                    try data.write(to: directory.appendingPathComponent(file), options: .atomic)
                    artifacts.append(.init(guid: guid, kind: "descriptor", file: file, byteCount: data.count,
                                           subunitType: subunit.type, subunitID: subunit.id))
                }
            }
        }
        let manifest = Manifest(schemaVersion: snapshot.schemaVersion, capturedAt: snapshot.capturedAt, artifacts: artifacts)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        try encoder.encode(manifest).write(to: manifestURL, options: .atomic)
        return manifestURL
    }
}
