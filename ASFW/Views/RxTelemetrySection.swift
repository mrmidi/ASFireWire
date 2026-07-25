//
//  RxTelemetrySection.swift
//  ASFW
//
//  Capture-ring telemetry for identifying safely removable RX buffering.
//

import SwiftUI

struct RxTelemetrySection: View {
    let endpoint: AudioTelemetryEndpoint

    var body: some View {
        TelemetryPanel(title: "RX buffer safety") {
            HStack(spacing: 12) {
                TelemetryMetricCard(
                    title: "Available now",
                    value: "\(endpoint.rxCurrentAvailableFrames.formatted()) / \(endpoint.inputFrameCapacityFrames.formatted())",
                    detail: "frames available / capacity",
                    tint: endpoint.rxCurrentAvailableFrames == 0 ? .red : .blue
                )
                TelemetryMetricCard(
                    title: "Interval low",
                    value: endpoint.rxIntervalMinimumAvailable?.formatted() ?? "—",
                    detail: "minimum available frames",
                    tint: intervalTint(endpoint.rxIntervalMinimumAvailable)
                )
                TelemetryMetricCard(
                    title: "Interval high",
                    value: endpoint.hasCompletedRxInterval
                        ? endpoint.rxCompletedIntervalMaximumAvailableFrames.formatted()
                        : "—",
                    detail: "maximum occupancy frames",
                    tint: .teal
                )
                TelemetryMetricCard(
                    title: "Minimum headroom",
                    value: endpoint.rxIntervalMinimumFreeHeadroom?.formatted() ?? "—",
                    detail: "writer-free frames",
                    tint: intervalTint(endpoint.rxIntervalMinimumFreeHeadroom)
                )
                TelemetryMetricCard(
                    title: "Interval faults",
                    value: "\(endpoint.rxCompletedIntervalOverrunEvents + endpoint.rxCompletedIntervalStarvationEvents)",
                    detail: "overruns + starvations",
                    tint: intervalFaultTint
                )
            }

            HistogramChart(
                labels: AudioTelemetryEndpoint.rxOccupancyBucketLabels,
                counts: endpoint.rxCompletedOccupancyHistogram,
                tint: .mint
            )
            .frame(height: 150)
            .accessibilityLabel("Capture-ring occupancy distribution for the last completed interval")

            HStack(spacing: 24) {
                Label(
                    "\(endpoint.rxCaptureOverrunEvents.formatted()) overruns · \(endpoint.rxTotalOverwrittenFrames.formatted()) overwritten frames",
                    systemImage: "exclamationmark.triangle"
                )
                Label(
                    "\(endpoint.rxCaptureStarvationEvents.formatted()) starvations · \(endpoint.rxTotalStarvedFrames.formatted()) zero-filled frames",
                    systemImage: "waveform.badge.exclamationmark"
                )
            }
            .font(.caption)
            .foregroundStyle(lifetimeFaultTint)

            HStack(spacing: 24) {
                Label("\(endpoint.rxReplayEntries.formatted()) replay entries", systemImage: "arrow.triangle.2.circlepath")
                Label("\(endpoint.rxReplayEpochResets.formatted()) epoch resets", systemImage: "arrow.counterclockwise")
            }
            .font(.caption)
            .foregroundStyle(.secondary)

            Text("Silence and valid PCM/MIDI-labelled packets advance this telemetry normally. It measures frame ownership, not sample amplitude.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private var intervalFaultTint: Color {
        endpoint.rxCompletedIntervalOverrunEvents + endpoint.rxCompletedIntervalStarvationEvents == 0
            ? .green
            : .red
    }

    private var lifetimeFaultTint: Color {
        endpoint.rxCaptureOverrunEvents + endpoint.rxCaptureStarvationEvents == 0
            ? .secondary
            : .red
    }

    private func intervalTint(_ value: UInt64?) -> Color {
        guard let value else { return .secondary }
        return value == 0 ? .red : .blue
    }
}
