//
//  TxLatencyView.swift
//  ASFW
//
//  Created for ASFireWire Project.
//  Diagnostic view for TX latency metering (E0 -> E2).
//

import SwiftUI
import Combine
import AppKit
import UniformTypeIdentifiers

@MainActor
final class TxLatencyViewModel: ObservableObject {
    @Published var availableEndpoints: [AudioEndpointID] = []
    @Published var selectedEndpointID: AudioEndpointID?
    @Published var durationSeconds: Double = 5.0
    @Published var strataSize: Int = 8
    @Published var seed: String = "0"
    @Published var assumedDriftPpm: Double = 100.0

    @Published private(set) var isCapturing: Bool = false
    @Published private(set) var currentHeader: TxLatencySessionHeader?
    @Published private(set) var report: TxLatencySessionReport?
    @Published private(set) var errorMessage: String?
    @Published private(set) var lastFetchTime: Date?

    private let connector: ASFWDriverConnector
    private var pollTask: Task<Void, Never>?

    init(connector: ASFWDriverConnector) {
        self.connector = connector
        refreshEndpoints()
    }

    func refreshEndpoints() {
        if let telemetry = connector.getAudioTelemetry() {
            let ids = telemetry.endpoints.map { $0.endpointId }
            availableEndpoints = ids
            if selectedEndpointID == nil || !ids.contains(where: { $0 == selectedEndpointID }) {
                selectedEndpointID = ids.first
            }
        }
    }

    func startSession() {
        guard let endpoint = selectedEndpointID else {
            errorMessage = "No audio endpoint selected."
            return
        }

        errorMessage = nil
        report = nil
        currentHeader = nil
        isCapturing = true

        let seedVal = UInt32(seed) ?? 0
        let (kr, sessionId) = connector.startTxLatencySession(
            endpointID: endpoint,
            durationSeconds: UInt32(durationSeconds),
            strataSize: UInt32(strataSize),
            seed: seedVal,
            assumedDriftPpm: UInt32(assumedDriftPpm)
        )

        guard kr == KERN_SUCCESS else {
            errorMessage = "Failed to start session: error code \(kr)"
            isCapturing = false
            return
        }

        startPolling(endpoint: endpoint, sessionId: sessionId)
    }

    func stopSession() {
        guard let endpoint = selectedEndpointID else { return }
        let sid = currentHeader?.sessionId ?? 0
        _ = connector.stopTxLatencySession(endpointID: endpoint, sessionId: sid)
    }

    private func startPolling(endpoint: AudioEndpointID, sessionId: UInt32) {
        pollTask?.cancel()
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(400))
                guard let self = self else { return }

                if let page = self.connector.getTxLatencyResultsPage(endpointID: endpoint, pageIndex: 0, samplesPerPage: 1, sessionId: sessionId) {
                    self.currentHeader = page.header
                    self.lastFetchTime = Date()

                    if page.header.sessionState == .frozen {
                        // Capture complete, fetch full report
                        if let fullReport = self.connector.fetchTxLatencySession(endpointID: endpoint, sessionId: sessionId) {
                            self.report = fullReport
                            self.currentHeader = fullReport.header
                        }
                        self.isCapturing = false
                        break
                    }
                }
            }
        }
    }

    func exportCSV() {
        guard let report = report else { return }
        let csv = report.toCSV()
        let panel = NSSavePanel()
        panel.title = "Export TX Latency Report"
        panel.nameFieldStringValue = "tx_latency_endpoint_\(report.header.endpointId.rawValue).csv"
        panel.allowedContentTypes = [UTType.commaSeparatedText]

        if panel.runModal() == .OK, let url = panel.url {
            do {
                try csv.write(to: url, atomically: true, encoding: .utf8)
            } catch {
                errorMessage = "Failed to save CSV: \(error.localizedDescription)"
            }
        }
    }

    func copyCSVToClipboard() {
        guard let report = report else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(report.toCSV(), forType: .string)
    }
}

struct TxLatencyView: View {
    @StateObject private var viewModel: TxLatencyViewModel

