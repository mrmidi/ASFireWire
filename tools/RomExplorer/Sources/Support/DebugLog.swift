import Foundation
#if canImport(os)
import os
#endif

/// Lightweight logging helper used by the ROM explorer models.
public enum DebugLog {
    // Resolve a reasonable subsystem name once so unified logging can group entries.
    private static let subsystem: String = {
        if let bundleId = Bundle.main.bundleIdentifier, !bundleId.isEmpty { return bundleId }
        if let exec = Bundle.main.executableURL?.lastPathComponent, !exec.isEmpty { return exec }
        return ProcessInfo.processInfo.processName
    }()

    private static let category = "General"

    #if canImport(os)
    private static let logger = Logger(subsystem: subsystem, category: category)

    public static func info(_ message: @autoclosure @escaping () -> String, file: StaticString = #fileID, function: StaticString = #function, line: UInt = #line) {
        logger.info("\(message())")
    }

    public static func warn(_ message: @autoclosure @escaping () -> String, file: StaticString = #fileID, function: StaticString = #function, line: UInt = #line) {
        logger.warning("\(message())")
    }

    public static func error(_ message: @autoclosure @escaping () -> String, file: StaticString = #fileID, function: StaticString = #function, line: UInt = #line) {
        logger.error("\(message())")
    }

    public static func log(level: String, _ message: @autoclosure @escaping () -> String, file: StaticString = #fileID, function: StaticString = #function, line: UInt = #line) {
        switch level.lowercased() {
        case "info", "i":
            info(message(), file: file, function: function, line: line)
        case "warn", "warning", "w":
            warn(message(), file: file, function: function, line: line)
        case "error", "err", "e":
            error(message(), file: file, function: function, line: line)
        case "debug", "d":
            logger.debug("\(message())")
        default:
            logger.log("\(message())")
        }
    }
    #else
    private static func fallback(_ level: String, message: @escaping () -> String, file: StaticString, function: StaticString, line: UInt) {
        print("[\(level)] \(message()) @ \(file):\(line) \(function)")
    }

    public static func info(_ message: @autoclosure @escaping () -> String, file: StaticString = #fileID, function: StaticString = #function, line: UInt = #line) {
        fallback("INFO", message: message, file: file, function: function, line: line)
    }

    public static func warn(_ message: @autoclosure @escaping () -> String, file: StaticString = #fileID, function: StaticString = #function, line: UInt = #line) {
        fallback("WARN", message: message, file: file, function: function, line: line)
    }

    public static func error(_ message: @autoclosure @escaping () -> String, file: StaticString = #fileID, function: StaticString = #function, line: UInt = #line) {
        fallback("ERROR", message: message, file: file, function: function, line: line)
    }

    public static func log(level: String, _ message: @autoclosure @escaping () -> String, file: StaticString = #fileID, function: StaticString = #function, line: UInt = #line) {
        fallback(level.uppercased(), message: message, file: file, function: function, line: line)
    }
    #endif
}
