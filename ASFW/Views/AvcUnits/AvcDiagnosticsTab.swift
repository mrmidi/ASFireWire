import AppKit
import SwiftUI

/// How discovery went: what was asked, what failed, how fast the device answered.
struct AvcDiagnosticsTab: View {
    let unit: AvcUnitDashboard
    var developerTools: AnyView?

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            discovery
            failedProbes
            descriptors
            routeChecks
            if let developerTools { developerTools }
        }
    }

    private var discovery: some View {
        AvcCard {
            VStack(alignment: .leading, spacing: 12) {
                AvcSectionHeader(title: "Discovery", systemImage: "stethoscope", tint: .teal)
                AvcFact(label: "Result", value: unit.health.label)
                if case .partial(let kind) = unit.health { AvcFact(label: "Stopped by", value: AvcUnitDashboard.humanized(kind)) }
                if let session = unit.document?.session, session > 0 { AvcFact(label: "Session", value: "\(session)", monospaced: true) }
                if let probes = unit.document?.snapshot?.probeCount { AvcFact(label: "Probes sent", value: "\(probes)", monospaced: true) }
                if let latency = unit.latency {
                    AvcFact(label: "FCP exchanges", value: "\(latency.count) (\(latency.failed) not answered)", monospaced: true)
                    AvcFact(label: "Response time", value: String(format: "avg %.1f ms · max %.1f ms", latency.averageMs, latency.maxMs), monospaced: true)
                }
                if let dropped = unit.document?.exchanges?.dropped, dropped > 0 {
                    AvcFact(label: "Exchange log", value: "\(dropped) later exchanges not kept")
                }
            }
        }
    }

    @ViewBuilder private var failedProbes: some View {
        let groups = unit.failedProbeGroups
        if !groups.isEmpty {
            AvcCard(padding: 0) {
                VStack(alignment: .leading, spacing: 0) {
                    AvcSectionHeader(title: "Probes that failed", caption: "\(groups.reduce(0) { $0 + $1.count }) in total, grouped", systemImage: "exclamationmark.triangle", tint: .orange)
                        .padding(16)
                    Divider()
                    ForEach(groups) { group in
                        HStack(spacing: 10) {
                            Text(group.opcode).font(.callout)
                            Text(group.address).font(.caption).foregroundStyle(.secondary)
                            Spacer()
                            AvcChip(text: group.error, tint: .orange)
                            Text("×\(group.count)").font(.system(.caption, design: .monospaced)).foregroundStyle(.secondary)
                        }
                        .padding(.horizontal, 16).padding(.vertical, 8)
                        if group.id != groups.last?.id { Divider().padding(.leading, 16) }
                    }
                }
            }
        }
    }

    @ViewBuilder private var descriptors: some View {
        let reads = unit.document?.snapshot?.descriptors ?? []
        if !reads.isEmpty {
            AvcCard(padding: 0) {
                VStack(alignment: .leading, spacing: 0) {
                    AvcSectionHeader(title: "Descriptors read", systemImage: "doc.text", tint: .blue).padding(16)
                    Divider()
                    ForEach(reads) { read in
                        HStack(spacing: 10) {
                            Text(read.subunit.title).font(.callout).frame(width: 90, alignment: .leading)
                            Text(read.specifierText ?? read.specifier.map { "Specifier 0x\($0)" } ?? "descriptor").font(.caption).foregroundStyle(.secondary).lineLimit(1)
                            Spacer()
                            if let error = read.primaryError { AvcChip(text: error.displayText, tint: .orange) }
                            if let parse = read.parseError { AvcChip(text: "parse: \(AvcUnitDashboard.humanized(parse.kind)) @\(parse.offset)", tint: .red) }
                            Text("\(read.bytes) B").font(.system(.caption, design: .monospaced)).foregroundStyle(.secondary)
                            if let data = read.data, !data.isEmpty {
                                Button {
                                    NSPasteboard.general.clearContents()
                                    NSPasteboard.general.setString(data, forType: .string)
                                } label: { Image(systemName: "doc.on.doc").font(.caption) }
                                    .buttonStyle(.plain).foregroundStyle(.secondary).help("Copy the descriptor bytes as hex")
                            }
                        }
                        .padding(.horizontal, 16).padding(.vertical, 8)
                        if read.id != reads.last?.id { Divider().padding(.leading, 16) }
                    }
                }
            }
        }
    }

    @ViewBuilder private var routeChecks: some View {
        let deviations = unit.routeDeviations
        if !deviations.isEmpty {
            AvcCard(padding: 0) {
                VStack(alignment: .leading, spacing: 0) {
                    AvcSectionHeader(title: "Where the device departs from the spec", caption: "Route status checked against TA 2002010", systemImage: "checkmark.shield", tint: .yellow)
                        .padding(16)
                    Divider()
                    ForEach(deviations) { item in
                        VStack(alignment: .leading, spacing: 3) {
                            Text(item.text).font(.callout)
                            Text("\(item.plugs.count) plug\(item.plugs.count == 1 ? "" : "s"): " + item.plugs.prefix(4).joined(separator: ", ") + (item.plugs.count > 4 ? " …" : ""))
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        .padding(.horizontal, 16).padding(.vertical, 8)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        if item.id != deviations.last?.id { Divider().padding(.leading, 16) }
                    }
                }
            }
        }
    }
}
