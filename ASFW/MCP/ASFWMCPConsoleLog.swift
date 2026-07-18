import Foundation
import os

// Unified-log mirror for MCP control-plane activity.
//
// Driver-side traffic shows up in `log stream` as [UserClient]/[Async]/[FCP]
// lines, but without this mirror the MCP tool call that *caused* that traffic
// is invisible, so a console capture cannot be correlated with control-plane
// activity. The tag lives in the message text (same convention as the dext's
// [Tag] prefixes) so one predicate covers app and driver:
//
//   log stream --info --debug --predicate 'eventMessage CONTAINS "[MCP]"'
//
// Levels: tool calls log at default level (they are rare and operator
// driven); resource reads log at debug because health/telemetry are polled
// by every client connect and would otherwise drown the default view.
nonisolated enum ASFWMCPConsoleLog {
    private static let logger = Logger(subsystem: "net.mrmidi.ASFW", category: "MCP")
    private static let argumentLimit = 300
    private static let resultLimit = 700
    private static let reasonLimit = 200

    static func toolRequested(name: String, arguments: ASFWMCPValue) {
        logger.log("[MCP] call \(name, privacy: .public) args=\(compactJSON(arguments, limit: argumentLimit), privacy: .public)")
    }

    static func toolCompleted(name: String, result: ASFWMCPToolCallResult, startedAt: ContinuousClock.Instant) {
        let milliseconds = (ContinuousClock.now - startedAt).milliseconds
        let payload = compactJSON(result.data, limit: resultLimit)
        if result.ok {
            logger.log("[MCP] done \(name, privacy: .public) ok=1 ms=\(milliseconds) data=\(payload, privacy: .public)")
        } else {
            let code = result.errors.first?.code.rawValue ?? "unknown"
            let reason = truncated(result.errors.first?.reason ?? "", limit: reasonLimit)
            logger.error("[MCP] fail \(name, privacy: .public) code=\(code, privacy: .public) ms=\(milliseconds) reason=\(reason, privacy: .public) data=\(payload, privacy: .public)")
        }
    }

    static func resourceRead(uri: String, ok: Bool) {
        if ok {
            logger.debug("[MCP] read \(uri, privacy: .public) ok=1")
        } else {
            logger.error("[MCP] read \(uri, privacy: .public) ok=0")
        }
    }

    /// Deterministic single-line JSON (sorted object keys) bounded to `limit`
    /// characters so argument echoes can never flood the console.
    static func compactJSON(_ value: ASFWMCPValue, limit: Int) -> String {
        truncated(render(value), limit: limit)
    }

    private static func render(_ value: ASFWMCPValue) -> String {
        switch value {
        case .null:
            return "null"
        case .bool(let flag):
            return flag ? "true" : "false"
        case .int(let number):
            return String(number)
        case .uint64(let number):
            return String(number)
        case .string(let text):
            let escaped = text
                .replacingOccurrences(of: "\\", with: "\\\\")
                .replacingOccurrences(of: "\"", with: "\\\"")
            return "\"\(escaped)\""
        case .array(let values):
            // Raw payloads arrive as byte arrays; render them as one hex
            // string ("hex:0009 00C8 …") instead of a wall of decimals so the
            // console line stays decodable at a glance.
            if let hex = hexBlob(values) {
                return "\"\(hex)\""
            }
            return "[" + values.map(render).joined(separator: ",") + "]"
        case .object(let fields):
            let body = fields.keys.sorted()
                .map { "\"\($0)\":\(render(fields[$0]!))" }
                .joined(separator: ",")
            return "{" + body + "}"
        }
    }

    private static func truncated(_ text: String, limit: Int) -> String {
        guard text.count > limit else { return text }
        return text.prefix(limit) + "…"
    }

    /// Collapses an all-byte integer array (>= 8 entries, every value in
    /// 0...255) into a grouped hex string. Returns nil when the array is not
    /// byte-like so ordinary arrays render as JSON.
    private static func hexBlob(_ values: [ASFWMCPValue]) -> String? {
        guard values.count >= 8 else { return nil }
        var bytes: [UInt8] = []
        bytes.reserveCapacity(values.count)
        for value in values {
            guard case .int(let number) = value, (0...255).contains(number) else { return nil }
            bytes.append(UInt8(number))
        }
        let hex = bytes.map { String(format: "%02X", $0) }
        var grouped: [String] = []
        var index = 0
        while index < hex.count {
            let end = min(index + 4, hex.count)
            grouped.append(hex[index..<end].joined())
            index = end
        }
        return "hex:" + grouped.joined(separator: " ")
    }
}

private nonisolated extension Duration {
    var milliseconds: Int64 {
        components.seconds * 1000 + components.attoseconds / 1_000_000_000_000_000
    }
}
