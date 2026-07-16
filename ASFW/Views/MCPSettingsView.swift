import SwiftUI

struct MCPSettingsView: View {
    @ObservedObject var viewModel: ASFWMCPControlViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            Text("MCP Control Plane")
                .font(.title2)
                .fontWeight(.bold)

            VStack(alignment: .leading, spacing: 14) {
                Toggle("Enabled", isOn: $viewModel.isEnabled)
                    .font(.headline)
                    .disabled(viewModel.isChangingState)
                    .onChange(of: viewModel.isEnabled) { _, _ in
                        viewModel.applyEnabledState()
                    }

                HStack(spacing: 12) {
                    Label("HTTP/SSE", systemImage: "network")
                        .frame(width: 120, alignment: .leading)
                    Text(viewModel.status.isRunning ? "Running" : "Stopped")
                        .foregroundStyle(viewModel.status.isRunning ? .green : .secondary)
                }

                HStack(spacing: 12) {
                    Label("Port", systemImage: "number")
                        .frame(width: 120, alignment: .leading)
                    TextField("8765", text: $viewModel.portText)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 120)
                        .disabled(!viewModel.canEditPort)
                }

                HStack(spacing: 12) {
                    Label("Endpoint", systemImage: "link")
                        .frame(width: 120, alignment: .leading)
                    Text(viewModel.endpointText)
                        .font(.system(.body, design: .monospaced))
                        .textSelection(.enabled)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }

                HStack(spacing: 12) {
                    Label("Session", systemImage: "person.crop.circle.badge.checkmark")
                        .frame(width: 120, alignment: .leading)
                    Text(viewModel.sessionText)
                        .foregroundStyle(viewModel.status.activeHTTPConnections > 0 ? .green : .secondary)
                }

                Toggle("Allow guarded FCP experiments", isOn: $viewModel.guardedFCPExperimentsEnabled)
                    .disabled(!viewModel.canEditGuardedFCPExperiments)
                    .onChange(of: viewModel.guardedFCPExperimentsEnabled) { _, enabled in
                        viewModel.setGuardedFCPExperimentsEnabled(enabled)
                    }

                if viewModel.guardedFCPExperimentsEnabled {
                    Label("Developer tools require a current GUID, node, and generation. The in-process MCP test gate must pass before startup.", systemImage: "exclamationmark.shield")
                        .font(.footnote)
                        .foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }

                if let error = viewModel.lastError {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .padding()
            .background(Color(nsColor: .controlBackgroundColor))
            .cornerRadius(8)

            HStack(spacing: 12) {
                Button {
                    Task { await viewModel.start() }
                } label: {
                    Label("Start", systemImage: "play.fill")
                }
                .disabled(viewModel.status.isRunning || viewModel.isChangingState)

                Button {
                    Task { await viewModel.stop() }
                } label: {
                    Label("Stop", systemImage: "stop.fill")
                }
                .disabled(!viewModel.status.isRunning || viewModel.isChangingState)

                Button {
                    Task { await viewModel.runReadOnlyHardwareSmoke() }
                } label: {
                    Label("Run read-only smoke", systemImage: "checkmark.shield")
                }
                .disabled(!viewModel.canRunReadOnlyHardwareSmoke)

                if viewModel.isChangingState {
                    ProgressView()
                        .controlSize(.small)
                }
            }

            if let report = viewModel.hardwareSmokeReport {
                Label(report.conciseSummary, systemImage: report.failures.isEmpty ? "checkmark.circle" : "exclamationmark.triangle")
                    .font(.footnote.monospaced())
                    .foregroundStyle(report.failures.isEmpty ? Color.secondary : Color.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Spacer()
        }
        .padding()
    }
}

#Preview {
    MCPSettingsView(viewModel: ASFWMCPControlViewModel(connector: ASFWDriverConnector()))
        .frame(width: 720, height: 420)
}
