import SwiftUI

/// The unit's plugs and each subunit's plugs, with the stream formats they carry.
struct AvcPlugsTab: View {
    let unit: AvcUnitDashboard

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            unitPlugs
            ForEach(unit.subunits, id: \.ref) { subunit in
                SubunitPlugCard(unit: unit, subunit: subunit)
            }
        }
    }

    private var unitPlugs: some View {
        let counts = unit.document?.snapshot?.unit
        let iso = (counts?.isoInputs ?? Int(unit.wireUnit?.isoInputPlugs ?? 0), counts?.isoOutputs ?? Int(unit.wireUnit?.isoOutputPlugs ?? 0))
        let ext = (counts?.externalInputs ?? Int(unit.wireUnit?.extInputPlugs ?? 0), counts?.externalOutputs ?? Int(unit.wireUnit?.extOutputPlugs ?? 0))
        let plugs = unit.plugs(of: nil)
        return AvcCard(padding: 0) {
            VStack(alignment: .leading, spacing: 0) {
                AvcSectionHeader(title: "Unit plugs", caption: "The device's connections to the bus and to the outside", systemImage: "cable.connector", tint: .indigo)
                    .padding(16)
                Divider()
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 150), spacing: 12)], spacing: 12) {
                    PlugCount(title: "Isochronous in", value: iso.0, systemImage: "arrow.down.circle.fill", tint: AvcPalette.playback)
                    PlugCount(title: "Isochronous out", value: iso.1, systemImage: "arrow.up.circle.fill", tint: AvcPalette.capture)
                    PlugCount(title: "External in", value: ext.0, systemImage: "arrow.down.circle", tint: .purple)
                    PlugCount(title: "External out", value: ext.1, systemImage: "arrow.up.circle", tint: .orange)
                }
                .padding(16)
                if !plugs.isEmpty {
                    Divider()
                    ForEach(plugs, id: \.key) { plug in
                        PlugFormatRow(plug: plug)
                        if plug.key != plugs.last?.key { Divider().padding(.leading, 16) }
                    }
                }
            }
        }
    }
}

private struct PlugCount: View {
    let title: String
    let value: Int
    let systemImage: String
    let tint: Color

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: systemImage).foregroundStyle(tint).font(.title3)
            VStack(alignment: .leading, spacing: 0) {
                Text("\(value)").font(.system(.title3, design: .rounded)).fontWeight(.semibold).monospacedDigit()
                Text(title).font(.caption).foregroundStyle(.secondary)
            }
            Spacer(minLength: 0)
        }
        .padding(10)
        .background(tint.opacity(0.08), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
    }
}

private struct SubunitPlugCard: View {
    let unit: AvcUnitDashboard
    let subunit: AvcUnitDocument.SubunitInfo

    var body: some View {
        let contents = unit.contents(for: subunit.ref)
        let plugs = unit.plugs(of: subunit.ref)
        AvcCard(padding: 0) {
            VStack(alignment: .leading, spacing: 0) {
                AvcSectionHeader(title: "\(subunit.typeName ?? "Subunit") #\(subunit.id)", caption: "\(subunit.destinationPlugs) destination · \(subunit.sourcePlugs) source plugs",
                                 systemImage: icon, tint: tint) {
                    AvcFlow(spacing: 4) {
                        AvcChip(text: "\(subunit.destinationPlugs) in", tint: AvcPalette.playback)
                        AvcChip(text: "\(subunit.sourcePlugs) out", tint: AvcPalette.capture)
                    }
                }
                .padding(16)
                if let music = contents?.music, !music.plugs.isEmpty {
                    ForEach([true, false], id: \.self) { destination in
                        let group = music.plugs.filter { $0.destination == destination }
                        if !group.isEmpty {
                            Divider()
                            Text(destination ? "Destination plugs · host → device" : "Source plugs · device → host")
                                .font(.caption).fontWeight(.semibold).foregroundStyle(.secondary)
                                .padding(.horizontal, 16).padding(.top, 10).padding(.bottom, 2)
                            ForEach(group, id: \.key) { plug in
                                MusicPlugRow(plug: plug)
                                if plug.key != group.last?.key { Divider().padding(.leading, 16) }
                            }
                        }
                    }
                    if !plugs.isEmpty { Divider() }
                }
                let detailed = plugs.filter { $0.hasDetail }
                let bare = plugs.count - detailed.count
                ForEach(detailed, id: \.key) { plug in
                    PlugFormatRow(plug: plug)
                    if plug.key != detailed.last?.key || bare > 0 { Divider().padding(.leading, 16) }
                }
                if bare > 0 {
                    Text("\(bare) more plug\(bare == 1 ? "" : "s") with no format reported").font(.caption).foregroundStyle(.secondary)
                        .padding(.horizontal, 16).padding(.vertical, 10)
                }
                if contents?.music?.plugs.isEmpty != false && plugs.isEmpty {
                    Divider()
                    Text("No plug detail was read for this subunit.").font(.callout).foregroundStyle(.secondary).padding(16)
                }
            }
        }
    }

