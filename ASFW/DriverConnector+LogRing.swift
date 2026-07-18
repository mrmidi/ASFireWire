import Foundation

// Client for the driver-owned log ring (user-client selectors 1011/1012).
//
// The dext keeps a 10 MiB ring of structured ASFW_LOG records (sequence,
// timestamp, category, level, formatted text). This extension drains it with
// category/level/substring filters using the packed wire format defined in
// ASFWDriver/Logging/LogRing.hpp: a 40-byte response header followed by
// recordCount x { 20-byte record header, message bytes }.

struct ASFWLogRingQuery: Sendable {
    var afterSequence: UInt64 = 0
    var categoryMask: UInt32 = 0xFFFF_FFFF
    var maxLevel: UInt8 = 4
    var contains: String = ""
    /// Total records to gather; the connector loops 4 KiB drains until done.
    var maxRecords: Int = 200
}

struct ASFWLogRingRecord: Sendable {
    let sequence: UInt64
    let timestampNs: UInt64
    let category: UInt8
    let level: UInt8
    let message: String

    var categoryName: String { ASFWLogRingCategories.name(for: category) }
    var levelName: String { ASFWLogRingCategories.levelName(for: level) }
}

struct ASFWLogRingQueryResponse: Sendable {
    let records: [ASFWLogRingRecord]
    let nextSequence: UInt64
    let latestSequence: UInt64
    let oldestSequence: UInt64
    let scannedCount: UInt32
    /// The requested cursor was from a newer/different ring instance; this
    /// response restarted from retained history.
    let cursorReset: Bool = false
}

struct ASFWLogRingStats: Sendable {
    let totalEmitted: UInt64
    let droppedRecords: UInt64
    let latestSequence: UInt64
    let oldestSequence: UInt64
    let capacityRecords: UInt32
    let perCategory: [String: UInt64]
}

/// Mirrors ASFW::Logging::LogCategory (wire values are frozen; append-only).
enum ASFWLogRingCategories {
    static let names: [String] = [
        "Controller", "Hardware", "BusReset", "Topology", "Metrics", "Async",
        "UserClient", "Discovery", "IRM", "BusManager", "ConfigROM",
        "MusicSubunit", "FCP", "CMP", "AVC", "Isoch", "Audio", "DirectAudio",
        "DICE", "Zts", "TxSyt", "PayloadWriter",
    ]

    static func name(for category: UInt8) -> String {
        Int(category) < names.count ? names[Int(category)] : "Unknown(\(category))"
    }

    static func index(of name: String) -> Int? {
        names.firstIndex { $0.caseInsensitiveCompare(name) == .orderedSame }
    }

    /// Builds a category bitmask from names; nil input or empty list = all.
    static func mask(for categoryNames: [String]?) -> UInt32? {
        guard let categoryNames, !categoryNames.isEmpty else { return 0xFFFF_FFFF }
        var mask: UInt32 = 0
        for name in categoryNames {
            guard let index = index(of: name) else { return nil }
            mask |= 1 << UInt32(index)
        }
        return mask
    }

    static func levelName(for level: UInt8) -> String {
        switch level {
        case 0: return "error"
        case 1: return "warning"
        case 2: return "notice"
        case 3: return "info"
        default: return "debug"
        }
    }
}

extension ASFWDriverConnector {
    private enum LogRingWire {
        static let querySelector: UInt32 = 1011
        static let statsSelector: UInt32 = 1012
        static let requestSize = 72
        static let responseHeaderSize = 40
        static let recordHeaderSize = 20
        static let containsCapacity = 48
        static let perCallCapacity = 4096
    }

    /// Drains filtered records, looping the 4 KiB user-client calls until
    /// `maxRecords` are gathered or the initial ring frontier is caught up.
    func queryLogRecords(_ query: ASFWLogRingQuery) -> ASFWLogRingQueryResponse? {
        var records: [ASFWLogRingRecord] = []
        var cursor = query.afterSequence
        var latest: UInt64 = 0
        var oldest: UInt64 = 0
        var scanned: UInt32 = 0
        var targetLatest: UInt64?
        var cursorReset = false

        while query.maxRecords > records.count {
            let remaining = query.maxRecords - records.count
            let request = encodeRequest(
                query, afterSequence: cursor, maxRecords: remaining)
            guard let data = transport.callStruct(
                selector: LogRingWire.querySelector,
                input: request,
                initialCap: LogRingWire.perCallCapacity
            ), let page = Self.decodeLogPage(data) else {
                return records.isEmpty
                    ? nil
                    : ASFWLogRingQueryResponse(records: records, nextSequence: cursor,
                                               latestSequence: latest, oldestSequence: oldest,
                                               scannedCount: scanned, cursorReset: cursorReset)
            }

            records.append(contentsOf: page.records.prefix(query.maxRecords - records.count))
            latest = page.latestSequence
            oldest = page.oldestSequence
            scanned &+= page.scannedCount
            cursorReset = cursorReset || page.cursorReset
            targetLatest = page.cursorReset ? page.latestSequence : (targetLatest ?? page.latestSequence)
            if page.cursorReset {
                cursor = page.nextSequence
            } else {
                guard page.nextSequence > cursor else { break }
                cursor = page.nextSequence
            }
            if cursor >= targetLatest! {
                break
            }
        }

        return ASFWLogRingQueryResponse(records: records, nextSequence: cursor,
                                        latestSequence: latest, oldestSequence: oldest,
                                        scannedCount: scanned, cursorReset: cursorReset)
    }

