import SwiftUI

/// The device's streams as the host sees them: what goes to it, what comes from it, and how its outputs are routed.
struct AvcSignalTab: View {
    let unit: AvcUnitDashboard

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(alignment: .top, spacing: 16) {
                if let playback = unit.playback {
                    AvcChannelCard(title: "Playback", caption: "Host → device", systemImage: "arrow.down.to.line",
                                   tint: AvcPalette.playback, stream: playback,
                                   rows: AvcUnitDashboard.channelRows(playback), midiLabels: [])
                }
                if let capture = unit.capture {
                    AvcChannelCard(title: "Capture", caption: "Device → host", systemImage: "arrow.up.to.line",
                                   tint: AvcPalette.capture, stream: capture,
                                   rows: AvcUnitDashboard.channelRows(capture), midiLabels: unit.midiLabels)
                }
            }
            if unit.playback == nil && unit.capture == nil {
                AvcNoticeCard(title: "No stream information", message: "The driver has not built a stream description for this unit yet. Re-scan the bus, or open Diagnostics to see what discovery found.", systemImage: "waveform.slash")
            }
            if !unit.routing.isEmpty { AvcRoutingCard(items: unit.routing) }
        }
    }
}

/// One direction's channels, in the order they sit in the stream.
struct AvcChannelCard: View {
    let title: String
    let caption: String
    let systemImage: String
    let tint: Color
    let stream: AvcUnitDocument.Stream
    let rows: [AvcUnitDashboard.ChannelRow]
    let midiLabels: [String]

    var body: some View {
        AvcCard(padding: 0) {
            VStack(alignment: .leading, spacing: 0) {
                AvcSectionHeader(title: title, caption: caption, systemImage: systemImage, tint: tint) {
                    AvcChip(text: "\(stream.channels) ch", tint: tint, filled: true, monospaced: true)
                }
                .padding(16)
                Divider()
                if rows.isEmpty {
                    Text("The device reports \(stream.channels) channels without names.")
                        .font(.callout).foregroundStyle(.secondary).padding(16)
                } else {
                    ForEach(rows) { row in
                        ChannelLine(row: row, tint: tint)
                        // A thin rule after every stereo pair keeps long lists readable.
                        if row.number % 2 == 0 && row.number < rows.count { Divider().padding(.leading, 56) }
                    }
                }
                if stream.midi > 0 {
                    Divider()
                    midiLines
                }
            }
        }
    }

    @ViewBuilder private var midiLines: some View {
        if midiLabels.isEmpty {
            HStack(spacing: 12) {
                Image(systemName: "pianokeys").foregroundStyle(AvcPalette.midi).frame(width: 28)
                Text("\(stream.midi) MIDI stream\(stream.midi == 1 ? "" : "s")").font(.callout)
                Spacer()
                AvcChip(text: "MIDI", tint: AvcPalette.midi)
            }
            .padding(.horizontal, 16).padding(.vertical, 10)
        } else {
            ForEach(Array(midiLabels.enumerated()), id: \.offset) { index, label in
                HStack(spacing: 12) {
                    Image(systemName: "pianokeys").foregroundStyle(AvcPalette.midi).frame(width: 28)
                    Text(label.isEmpty ? "MIDI \(index + 1)" : label).font(.callout)
                    Spacer()
                    AvcChip(text: "MIDI", tint: AvcPalette.midi)
                }
                .padding(.horizontal, 16).padding(.vertical, 10)
            }
        }
    }
}

private struct ChannelLine: View {
    let row: AvcUnitDashboard.ChannelRow
    let tint: Color

    var body: some View {
        HStack(spacing: 12) {
            Text("\(row.number)")
                .font(.system(.caption, design: .rounded)).fontWeight(.semibold).monospacedDigit()
                .frame(width: 28, height: 24)
                .foregroundStyle(tint)
                .background(tint.opacity(0.14), in: RoundedRectangle(cornerRadius: 7, style: .continuous))
            Text(row.name).font(.callout).lineLimit(1)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 7)
    }
}

/// The device's selectors: for each, its inputs and the one that is selected.
struct AvcRoutingCard: View {
    let items: [AvcUnitDashboard.RoutingItem]

    var body: some View {
        AvcCard(padding: 0) {
            VStack(alignment: .leading, spacing: 0) {
                AvcSectionHeader(title: "Routing", caption: "Selectors and the input each one is set to", systemImage: "point.3.connected.trianglepath.dotted", tint: AvcPalette.control) {
                    AvcChip(text: "\(items.count) selectors", tint: AvcPalette.control)
                }
                .padding(16)
                Divider()
                ForEach(items) { item in
                    RoutingLine(item: item)
                    if item.id != items.last?.id { Divider().padding(.leading, 16) }
                }
            }
        }
    }
}

private struct RoutingLine: View {
    let item: AvcUnitDashboard.RoutingItem

    private var distinctNames: [String] { Array(Set(item.inputs)).sorted() }
    private var allSame: Bool { item.inputs.count > 1 && distinctNames.count == 1 }

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(item.title).font(.callout).fontWeight(.medium)
                Text(selectedText).font(.caption).foregroundStyle(.secondary)
            }
            Spacer(minLength: 12)
            HStack(spacing: 5) {
                ForEach(Array(item.inputs.enumerated()), id: \.offset) { index, name in
                    let selected = index == item.current
                    Text(allSame || item.inputs.count > 4 ? "\(index + 1)" : name)
                        .font(.caption).fontWeight(selected ? .semibold : .regular).monospacedDigit()
                        .padding(.horizontal, 9).padding(.vertical, 4)
                        .foregroundStyle(selected ? Color.white : Color.secondary)
                        .background(selected ? AvcPalette.control : Color.primary.opacity(0.07), in: Capsule())
                        .help(name)
                }
            }
        }
        .padding(.horizontal, 16).padding(.vertical, 11)
    }

    private var selectedText: String {
        guard let current = item.current, item.inputs.indices.contains(current) else { return "Not reported" }
        return "Input \(current + 1) · \(item.inputs[current])"
    }
}

/// A message card for a screen with nothing to show yet.
struct AvcNoticeCard: View {
    let title: String
    let message: String
    var systemImage: String = "info.circle"

    var body: some View {
        AvcCard {
            HStack(alignment: .top, spacing: 14) {
                Image(systemName: systemImage).font(.title2).foregroundStyle(.secondary)
                VStack(alignment: .leading, spacing: 4) {
                    Text(title).font(.headline)
                    Text(message).font(.callout).foregroundStyle(.secondary)
                }
            }
        }
    }
}
