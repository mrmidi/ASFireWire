//
//  DiagnosticsView.swift
//  ASFW
//
//  Created by ASFireWire Project on 29.05.2026.
//

import SwiftUI
import UniformTypeIdentifiers

struct DiagnosticsView: View {
    @ObservedObject var store: DiagnosticsStore
    @State private var showingClearConfirmation = false
    @State private var copyFeedbackText = "Copy Report"
    @State private var copyFeedbackIcon = "doc.on.doc"
    
    var body: some View {
        VStack(spacing: 0) {
            // Header Bar
            HStack(spacing: 12) {
                Text("1394 Diagnostics Cockpit")
                    .font(.title2)
                    .fontWeight(.semibold)
                
                Spacer()
                
                if store.isRefreshing || store.isClearingTrace {
                    ProgressView()
                        .controlSize(.small)
                        .frame(width: 16, height: 16)
                }
                
                Button(action: { store.refresh() }) {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .buttonStyle(.borderedProminent)
                .disabled(store.isRefreshing || store.isClearingTrace)
                
                Button(action: { showingClearConfirmation = true }) {
                    Label("Clear Trace", systemImage: "trash")
                }
                .buttonStyle(.bordered)
                .disabled(store.isRefreshing || store.isClearingTrace)
                .tint(.red)
                
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
            
            // Error banner if any
            if let error = store.error {
                HStack(spacing: 8) {
                    Image(systemName: "exclamationmark.octagon.fill")
                        .foregroundColor(.red)
                    Text(error)
                        .font(.callout)
                        .foregroundColor(.primary)
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
            
            // Monospaced Report Viewer
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
        .confirmationDialog(
            "Are you sure you want to clear the async transaction trace ring buffer on the driver?",
            isPresented: $showingClearConfirmation
        ) {
            Button("Clear Trace", role: .destructive) {
                store.clearTrace()
            }
            Button("Cancel", role: .cancel) {}
        }
        .onAppear {
            store.refresh()
        }
    }
    
    // MARK: - Actions
    
    private func copyReportToPasteboard() {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.writeObjects([store.reportText as NSString])
        
        // Provide visual feedback
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
        savePanel.nameFieldStringValue = "ASFW_1394_Diagnostics_Report.txt"
        savePanel.title = "Save Diagnostics Report"
        savePanel.prompt = "Save"
        
        savePanel.begin { response in
            if response == .OK, let url = savePanel.url {
                do {
                    try store.reportText.write(to: url, atomically: true, encoding: .utf8)
                    print("[DiagnosticsView] ✅ Report saved successfully to \(url.path)")
                } catch {
                    print("[DiagnosticsView] ❌ Failed to save report: \(error)")
                    store.error = "Failed to save file: \(error.localizedDescription)"
                }
            }
        }
    }
}
