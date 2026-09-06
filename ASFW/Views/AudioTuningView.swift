import SwiftUI

struct AudioTuningView: View {
    @ObservedObject var connector: ASFWDriverConnector
    @State private var endpointID: AudioEndpointID?
    @State private var snapshot: AudioTuningSnapshotWire?
    @State private var loadError: String?
    @State private var applyError: String?
    @State private var slackPackets: UInt32 = 72
    @State private var previewIoBuffer: UInt32 = 128
    @State private var confirmingApply = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                HStack {
                    Text("Audio Geometry").font(.title2.bold())
                    Spacer()
                    Button("Reload") { reload(seedEdits: true) }
                }
                if let s = snapshot {
                    GroupBox("Transmit depth") {
                        VStack(alignment: .leading, spacing: 10) {
                            Picker("Dispatch slack", selection: $slackPackets) {
                                ForEach(presets(s), id: \.self) { packets in
                                    Text("\(packets) packets\(packets == 72 ? " (default)" : "")")
                                        .tag(packets)
                                }
                            }
                            .disabled(s.pendingGroups != 0 || s.supportedGroups & 1 == 0)
                            Text("Active: \(s.txDispatchSlackPackets) slack packets; "
                                 + "\(s.preparedTargetPackets) prepared packets; "
                                 + frameLabel(UInt64(s.preparedLeadFrames), rate: s.sampleRateHz))
                            let target = UInt64(slackPackets) + UInt64(s.txOwnershipGuardPackets)
                            Text("Proposed preparation horizon: \(target) packets · "
                                 + String(format: "%.2f ms", Double(target) / 8.0))
                            Text("Preparation horizon is not measured round-trip latency.")
                                .foregroundStyle(.secondary)
                            if slackPackets < 72 {
                                Text("Below the asserted dispatch-slack floor. Coalesced completions may exhaust the prepared window.")
                                    .foregroundStyle(.orange)
                            }
                        }.frame(maxWidth: .infinity, alignment: .leading).padding(6)
                    }
                    GroupBox("CoreAudio declarations · read only") {
                        VStack(alignment: .leading, spacing: 6) {
                            Text("Output: \(s.outputLatencyFrames) latency + \(s.outputSafetyOffsetFrames) safety frames")
                            Text("Input: \(s.inputLatencyFrames) latency + \(s.inputSafetyOffsetFrames) safety frames")
                            Text("Ring: \(s.frameRingFrames) frames · ZTS period: \(s.zeroTimestampPeriodFrames) frames")
                            Text("Client budget limit: \(s.clientIoBudgetFrames) frames · \(s.sampleRateHz) Hz")
                            Text("Runtime declaration and HAL geometry changes are unavailable in this build.")
                                .foregroundStyle(.secondary)
                        }.frame(maxWidth: .infinity, alignment: .leading).padding(6)
                    }
                    GroupBox("Declared latency preview") {
                        VStack(alignment: .leading, spacing: 8) {
                            Picker("Client buffer (preview only)", selection: $previewIoBuffer) {
                                ForEach([32, 64, 128, 256, 512], id: \.self) { frames in
                                    Text("\(frames)").tag(UInt32(frames))
                                }
                            }
                            Text("Round trip: " + frameLabel(
                                AudioTuningPresentation.roundTripFrames(s, io: previewIoBuffer), rate: s.sampleRateHz))
                            Text("Output: " + frameLabel(
                                AudioTuningPresentation.outputFrames(s, io: previewIoBuffer), rate: s.sampleRateHz))
                            Text("Arithmetic over installed declarations. Measure electrical loopback to compare with the physical path.")
                                .foregroundStyle(.secondary)
                        }.frame(maxWidth: .infinity, alignment: .leading).padding(6)
                    }
                    HStack {
                        Button("Apply and restart streams") { confirmingApply = true }
                            .disabled(s.pendingGroups != 0 || s.ready == 0
                                      || s.supportedGroups & 1 == 0
                                      || slackPackets == s.txDispatchSlackPackets)
                        Button("Reset transmit depth") { slackPackets = 72 }
                        Spacer()
                        Text(s.streaming != 0 ? "IO running" : "IO stopped")
                    }
                    Text(AudioTuningPresentation.status(s)).font(.caption)
                }
                if let loadError { Text(loadError).foregroundStyle(.orange) }
                if let applyError { Text(applyError).foregroundStyle(.orange) }
            }.padding(20)
        }
        .task {
            reload(seedEdits: true)
            while !Task.isCancelled {
                do { try await Task.sleep(for: .seconds(1)) } catch { return }
                reload(seedEdits: false)
            }
        }
        .confirmationDialog("Restart audio streams?", isPresented: $confirmingApply) {
            Button("Apply and restart") { apply() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Audio stops and restarts. Playing apps will hear the gap.")
        }
    }

    private func presets(_ s: AudioTuningSnapshotWire) -> [UInt32] {
        Array(Set([72, 48, 36, 24, 12, s.txDispatchSlackPackets])).sorted(by: >)
    }

    private func reload(seedEdits: Bool) {
        let endpoints = connector.getAudioConfigurationEndpointIDs()
        let endpoint = endpointID.flatMap { endpoints.contains($0) ? $0 : nil } ?? endpoints.first
        let changed = endpoint != endpointID
        endpointID = endpoint
        guard let endpoint, let wire = connector.getAudioRuntimeTuning(endpointID: endpoint) else {
            snapshot = nil
            loadError = endpoint == nil ? "No audio endpoint is published." : connector.lastError
            return
        }
        snapshot = wire
        loadError = nil
        if seedEdits || changed { slackPackets = wire.txDispatchSlackPackets }
    }

    private func apply() {
        guard let s = snapshot, let endpointID else { return }
        applyError = nil
        var request = AudioTuningRequestWire()
        request.endpointId = endpointID.rawValue
        request.groups = AudioTuningGroup.transmitDepth
        request.txDispatchSlackPackets = slackPackets
        request.txOwnershipGuardPackets = s.txOwnershipGuardPackets
        if connector.requestAudioRuntimeTuning(request) == KERN_SUCCESS {
            reload(seedEdits: false)
        } else {
            applyError = connector.lastError ?? "Apply failed."
        }
    }

    private func frameLabel(_ frames: UInt64, rate: UInt32) -> String {
        guard rate != 0 else { return "\(frames) frames" }
        return "\(frames) frames · " + String(format: "%.2f ms", Double(frames) * 1000 / Double(rate))
    }
}
