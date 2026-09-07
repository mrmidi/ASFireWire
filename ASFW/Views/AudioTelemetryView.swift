//
//  AudioTelemetryView.swift
//  ASFW
//
//  TX-first telemetry dashboard with RX capture-ring safety telemetry.
//

import Charts
import Combine
import Darwin.Mach
import SwiftUI

@MainActor
final class AudioTelemetryViewModel: ObservableObject {
    @Published private(set) var endpoints: [AudioTelemetryEndpoint] = []
    @Published var selectedEndpointID: AudioEndpointID?
    @Published private(set) var marginHistory: [MarginHistoryPoint] = []
    @Published private(set) var lastUpdated: Date?

    private let connector: ASFWDriverConnector
    private var observedIntervals: [AudioEndpointID: UInt64] = [:]

    init(connector: ASFWDriverConnector) {
        self.connector = connector
    }

    var selectedEndpoint: AudioTelemetryEndpoint? {
        if let selectedEndpointID,
           let endpoint = endpoints.first(where: { $0.endpointId == selectedEndpointID }) {
            return endpoint
        }
        return endpoints.first
    }

    func poll() async {
        while !Task.isCancelled {
            refresh()
            try? await Task.sleep(for: .seconds(1))
        }
    }

    private func refresh() {
        guard let snapshot = connector.getAudioTelemetry() else { return }
        endpoints = snapshot.endpoints
        if selectedEndpointID == nil || !endpoints.contains(where: { $0.endpointId == selectedEndpointID }) {
            selectedEndpointID = endpoints.first?.endpointId
        }
        lastUpdated = Date()

        for endpoint in endpoints where endpoint.hasCompletedInterval {
            guard observedIntervals[endpoint.endpointId] != endpoint.completedIntervalSequence,
                  let intervalMinimum = endpoint.intervalMinimum else { continue }
            observedIntervals[endpoint.endpointId] = endpoint.completedIntervalSequence
            marginHistory.append(MarginHistoryPoint(
                endpointID: endpoint.endpointId,
                timestamp: lastUpdated ?? Date(),
                packets: intervalMinimum,
                latencyMicroseconds: HostTimebase.shared.microseconds(
                    fromHostTicks: endpoint.completedIntervalMaxLatencyTicks)
            ))
        }
        if marginHistory.count > 90 {
            marginHistory.removeFirst(marginHistory.count - 90)
        }
    }
}

struct AudioTelemetryView: View {
    @StateObject private var viewModel: AudioTelemetryViewModel

    init(connector: ASFWDriverConnector) {
        _viewModel = StateObject(wrappedValue: AudioTelemetryViewModel(connector: connector))
    }

    var body: some View {
        Group {
            if let endpoint = viewModel.selectedEndpoint {
                ScrollView {
                    VStack(alignment: .leading, spacing: 18) {
                        header(endpoint)
                        if viewModel.endpoints.count > 1 {
                            Picker("Audio endpoint", selection: $viewModel.selectedEndpointID) {
                                ForEach(viewModel.endpoints) { candidate in
                                    Text(candidate.identityText).tag(Optional(candidate.endpointId))
                                }
                            }
                            .pickerStyle(.menu)
                            .frame(maxWidth: 300)
                        }
                        marginCards(endpoint)
                        charts(endpoint)
                        latencySummary(endpoint)
                        RxTelemetrySection(endpoint: endpoint)
                    }
                    .padding(20)
                }
            } else {
                ContentUnavailableView(
                    "No audio telemetry yet",
                    systemImage: "waveform.badge.magnifyingglass",
                    description: Text("Connect an ASFW audio endpoint and start audio once. The dashboard appears after its direct audio mapping is ready.")
                )
            }
        }
        .navigationTitle("Audio Telemetry")
        .task { await viewModel.poll() }
    }

