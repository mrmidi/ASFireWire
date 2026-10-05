//
//  AudioGeometryView.swift
//  ASFW
//
//  Observability panel for FireWire audio timing geometry, interrupt cadence,
//  ring buffers, and CoreAudio declarations.
//

import SwiftUI

struct AudioGeometryView: View {
    @ObservedObject var connector: ASFWDriverConnector

    @State private var selectedEndpointId: UInt64?
    @State private var availableEndpoints: [AudioTelemetryEndpoint] = []
    @State private var snapshot: AudioGeometrySnapshot?
    @State private var previewIoBuffer: UInt32 = 128
    @State private var cadence = AudioCadenceObserver()
    @State private var margin = AudioMarginObserver()
    @State private var loadError: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                header
                if availableEndpoints.count > 1 {
                    endpointPicker
                }

                if let s = snapshot {
                    if !s.isReady {
                        notReadyBanner
                    }
                    StreamIdentityStrip(s: s)
                    TransmitDepthCard(s: s, margin: margin)
                    InterruptCadenceSection(s: s, cadence: cadence)

                    LazyVGrid(
                        columns: [GridItem(.adaptive(minimum: 330), spacing: 12, alignment: .topLeading)],
                        alignment: .leading,
                        spacing: 12
                    ) {
                        IsochRingSection(s: s)
                        HalGeometrySection(s: s)
                        DeclarationsSection(s: s)
                    }

                    LatencyPreviewSection(s: s, previewIoBuffer: $previewIoBuffer)
                } else {
                    noEndpointBanner
                }

                if let loadError {
                    Text(loadError)
                        .font(.callout)
                        .foregroundStyle(.orange)
                }
            }
            .padding(16)
        }
        .task {
            refresh(resetObservers: true)
            while !Task.isCancelled {
                do {
                    try await Task.sleep(for: .seconds(1))
                } catch {
                    return
                }
                refresh(resetObservers: false)
            }
        }
    }

    // MARK: - Header & Banners

    private var header: some View {
        HStack(spacing: 8) {
            Text("Audio Geometry")
                .font(.title3.bold())
            Spacer()
            Button("Reload") {
                refresh(resetObservers: true)
            }
            .controlSize(.small)
        }
    }

    private var endpointPicker: some View {
        Picker("Endpoint", selection: $selectedEndpointId) {
            ForEach(availableEndpoints) { ep in
                Text(String(format: "GUID: 0x%016llX (%d Hz)", ep.guid, ep.sampleRateHz))
                    .tag(Optional(ep.guid))
            }
        }
        .pickerStyle(.menu)
        .frame(maxWidth: 320)
    }

    private var notReadyBanner: some View {
        Text("The audio graph has not published its geometry yet. Rate-dependent values may reflect defaults.")
            .font(.callout)
            .foregroundStyle(.orange)
            .padding(8)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.orange.opacity(0.12), in: RoundedRectangle(cornerRadius: 6))
    }

    private var noEndpointBanner: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label("No Active Audio Endpoint", systemImage: "waveform.badge.exclamationmark")
                .font(.headline)
            Text("Connect an IEEE 1394 audio interface or start an audio stream to inspect its live geometry.")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.quaternary.opacity(0.3), in: RoundedRectangle(cornerRadius: 8))
    }

    // MARK: - Refresh Logic

    private func refresh(resetObservers: Bool) {
        guard let telemetrySnapshot = connector.getAudioTelemetry() else {
            availableEndpoints = []
            snapshot = nil
            loadError = connector.lastError ?? "Driver audio telemetry not accessible."
            if resetObservers {
                cadence.reset()
                margin.reset()
            }
            return
        }

        availableEndpoints = telemetrySnapshot.endpoints
        loadError = nil

        let currentId = selectedEndpointId
        let activeEndpoint = availableEndpoints.first(where: { $0.guid == currentId })
            ?? availableEndpoints.first

        let changed = activeEndpoint?.guid != selectedEndpointId
        selectedEndpointId = activeEndpoint?.guid

        if resetObservers || changed {
            cadence.reset()
            margin.reset()
        }

        guard let endpoint = activeEndpoint else {
            snapshot = nil
            return
        }

        let resolved = AudioGeometrySnapshot.resolve(from: endpoint)
        snapshot = resolved

        // Sample cadence and margin:
        // Transmit packets are counted via preparation wake passes times group size
        let txPackets = endpoint.preparationWakeCount * UInt64(resolved.txPacketsPerGroup)
        let rxPackets = endpoint.rxPacketsSeen
        cadence.sample(
            txPackets: txPackets,
            rxPackets: rxPackets,
            atUptime: ProcessInfo.processInfo.systemUptime
        )

        margin.sample(
            appliedSequence: 1,
            intervalSequence: endpoint.completedIntervalSequence,
            intervalMinimum: endpoint.intervalMinimum
        )
    }
}