    private var icon: String { subunit.typeName == "Music" ? "music.note" : "speaker.wave.2.fill" }
    private var tint: Color { subunit.typeName == "Music" ? .orange : .purple }
}

private struct MusicPlugRow: View {
    let plug: AvcUnitDocument.MusicPlug

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: plug.destination ? "arrow.down.circle.fill" : "arrow.up.circle.fill")
                .foregroundStyle(plug.destination ? AvcPalette.playback : AvcPalette.capture)
                .frame(width: 22)
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 8) {
                    Text(plug.name.isEmpty ? "Plug \(plug.id)" : plug.name).font(.callout).fontWeight(.medium)
                    Text("Plug \(plug.id)").font(.caption).foregroundStyle(.secondary)
                }
                AvcFlow(spacing: 4) {
                    if let usage = plug.usageName { AvcChip(text: usage, tint: .secondary) }
                    ForEach(Array(plug.clusters.enumerated()), id: \.offset) { _, cluster in
                        AvcChip(text: "\(cluster.channels)× \(cluster.portTypeName ?? "port")", tint: cluster.portTypeName == "MIDI" ? AvcPalette.midi : .blue)
                    }
                }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16).padding(.vertical, 10)
    }
}

private struct PlugFormatRow: View {
    let plug: AvcUnitDocument.Plug

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Image(systemName: plug.isInput ? "arrow.down.circle" : "arrow.up.circle").foregroundStyle(.secondary)
                Text("\(plug.isInput ? "Input" : "Output") plug \(plug.id)").font(.callout).fontWeight(.medium)
                if let status = plug.route?.outputStatusName { AvcChip(text: status, tint: status == "effective" ? .green : .secondary) }
                Spacer(minLength: 0)
                if let rate = plug.currentDecoded?.rate { AvcChip(text: rate, tint: .indigo, filled: true) }
            }
            if let entries = plug.currentDecoded?.entries, !entries.isEmpty {
                AvcFlow(spacing: 4) {
                    ForEach(Array(entries.enumerated()), id: \.offset) { _, entry in
                        AvcChip(text: "\(entry.count)× \(entry.format ?? "format 0x\(String(entry.code, radix: 16))")", tint: entry.format == "MIDI conformant" ? AvcPalette.midi : .blue)
                    }
                }
            } else if let format = plug.currentDecoded {
                Text("Format not decoded").font(.caption).foregroundStyle(.secondary)
                Text(format.text).font(.system(.caption2, design: .monospaced)).foregroundStyle(.tertiary).lineLimit(2)
            }
            let rates = (plug.formationsDecoded ?? []).compactMap(\.rate)
            if rates.count > 1 {
                HStack(spacing: 6) {
                    Text("Supports").font(.caption).foregroundStyle(.secondary)
                    AvcFlow(spacing: 4) { ForEach(rates, id: \.self) { AvcChip(text: $0.replacingOccurrences(of: " kHz", with: ""), tint: .indigo, monospaced: true) } }
                }
            }
            if let route = plug.route, let source = route.sourceName, let destination = route.destinationName {
                Text("\(source) → \(destination)").font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        .padding(.horizontal, 16).padding(.vertical, 10)
    }
}
