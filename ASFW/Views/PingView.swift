//
//  PingView.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI
import Foundation

struct PingView: View {
    @ObservedObject var viewModel: DebugViewModel
    @State private var lastResponse: String?
    @State private var lastError: String?
    @State private var lastPingDate: Date?
    @State private var isSending = false

    var body: some View {
        VStack(alignment: .leading, spacing: 24) {
            GroupBox {
                VStack(alignment: .leading, spacing: 12) {
                    statusRow

                    if let response = lastResponse {
                        HStack {
                            Label("Response", systemImage: "arrow.turn.down.right")
                                .font(.headline)
                            Spacer()
                            Text(response)
                                .font(.system(.body, design: .monospaced))
                        }

                        if let date = lastPingDate {
                            Text("Received: \(date.formatted(date: .omitted, time: .standard))")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }

                    if let error = lastError {
                        HStack {
                            Image(systemName: "xmark.octagon.fill")
                                .foregroundColor(.red)
                            Text(error)
                                .font(.caption)
                                .foregroundColor(.red)
                        }
                    }
                }
                .padding()
            } label: {
                Label("Driver Ping", systemImage: "waveform.path.ecg")
                    .font(.headline)
            }

            Button {
                triggerPing()
            } label: {
                if isSending {
                    ProgressView()
                        .controlSize(.regular)
                        .padding(.trailing, 8)
                    Text("Pinging…")
                } else {
                    Label("Send Ping", systemImage: "paperplane")
                }
            }
            .buttonStyle(.borderedProminent)
            .disabled(isSending || !viewModel.isConnected)

            Spacer()
        }
        .padding()
        .navigationTitle("Ping")
    }

    private var statusRow: some View {
        HStack {
            if viewModel.isConnected {
                Label("Connected to driver", systemImage: "checkmark.circle.fill")
                    .foregroundColor(.green)
            } else {
                Label("Driver not connected", systemImage: "exclamationmark.triangle.fill")
                    .foregroundColor(.orange)
            }
            Spacer()
        }
    }

    private func triggerPing() {
        if !viewModel.isConnected {
            lastError = "Driver connection unavailable"
            return
        }

        isSending = true
        lastError = nil

        DispatchQueue.global(qos: .userInitiated).async {
            let response = viewModel.connector.ping()

            DispatchQueue.main.async {
                self.isSending = false
                if let response = response {
                    self.lastResponse = response
                    self.lastPingDate = Date()
                    self.lastError = nil
                } else {
                    self.lastResponse = nil
                    self.lastPingDate = nil
                    self.lastError = viewModel.connector.lastError ?? "Ping failed"
                }
            }
        }
    }
}

#if false
#Preview {
    PingView(viewModel: DebugViewModel())
}
#endif
