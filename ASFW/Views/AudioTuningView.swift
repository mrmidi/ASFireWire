import SwiftUI

// The Audio Geometry panel.
//
// Transmit depth is the only thing this build can change, so it sits first and
// carries the observed margin it buys — a control with no feedback is tuned
// blind. Everything below it is the driver's published geometry, laid out as
// reflowing cells (AudioGeometrySections) rather than one full-width row per
// value, which is what turned twenty-eight numbers into three screens.
//
// Nothing structural is hardcoded here. The dispatch-slack floor, the shipping
// default, the completion-group size and the cycle grid all arrive on the wire
// (AudioGeometryReport), so the ladder this panel offers and the warning it
// prints cannot drift from the ones the driver enforces.

struct AudioTuningView: View {
    @ObservedObject var connector: ASFWDriverConnector
    @State private var endpointID: AudioEndpointID?
    @State private var snapshot: AudioTuningSnapshotWire?
    @State private var loadError: String?
    @State private var applyError: String?
    @State private var slackPackets: UInt32 = 0
    @State private var previewIoBuffer: UInt32 = 128
    @State private var confirmingApply = false
    /// Request id whose rejection or abort has already been folded back into
    /// the editor, so a terminal failure reseeds exactly once.
    @State private var acknowledgedFailure: UInt32 = 0
    /// Observed counterparts to the published geometry. See
    /// AudioGeometryObservers.swift for why the app has to hold this state.
    @State private var cadence = AudioCadenceObserver()
    @State private var margin = AudioMarginObserver()
    @State private var telemetry: AudioTelemetryEndpoint?

