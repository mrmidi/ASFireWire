//
//  AVCReportView.swift
//  ASFW
//
//  Read-only TA 1394 AV/C device report, intended to be copied into a bug
//  report so an unsupported device can be described completely by its owner,
//  and to drive protocol bring-up for a device ASFW does not handle yet.
//
//  Covers every AV/C family — BridgeCo (BeBoB), Oxford, Fireworks, and plain
//  generic AV/C units — with vendor-specific sections appended when detected.
//

import SwiftUI
import UniformTypeIdentifiers

struct AVCReportView: View {
    @ObservedObject var store: AVCReportStore
    @State private var copyFeedbackText = "Copy Report"
    @State private var copyFeedbackIcon = "doc.on.doc"

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text("AV/C Device Report")
                    .font(.title2)
                    .fontWeight(.semibold)

                if store.deviceCount > 0 {
                    Text("\(store.deviceCount) device\(store.deviceCount == 1 ? "" : "s")")
                        .font(.caption)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 3)
                        .background(Color.secondary.opacity(0.15))
                        .clipShape(Capsule())
                }

                Spacer()

                if store.isRefreshing {
                    ProgressView()
                        .controlSize(.small)
                        .frame(width: 16, height: 16)
                }

                Button(action: { store.refresh() }) {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .buttonStyle(.borderedProminent)
                .disabled(store.isRefreshing)

                Button(action: copyReportToPasteboard) {
                    Label(copyFeedbackText, systemImage: copyFeedbackIcon)
                }
                .buttonStyle(.bordered)

                Button(action: saveReportToFile) {
                    Label("Save to .txt...", systemImage: "square.and.arrow.down")
                }
                .buttonStyle(.bordered)
            }
            .padding()
            .background(Color(NSColor.controlBackgroundColor))

            if let error = store.error {
                HStack(spacing: 8) {
                    Image(systemName: "exclamationmark.octagon.fill")
                        .foregroundColor(.red)
                    Text(error)
                        .font(.callout)
                    Spacer()
                    Button(action: { store.error = nil }) {
                        Image(systemName: "xmark")
                            .foregroundColor(.secondary)
                    }
                    .buttonStyle(.plain)
                }
                .padding()
                .background(Color.red.opacity(0.15))
                .border(Color.red.opacity(0.3), width: 1)
            }

            HStack(spacing: 6) {
                Image(systemName: "info.circle")
                    .foregroundColor(.secondary)
                Text("Read-only, STATUS commands only. Probing is suppressed on firmware "
                     + "known to hang on it. Avoid running this while audio is playing.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                Spacer()
            }
            .padding(.horizontal)
            .padding(.vertical, 6)
            .background(Color(NSColor.controlBackgroundColor).opacity(0.5))

            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    Text(store.reportText)
                        .font(.system(.body, design: .monospaced))
                        .padding()
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                }
            }
            .background(Color(NSColor.textBackgroundColor))
        }
    }

    // MARK: - Actions

    private func copyReportToPasteboard() {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.writeObjects([store.reportText as NSString])

        withAnimation {
            copyFeedbackText = "Copied!"
            copyFeedbackIcon = "checkmark"
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
            withAnimation {
                copyFeedbackText = "Copy Report"
                copyFeedbackIcon = "doc.on.doc"
            }
        }
    }

    private func saveReportToFile() {
        let savePanel = NSSavePanel()
        savePanel.allowedContentTypes = [.plainText]
        savePanel.nameFieldStringValue = "ASFW_AVC_Report.txt"
        savePanel.title = "Save AV/C Report"
        savePanel.prompt = "Save"

        savePanel.begin { response in
            if response == .OK, let url = savePanel.url {
                do {
                    try store.reportText.write(to: url, atomically: true, encoding: .utf8)
                } catch {
                    store.error = "Failed to save file: \(error.localizedDescription)"
                }
            }
        }
    }
}