    func logRingStats() -> ASFWLogRingStats? {
        guard let data = transport.callStruct(
            selector: LogRingWire.statsSelector, input: nil, initialCap: 256
        ) else {
            return nil
        }
        return Self.decodeLogStats(data)
    }

    private func encodeRequest(
        _ query: ASFWLogRingQuery,
        afterSequence: UInt64,
        maxRecords: Int
    ) -> Data {
        var data = Data(count: LogRingWire.requestSize)
        data.withUnsafeMutableBytes { raw in
            raw.storeBytes(of: afterSequence.littleEndian, toByteOffset: 0, as: UInt64.self)
            raw.storeBytes(of: query.categoryMask.littleEndian, toByteOffset: 8, as: UInt32.self)
            raw.storeBytes(of: UInt32(query.maxLevel).littleEndian, toByteOffset: 12, as: UInt32.self)
            raw.storeBytes(
                of: UInt32(clamping: maxRecords).littleEndian,
                toByteOffset: 16,
                as: UInt32.self
            )
            raw.storeBytes(of: UInt32(0), toByteOffset: 20, as: UInt32.self) // reserved
            let needle = Array(query.contains.utf8.prefix(LogRingWire.containsCapacity - 1))
            for (offset, byte) in needle.enumerated() {
                raw.storeBytes(of: byte, toByteOffset: 24 + offset, as: UInt8.self)
            }
        }
        return data
    }

    /// Decodes one packed 1011 response page. Internal for unit tests.
    static func decodeLogPage(_ data: Data) -> ASFWLogRingQueryResponse? {
        guard data.count >= LogRingWire.responseHeaderSize else { return nil }
        let bytes = [UInt8](data)

        func u32(_ offset: Int) -> UInt32 {
            UInt32(bytes[offset]) | UInt32(bytes[offset + 1]) << 8 |
            UInt32(bytes[offset + 2]) << 16 | UInt32(bytes[offset + 3]) << 24
        }
        func u64(_ offset: Int) -> UInt64 {
            UInt64(u32(offset)) | UInt64(u32(offset + 4)) << 32
        }

        let recordCount = u32(0)
        let scannedCount = u32(4)
        let nextSequence = u64(8)
        let latestSequence = u64(16)
        let oldestSequence = u64(24)
        let payloadBytes = Int(u32(32))
        let flags = u32(36)
        guard data.count >= LogRingWire.responseHeaderSize + payloadBytes else { return nil }

        var records: [ASFWLogRingRecord] = []
        records.reserveCapacity(Int(recordCount))
        var offset = LogRingWire.responseHeaderSize
        let end = LogRingWire.responseHeaderSize + payloadBytes
        while records.count < recordCount,
              offset + LogRingWire.recordHeaderSize <= end {
            let sequence = u64(offset)
            let timestampNs = u64(offset + 8)
            let category = bytes[offset + 16]
            let level = bytes[offset + 17]
            let messageLength = Int(UInt16(bytes[offset + 18]) | UInt16(bytes[offset + 19]) << 8)
            let messageStart = offset + LogRingWire.recordHeaderSize
            guard messageStart + messageLength <= end else { return nil }
            let message = String(
                decoding: bytes[messageStart..<messageStart + messageLength],
                as: UTF8.self)
            records.append(ASFWLogRingRecord(sequence: sequence, timestampNs: timestampNs,
                                             category: category, level: level,
                                             message: message))
            offset = messageStart + messageLength
        }
        guard records.count == recordCount else { return nil }
        return ASFWLogRingQueryResponse(records: records, nextSequence: nextSequence,
                                        latestSequence: latestSequence,
                                        oldestSequence: oldestSequence,
                                        scannedCount: scannedCount,
                                        cursorReset: (flags & 1) != 0)
    }

    /// Decodes the 1012 stats struct. Internal for unit tests.
    static func decodeLogStats(_ data: Data) -> ASFWLogRingStats? {
        guard data.count >= 32 else { return nil }
        let bytes = [UInt8](data)
        func u32(_ offset: Int) -> UInt32 {
            UInt32(bytes[offset]) | UInt32(bytes[offset + 1]) << 8 |
            UInt32(bytes[offset + 2]) << 16 | UInt32(bytes[offset + 3]) << 24
        }
        func u64(_ offset: Int) -> UInt64 {
            UInt64(u32(offset)) | UInt64(u32(offset + 4)) << 32
        }
        let categoryCount = Int(u32(28))
        let droppedOffset = 32 + categoryCount * 8
        guard data.count >= droppedOffset + 8 else { return nil }
        var perCategory: [String: UInt64] = [:]
        for index in 0..<categoryCount {
            let count = u64(32 + index * 8)
            if count > 0 {
                perCategory[ASFWLogRingCategories.name(for: UInt8(index))] = count
            }
        }
        return ASFWLogRingStats(totalEmitted: u64(0), droppedRecords: u64(droppedOffset), latestSequence: u64(8),
                                oldestSequence: u64(16), capacityRecords: u32(24),
                                perCategory: perCategory)
    }
}
