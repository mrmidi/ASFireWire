import SwiftUI

// On-demand packet inspector backed by the dext's LabDiagUserClient. One
// "Dump" press = one work-queue snapshot of the most recent N packets —
// nothing here touches the streaming path.

@MainActor
final class PacketInspectorModel: ObservableObject {
    @Published var dump: PacketDump?
    @Published var selection: UInt64?
    @Published var status = "No dump yet."
    @Published var requestedCount = 4

    private let client = PacketDumpClient()

    func performDump() {
        do {
            let result = try client.dump(count: UInt32(requestedCount))
            dump = result
            if let selected = selection,
               !result.records.contains(where: { $0.id == selected }) {
                selection = result.records.last?.id
            } else if selection == nil {
                selection = result.records.last?.id
            }
            let data = result.records.filter(\.isData).count
            status = "\(result.records.count) packets (\(data) data) @ cursor \(result.header.nextPacketIndex), io \(result.header.ioRunning ? "running" : "stopped")"
        } catch {
            status = error.localizedDescription
        }
    }
}

struct PacketInspectorView: View {
    @StateObject private var model = PacketInspectorModel()

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            controls
            if let dump = model.dump {
                cadenceStrip(dump)
                contextLine(dump)
                Divider()
                HSplitView {
                    recordList(dump)
                        .frame(minWidth: 230, idealWidth: 250)
                    detailPane(dump)
                        .frame(minWidth: 380, maxWidth: .infinity, maxHeight: .infinity)
                }
            } else {
                Spacer()
            }
        }
    }

    private var controls: some View {
        HStack(spacing: 12) {
            Button("Dump") { model.performDump() }
                .keyboardShortcut("d")
            Picker("Packets:", selection: $model.requestedCount) {
                ForEach([2, 4, 6], id: \.self) { Text("\($0)").tag($0) }
            }
            .pickerStyle(.segmented)
            .frame(width: 140)
            Text(model.status)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .textSelection(.enabled)
        }
    }

    // One chip per packet: the N/D cadence at a glance, click to select.
    private func cadenceStrip(_ dump: PacketDump) -> some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 3) {
                ForEach(dump.records) { record in
                    Button {
                        model.selection = record.id
                    } label: {
                        Text(record.isPublished ? (record.isData ? "D" : "N") : "·")
                            .font(.system(.caption2, design: .monospaced).bold())
                            .frame(width: 18, height: 18)
                            .background(chipColor(record).opacity(
                                model.selection == record.id ? 1.0 : 0.45))
                            .foregroundStyle(.white)
                            .clipShape(RoundedRectangle(cornerRadius: 4))
                    }
                    .buttonStyle(.plain)
                    .help("packet #\(record.packetIndex)")
                }
            }
        }
    }

    private func chipColor(_ record: PacketDumpRecord) -> Color {
        if !record.isPublished { return .red }
        return record.isData ? .blue : .gray
    }

    private func contextLine(_ dump: PacketDump) -> some View {
        let h = dump.header
        return Text("zts \(h.ztsSampleTime) (period \(h.periodIndex)×\(h.ztsPeriodFrames)) · exposed \(h.exposedFrames) · writeEnds \(h.writeEndCount) · payload \(h.framesWritten)/\(h.framesVisited) (miss \(h.framesWithoutPacket)+\(h.framesOutsidePacket), raced \(h.framesRacedReuse))")
            .font(.system(.caption2, design: .monospaced))
            .foregroundStyle(.secondary)
            .textSelection(.enabled)
    }

    private func recordList(_ dump: PacketDump) -> some View {
        List(dump.records, selection: $model.selection) { record in
            HStack {
                Text(record.isPublished ? (record.isData ? "D" : "N") : "·")
                    .font(.system(.caption, design: .monospaced).bold())
                    .foregroundStyle(chipColor(record))
                VStack(alignment: .leading, spacing: 1) {
                    Text("#\(record.packetIndex)")
                        .font(.system(.caption, design: .monospaced))
                    if let cip = record.cip, record.isPublished {
                        Text("dbc \(String(format: "%02X", cip.dbc)) · syt \(cip.sytIsNoInfo ? "—" : String(format: "%04X", cip.syt))")
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(.secondary)
                    }
                }
                Spacer()
                if record.isData {
                    Text("\(record.firstAudioFrame)…\(record.firstAudioFrame + UInt64(record.framesInPacket) - 1)")
                        .font(.system(.caption2, design: .monospaced))
                        .foregroundStyle(.tertiary)
                }
            }
            .tag(record.id)
        }
        .listStyle(.inset)
    }

    @ViewBuilder
    private func detailPane(_ dump: PacketDump) -> some View {
        if let record = dump.records.first(where: { $0.id == model.selection }) {
            ScrollView {
                VStack(alignment: .leading, spacing: 10) {
                    recordSummary(record)
                    if let cip = record.cip, record.isPublished {
                        cipCard(cip)
                    }
                    if record.isData {
                        slotGrid(record)
                    }
                }
                .padding(10)
            }
        } else {
            Text("Select a packet")
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    private func recordSummary(_ record: PacketDumpRecord) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("Packet #\(record.packetIndex)")
                .font(.headline)
            Text("\(record.isData ? "data" : (record.isPublished ? "no-data" : "evicted")) · \(record.byteCount) B · slot \(record.slotStateName) gen \(record.generation)\(record.hasLiveBytes ? " · LIVE (PCM may still land)" : "")")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)
        }
    }

    private func cipCard(_ cip: CIPHeader) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(String(format: "CIP  %08X %08X", cip.q0, cip.q1))
                .font(.system(.caption, design: .monospaced).bold())
            Text(String(format: "sid %u · dbs %u · fn %u · qpc %u · sph %u · dbc %02X · fmt %02X · fdf %02X · syt %@",
                        cip.sid, cip.dbs, cip.fn, cip.qpc, cip.sph, cip.dbc,
                        cip.fmt, cip.fdf,
                        cip.sytIsNoInfo ? "NO_INFO" : String(format: "%04X", cip.syt)))
                .font(.system(.caption2, design: .monospaced))
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
        }
        .padding(6)
        .background(.quaternary.opacity(0.4))
        .clipShape(RoundedRectangle(cornerRadius: 6))
    }

    private func slotGrid(_ record: PacketDumpRecord) -> some View {
        let columns = [GridItem(.fixed(44), alignment: .leading)]
            + (0..<Int(record.dbs)).map { _ in GridItem(.flexible(minimum: 64), alignment: .leading) }
        return LazyVGrid(columns: columns, spacing: 3) {
            Text("frame")
                .font(.system(.caption2, design: .monospaced).bold())
            ForEach(0..<Int(record.dbs), id: \.self) { slot in
                Text("ch\(slot)")
                    .font(.system(.caption2, design: .monospaced).bold())
            }
            ForEach(0..<Int(record.framesInPacket), id: \.self) { frame in
                Text("\(record.firstAudioFrame + UInt64(frame))")
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(.secondary)
                ForEach(0..<Int(record.dbs), id: \.self) { slot in
                    slotCell(record, frame: frame, slot: slot)
                }
            }
        }
    }

    @ViewBuilder
    private func slotCell(_ record: PacketDumpRecord, frame: Int, slot: Int) -> some View {
        if let word = record.slotWord(frame: frame, slot: slot) {
            VStack(alignment: .leading, spacing: 0) {
                Text(String(format: "%08X", word))
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(word == 0 ? .tertiary : .primary)
                Text(String(format: "%+.5f", PacketDumpRecord.pcmValue(word)))
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(word == 0 ? .quaternary : .secondary)
            }
        } else {
            Text("—")
                .font(.system(.caption2, design: .monospaced))
                .foregroundStyle(.quaternary)
        }
    }
}
