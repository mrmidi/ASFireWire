import SwiftUI

/// What the subunits say they can do: the Music subunit's identifier and the Audio subunit's function blocks.
struct AvcCapabilitiesTab: View {
    let unit: AvcUnitDashboard

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            if let identifier = unit.musicIdentifier {
                MusicIdentifierCard(identifier: identifier, status: unit.musicStatus)
            }
            if !unit.audioBlocks.isEmpty {
                AudioBlocksCard(blocks: unit.audioBlocks)
            }
            if unit.musicIdentifier == nil && unit.audioBlocks.isEmpty {
                AvcNoticeCard(title: "No capability data", message: "The subunits' descriptors were not read. Re-scan the bus, or open Diagnostics to see why.", systemImage: "list.bullet.rectangle")
            }
        }
    }
}

private struct MusicIdentifierCard: View {
    let identifier: AvcUnitDocument.MusicIdentifier
    let status: AvcUnitDocument.MusicStatus?

    var body: some View {
        AvcCard(padding: 0) {
            VStack(alignment: .leading, spacing: 0) {
                AvcSectionHeader(title: "Music subunit", caption: "Static capabilities (identifier descriptor)", systemImage: "music.note", tint: .orange) {
                    AvcChip(text: "v\(identifier.versionText)", tint: .orange, filled: true, monospaced: true)
                }
                .padding(16)
                Divider()
                VStack(alignment: .leading, spacing: 12) {
                    AvcFlow(spacing: 6) {
                        ForEach(identifier.capabilityNames, id: \.self) { AvcChip(text: $0, tint: .orange) }
                    }
                    if let general = identifier.general {
                        AvcFact(label: "Transmit", value: general.transmitName ?? "—")
                        AvcFact(label: "Receive", value: general.receiveName ?? "—")
                    }
                    if let midi = identifier.midi {
                        AvcFact(label: "MIDI", value: "\(midi.versionText) · up to \(midi.maxInputPorts) in / \(midi.maxOutputPorts) out")
                    }
                    if let sync = identifier.audioSyncName { AvcFact(label: "Audio sync", value: sync) }
                    if let smpte = identifier.smpteTimeCodeName { AvcFact(label: "SMPTE time code", value: smpte) }
                    if let count = identifier.sampleCountName { AvcFact(label: "Sample count", value: count) }
                    if let declared = status?.declaredSourcePlugs {
                        AvcFact(label: "Source plugs described", value: "\(declared)")
                    }
                }
                .padding(16)
                if let formats = identifier.audio, !formats.isEmpty {
                    Divider()
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Audio formats").font(.subheadline).fontWeight(.semibold)
                        Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 6) {
                            GridRow {
                                Text("Rate").foregroundStyle(.secondary)
                                Text("Max in").foregroundStyle(.secondary)
                                Text("Max out").foregroundStyle(.secondary)
                                Text("Format").foregroundStyle(.secondary)
                            }
                            .font(.caption)
                            ForEach(Array(formats.enumerated()), id: \.offset) { _, format in
                                GridRow {
                                    Text(format.fdfName ?? "—").monospacedDigit()
                                    Text("\(format.maxInputChannels)").monospacedDigit()
                                    Text("\(format.maxOutputChannels)").monospacedDigit()
                                    Text(format.am824LabelName ?? "—")
                                }
                                .font(.callout)
                            }
                        }
                    }
                    .padding(16)
                }
            }
        }
    }
}

private struct AudioBlocksCard: View {
    let blocks: [AvcUnitDocument.FunctionBlock]

    private var census: [(name: String, count: Int)] {
        var order: [String] = []
        var counts: [String: Int] = [:]
        for block in blocks {
            let name = block.typeName ?? "Block"
            if counts[name] == nil { order.append(name) }
            counts[name, default: 0] += 1
        }
        return order.map { ($0, counts[$0] ?? 0) }
    }

    private var mixers: [AvcUnitDocument.FunctionBlock] { blocks.filter { $0.subTypeName == "MIXER" } }
    private var features: [AvcUnitDocument.FunctionBlock] { blocks.filter { $0.typeName == "Feature" } }

    var body: some View {
        AvcCard(padding: 0) {
            VStack(alignment: .leading, spacing: 0) {
                AvcSectionHeader(title: "Audio subunit", caption: "Function blocks (identifier descriptor)", systemImage: "speaker.wave.2.fill", tint: .purple) {
                    AvcChip(text: "\(blocks.count) blocks", tint: .purple, filled: true)
                }
                .padding(16)
                Divider()
                VStack(alignment: .leading, spacing: 12) {
                    AvcFlow(spacing: 6) {
                        ForEach(census, id: \.name) { AvcChip(text: "\($0.count) \($0.name.lowercased())", tint: .purple) }
                    }
                    if !features.isEmpty {
                        let names = Set(features.flatMap { $0.controls?.masterNames ?? [] }).sorted()
                        AvcFact(label: "Feature controls", value: names.isEmpty ? "none advertised" : names.joined(separator: ", "))
                    }
                    if !mixers.isEmpty {
                        let programmable = mixers.reduce(0) { $0 + ($1.programmableMixerControls ?? 0) }
                        AvcFact(label: "Mixers", value: programmable == 0 ? "\(mixers.count), fixed (no programmable controls)" : "\(mixers.count), \(programmable) programmable controls")
                    }
                }
                .padding(16)
                Divider()
                ForEach(blocks, id: \.key) { block in
                    BlockLine(block: block)
                    if block.key != blocks.last?.key { Divider().padding(.leading, 16) }
                }
            }
        }
    }
}

private struct BlockLine: View {
    let block: AvcUnitDocument.FunctionBlock

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            AvcChip(text: block.typeName ?? "Block", tint: tint)
                .frame(width: 84, alignment: .leading)
            VStack(alignment: .leading, spacing: 2) {
                Text(block.title).font(.callout)
                if let sources = sourceText { Text(sources).font(.caption).foregroundStyle(.secondary).lineLimit(1) }
            }
            Spacer(minLength: 8)
            if let channels = block.clusterChannels, channels > 0 { Text("\(channels) ch").font(.caption).foregroundStyle(.secondary) }
        }
        .padding(.horizontal, 16).padding(.vertical, 7)
    }

    private var tint: Color {
        switch block.typeName {
        case "Feature": .blue
        case "Selector": .purple
        case "Processing": .orange
        default: .gray
        }
    }

    private var sourceText: String? {
        let names = block.inputs.map { $0.name ?? ($0.kind == "notConnected" ? "not connected" : $0.kind == "subunitDestinationPlug" ? "destination plug \($0.id)" : "#\($0.id)") }
        guard !names.isEmpty else { return nil }
        let distinct = Array(NSOrderedSet(array: names)) as? [String] ?? names
        return "from " + distinct.prefix(3).joined(separator: ", ") + (distinct.count > 3 ? " +\(distinct.count - 3)" : "")
    }
}