    init(connector: ASFWDriverConnector) {
        _viewModel = StateObject(wrappedValue: TxLatencyViewModel(connector: connector))
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                headerView

                if let err = viewModel.errorMessage {
                    errorBanner(err)
                }

                configurationView

                if let header = viewModel.currentHeader {
                    sessionStatusView(header)
                }

                if let report = viewModel.report {
                    summaryStatisticsView(report)
                    samplesTableView(report.samples)
                } else if viewModel.isCapturing {
                    capturingProgressView
                }
            }
            .padding(20)
        }
        .navigationTitle("TX Latency Metering (E0 -> E2)")
        .onAppear {
            viewModel.refreshEndpoints()
        }
    }

    // MARK: - Subviews

    private var headerView: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Transmit Latency Metering")
                .font(.title2.bold())
            Text("Measures the conservative elapsed time between complete PCM payload availability (E0) and hardware transmission (E2) bounds with bracket, granularity, rounding, and drift accounting.")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
    }

    private func errorBanner(_ msg: String) -> some View {
        HStack {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundColor(.orange)
            Text(msg)
                .font(.subheadline)
            Spacer()
        }
        .padding(10)
        .background(Color.orange.opacity(0.15))
        .cornerRadius(8)
    }

    private var configurationView: some View {
        GroupBox("Session Configuration") {
            VStack(alignment: .leading, spacing: 14) {
                HStack(spacing: 20) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Audio Endpoint")
                            .font(.caption.bold())
                        Picker("", selection: $viewModel.selectedEndpointID) {
                            if viewModel.availableEndpoints.isEmpty {
                                Text("No streaming endpoints").tag(Optional<AudioEndpointID>.none)
                            } else {
                                ForEach(viewModel.availableEndpoints, id: \.self) { ep in
                                    Text("Endpoint \(ep.rawValue)").tag(Optional(ep))
                                }
                            }
                        }
                        .labelsHidden()
                        .frame(width: 180)
                    }

                    VStack(alignment: .leading, spacing: 4) {
                        Text("Duration: \(Int(viewModel.durationSeconds))s")
                            .font(.caption.bold())
                        Slider(value: $viewModel.durationSeconds, in: 1...60, step: 1)
                            .frame(width: 140)
                    }

                    VStack(alignment: .leading, spacing: 4) {
                        Text("Strata Period")
                            .font(.caption.bold())
                        Picker("", selection: $viewModel.strataSize) {
                            Text("1 (all)").tag(1)
                            Text("2").tag(2)
                            Text("4").tag(4)
                            Text("8 (default)").tag(8)
                            Text("16").tag(16)
                            Text("32").tag(32)
                        }
                        .labelsHidden()
                        .frame(width: 110)
                    }

                    VStack(alignment: .leading, spacing: 4) {
                        Text("Drift Ceiling: \(Int(viewModel.assumedDriftPpm)) ppm")
                            .font(.caption.bold())
                        Slider(value: $viewModel.assumedDriftPpm, in: 10...500, step: 10)
                            .frame(width: 140)
                    }

                    Spacer()

                    if viewModel.isCapturing {
                        Button(action: viewModel.stopSession) {
                            Label("Stop Session", systemImage: "stop.circle.fill")
                        }
                        .buttonStyle(.borderedProminent)
                        .tint(.red)
                    } else {
                        Button(action: viewModel.startSession) {
                            Label("Arm & Capture", systemImage: "play.circle.fill")
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(viewModel.selectedEndpointID == nil)
                    }
                }
            }
            .padding(10)
        }
    }

    private func sessionStatusView(_ header: TxLatencySessionHeader) -> some View {
        GroupBox("Session Status") {
            HStack(spacing: 24) {
                statusBadge(header.sessionState)

                VStack(alignment: .leading, spacing: 2) {
                    Text("Session ID")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text("#\(header.sessionId)")
                        .font(.headline.monospacedDigit())
                }

                VStack(alignment: .leading, spacing: 2) {
                    Text("Packets Seen")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text("\(header.dataPacketsSeen)")
                        .font(.headline.monospacedDigit())
                }

                VStack(alignment: .leading, spacing: 2) {
                    Text("Samples Captured")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text("\(header.samplesCaptured)")
                        .font(.headline.monospacedDigit())
                }

                VStack(alignment: .leading, spacing: 2) {
                    Text("Termination")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text(header.terminationReason.description)
                        .font(.headline)
                }

                Spacer()

                if viewModel.report != nil {
                    Button(action: viewModel.exportCSV) {
                        Label("Export CSV", systemImage: "square.and.arrow.up")
                    }
                    Button(action: viewModel.copyCSVToClipboard) {
                        Label("Copy CSV", systemImage: "doc.on.doc")
                    }
                }
            }
            .padding(8)
        }
    }

    private var capturingProgressView: some View {
        HStack(spacing: 12) {
            ProgressView()
                .controlSize(.small)
            Text("Capturing packet samples in driver ring...")
                .font(.subheadline)
                .foregroundStyle(.secondary)
            Spacer()
        }
        .padding()
        .background(Color.blue.opacity(0.08))
        .cornerRadius(8)
    }

    private func summaryStatisticsView(_ report: TxLatencySessionReport) -> some View {
        VStack(spacing: 16) {
            // Stats Row
            HStack(spacing: 16) {
                metricCard(
                    title: "Min Wait (E0→E2)",
                    value: String(format: "%.1f µs", report.matchedStats.minWaitMicros),
                    sub: "Earliest completion"
                )
                metricCard(
                    title: "Median Wait",
                    value: String(format: "%.1f µs", report.matchedStats.medianWaitMicros),
                    sub: "P50 center wait"
                )
                metricCard(
                    title: "P95 Wait",
                    value: String(format: "%.1f µs", report.matchedStats.p95WaitMicros),
                    sub: "95th percentile"
                )
                metricCard(
                    title: "Max Wait",
                    value: String(format: "%.1f µs", report.matchedStats.maxWaitMicros),
                    sub: "Worst-case observed"
                )
                metricCard(
                    title: "Mean Uncertainty",
                    value: String(format: "±%.1f µs", report.matchedStats.meanUncertaintyMicros),
                    sub: "4 uncertainty terms"
                )
            }

            // Population Breakdown Row
            GroupBox("Population Breakdown") {
                HStack(spacing: 20) {
                    populationItem(title: "Matched", count: report.header.resolvedCount, color: .green)
                    populationItem(title: "Substituted", count: report.header.substitutionCount, color: .orange)
                    populationItem(title: "Unresolved", count: report.header.unresolvedCount, color: .yellow)
                    populationItem(title: "Failed", count: report.header.transmitFailedCount, color: .red)
                    populationItem(title: "Invalid", count: report.header.invalidCount, color: .purple)
                    populationItem(title: "Missed Stamps", count: report.header.stampsMissedCount, color: .gray)
                }
                .padding(8)
            }
        }
    }

    private func samplesTableView(_ samples: [TxLatencySample]) -> some View {
        GroupBox("Sampled Transmissions (\(samples.count) total)") {
            Table(samples) {
                TableColumn("Packet") { s in
                    Text("#\(s.packetIndex)")
                        .font(.body.monospacedDigit())
                }
                .width(min: 80, ideal: 90)

                TableColumn("Outcome") { s in
                    outcomeTag(s.outcome)
                }
                .width(min: 100, ideal: 120)

                TableColumn("Audio Frames") { s in
                    Text("[\(s.pcmCommittedStartFrame)..\(s.pcmCommittedEndFrame)) (\(s.frameSpan))")
                        .font(.caption.monospacedDigit())
                }
                .width(min: 150, ideal: 180)

                TableColumn("Wait Bounds (µs)") { s in
                    Text(String(format: "[%.1f .. %.1f]", s.waitMinMicros, s.waitMaxMicros))
                        .font(.body.monospacedDigit())
                }
                .width(min: 120, ideal: 140)

                TableColumn("Center ± U") { s in
                    Text(String(format: "%.1f ± %.1f µs", s.waitCenterMicros, s.uncertaintyMicros))
                        .font(.body.monospacedDigit())
                }
                .width(min: 120, ideal: 140)

                TableColumn("Phase / Image") { s in
                    Text("P\(s.cyclePhaseMod8) · Img \(s.selectedImage)")
                        .font(.caption)
                }
                .width(min: 90, ideal: 100)

                TableColumn("Provenance / Reason") { s in
                    if s.outcome == .unresolved {
                        Text(s.unresolvedReason.description)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    } else if s.hasTransportDecision {
                        // Gated on the join result, not on a field being
                        // non-zero: SealResult 0 is a real outcome, so a zero
                        // seal cannot stand in for "no decision recorded".
                        Text(s.decisionEvidenceSummary)
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(s.bindResult == 1 ? .green : .orange)
                    } else {
                        Text(s.pcmIdentityProven ? "Proven" : "Unproven")
                            .font(.caption)
                            .foregroundStyle(s.pcmIdentityProven ? .green : .secondary)
                    }
                }
                .width(min: 160, ideal: 200)
            }
            .frame(minHeight: 300)
        }
    }

    // MARK: - Helper Views

    private func statusBadge(_ state: TxLatencySessionState) -> some View {
        Text(state.description.uppercased())
            .font(.caption.bold())
            .padding(.horizontal, 10)
            .padding(.vertical, 4)
            .background(badgeColor(state).opacity(0.2))
            .foregroundColor(badgeColor(state))
            .cornerRadius(6)
    }

    private func badgeColor(_ state: TxLatencySessionState) -> Color {
        switch state {
        case .idle: return .gray
        case .arming: return .blue
        case .capturing: return .green
        case .stopRequested: return .orange
        case .frozen: return .indigo
        }
    }

    private func outcomeTag(_ outcome: TxLatencyOutcome) -> some View {
        Text(outcome.description)
            .font(.caption.bold())
            .padding(.horizontal, 8)
            .padding(.vertical, 2)
            .background(outcomeColor(outcome).opacity(0.15))
            .foregroundColor(outcomeColor(outcome))
            .cornerRadius(4)
    }

    private func outcomeColor(_ outcome: TxLatencyOutcome) -> Color {
        switch outcome {
        case .matched: return .green
        case .substituted: return .orange
        case .unresolved: return .yellow
        case .transmitFailed: return .red
        case .invalid: return .purple
        case .unknown: return .gray
        }
    }

    private func metricCard(title: String, value: String, sub: String) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 4) {
                Text(title)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(value)
                    .font(.title3.bold().monospacedDigit())
                Text(sub)
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(4)
        }
    }

    private func populationItem(title: String, count: UInt32, color: Color) -> some View {
        VStack(spacing: 2) {
            Text(title)
                .font(.caption)
                .foregroundStyle(.secondary)
            Text("\(count)")
                .font(.headline.monospacedDigit())
                .foregroundColor(color)
        }
        .frame(maxWidth: .infinity)
    }
}
