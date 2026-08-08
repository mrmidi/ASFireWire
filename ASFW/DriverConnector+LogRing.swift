import Foundation

// Client for the driver-owned log ring (user-client selectors 1011/1012/1014).
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

struct ASFWLogRingRecord: Identifiable, Equatable, Sendable {
    let sequence: UInt64
    let timestampNs: UInt64
    let category: UInt8
    let level: UInt8
    let message: String

    var id: UInt64 { sequence }
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
    let cursorReset: Bool

    init(records: [ASFWLogRingRecord], nextSequence: UInt64,
         latestSequence: UInt64, oldestSequence: UInt64,
         scannedCount: UInt32, cursorReset: Bool = false) {
        self.records = records
        self.nextSequence = nextSequence
        self.latestSequence = latestSequence
        self.oldestSequence = oldestSequence
        self.scannedCount = scannedCount
        self.cursorReset = cursorReset
    }
}

struct ASFWLogRingStats: Sendable {
    let totalEmitted: UInt64
    let droppedRecords: UInt64
    let latestSequence: UInt64
    let oldestSequence: UInt64
    let capacityRecords: UInt32
    let perCategory: [String: UInt64]
}

/// Legacy category mapping retained for MCP request compatibility. The GUI
/// renders selector 1014's driver-exported catalog and presets instead.
enum ASFWLogRingCategories {
    static let names: [String] = [
        "Controller", "Hardware", "BusReset", "Topology", "Metrics", "Async",
        "UserClient", "Discovery", "IRM", "BusManager", "ConfigROM",
        "MusicSubunit", "FCP", "CMP", "AVC", "Isoch", "Audio", "DirectAudio",
        "DICE", "Zts", "TxSyt", "PayloadWriter", "Oxfw",
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
        static let catalogSelector: UInt32 = 1014
        static let requestSize = 72
        static let responseHeaderSize = 40
        static let recordHeaderSize = 20
        static let containsCapacity = 48
        static let perCallCapacity = 4096
        static let catalogHeaderSize = 16
        static let categoryRecordSize = 32
        static let categoryNameCapacity = 28
        static let presetRecordSize = 32
        static let presetNameCapacity = 24
        static let catalogABIVersion: UInt32 = 1
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
                initialCap: LogRingWire.perCallCapacity,
                traceCalls: false
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
            selector: LogRingWire.statsSelector,
            input: nil,
            initialCap: 256,
            traceCalls: false
        ) else {
            return nil
        }
        return Self.decodeLogStats(data)
    }

    func logCategoryCatalog() -> ASFWLogCategoryCatalog? {
        guard let data = transport.callStruct(
            selector: LogRingWire.catalogSelector,
            input: nil,
            initialCap: 1_024,
            traceCalls: false
        ) else {
            return nil
        }
        return Self.decodeLogCategoryCatalog(data)
    }

    /// Executes log-ring I/O away from the main actor and serializes it with
    /// connector lifecycle work. The GUI uses this instead of blocking redraws.
    func queryLogRecordsAsync(_ query: ASFWLogRingQuery) async -> ASFWLogRingQueryResponse? {
        await withCheckedContinuation { continuation in
            connectionQueue.async { [weak self] in
                continuation.resume(returning: self?.queryLogRecords(query))
            }
        }
    }

    func logRingStatsAsync() async -> ASFWLogRingStats? {
        await withCheckedContinuation { continuation in
            connectionQueue.async { [weak self] in
                continuation.resume(returning: self?.logRingStats())
            }
        }
    }

    func logCategoryCatalogAsync() async -> ASFWLogCategoryCatalog? {
        await withCheckedContinuation { continuation in
            connectionQueue.async { [weak self] in
                continuation.resume(returning: self?.logCategoryCatalog())
            }
        }
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

    /// Decodes selector 1014. Internal for Swift Testing coverage.
    static func decodeLogCategoryCatalog(_ data: Data) -> ASFWLogCategoryCatalog? {
        guard data.count >= LogRingWire.catalogHeaderSize else { return nil }
        let bytes = [UInt8](data)

        func u16(_ offset: Int) -> UInt16 {
            UInt16(bytes[offset]) | UInt16(bytes[offset + 1]) << 8
        }
        func u32(_ offset: Int) -> UInt32 {
            UInt32(bytes[offset]) | UInt32(bytes[offset + 1]) << 8 |
                UInt32(bytes[offset + 2]) << 16 | UInt32(bytes[offset + 3]) << 24
        }
        func string(at offset: Int, length: Int, capacity: Int) -> String? {
            guard length > 0, length <= capacity,
                  offset >= 0, offset + length <= bytes.count else {
                return nil
            }
            return String(bytes: bytes[offset..<offset + length], encoding: .utf8)
        }

        let abiVersion = u32(0)
        let categoryCount = Int(u16(4))
        let presetCount = Int(u16(6))
        let categoryRecordSize = Int(u16(8))
        let presetRecordSize = Int(u16(10))
        let totalBytes = Int(u32(12))
        guard abiVersion == LogRingWire.catalogABIVersion,
              categoryCount > 0,
              categoryCount <= 32,
              presetCount <= 32,
              categoryRecordSize == LogRingWire.categoryRecordSize,
              presetRecordSize == LogRingWire.presetRecordSize else {
            return nil
        }

        let categoryBytes = categoryCount * categoryRecordSize
        let presetBytes = presetCount * presetRecordSize
        let expectedBytes = LogRingWire.catalogHeaderSize + categoryBytes + presetBytes
        guard totalBytes == expectedBytes, data.count >= totalBytes else { return nil }

        var categories: [ASFWLogCategoryDescriptor] = []
        categories.reserveCapacity(categoryCount)
        var seenCategories: Set<UInt8> = []
        var knownMask: UInt32 = 0
        for index in 0..<categoryCount {
            let base = LogRingWire.catalogHeaderSize + index * categoryRecordSize
            let category = bytes[base]
            let nameLength = Int(bytes[base + 1])
            guard category < 32,
                  seenCategories.insert(category).inserted,
                  let name = string(
                    at: base + 4,
                    length: nameLength,
                    capacity: LogRingWire.categoryNameCapacity
                  ) else {
                return nil
            }
            knownMask |= 1 << UInt32(category)
            categories.append(ASFWLogCategoryDescriptor(id: category, name: name))
        }

        var presets: [ASFWLogCategoryPreset] = []
        presets.reserveCapacity(presetCount)
        let presetsStart = LogRingWire.catalogHeaderSize + categoryBytes
        for index in 0..<presetCount {
            let base = presetsStart + index * presetRecordSize
            let categoryMask = u32(base)
            let nameLength = Int(bytes[base + 4])
            guard categoryMask & ~knownMask == 0,
                  let name = string(
                    at: base + 8,
                    length: nameLength,
                    capacity: LogRingWire.presetNameCapacity
                  ) else {
                return nil
            }
            presets.append(ASFWLogCategoryPreset(
                id: index,
                name: name,
                categoryMask: categoryMask
            ))
        }

        return ASFWLogCategoryCatalog(categories: categories, presets: presets)
    }
}
