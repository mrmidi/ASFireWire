import SwiftUI

/// The mute and volume state the driver read from the device's feature blocks, one card per block.
struct AvcControlsTab: View {
    let unit: AvcUnitDashboard

    var body: some View {
        let blocks = unit.controlBlocks
        VStack(alignment: .leading, spacing: 14) {
            if blocks.isEmpty {
                AvcNoticeCard(title: "No control values", message: "Discovery read no feature controls from this device. They appear here when it has mute or volume controls and discovery completed.", systemImage: "slider.horizontal.3")
            } else {
                HStack(spacing: 8) {
                    Image(systemName: "eye").foregroundStyle(.secondary)
                    Text("Values as of the last discovery. This screen does not change the device.").font(.callout).foregroundStyle(.secondary)
                }
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 320), spacing: 16, alignment: .top)], alignment: .leading, spacing: 16) {
                    ForEach(blocks) { block in AvcControlBlockCard(block: block) }
                }
            }
        }
    }
}

struct AvcControlBlockCard: View {
    let block: AvcUnitDashboard.ControlBlock

    var body: some View {
        AvcCard(padding: 0) {
            VStack(alignment: .leading, spacing: 0) {
                AvcSectionHeader(title: block.title, caption: "\(block.channels.count) channel\(block.channels.count == 1 ? "" : "s")", systemImage: "slider.vertical.3", tint: AvcPalette.control)
                    .padding(14)
                Divider()
                ForEach(block.channels) { channel in
                    ControlChannelRow(channel: channel)
                    if channel.id != block.channels.last?.id { Divider().padding(.leading, 14) }
                }
            }
        }
    }
}

private struct ControlChannelRow: View {
    let channel: AvcUnitDashboard.ControlChannel

    var body: some View {
        HStack(spacing: 12) {
            Text(channel.isMaster ? "Master" : "Ch \(channel.channel)")
                .font(.caption).fontWeight(.medium)
                .frame(width: 54, alignment: .leading)
                .foregroundStyle(channel.isMaster ? Color.primary : Color.secondary)
            muteIcon
            LevelBar(db: channel.volumeDb, silent: channel.volumeIsSilence, muted: channel.muted == true)
            Text(volumeLabel)
                .font(.system(.caption, design: .monospaced))
                .frame(width: 62, alignment: .trailing)
                .foregroundStyle(channel.muted == true ? Color.secondary : Color.primary)
        }
        .padding(.horizontal, 14).padding(.vertical, 8)
    }

    @ViewBuilder private var muteIcon: some View {
        if let muted = channel.muted {
            Image(systemName: muted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                .font(.caption)
                .foregroundStyle(muted ? Color.red : Color.secondary)
                .frame(width: 22)
                .help(muted ? "Muted" : "Not muted")
        } else {
            Color.clear.frame(width: 22, height: 1)
        }
    }

    private var volumeLabel: String {
        if channel.volumeIsSilence { return "−∞ dB" }
        guard let db = channel.volumeDb else { return channel.volumeText == nil ? "—" : "?" }
        return String(format: "%+.1f dB", db)
    }
}

/// A level bar from -60 dB to +12 dB. The range is a presentation choice; the value is the device's.
private struct LevelBar: View {
    let db: Double?
    let silent: Bool
    let muted: Bool

    private var fraction: Double {
        if silent { return 0 }
        guard let db else { return 0 }
        return min(max((db + 60) / 72, 0), 1)
    }

    var body: some View {
        GeometryReader { proxy in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.primary.opacity(0.09))
                Capsule()
                    .fill(AnyShapeStyle(muted ? AnyShapeStyle(Color.gray.opacity(0.5)) : AnyShapeStyle(AvcPalette.control.gradient)))
                    .frame(width: max(fraction * proxy.size.width, db == nil && !silent ? 0 : 4))
                // The 0 dB mark.
                Rectangle().fill(Color.primary.opacity(0.25)).frame(width: 1.5, height: 10)
                    .offset(x: proxy.size.width * (60.0 / 72.0))
            }
        }
        .frame(height: 6)
    }
}
