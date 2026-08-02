import Foundation

// Driver-owned diagnostic log ring. This is deliberately a read-only MCP
// surface: querying it never touches the FireWire bus and is safe while audio
// is running. Category names mirror ASFW::Logging::LogCategory exactly.

extension ASFWMCPToolCatalog {
    static let loggingTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(
            name: "asfw_log_query",
            group: "logging",
            visibility: .readOnly,
            readOnly: true,
            idempotent: true,
            summary: "Query the bounded driver log ring by cursor, category, severity, and substring."
        ),
        ASFWMCPToolDefinition(
            name: "asfw_log_stats",
            group: "logging",
            visibility: .readOnly,
            readOnly: true,
            idempotent: true,
            summary: "Return driver log-ring capacity, retention cursor, drop count, and per-category totals."
        ),
    ]
}

extension ASFWLogRingRecord {
    var mcpValue: ASFWMCPValue {
        .object([
            "sequence": .uint64(sequence),
            "timestampNs": .uint64(timestampNs),
            "category": .string(categoryName),
            "categoryId": .int(Int(category)),
            "level": .string(levelName),
            "levelId": .int(Int(level)),
            "message": .string(message),
        ])
    }
}

extension ASFWLogRingQueryResponse {
    var mcpValue: ASFWMCPValue {
        .object([
            "records": .array(records.map(\.mcpValue)),
            "recordCount": .int(records.count),
            "nextSequence": .uint64(nextSequence),
            "latestSequence": .uint64(latestSequence),
            "oldestSequence": .uint64(oldestSequence),
            "scannedCount": .uint64(UInt64(scannedCount)),
            "cursorReset": .bool(cursorReset),
        ])
    }
}

extension ASFWLogRingStats {
    var mcpValue: ASFWMCPValue {
        .object([
            "totalEmitted": .uint64(totalEmitted),
            "droppedRecords": .uint64(droppedRecords),
            "latestSequence": .uint64(latestSequence),
            "oldestSequence": .uint64(oldestSequence),
            "capacityRecords": .int(Int(capacityRecords)),
            "perCategory": .object(perCategory.mapValues(ASFWMCPValue.uint64)),
        ])
    }
}
