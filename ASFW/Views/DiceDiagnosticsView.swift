import SwiftUI
import UniformTypeIdentifiers

struct DiceDiagnosticsView: View {
    @ObservedObject var store: DiceDiagnosticsStore
    @State private var exporting = false
    @State private var readFIFOEvents = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Text("DICE Diagnostics").font(.title2.bold())
                Spacer()
                if store.isCapturing { ProgressView().controlSize(.small) }
                Button("Refresh", systemImage: "arrow.clockwise") { store.capture() }
                    .disabled(store.isCapturing)
                Button("Copy", systemImage: "doc.on.doc") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(report, forType: .string)
                }.disabled(store.snapshot?.guid != store.selectedGUID || store.snapshot == nil)
                Button("Save", systemImage: "square.and.arrow.down") { exporting = true }
                    .disabled(store.snapshot?.guid != store.selectedGUID || store.snapshot == nil)
            }
            Picker("Device", selection: $store.selectedGUID) {
                Text("Select a device").tag(Optional<UInt64>.none)
                ForEach(store.devices) { device in
                    Text("\(device.vendorName) \(device.modelName)").tag(Optional(device.guid))
                }
            }.disabled(store.isCapturing)

            #if DEBUG
            HStack {
                Button("Capture receiver hardware", systemImage: "cpu") { store.capture(hardware: true, fifoEvents: readFIFOEvents) }
                    .disabled(store.isCapturing || !store.canCaptureHardware)
                Text("Pro 24 DSP only. Uses firmware mailbox commands to read fixed chip registers.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Toggle("Read FIFO underflow/overflow flags (clears them on the device)", isOn: $readFIFOEvents)
                .disabled(store.isCapturing || !store.canCaptureHardware)
                .font(.caption)
            #endif
            Text("Manual captures only. Standard reads preserve slip history. FIFO ranges show allocated capacity, not live fill. Values are read sequentially; hardware event flags are latched history, including possible startup events.")
                .font(.callout).foregroundStyle(.secondary)
            if let error = store.error {
                Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
            }
            if let snapshot = store.snapshot, snapshot.guid == store.selectedGUID {
                HStack {
                    Text("Captured \(snapshot.capturedAt.formatted(date: .abbreviated, time: .standard)) · generation \(snapshot.generation)")
                        .font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button("Pin baseline", systemImage: "pin") { store.pinBaseline() }
                    if store.baseline != nil {
                        Button("Clear baseline") { store.clearBaseline() }
                    }
                }
                if let baseline = store.baseline, baseline.guid == snapshot.guid {
                    Text("Baseline: \(baseline.capturedAt.formatted(date: .abbreviated, time: .standard)) · generation \(baseline.generation). Highlighted rows changed; latched events may span both captures.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                ScrollView {
                    Grid(alignment: .leading, horizontalSpacing: 20, verticalSpacing: 12) {
                        GridRow {
                            Text("Register / field").bold()
                            Text("Current").bold()
                            Text("Raw").bold()
                            Text("Baseline").bold()
                        }
                        ForEach(snapshot.values) { field in
                            let previous = snapshot.baselineValue(for: field, baseline: store.baseline)
                            GridRow(alignment: .top) {
                                VStack(alignment: .leading) {
                                    Text(field.id).font(.system(.body, design: .monospaced))
                                    Text(field.group).font(.caption).foregroundStyle(.secondary)
                                }
                                VStack(alignment: .leading) {
                                    Text(field.value)
                                    if let previous, previous.raw != field.raw {
                                        Label("Changed", systemImage: "arrow.triangle.2.circlepath")
                                            .font(.caption).foregroundStyle(.orange)
                                    }
                                }
                                Text(field.rawText).font(.system(.body, design: .monospaced))
                                Text(previous?.value ?? "—").foregroundStyle(.secondary)
                            }
                            .padding(6)
                            .background(previous.map { $0.raw != field.raw } == true ? Color.orange.opacity(0.15) : .clear)
                        }
                    }
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            } else {
                ContentUnavailableView("No capture", systemImage: "waveform.path.ecg",
                    description: Text("Select a DICE device and click Refresh. Pin a working capture to compare with a silent start."))
            }
            Spacer(minLength: 0)
        }
        .padding()
        .onAppear { store.refreshDevices() }
        .onDisappear { store.cancel() }
        .fileExporter(isPresented: $exporting, document: DiceDiagnosticsDocument(text: report),
                      contentType: .plainText, defaultFilename: "DICE-Diagnostics") { _ in }
    }

    private var report: String {
        let current = store.snapshot?.report ?? "No capture."
        guard let baseline = store.baseline else { return current }
        return current + "\n\nBaseline:\n" + baseline.report
    }
}
