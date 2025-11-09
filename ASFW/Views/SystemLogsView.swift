//
//  SystemLogsView.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI

struct SystemLogsView: View {
    @ObservedObject var viewModel: DriverViewModel
    
    var body: some View {
        VStack(spacing: 0) {
            // Log list
            ScrollViewReader { proxy in
                List {
                    ForEach(viewModel.logMessages) { log in
                        LogEntryRow(entry: log)
                            .id(log.id)
                    }
                }
                .listStyle(.plain)
                .onChange(of: viewModel.logMessages.count) { _, _ in
                    if let lastLog = viewModel.logMessages.last {
                        withAnimation {
                            proxy.scrollTo(lastLog.id, anchor: .bottom)
                        }
                    }
                }
            }
        }
        .navigationTitle("System Logs")
        .toolbar {
            Button {
                viewModel.logMessages.removeAll()
            } label: {
                Label("Clear", systemImage: "trash")
            }
        }
    }
}

struct LogEntryRow: View {
    let entry: DriverViewModel.LogEntry
    
    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: entry.level.systemImage)
                .foregroundStyle(entry.level.color)
                .imageScale(.medium)
                .frame(width: 20)
            
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text(entry.formattedTime)
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                    
                    Text("[\(entry.source.rawValue)]")
                        .font(.caption.monospaced())
                        .foregroundStyle(.blue)
                }
                
                Text(entry.message)
                    .font(.body)
                    .textSelection(.enabled)
            }
        }
        .padding(.vertical, 4)
    }
}