    private static let previewLadder: [UInt32] = [32, 64, 128, 256, 512]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                header
                if let s = snapshot {
                    if s.ready == 0 { notReadyBanner }
                    StreamIdentityStrip(s: s)
                    transmitDepth(s)
                    InterruptCadenceSection(s: s, cadence: cadence)
                    // Three reference cards flow two or three across on a wide
                    // window instead of stacking full width.
                    LazyVGrid(
                        columns: [GridItem(.adaptive(minimum: 330), spacing: 12,
                                           alignment: .topLeading)],
                        alignment: .leading, spacing: 12
                    ) {
                        IsochRingSection(s: s)
                        HalGeometrySection(s: s)
                        DeclarationsSection(s: s)
                    }
                    footer(s)
                }
                if let loadError { Text(loadError).font(.callout).foregroundStyle(.orange) }
                if let applyError { Text(applyError).font(.callout).foregroundStyle(.orange) }
            }
            .padding(16)
        }
        .task {
            reload(rediscover: true, seedEdits: true)
            while !Task.isCancelled {
                do { try await Task.sleep(for: .seconds(1)) } catch { return }
                reload(rediscover: false, seedEdits: false)
            }
        }
        .confirmationDialog("Restart audio streams?", isPresented: $confirmingApply) {
            Button("Apply and restart") { apply() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Audio stops and restarts. Playing apps will hear the gap.")
        }
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 8) {
            Text("Audio Geometry").font(.title3.bold())
            Spacer()
            Button("Reload") { reload(rediscover: true, seedEdits: true) }
                .controlSize(.small)
        }
    }

    // A not-ready endpoint still reports. The request state in the footer is the
    // only record of a change that the disconnect clearing `ready` also aborted,
    // so the panel says why it is inert instead of vanishing behind an IOReturn.
    private var notReadyBanner: some View {
        Text("The audio graph has not published its geometry yet. Rate-dependent "
             + "values are unavailable and nothing can be applied.")
            .font(.callout).foregroundStyle(.orange)
    }

    // MARK: - Transmit depth

    @ViewBuilder
    private func transmitDepth(_ s: AudioTuningSnapshotWire) -> some View {
        GeometryCard(
            title: "Transmit depth",
            note: "A preparation horizon is durable packet storage, not measured "
                + "round-trip latency. The margin below is observed since this "
                + "geometry took effect (apply \(s.appliedSequence)); overrunning "
                + "the floor holes the descriptor ring, which is a transport "
                + "failure, not a recoverable content gap."
        ) {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 8) {
                    Picker("Dispatch slack", selection: $slackPackets) {
                        ForEach(AudioTuningPresentation.slackPresets(s), id: \.self) { packets in
                            Text(presetLabel(packets, s)).tag(packets)
                        }
                    }
                    .labelsHidden()
                    .frame(maxWidth: 260)
                    .disabled(!canEdit(s))
                    Button("Apply") { confirmingApply = true }
                        .disabled(!canEdit(s) || slackPackets == s.txDispatchSlackPackets)
                    Button("Reset") { slackPackets = s.txDispatchSlackDefaultPackets }
                        .disabled(s.txDispatchSlackDefaultPackets == 0)
                    Spacer()
                }
                .controlSize(.small)

                MetricGrid(minimumWidth: 148) {
                    MetricCell(
                        label: "In force",
                        value: AudioTuningPresentation.groupsLabel(
                            s.txDispatchSlackPackets, s),
                        detail: "\(s.preparedTargetPackets) prepared · "
                            + GeometryFormat.frames(UInt64(s.preparedLeadFrames)))
                    MetricCell(
                        label: "Proposed horizon",
                        value: GeometryFormat.packets(
                            AudioTuningPresentation.preparedTargetPackets(
                                slack: slackPackets, s)),
                        detail: GeometryFormat.packetMilliseconds(
                            AudioTuningPresentation.preparedTargetPackets(
                                slack: slackPackets, s), s),
                        note: "Dispatch slack plus the "
                            + "\(s.txOwnershipGuardPackets) packet ownership guard.")
                    MetricCell(
                        label: "Worst margin",
                        value: worstMarginValue,
                        detail: worstMarginDetail,
                        // The warning renders as visible text below; a fault
                        // must not live only in a tooltip. This stays the
                        // neutral explanation of what the number is.
                        note: "Lowest committed margin observed since this "
                            + "geometry took effect, against a fatal floor the "
                            + "driver publishes.",
                        emphasis: marginTint)
                    if let t = telemetry {
                        MetricCell(
                            label: "Current margin",
                            value: GeometryFormat.packets(
                                UInt64(t.currentCommittedMarginPackets)),
                            detail: "floor \(t.hardwareFloorPackets) packets")
                    }
                }

                if telemetry == nil {
                    Text("No telemetry for this endpoint yet — start audio once "
                         + "to see the margin this setting buys.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                if let note = marginNote {
                    Text(note).font(.caption).foregroundStyle(.orange)
                }
                if AudioTuningPresentation.isBelowFloor(slackPackets, s) {
                    Text(belowFloorWarning(s)).font(.caption).foregroundStyle(.orange)
                }
            }
        }
    }

    private func presetLabel(_ packets: UInt32, _ s: AudioTuningSnapshotWire) -> String {
        var label = AudioTuningPresentation.groupsLabel(packets, s)
        if packets == s.txDispatchSlackDefaultPackets { label += " (default)" }
        else if AudioTuningPresentation.isBelowFloor(packets, s) { label += " — below floor" }
        return label
    }

    private func belowFloorWarning(_ s: AudioTuningSnapshotWire) -> String {
        "Below the asserted floor of "
            + AudioTuningPresentation.groupsLabel(s.txDispatchSlackFloorPackets, s)
            + ". A coalesced completion may exhaust the prepared window and hole "
            + "the descriptor ring, which silence substitution cannot cover."
    }

    // What the setting above actually bought. The history is discarded whenever
    // `appliedSequence` moves, so the worst case shown always belongs to the
    // geometry shown.
    private var worstMarginValue: String {
        guard telemetry != nil else { return "—" }
        guard let worst = margin.worstIntervalMarginPackets else {
            return margin.intervalsObserved == 0 ? "waiting" : "not measured"
        }
        return "\(worst) packets"
    }

    private var worstMarginDetail: String? {
        guard let t = telemetry, margin.worstIntervalMarginPackets != nil else { return nil }
        var parts = ["\(margin.intervalsObserved) intervals"]
        if let ratio = margin.marginOverFloor(hardwareFloorPackets: t.hardwareFloorPackets) {
            parts.append(String(format: "%.1f× floor", ratio))
        }
        return parts.joined(separator: " · ")
    }

    private var marginRatio: Double? {
        guard let t = telemetry else { return nil }
        return margin.marginOverFloor(hardwareFloorPackets: t.hardwareFloorPackets)
    }

    private var marginTint: Color? {
        guard let ratio = marginRatio else { return nil }
        if ratio < 1 { return .red }
        return ratio < 2 ? .orange : nil
    }

    private var marginNote: String? {
        guard let ratio = marginRatio else { return nil }
        if ratio < 1 {
            return "The worst interval went below the hardware floor. The "
                + "descriptor ring was holed."
        }
        if ratio < 2 {
            return "The worst interval came within 2× of the hardware floor. "
                + "This dispatch slack has little room left."
        }
        return nil
    }

    // MARK: - Footer

    @ViewBuilder
    private func footer(_ s: AudioTuningSnapshotWire) -> some View {
        GeometryCard(
            title: "Declared latency",
            note: "Arithmetic over the installed declarations plus a host buffer "
                + "— the sum a host prints as its resulting latency. It is not a "
                + "measurement: use tools/rtl/rtl_loopback with a physical "
                + "loopback cable for the real number."
        ) {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 10) {
                    Picker("Client buffer", selection: $previewIoBuffer) {
                        ForEach(previewOptions(s), id: \.self) { frames in
                            Text("\(frames)").tag(frames)
                        }
                    }
                    .controlSize(.small)
                    .frame(maxWidth: 190)
                    MetricCell(
                        label: "Round trip",
                        value: GeometryFormat.frames(
                            AudioTuningPresentation.roundTripFrames(s, io: previewIoBuffer)),
                        detail: GeometryFormat.frameMilliseconds(
                            AudioTuningPresentation.roundTripFrames(s, io: previewIoBuffer), s))
                    MetricCell(
                        label: "Output",
                        value: GeometryFormat.frames(
                            AudioTuningPresentation.outputFrames(s, io: previewIoBuffer)),
                        detail: GeometryFormat.frameMilliseconds(
                            AudioTuningPresentation.outputFrames(s, io: previewIoBuffer), s))
                }
                Divider()
                HStack(alignment: .top, spacing: 8) {
                    Text(AudioTuningPresentation.status(s))
                        .font(.caption).foregroundStyle(.secondary)
                    if s.pendingGroups != 0 {
                        Text("parked, waiting for a configuration window")
                            .font(.caption).foregroundStyle(.orange)
                    }
                    Spacer()
                }
                // The driver's own verdict on the geometry actually installed,
                // which the panel cannot derive.
                ForEach(AudioTuningWarning.labels(s.lastWarnings), id: \.self) { warning in
                    Text(warning).font(.caption).foregroundStyle(.orange)
                }
            }
        }
    }

    /// The client budget is a hard ceiling on what a client may ask for, so the
    /// ladder never offers a buffer the device would refuse.
    private func previewOptions(_ s: AudioTuningSnapshotWire) -> [UInt32] {
        let ceiling = s.clientIoBudgetFrames
        let allowed = Self.previewLadder.filter { ceiling == 0 || $0 <= ceiling }
        return allowed.isEmpty ? [Self.previewLadder[0]] : allowed
    }

    private func canEdit(_ s: AudioTuningSnapshotWire) -> Bool {
        s.ready != 0 && s.pendingGroups == 0
            && s.supportedGroups & AudioTuningGroup.transmitDepth != 0
    }

    // MARK: - IO

    /// Endpoint ids are assigned per bus generation, so discovery runs on the
    /// first load, on an explicit reload, and whenever a read fails — not on
    /// every one-second poll, which would re-enumerate endpoints that only
    /// change on a bus reset.
    private func reload(rediscover: Bool, seedEdits: Bool) {
        var endpoint = endpointID
        if rediscover || endpoint == nil {
            let endpoints = connector.getAudioConfigurationEndpointIDs()
            endpoint = endpoint.flatMap { endpoints.contains($0) ? $0 : nil }
                ?? endpoints.first
        }
        let changed = endpoint != endpointID
        endpointID = endpoint
        if changed {
            // Both observers accumulate per endpoint. `appliedSequence` is
            // per-endpoint too, so it cannot detect this transition on its own:
            // two endpoints can sit on the same apply count and one device's
            // margin would be shown beside another's setting.
            cadence.reset()
            margin.reset()
        }

        guard let endpoint, let wire = connector.getAudioRuntimeTuning(endpointID: endpoint)
        else {
            snapshot = nil
            telemetry = nil
            // Rates are differences between consecutive samples; a gap in the
            // series makes the next pair meaningless, so re-baseline rather
            // than differencing across it.
            cadence.reset()
            loadError = endpoint == nil
                ? "No audio endpoint is published."
                : connector.lastError
            // A failed read may mean the generation moved under us; let the next
            // tick rediscover rather than polling a stale id forever.
            if !rediscover { endpointID = nil }
            return
        }
        snapshot = wire
        loadError = nil
        sampleObserved(wire, endpointID: endpoint)

        if seedEdits || changed {
            slackPackets = wire.txDispatchSlackPackets
            acknowledgedFailure = wire.requestId
        } else if AudioTuningPresentation.Status.isTerminalFailure(wire.requestStatus),
                  wire.requestId != acknowledgedFailure {
            // The driver refused this candidate. Leaving the editor on the
            // rejected value would show it as the setting in force.
            acknowledgedFailure = wire.requestId
            slackPackets = wire.txDispatchSlackPackets
        }

        let options = previewOptions(wire)
        if !options.contains(previewIoBuffer) {
            previewIoBuffer = options.last ?? previewIoBuffer
        }
    }

    /// Joins the geometry snapshot to the telemetry endpoint of the SAME id.
    /// The telemetry surface reports every endpoint; taking the first one would
    /// attribute another device's margin to this device's setting.
    private func sampleObserved(_ wire: AudioTuningSnapshotWire,
                                endpointID: AudioEndpointID) {
        guard let endpoint = connector.getAudioTelemetry()?
            .endpoints.first(where: { $0.endpointId == endpointID }) else {
            telemetry = nil
            cadence.reset()
            return
        }
        telemetry = endpoint
        cadence.sample(txPackets: endpoint.txTransportCompletionCursor,
                       rxPackets: endpoint.rxPacketsSeen,
                       atUptime: ProcessInfo.processInfo.systemUptime)
        margin.sample(appliedSequence: wire.appliedSequence,
                      intervalSequence: endpoint.completedIntervalSequence,
                      intervalMinimum: endpoint.intervalMinimum)
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
            reload(rediscover: false, seedEdits: false)
        } else {
            applyError = connector.lastError ?? "Apply failed."
        }
    }
}
