import SwiftUI

struct DriverLogRow: View {
    let record: ASFWLogRingRecord
    let categoryName: String

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: levelIcon)
                .foregroundStyle(levelColor)
                .frame(width: 18)
                .accessibilityLabel(record.levelName.capitalized)

            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 8) {
                    Text("#\(record.sequence)")
                    Text(timestampText)
                    Text("[\(categoryName)]")
                        .foregroundStyle(.blue)
                    Text(record.levelName.uppercased())
                        .foregroundStyle(levelColor)
                }
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)

                Text(record.message)
                    .font(.system(.body, design: .monospaced))
            }
        }
        .padding(.vertical, 3)
        .textSelection(.enabled)
    }

    private var timestampText: String {
        let seconds = Double(record.timestampNs) / 1_000_000_000
        return String(format: "%.6f s", seconds)
    }

    private var levelIcon: String {
        switch record.level {
        case 0: return "xmark.octagon.fill"
        case 1: return "exclamationmark.triangle.fill"
        case 2: return "exclamationmark.circle.fill"
        case 3: return "info.circle.fill"
        default: return "ladybug.fill"
        }
    }

    private var levelColor: Color {
        switch record.level {
        case 0: return .red
        case 1: return .orange
        case 2: return .yellow
        case 3: return .blue
        default: return .secondary
        }
    }
}
