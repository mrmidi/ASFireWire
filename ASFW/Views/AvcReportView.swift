import SwiftUI
import UniformTypeIdentifiers

struct AvcReportView: View {
    @ObservedObject var store: AvcReportStore
    @State private var refreshing = false
    @State private var openingDump = false
    @State private var copied = false

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text("AV/C Device Report").font(.title2).fontWeight(.semibold)
                Text("\(store.deviceCount) device\(store.deviceCount == 1 ? "" : "s")")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                if store.isRefreshing { ProgressView().controlSize(.small) }
                Button("Refresh", systemImage: "arrow.clockwise") { refreshing = true }
                    .buttonStyle(.borderedProminent).disabled(store.isRefreshing || refreshing)
                Button(copied ? "Copied!" : "Copy Report", systemImage: copied ? "checkmark" : "doc.on.doc") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.writeObjects([store.reportText as NSString])
                    copied = true
                }.disabled(store.snapshot == nil)
                Menu("Save", systemImage: "square.and.arrow.down") {
                    Button("Save Text Report…") { save(json: false) }
                    Button("Save JSON Dump…") { save(json: true) }
                    Button("Save Binary Dump Folder…") { saveBinaryFolder() }
                }.disabled(store.snapshot == nil)
                Button("Open Dump…", systemImage: "folder") { openingDump = true }
                    .disabled(store.isRefreshing || refreshing)
            }
            .padding().background(Color(nsColor: .controlBackgroundColor))
            if let error = store.error {
                HStack {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
                    Spacer()
                    Button("Dismiss Error", systemImage: "xmark") { store.error = nil }.labelStyle(.iconOnly)
                }.padding().background(Color.red.opacity(0.1))
            }
            HStack {
                Label(store.isImported ? "Saved dump — no driver connection required." : "Refresh runs normal AV/C discovery manually. Reports include parsed data and original captured bytes.", systemImage: "info.circle")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
            }.padding(.horizontal).padding(.vertical, 8)
            ScrollView([.horizontal, .vertical]) {
                Text(store.reportText).font(.system(.body, design: .monospaced))
                    .textSelection(.enabled).padding().frame(maxWidth: .infinity, alignment: .leading)
            }.background(Color(nsColor: .textBackgroundColor))
        }
        .task(id: refreshing) {
            guard refreshing else { return }
            await store.refresh()
            refreshing = false
        }
        .task(id: copied) {
            guard copied else { return }
            do { try await Task.sleep(for: .seconds(1.5)); copied = false } catch {}
        }
        .fileImporter(isPresented: $openingDump, allowedContentTypes: [.json]) { result in
            switch result {
            case .success(let url): store.openDump(url)
            case .failure(let error): store.error = "Could not open AV/C dump: \(error.localizedDescription)"
            }
        }
    }

    private func save(json: Bool) {
        guard let snapshot = store.snapshot else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = json ? [.json] : [.plainText]
        panel.nameFieldStringValue = json ? "ASFW_AVC_Dump.json" : "ASFW_AVC_Report.txt"
        panel.title = json ? "Save AV/C Dump" : "Save AV/C Report"
        // Capture the visible snapshot before opening the panel.
        let text = store.reportText
        panel.begin { response in
            guard response == .OK, let url = panel.url else { return }
            do {
                let data = try json ? snapshot.jsonData() : Data(text.utf8)
                try data.write(to: url, options: .atomic)
            } catch { store.error = "Could not save AV/C report: \(error.localizedDescription)" }
        }
    }
    private func saveBinaryFolder() {
        guard let snapshot = store.snapshot else { return }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.title = "Choose a destination for the AV/C dump folder"
        panel.begin { response in
            guard response == .OK, let destination = panel.url else { return }
            do {
                let directory = destination.appendingPathComponent("ASFW_AVC_Dump_" + UUID().uuidString)
                try AvcReportExporter.export(snapshot, to: directory)
            } catch { store.error = "Could not save binary dump: \(error.localizedDescription)" }
        }
    }

}
