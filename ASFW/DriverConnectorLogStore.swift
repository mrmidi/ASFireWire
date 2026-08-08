import Foundation

struct DriverConnectorLogMessage: Identifiable, Equatable {
    let id = UUID()
    let timestamp: Date
    let level: Level
    let message: String

    enum Level {
        case info, warning, error, success

        var emoji: String {
            switch self {
            case .info: return "ℹ️"
            case .warning: return "⚠️"
            case .error: return "❌"
            case .success: return "✅"
            }
        }

        var color: String {
            switch self {
            case .info: return "blue"
            case .warning: return "orange"
            case .error: return "red"
            case .success: return "green"
            }
        }
    }
}

final class DriverConnectorLogStore {
    private var buffer: BoundedDeque<DriverConnectorLogMessage>

    init(maxEntries: Int = 100) {
        buffer = BoundedDeque(capacity: maxEntries)
    }

    func append(_ message: DriverConnectorLogMessage) -> [DriverConnectorLogMessage] {
        buffer.append(message)
        return buffer.elements
    }

    var messages: [DriverConnectorLogMessage] {
        buffer.elements
    }

    func clear() {
        buffer.removeAll()
    }
}
