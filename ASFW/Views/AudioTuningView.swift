import SwiftUI

/// Audio geometry tuning.
///
/// Exists so a latency hypothesis costs a stream restart instead of a rebuild
/// and reinstall. Every editable value has a compile-time default that is the
/// shipping configuration; the presets below the default deliberately leave the
/// envelope the driver's static_asserts guarantee, which is the point, so they
/// are labelled rather than offered as equals.
struct AudioTuningView: View {
    @ObservedObject var connector: ASFWDriverConnector

    // Discovered rather than passed in: endpoint ids are assigned by the driver
    // and change across generations, so a hard-coded one is wrong the first
    // time a device is replugged.
    @State private var endpointID: AudioEndpointID?
    @State private var snapshot: AudioTuningSnapshotWire?
    @State private var loadError: String?

    // Edited values. Seeded from the snapshot on first load so the form always
    // opens showing what the driver is actually running.
    @State private var slackGroups: UInt32 = 12
    @State private var outputLatency: String = ""
    @State private var inputLatency: String = ""
    @State private var outputSafety: String = ""
    @State private var inputSafety: String = ""
    @State private var ztsPeriod: UInt32 = 8192
    @State private var frameRing: UInt32 = 8192

    @State private var editDepth = false
    @State private var editDeclarations = false
    @State private var editHalGeometry = false

    @State private var applying = false
    @State private var applyResult: String?
    @State private var confirmingApply = false

    /// The client buffer size the math is shown against. Purely a display
    /// choice: it is the *client's* budget, not ours, and changing it here
    /// changes nothing in the driver.
    @State private var previewIoBuffer: UInt32 = 128