    private func header(_ endpoint: AudioTelemetryEndpoint) -> some View {
        HStack(alignment: .firstTextBaseline) {
            VStack(alignment: .leading, spacing: 4) {
                Text(endpoint.identityText)
                    .font(.title2.weight(.semibold))
                Text("\(endpoint.sampleRateHz.formatted()) Hz · \(endpoint.outputChannels) out / \(endpoint.inputChannels) in")
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Label(endpoint.isStreaming ? "Streaming" : "Mapped, idle", systemImage: endpoint.isStreaming ? "play.circle.fill" : "pause.circle")
                .foregroundStyle(endpoint.isStreaming ? .green : .secondary)
                .accessibilityLabel(endpoint.isStreaming ? "Audio stream active" : "Audio mapping idle")
        }
    }

    private func marginCards(_ endpoint: AudioTelemetryEndpoint) -> some View {
        HStack(spacing: 12) {
            TelemetryMetricCard(title: "Current margin", value: endpoint.currentCommittedMarginPackets.formatted(), detail: "packets", tint: .blue)
            TelemetryMetricCard(title: "Interval low", value: endpoint.intervalMinimum?.formatted() ?? "—", detail: "this interval", tint: marginTint(endpoint.intervalMinimum, floor: endpoint.hardwareFloorPackets, lead: endpoint.preparationLeadPackets))
            TelemetryMetricCard(title: "Lifetime low", value: endpoint.lifetimeMinimum?.formatted() ?? "—", detail: "since start", tint: marginTint(endpoint.lifetimeMinimum, floor: endpoint.hardwareFloorPackets, lead: endpoint.preparationLeadPackets))
            TelemetryMetricCard(title: "Safety geometry", value: "\(endpoint.hardwareFloorPackets) / \(endpoint.preparationLeadPackets)", detail: "floor / lead", tint: .secondary)
        }
    }

    @ViewBuilder
    private func charts(_ endpoint: AudioTelemetryEndpoint) -> some View {
        HStack(alignment: .top, spacing: 16) {
            TelemetryPanel(title: "Preparation latency · last interval") {
                HistogramChart(
                    labels: AudioTelemetryEndpoint.latencyBucketLabels,
                    counts: endpoint.completedLatencyHistogram,
                    tint: .purple
                )
            }
            TelemetryPanel(title: "Committed margin · last interval · fractions of the hardware ring") {
                HistogramChart(
                    labels: AudioTelemetryEndpoint.marginBucketLabels,
                    counts: endpoint.completedMarginHistogram,
                    tint: .blue
                )
            }
        }
        .frame(minHeight: 240)
        .overlay(alignment: .bottomLeading) {
            if !endpoint.hasCompletedInterval {
                Text("Waiting for the first completed heartbeat interval.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }

        // Plot preparation latency, not margin.
        //
        // Margin is pinned at the prepared target -- the producer tops up to it
        // on every wake -- so charting it drew a flat line that could only move
        // during a catastrophe, while the quantity that varies every interval
        // had no chart at all. Scheduling hiccups now show up as spikes against
        // the 750 µs budget, which is what this page is for.
        TelemetryPanel(title: "Interval max preparation latency") {
            Chart(viewModel.marginHistory.filter { $0.endpointID == endpoint.endpointId }) { point in
                LineMark(
                    x: .value("Time", point.timestamp),
                    y: .value("µs", point.latencyMicroseconds)
                )
                .foregroundStyle(.purple)
                PointMark(
                    x: .value("Time", point.timestamp),
                    y: .value("µs", point.latencyMicroseconds)
                )
                .foregroundStyle(.purple)
                RuleMark(y: .value("Budget", 750))
                    .foregroundStyle(.orange)
                    .lineStyle(StrokeStyle(dash: [4, 3]))
                    .annotation(position: .top, alignment: .leading) {
                        Text("750 µs budget")
                            .font(.caption2)
                            .foregroundStyle(.orange)
                    }
                RuleMark(y: .value("Early warning", 1500))
                    .foregroundStyle(.red)
                    .lineStyle(StrokeStyle(dash: [4, 3]))
                    .annotation(position: .top, alignment: .leading) {
                        Text("1.5 ms early warning")
                            .font(.caption2)
                            .foregroundStyle(.red)
                    }
            }
            .chartYAxisLabel("µs")
            .frame(height: 180)
            .accessibilityLabel("Interval maximum preparation latency against the 750 microsecond budget")
        }
    }

    private func latencySummary(_ endpoint: AudioTelemetryEndpoint) -> some View {
        HStack(spacing: 12) {
            TelemetryMetricCard(title: "Last wake", value: endpoint.lastPreparationLatencyTicks.microsecondsText, detail: "preparation latency", tint: .purple)
            TelemetryMetricCard(title: "Interval max", value: endpoint.completedIntervalMaxLatencyTicks.microsecondsText, detail: "preparation latency", tint: .purple)
            TelemetryMetricCard(title: "Lifetime max", value: endpoint.maxPreparationLatencyTicks.microsecondsText, detail: "preparation latency", tint: .orange)
            TelemetryMetricCard(title: "Late wakes", value: endpoint.lateWakeCount.formatted(), detail: "of \(endpoint.preparationWakeCount.formatted()) over 750 µs", tint: endpoint.lateWakeCount == 0 ? .green : .orange)
            TelemetryMetricCard(title: "≥1.5 ms", value: endpoint.preparationAtLeast1500Us.formatted(), detail: "early-warning wakes", tint: endpoint.preparationAtLeast1500Us == 0 ? .green : .orange)
        }
    }

    // Judge the margin against the geometry actually in force, not a fixed
    // multiple of the floor.
    //
    // The old rule was `<= floor * 2 ? .red : <= floor * 4 ? .orange : .green`.
    // Margin is pinned at the prepared lead, and the lead is floor + slack, so
    // the ratio is fixed by the geometry: it was 2.5x at ring 48 / slack 72,
    // and became exactly 2.0x when both went to 504. A healthy stream therefore
    // rendered RED, and green -- needing slack above three times the ring --
    // was unreachable by any geometry anyone would ship.
    //
    // What the operator needs to know is simply: are we where this geometry
    // says we should be, have we slipped, or are we below the fatal floor.
    private func marginTint(_ value: UInt32?, floor: UInt32, lead: UInt32) -> Color {
        guard let value else { return .secondary }
        if value < floor { return .red }      // the descriptor ring can hole
        if value < lead { return .orange }    // slipped below target
        return .green                          // sitting at target
    }
}

/// One completed heartbeat interval. Margin and preparation latency are sampled
/// at the same instant behind the same interval-sequence gate, so they ride one
/// point rather than two arrays that could drift apart.
struct MarginHistoryPoint: Identifiable {
    let endpointID: AudioEndpointID
    let timestamp: Date
    let packets: UInt32
    /// Worst preparation latency in this interval. This is the quantity that
    /// actually moves; margin is pinned at the prepared target by construction.
    let latencyMicroseconds: UInt64
    var id: String { "\(endpointID.rawValue)-\(timestamp.timeIntervalSinceReferenceDate)" }
}

struct TelemetryMetricCard: View {
    let title: String
    let value: String
    let detail: String
    let tint: Color

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.title3.monospacedDigit()).foregroundStyle(tint)
            Text(detail).font(.caption2).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(12)
        .background(.quaternary, in: .rect(cornerRadius: 10))
        .accessibilityElement(children: .combine)
    }
}

struct TelemetryPanel<Content: View>: View {
    let title: String
    @ViewBuilder let content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title).font(.headline)
            content
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(14)
        .background(.quaternary, in: .rect(cornerRadius: 12))
    }
}

