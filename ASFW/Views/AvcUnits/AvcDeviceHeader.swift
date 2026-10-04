import AppKit
import SwiftUI

/// The device at a glance: who it is, whether discovery is complete, and the numbers that matter.
struct AvcDeviceHeader: View {
    let unit: AvcUnitDashboard

    var body: some View {
        AvcCard(padding: 20) {
            VStack(alignment: .leading, spacing: 18) {
                identity
                tiles
            }
        }
    }

    private var identity: some View {
        HStack(alignment: .center, spacing: 16) {
            Image(systemName: "hifispeaker.2.fill")
                .font(.system(size: 26, weight: .medium))
                .foregroundStyle(.white)
                .frame(width: 60, height: 60)
                .background(LinearGradient(colors: [.indigo, .purple], startPoint: .topLeading, endPoint: .bottomTrailing),
                            in: RoundedRectangle(cornerRadius: 16, style: .continuous))
            VStack(alignment: .leading, spacing: 4) {
                Text(unit.title).font(.title2).fontWeight(.semibold)
                Text(unit.subtitle).font(.callout).foregroundStyle(.secondary)
                HStack(spacing: 6) {
                    Text(unit.guidHex).font(.system(.caption, design: .monospaced)).foregroundStyle(.secondary).textSelection(.enabled)
                    Button {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(unit.guidHex, forType: .string)
                    } label: { Image(systemName: "doc.on.doc").font(.caption) }
                        .buttonStyle(.plain)
                        .foregroundStyle(.secondary)
                        .help("Copy GUID")
                }
            }
            Spacer(minLength: 12)
            VStack(alignment: .trailing, spacing: 6) {
                AvcStatusPill(text: unit.health.label, tint: healthTint)
                if let state = unit.deviceState {
                    Text(state.capitalized).font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }

    private var healthTint: Color {
        switch unit.health {
        case .complete: .green
        case .partial: .orange
        case .cancelled: .yellow
        case .noData: .gray
        }
    }

    private var tiles: some View {
        LazyVGrid(columns: Array(repeating: GridItem(.flexible(minimum: 150), spacing: 12, alignment: .top), count: max(1, min(tileCount, 6))), spacing: 12) {
            if let rate = unit.sampleRate {
                AvcStatTile(title: "Sample rate", value: AvcUnitDashboard.rateLabel(rate).replacingOccurrences(of: " kHz", with: ""),
                            unit: "kHz", systemImage: "waveform", tint: .indigo) {
                    Text("Last confirmed rate").font(.caption).foregroundStyle(.secondary)
                    AvcFlow(spacing: 4) {
                        ForEach(unit.supportedRates, id: \.self) { hz in
                            AvcChip(text: AvcUnitDashboard.rateLabel(hz).replacingOccurrences(of: " kHz", with: ""),
                                    tint: .indigo, filled: hz == rate, monospaced: true)
                        }
                    }
                }
            }
            if let playback = unit.playback {
                streamTile("Playback", systemImage: "arrow.down.to.line", stream: playback, tint: AvcPalette.playback, caption: "Host → device")
            }
            if let capture = unit.capture {
                streamTile("Capture", systemImage: "arrow.up.to.line", stream: capture, tint: AvcPalette.capture, caption: "Device → host")
            }
            if !unit.midiLabels.isEmpty {
                AvcStatTile(title: "MIDI", value: "\(unit.midiLabels.count)", unit: unit.midiLabels.count == 1 ? "port" : "ports",
                            systemImage: "pianokeys", tint: AvcPalette.midi) {
                    Text(unit.midiLabels.joined(separator: ", ")).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                }
            }
            if let sync = unit.musicIdentifier?.audioSyncName ?? syncFromStatus, sync != "none" {
                AvcStatTile(title: "Audio sync", value: sync.replacingOccurrences(of: "+", with: " + "),
                            systemImage: "arrow.triangle.2.circlepath", tint: AvcPalette.sync) {
                    Text("Sync sources the subunit takes").font(.caption).foregroundStyle(.secondary)
                }
            }
            if !unit.subunits.isEmpty {
                AvcStatTile(title: "Subunits", value: "\(unit.subunits.count)", systemImage: "square.stack.3d.up", tint: AvcPalette.control) {
                    Text(unit.subunits.map { "\($0.ref.title) #\($0.id)" }.joined(separator: " · "))
                        .font(.caption).foregroundStyle(.secondary).lineLimit(2)
                }
            }
        }
    }

    /// How many tiles the strip will show, so the grid has one column for each.
    private var tileCount: Int {
        var count = 0
        if unit.sampleRate != nil { count += 1 }
        if unit.playback != nil { count += 1 }
        if unit.capture != nil { count += 1 }
        if !unit.midiLabels.isEmpty { count += 1 }
        if let sync = unit.musicIdentifier?.audioSyncName ?? syncFromStatus, sync != "none" { count += 1 }
        if !unit.subunits.isEmpty { count += 1 }
        return count
    }

    private var syncFromStatus: String? {
        unit.musicStatus?.sourcePlugs.compactMap { $0.activity?.audioSyncName }.first
    }

    private func streamTile(_ title: String, systemImage: String, stream: AvcUnitDocument.Stream, tint: Color, caption: String) -> some View {
        AvcStatTile(title: title, value: "\(stream.channels)", unit: stream.channels == 1 ? "channel" : "channels",
                    systemImage: systemImage, tint: tint) {
            HStack(spacing: 6) {
                Text(caption).font(.caption).foregroundStyle(.secondary)
                if stream.midi > 0 { AvcChip(text: "+ \(stream.midi) MIDI", tint: AvcPalette.midi) }
            }
        }
    }
}
