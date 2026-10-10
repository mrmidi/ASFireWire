import SwiftUI

struct BusInspectorNodeRow: View {
    let item: BusInspectorNode

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(item.title)
                .font(.headline)
                .lineLimit(2)
            HStack(spacing: 6) {
                Text("Node \(item.node.nodeId)")
                Text("· S\(item.node.maxSpeedMbps)")
                if item.isLocal { Text("· Local") }
                if item.node.isRoot { Text("· Root") }
            }
            .font(.caption)
            .foregroundStyle(.secondary)
            if let device = item.device {
                HStack {
                    StateLabel(state: device.stateString)
                    Text("\(device.units.count) unit\(device.units.count == 1 ? "" : "s")")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            } else {
                Text(item.isLocal ? "Host node" : (item.node.linkActive ? "Discovery pending" : "Link inactive"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 5)
    }
}