struct HistogramChart: View {
    let labels: [String]
    let counts: [UInt64]
    let tint: Color

    private var buckets: [(label: String, count: UInt64)] {
        Array(zip(labels, counts)).map { (label: $0.0, count: $0.1) }
    }

    var body: some View {
        Chart(buckets, id: \.label) { bucket in
            BarMark(
                x: .value("Count", bucket.count),
                y: .value("Bucket", bucket.label)
            )
            .foregroundStyle(tint.gradient)
            .accessibilityLabel("\(bucket.label): \(bucket.count) samples")
        }
        .chartXAxisLabel("Samples")
        .frame(height: 180)
    }
}

private extension AudioTelemetryEndpoint {
    var identityText: String {
        "Endpoint \(endpointId.rawValue) · device \(deviceInstanceId.rawValue) · observed \(String(format: "0x%016llX", observedGuid))"
    }
}

private extension UInt64 {
    var microsecondsText: String {
        let timebase = HostTimebase.shared
        return "\(timebase.microseconds(fromHostTicks: self).formatted()) µs"
    }
}

private final class HostTimebase {
    static let shared = HostTimebase()
    private let info: mach_timebase_info_data_t

    private init() {
        var timebase = mach_timebase_info_data_t()
        mach_timebase_info(&timebase)
        info = timebase
    }

    func microseconds(fromHostTicks ticks: UInt64) -> UInt64 {
        UInt64((Double(ticks) * Double(info.numer) / Double(info.denom)) / 1_000)
    }
}