    private static let ioBufferChoices: [UInt32] = [32, 64, 128, 256, 512]
    private static let ztsChoices: [UInt32] = [1024, 2048, 4096, 8192]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                header
                if let snapshot {
                    transmitDepthSection(snapshot)
                    declarationsSection(snapshot)
                    halGeometrySection(snapshot)
                    coreAudioSection(snapshot)
                    applySection(snapshot)
                } else {
                    ContentUnavailableView(
                        "No audio endpoint",
                        systemImage: "waveform.slash",
                        description: Text(loadError
                            ?? "Connect a device and start audio to read its geometry.")
                    )
                }
            }
            .padding(20)
        }
        .onAppear(perform: reload)
    }

    // MARK: - Sections

    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("Audio Geometry").font(.title2.bold())
                Text("Tunes the running driver. No rebuild required.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            Button("Reload", systemImage: "arrow.clockwise", action: reload)
                .labelStyle(.iconOnly)
        }
    }

    private func transmitDepthSection(_ s: AudioTuningSnapshotWire) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 10) {
                Toggle("Change transmit depth", isOn: $editDepth)

                Picker("Dispatch slack", selection: $slackGroups) {
                    ForEach(depthPresets(s), id: \.groups) { preset in
                        Text(preset.label).tag(preset.groups)
                    }
                }
                .disabled(!editDepth)

                let target = s.txOwnershipGuardPackets
                    + slackGroups * max(s.txPacketsPerGroup, 1)
                let lead = target * max(s.framesPerPacketAverage, 1)
                metricGrid([
                    ("prepared target", "\(target) packets"),
                    ("prepared lead", frameLabel(lead, rate: s.sampleRateHz)),
                    ("ownership guard", "\(s.txOwnershipGuardPackets) packets"),
                    ("shared slot store", "\(s.txSharedSlotPackets) packets"),
                ])

                if editDepth, slackGroups < 12 {
                    warning("Below the driver's asserted floor of 12 completion "
                        + "groups. A coalesced completion delta larger than this "
                        + "holes the descriptor ring, and silence substitution "
                        + "cannot cover a transport failure.")
                }
            }
            .padding(6)
        } label: { Label("Transmit depth", systemImage: "arrow.up.forward") }
    }

    private func declarationsSection(_ s: AudioTuningSnapshotWire) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 10) {
                Toggle("Change declarations", isOn: $editDeclarations)
                Text("What CoreAudio is told. These do not change the physical "
                    + "path — they change what hosts believe about it.")
                    .font(.caption).foregroundStyle(.secondary)

                Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 6) {
                    GridRow {
                        Text("Output").gridColumnAlignment(.trailing)
                        field("latency", $outputLatency)
                        field("safety", $outputSafety)
                    }
                    GridRow {
                        Text("Input").gridColumnAlignment(.trailing)
                        field("latency", $inputLatency)
                        field("safety", $inputSafety)
                    }
                }
                .disabled(!editDeclarations)

                Text("Blank keeps the device profile's own value.")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            .padding(6)
        } label: { Label("Declarations", systemImage: "doc.text") }
    }

    private func halGeometrySection(_ s: AudioTuningSnapshotWire) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 10) {
                Toggle("Change HAL geometry", isOn: $editHalGeometry)
                Picker("Zero-timestamp period", selection: $ztsPeriod) {
                    ForEach(Self.ztsChoices, id: \.self) { value in
                        Text("\(value) fr · \(msLabel(value, rate: s.sampleRateHz))")
                            .tag(value)
                    }
                }
                .disabled(!editHalGeometry)
                metricGrid([
                    ("frame ring", "\(s.frameRingFrames) fr"),
                    ("client io budget", "\(s.clientIoBudgetFrames) fr"),
                ])
                if editHalGeometry {
                    warning("Republishes the CoreAudio device: the period is "
                        + "fixed when the device object is created. Every app "
                        + "using it must re-select the device.")
                }
            }
            .padding(6)
        } label: { Label("HAL geometry", systemImage: "clock") }
    }

    private func coreAudioSection(_ s: AudioTuningSnapshotWire) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 10) {
                Picker("Client I/O buffer", selection: $previewIoBuffer) {
                    ForEach(Self.ioBufferChoices, id: \.self) { Text("\($0)").tag($0) }
                }
                .pickerStyle(.segmented)
                Text("A per-client budget, not ours. Shown so the arithmetic "
                    + "below matches what that client would display.")
                    .font(.caption2).foregroundStyle(.secondary)

                let outLat = resolved(outputLatency, s.outputLatencyFrames)
                let inLat = resolved(inputLatency, s.inputLatencyFrames)
                let outSafe = resolved(outputSafety, s.outputSafetyOffsetFrames)
                let inSafe = resolved(inputSafety, s.inputSafetyOffsetFrames)
                let outPath = previewIoBuffer + outSafe + outLat
                let inPath = previewIoBuffer + inSafe + inLat
                let roundTrip = outPath + inPath

                metricGrid([
                    ("sample rate", "\(s.sampleRateHz) Hz"),
                    ("channels", "\(s.inputChannels) in / \(s.outputChannels) out"),
                    ("scheduling distance",
                     "\(2 * previewIoBuffer + outSafe + inSafe) fr"),
                    ("declared hardware", "\(outLat + inLat) fr"),
                ])

                Divider()
                HStack(alignment: .firstTextBaseline) {
                    Text("Round trip").font(.headline)
                    Spacer()
                    Text(frameLabel(roundTrip, rate: s.sampleRateHz))
                        .font(.title3.monospacedDigit())
                }
                HStack {
                    Text("Output path").foregroundStyle(.secondary)
                    Spacer()
                    Text(frameLabel(outPath, rate: s.sampleRateHz)).monospacedDigit()
                }
                Text("This is arithmetic over our declarations, not a "
                    + "measurement. Compare it against tools/rtl/rtl_loopback; "
                    + "the difference is the residual.")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            .padding(6)
        } label: { Label("What CoreAudio sees", systemImage: "speaker.wave.2") }
    }

    private func applySection(_ s: AudioTuningSnapshotWire) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            let cost = applyCost(s)
            if cost.isDisruptive {
                Label(cost.label, systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange).font(.callout)
            }
            HStack {
                Button("Apply") { confirmingApply = true }
                    .buttonStyle(.borderedProminent)
                    .disabled(applying || !cost.isDisruptive)
                Button("Reset to defaults", action: resetToDefaults)
                if applying { ProgressView().controlSize(.small) }
                Spacer()
            }
            if let applyResult {
                Text(applyResult).font(.caption).foregroundStyle(.secondary)
            }
            if s.pendingGroups != 0 {
                Text("A request is parked and has not been applied yet — the "
                    + "host has not granted the configuration window.")
                    .font(.caption).foregroundStyle(.orange)
            }
        }
        .confirmationDialog(
            applyCost(s) == .deviceRepublish
                ? "Republish the CoreAudio device?"
                : "Restart the audio streams?",
            isPresented: $confirmingApply, titleVisibility: .visible
        ) {
            Button("Apply and restart", role: .destructive) { apply(s) }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(applyCost(s) == .deviceRepublish
                ? "Audio stops and the device disappears and reappears. Apps "
                    + "using it must re-select it."
                : "Audio stops and restarts. Playing apps will hear the gap.")
        }
    }

    // MARK: - Helpers

    private struct DepthPreset { let groups: UInt32; let label: String }

    private func depthPresets(_ s: AudioTuningSnapshotWire) -> [DepthPreset] {
        let perGroup = max(s.txPacketsPerGroup, 1)
        let framesPerPacket = max(s.framesPerPacketAverage, 1)
        let ladder: [UInt32] = [12, 8, 6, 4, 2]
        return ladder.map { groups in
            let packets = groups * perGroup
            let lead = (s.txOwnershipGuardPackets + packets) * framesPerPacket
            let suffix = groups == 12 ? " (default)" : "  ⚠ below floor"
            return DepthPreset(
                groups: groups,
                label: "\(groups) groups — \(packets) pkt · lead \(lead) fr"
                    + msSuffix(lead, rate: s.sampleRateHz) + suffix)
        }
    }

    private func applyCost(_ s: AudioTuningSnapshotWire) -> AudioTuningApplyCost {
        if editHalGeometry, ztsPeriod != s.zeroTimestampPeriodFrames {
            return .deviceRepublish
        }
        let perGroup = max(s.txPacketsPerGroup, 1)
        if editDepth, slackGroups * perGroup != s.txDispatchSlackPackets {
            return .streamRearm
        }
        if editDeclarations,
           resolved(outputLatency, s.outputLatencyFrames) != s.outputLatencyFrames
            || resolved(inputLatency, s.inputLatencyFrames) != s.inputLatencyFrames
            || resolved(outputSafety, s.outputSafetyOffsetFrames) != s.outputSafetyOffsetFrames
            || resolved(inputSafety, s.inputSafetyOffsetFrames) != s.inputSafetyOffsetFrames {
            return .streamRearm
        }
        return .nothing
    }

    private func reload() {
        let endpoint = endpointID
            ?? connector.getAudioConfigurationEndpointIDs().first
        guard let endpoint else {
            loadError = "No audio endpoint is published."
            snapshot = nil
            return
        }
        endpointID = endpoint
        guard let wire = connector.getAudioRuntimeTuning(endpointID: endpoint) else {
            loadError = connector.lastError
            snapshot = nil
            return
        }
        loadError = nil
        snapshot = wire
        let perGroup = max(wire.txPacketsPerGroup, 1)
        slackGroups = wire.txDispatchSlackPackets / perGroup
        ztsPeriod = wire.zeroTimestampPeriodFrames
        frameRing = wire.frameRingFrames
        outputLatency = "\(wire.outputLatencyFrames)"
        inputLatency = "\(wire.inputLatencyFrames)"
        outputSafety = "\(wire.outputSafetyOffsetFrames)"
        inputSafety = "\(wire.inputSafetyOffsetFrames)"
    }

    private func resetToDefaults() {
        slackGroups = 12
        ztsPeriod = 8192
        frameRing = 8192
        outputLatency = ""; inputLatency = ""
        outputSafety = ""; inputSafety = ""
        applyResult = nil
    }

    private func apply(_ s: AudioTuningSnapshotWire) {
        applying = true
        applyResult = nil
        var request = AudioTuningRequestWire()
        request.endpointId = endpointID?.rawValue ?? 0
        // Start from what is running, so an unselected group is submitted
        // unchanged rather than as zero.
        request.txDispatchSlackPackets = s.txDispatchSlackPackets
        request.txOwnershipGuardPackets = s.txOwnershipGuardPackets
        request.outputLatencyFrames = s.outputLatencyFrames
        request.inputLatencyFrames = s.inputLatencyFrames
        request.outputSafetyOffsetFrames = s.outputSafetyOffsetFrames
        request.inputSafetyOffsetFrames = s.inputSafetyOffsetFrames
        request.frameRingFrames = s.frameRingFrames
        request.clientIoBudgetFrames = s.clientIoBudgetFrames
        request.zeroTimestampPeriodFrames = s.zeroTimestampPeriodFrames

        var groups: UInt32 = 0
        if editDepth {
            groups |= AudioTuningGroup.transmitDepth
            request.txDispatchSlackPackets =
                slackGroups * max(s.txPacketsPerGroup, 1)
        }
        if editDeclarations {
            groups |= AudioTuningGroup.declarations
            request.outputLatencyFrames = resolved(outputLatency, s.outputLatencyFrames)
            request.inputLatencyFrames = resolved(inputLatency, s.inputLatencyFrames)
            request.outputSafetyOffsetFrames = resolved(outputSafety, s.outputSafetyOffsetFrames)
            request.inputSafetyOffsetFrames = resolved(inputSafety, s.inputSafetyOffsetFrames)
        }
        if editHalGeometry {
            groups |= AudioTuningGroup.halGeometry
            request.zeroTimestampPeriodFrames = ztsPeriod
            request.frameRingFrames = frameRing
        }
        request.groups = groups

        let result = connector.requestAudioRuntimeTuning(request)
        applying = false
        applyResult = result == KERN_SUCCESS
            ? "Submitted. The driver applies it inside a configuration-change "
                + "window; reload to confirm what is in force."
            : (connector.lastError ?? "Apply failed.")
        // Deliberately not optimistic: the geometry is in force only when a
        // fresh snapshot reports it, so re-read rather than assume.
        if result == KERN_SUCCESS { reload() }
    }

    private func resolved(_ text: String, _ fallback: UInt32) -> UInt32 {
        text.isEmpty ? fallback : (UInt32(text) ?? fallback)
    }

    private func field(_ prompt: String, _ binding: Binding<String>) -> some View {
        TextField(prompt, text: binding)
            .textFieldStyle(.roundedBorder)
            .frame(width: 90)
            .monospacedDigit()
    }

    private func warning(_ text: String) -> some View {
        Label(text, systemImage: "exclamationmark.triangle")
            .font(.caption).foregroundStyle(.orange)
            .fixedSize(horizontal: false, vertical: true)
    }

    private func metricGrid(_ rows: [(String, String)]) -> some View {
        Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 3) {
            ForEach(rows, id: \.0) { row in
                GridRow {
                    Text(row.0).foregroundStyle(.secondary)
                    Text(row.1).monospacedDigit()
                }
            }
        }
        .font(.caption)
    }

    private func msLabel(_ frames: UInt32, rate: UInt32) -> String {
        guard rate > 0 else { return "—" }
        return String(format: "%.2f ms", Double(frames) * 1000.0 / Double(rate))
    }

    private func msSuffix(_ frames: UInt32, rate: UInt32) -> String {
        rate > 0 ? " · \(msLabel(frames, rate: rate))" : ""
    }

    private func frameLabel(_ frames: UInt32, rate: UInt32) -> String {
        rate > 0 ? "\(frames) fr · \(msLabel(frames, rate: rate))" : "\(frames) fr"
    }
}
