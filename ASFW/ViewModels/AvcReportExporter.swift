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

    /// One device's exchange log in the dump format tools/avc/avc_discover.py
    /// --replay reads, so a shared report rebuilds the capability graph offline.
    struct ReplayDump: Codable, Sendable {
        struct Device: Codable, Sendable {
            var vendorName: String
            var modelName: String
            var guid: String
            var nodeId: UInt8
            var generation: UInt32
            var capturedAt: Date
            var tool: String
            var session: UInt32
            var dropped: UInt32
        }
        struct Record: Codable, Sendable {
            var name: String
            var command: [UInt8]
            var response: [UInt8]
            var responseCode: UInt8?
            var outcome: String
            var interim: Bool
            var retries: UInt8
            var generation: UInt32
        }
        var device: Device
        var records: [Record]

        init(_ device: AvcReportSnapshot.Device, log: AvcReportSnapshot.ExchangeLog, capturedAt: Date) {
            self.device = .init(vendorName: device.vendorName, modelName: device.modelName,
                                guid: String(format: "0x%016llx", device.guid), nodeId: device.nodeID,
                                generation: device.generation, capturedAt: capturedAt,
                                tool: "ASFW AV/C Report", session: log.session, dropped: log.dropped)
            records = log.records.map {
                .init(name: String(format: "#%04u", $0.sequence), command: $0.command, response: $0.response,
                      responseCode: $0.response.first, outcome: $0.outcome, interim: $0.interim,
                      retries: $0.retries, generation: $0.generation)
            }
        }
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

        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        var artifacts: [Artifact] = []
        for device in snapshot.devices {
            let guid = String(format: "%016llX", device.guid)
            if let log = device.exchanges {
                let file = "\(guid)-fcp-exchanges.json"
                let data = try encoder.encode(ReplayDump(device, log: log, capturedAt: snapshot.capturedAt))
                try data.write(to: directory.appendingPathComponent(file), options: .atomic)
                artifacts.append(.init(guid: guid, kind: "fcpExchanges", file: file, byteCount: data.count,
                                       subunitType: nil, subunitID: nil))
            }
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
        try encoder.encode(manifest).write(to: manifestURL, options: .atomic)
        return manifestURL
    }
}
