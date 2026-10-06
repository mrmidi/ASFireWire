//
//  AudioGeometrySections.swift
//  ASFW
//
//  Read-only cards and instrument cells for the Audio Geometry panel.
//

import SwiftUI

// MARK: - Primitives

/// One published quantity. `detail` carries the same value in a second unit;
/// `note` is the reasoning, reachable on hover.
struct MetricCell: View {
    let label: String
    let value: String
    var detail: String?
    var note: String?
    var emphasis: Color?

    var body: some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(label)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
            Text(value)
                .font(.callout.weight(.medium))
                .monospacedDigit()
                .foregroundStyle(emphasis ?? Color.primary)
                .lineLimit(1)
            if let detail {
                Text(detail)
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                    .monospacedDigit()
                    .lineLimit(1)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .help(note ?? "")
    }
}

/// Cells reflow by available width: four columns on a wide window, two on a
/// narrow one, without a breakpoint to maintain.
struct MetricGrid<Content: View>: View {
    var minimumWidth: CGFloat = 150
    @ViewBuilder var content: Content

    var body: some View {
        LazyVGrid(
            columns: [GridItem(.adaptive(minimum: minimumWidth), spacing: 16,
                               alignment: .topLeading)],
            alignment: .leading, spacing: 10
        ) {
            content
        }
    }
}

/// A titled surface. One radius and one fill for every card on the page.
struct GeometryCard<Content: View>: View {
    let title: String
    var note: String?
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 4) {
                Text(title).font(.subheadline.weight(.semibold))
                if let note {
                    Image(systemName: "info.circle")
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                        .help(note)
                }
            }
            content
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.quaternary.opacity(0.28), in: RoundedRectangle(cornerRadius: 7))
    }
}

enum GeometryFormat {
    static func packets(_ count: UInt64) -> String { "\(count) packets" }

    static func packetMilliseconds(_ count: UInt64, _ s: AudioGeometrySnapshot) -> String? {
        guard let ms = s.milliseconds(packets: count) else { return nil }
        return String(format: "%.2f ms", ms)
    }

    static func frames(_ count: UInt64) -> String { "\(count) frames" }

    static func frameMilliseconds(_ count: UInt64, _ s: AudioGeometrySnapshot) -> String? {
        guard let ms = s.milliseconds(frames: count) else { return nil }
        return String(format: "%.2f ms", ms)
    }

    static func rate(_ perSecond: Double) -> String {
        String(format: "%.1f/s", perSecond)
    }
}

// MARK: - Stream identity

struct StreamIdentityStrip: View {
    let s: AudioGeometrySnapshot

    var body: some View {
        MetricGrid(minimumWidth: 132) {
            MetricCell(label: "Sample rate",
                       value: s.sampleRateHz == 0 ? "—" : "\(s.sampleRateHz) Hz",
                       detail: "\(s.outputChannels) out / \(s.inputChannels) in")
            if s.framesPerDataPacket != 0 {
                MetricCell(label: "Blocking SYT interval",
                           value: "\(s.framesPerDataPacket) frames",
                           detail: averageLine,
                           note: "Frames one DATA packet carries at this rate.")
                MetricCell(label: "Cadence block",
                           value: "\(s.cadenceBlockPackets) packets",
                           detail: "\(s.cadenceBlockFrames) frames",
                           note: "D,D,D,N — one NO-DATA packet per block.")
            } else {
                MetricCell(label: "Cadence", value: "—",
                           detail: "no supported rate",
                           note: "No supported rate is running, so the driver reports no cadence.")
            }
            MetricCell(label: "Transport",
                       value: s.isStreaming ? "IO running" : "IO stopped",
                       emphasis: s.isStreaming ? .green : .secondary)
        }
    }

    private var averageLine: String? {
        guard let avg = s.framesPerCycle() else { return nil }
        return String(format: "%.2f per cycle avg", avg)
    }
}

// MARK: - Transmit Depth (Observability)

struct TransmitDepthCard: View {
    let s: AudioGeometrySnapshot
    let margin: AudioMarginObserver

    var body: some View {
        GeometryCard(
            title: "Transmit depth & margin",
            note: "Preparation lead is durable packet storage. The margin is observed "
                + "relative to the hardware floor; overrunning the floor holes the "
                + "descriptor ring, which is a fatal transport stall."
        ) {
            MetricGrid(minimumWidth: 140) {
                MetricCell(
                    label: "Preparation lead",
                    value: GeometryFormat.packets(UInt64(s.preparationLeadPackets)),
                    detail: GeometryFormat.packetMilliseconds(UInt64(s.preparationLeadPackets), s),
                    note: "Total lead armed ahead of the hardware."
                )
                MetricCell(
                    label: "Hardware floor",
                    value: GeometryFormat.packets(UInt64(s.hardwareFloorPackets)),
                    detail: GeometryFormat.packetMilliseconds(UInt64(s.hardwareFloorPackets), s),
                    note: "Refill floor below which descriptor starvation occurs."
                )
                observedMarginCell
                if let lifetimeMin = s.lifetimeMinimum {
                    MetricCell(
                        label: "Lifetime min margin",
                        value: GeometryFormat.packets(UInt64(lifetimeMin)),
                        detail: GeometryFormat.packetMilliseconds(UInt64(lifetimeMin), s),
                        note: "Worst margin observed across the entire session."
                    )
                }
            }
        }
    }

    @ViewBuilder
    private var observedMarginCell: some View {
        let worst = margin.worstIntervalMarginPackets ?? s.currentCommittedMarginPackets
        let ratio = margin.marginOverFloor(hardwareFloorPackets: s.hardwareFloorPackets)
        let color: Color? = {
            guard let r = ratio else { return nil }
            if r < 1.0 { return .red }
            if r < 2.0 { return .orange }
            return .green
        }()
        let detailText: String? = {
            if let r = ratio {
                return String(format: "%.1f× floor (%d obs)", r, margin.intervalsObserved)
            }
            return nil
        }()

        MetricCell(
            label: "Observed margin",
            value: GeometryFormat.packets(UInt64(worst)),
            detail: detailText,
            note: "Minimum committed margin observed during this streaming epoch.",
            emphasis: color
        )
    }
}

// MARK: - Interrupt Cadence

struct InterruptCadenceSection: View {
    let s: AudioGeometrySnapshot
    let cadence: AudioCadenceObserver

    var body: some View {
        GeometryCard(title: "Interrupt cadence", note: mathNote) {
            VStack(spacing: 6) {
                headerRow
                Divider()
                directionRow(
                    "Transmit",
                    packetsPerGroup: s.txPacketsPerGroup,
                    micros: s.txInterruptIntervalMicroseconds,
                    observed: cadence.txPacketsPerSecond,
                    carries: s.framesPerCompletionGroupTx == 0
                        ? nil : "\(s.framesPerCompletionGroupTx) frames")
                directionRow(
                    "Receive",
                    packetsPerGroup: s.rxPacketsPerGroup,
                    micros: s.rxInterruptIntervalMicroseconds,
                    observed: cadence.rxPacketsPerSecond,
                    carries: s.maxFramesPerRxInterrupt == 0 ? nil
                        : "\(s.minFramesPerRxInterrupt)–\(s.maxFramesPerRxInterrupt) frames")
                if let combined = combinedRate {
                    HStack {
                        Text("Both contexts").font(.caption2).foregroundStyle(.tertiary)
                        Spacer()
                        Text(GeometryFormat.rate(combined))
                            .font(.caption2).monospacedDigit().foregroundStyle(.tertiary)
                    }
                }
                if let note = healthNote {
                    Text(note).font(.caption).foregroundStyle(.orange)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
    }

    private var headerRow: some View {
        HStack(spacing: 12) {
            Text("").frame(width: 64, alignment: .leading)
            Text("derived").frame(width: 76, alignment: .trailing)
            Text("observed").frame(width: 92, alignment: .trailing)
            Text("interval").frame(maxWidth: .infinity, alignment: .trailing)
        }
        .font(.caption2)
        .foregroundStyle(.tertiary)
    }

    @ViewBuilder
    private func directionRow(_ name: String, packetsPerGroup: UInt32,
                              micros: UInt32, observed: Double?,
                              carries: String?) -> some View {
        HStack(spacing: 12) {
            Text(name).font(.callout).frame(width: 64, alignment: .leading)
            Text(derived(micros))
                .font(.callout.weight(.medium)).monospacedDigit()
                .frame(width: 76, alignment: .trailing)
            HStack(spacing: 5) {
                Spacer(minLength: 0)
                Text(observedText(observed, packetsPerGroup: packetsPerGroup))
                    .font(.callout.weight(.medium)).monospacedDigit()
                    .foregroundStyle(observedTint(observed) ?? Color.primary)
                Circle()
                    .fill(observedTint(observed) ?? Color.secondary)
                    .frame(width: 6, height: 6)
                    .opacity(observed == nil ? 0.35 : 1)
            }
            .frame(width: 92, alignment: .trailing)
            VStack(alignment: .trailing, spacing: 1) {
                Text(micros == 0 ? "—" : "every \(packetsPerGroup) pkt · \(micros) µs")
                    .font(.caption).foregroundStyle(.secondary).monospacedDigit()
                if let carries {
                    Text(carries).font(.caption2).foregroundStyle(.tertiary).monospacedDigit()
                }
            }
            .frame(maxWidth: .infinity, alignment: .trailing)
        }
    }

    private func derived(_ micros: UInt32) -> String {
        guard let hz = AudioGeometrySnapshot.interruptsPerSecond(
            intervalMicroseconds: micros) else { return "—" }
        return GeometryFormat.rate(hz)
    }

    private func observedText(_ rate: Double?, packetsPerGroup: UInt32) -> String {
        guard let rate else { return "waiting…" }
        if rate == 0 { return "stalled" }
        guard let hz = cadence.interruptsPerSecond(
            packetsPerGroup: packetsPerGroup, packetRate: rate) else { return "—" }
        return GeometryFormat.rate(hz)
    }

    private func observedTint(_ rate: Double?) -> Color? {
        guard let rate else { return nil }
        if rate == 0 { return .red }
        let expected = Double(s.isochCyclesPerSecond)
        if abs(rate - expected) / expected > 0.1 { return .orange }
        return .green
    }

    private var combinedRate: Double? {
        guard let tx = cadence.txPacketsPerSecond,
              let rx = cadence.rxPacketsPerSecond,
              s.txPacketsPerGroup != 0, s.rxPacketsPerGroup != 0 else { return nil }
        let txHz = tx / Double(s.txPacketsPerGroup)
        let rxHz = rx / Double(s.rxPacketsPerGroup)
        return txHz + rxHz
    }

    private var healthNote: String? {
        if let tx = cadence.txPacketsPerSecond, tx == 0, s.isStreaming {
            return "Transmit interrupt cursor is frozen. Streaming is stalled."
        }
        if let rx = cadence.rxPacketsPerSecond, rx == 0, s.isStreaming {
            return "Receive interrupt cursor is frozen. Stream is silent."
        }
        return nil
    }

    private var mathNote: String {
        guard s.isochCyclesPerSecond != 0, s.txPacketsPerGroup != 0,
              let hz = AudioGeometrySnapshot.interruptsPerSecond(
                intervalMicroseconds: s.txInterruptIntervalMicroseconds) else {
            return "Cadence unavailable."
        }
        return String(
            format: "%u cycles/s ÷ %u packets per completion group = %.1f "
                + "interrupts/s per context. A cycle is %u µs at every sample "
                + "rate, so the cadence is rate-independent; only the frames it "
                + "carries scale.",
            s.isochCyclesPerSecond, s.txPacketsPerGroup, hz,
            s.microsecondsPerIsochCycle)
    }
}

// MARK: - Reference Cards

struct IsochRingSection: View {
    let s: AudioGeometrySnapshot

    var body: some View {
        GeometryCard(
            title: "Isochronous rings",
            note: "Ring depth buys runway for a late producer; it is not "
                + "presentation latency. Only the payload-finality lead is "
                + "reported to CoreAudio."
        ) {
            MetricGrid {
                packetCell("TX hardware ring", s.txHardwareRingPackets,
                           note: "Descriptors visible to OHCI.")
                packetCell("TX shared slots", s.txSharedSlotPackets,
                           note: "Durable packet storage. Capacity is not latency.")
                packetCell("RX descriptor ring", s.rxHardwareRingPackets,
                           note: "IR receive depth.")
                packetCell("Payload finality", s.txContentFreezePackets,
                           note: "The only TX depth that becomes output latency.")
                packetCell("Repoint guard", s.txRepointGuardPackets,
                           note: "Never rebound: the CommandPtr packet and its successor.")
                if s.txRingLapFrames != 0 {
                    MetricCell(
                        label: "TX ring lap",
                        value: GeometryFormat.frames(UInt64(s.txRingLapFrames)),
                        detail: GeometryFormat.frameMilliseconds(UInt64(s.txRingLapFrames), s),
                        note: "One traversal of the TX hardware ring (\(s.txRingLapFrames) frames)."
                    )
                }
            }
        }
    }

    private func packetCell(_ label: String, _ packets: UInt32, note: String) -> MetricCell {
        MetricCell(label: label,
                   value: GeometryFormat.packets(UInt64(packets)),
                   detail: GeometryFormat.packetMilliseconds(UInt64(packets), s),
                   note: note)
    }
}

struct HalGeometrySection: View {
    let s: AudioGeometrySnapshot

    var body: some View {
        GeometryCard(
            title: "HAL buffers",
            note: "Zero-timestamp period and frame ring wrap together under AudioDriverKit."
        ) {
            MetricGrid {
                frameCell("Frame ring", s.frameRingFrames,
                          note: "Active shared memory audio buffer.")
                frameCell("Client IO budget", s.clientIoBudgetFrames,
                          note: "Maximum nominal span for one client render callback.")
                frameCell("Zero-timestamp period", s.zeroTimestampPeriodFrames,
                          note: "Fixed at device initialization; wraps the ring.")
                frameCell("Scheduling jitter", s.schedulingJitterFrames,
                          note: "Publication retention limit.")
                frameCell("PCM publication cache", s.pcmPublicationCacheFrames,
                          note: "Byte retention capacity.")
                MetricCell(label: "Frame alignment",
                           value: "\(s.frameAlignmentFrames) frames")
                MetricCell(label: "Max blocking frames",
                           value: "\(s.maxBlockingFramesPerDataPacket) frames",
                           note: "Maximum frames per single FireWire packet.")
            }
        }
    }

    private func frameCell(_ label: String, _ frames: UInt32, note: String? = nil) -> MetricCell {
        MetricCell(label: label,
                   value: GeometryFormat.frames(UInt64(frames)),
                   detail: GeometryFormat.frameMilliseconds(UInt64(frames), s),
                   note: note)
    }
}

struct DeclarationsSection: View {
    let s: AudioGeometrySnapshot

    var body: some View {
        GeometryCard(
            title: "CoreAudio declarations",
            note: "Declared latency and safety offsets published to CoreAudio."
        ) {
            MetricGrid {
                MetricCell(
                    label: "Output",
                    value: "\(s.outputLatencyFrames) + \(s.outputSafetyOffsetFrames)",
                    detail: GeometryFormat.frameMilliseconds(
                        UInt64(s.outputLatencyFrames) + UInt64(s.outputSafetyOffsetFrames), s),
                    note: "Declared output latency plus safety offset in frames."
                )
                MetricCell(
                    label: "Input",
                    value: "\(s.inputLatencyFrames) + \(s.inputSafetyOffsetFrames)",
                    detail: GeometryFormat.frameMilliseconds(
                        UInt64(s.inputLatencyFrames) + UInt64(s.inputSafetyOffsetFrames), s),
                    note: "Declared input latency plus safety offset in frames."
                )
                if s.completionBatchFrames != 0 {
                    MetricCell(
                        label: "Completion batch",
                        value: GeometryFormat.frames(UInt64(s.completionBatchFrames)),
                        detail: GeometryFormat.frameMilliseconds(UInt64(s.completionBatchFrames), s),
                        note: "One RX completion group with all packets counted as DATA."
                    )
                }
                if s.txTransferDelayTicks != 0 {
                    MetricCell(
                        label: "Transfer delay",
                        value: "\(s.txTransferDelayTicks) ticks",
                        detail: String(format: "%.2f µs", Double(s.txTransferDelayTicks) * 1_000_000.0 / 24_576_000.0),
                        note: "IEC 61883-6 transfer delay applied to presentation timestamps."
                    )
                }
            }
        }
    }
}

// MARK: - Latency Preview Calculator

struct LatencyPreviewSection: View {
    let s: AudioGeometrySnapshot
    @Binding var previewIoBuffer: UInt32

    private static let previewLadder: [UInt32] = [32, 64, 128, 256, 512]

    var body: some View {
        GeometryCard(
            title: "Declared latency preview",
            note: "Arithmetic sum of installed declarations plus selected host buffer — "
                + "the figure a host/DAW displays as expected latency."
        ) {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 12) {
                    Picker("Client buffer", selection: $previewIoBuffer) {
                        ForEach(Self.previewLadder, id: \.self) { frames in
                            Text("\(frames) frames").tag(frames)
                        }
                    }
                    .controlSize(.small)
                    .frame(maxWidth: 190)

                    MetricCell(
                        label: "Round trip",
                        value: GeometryFormat.frames(s.roundTripFrames(io: previewIoBuffer)),
                        detail: GeometryFormat.frameMilliseconds(s.roundTripFrames(io: previewIoBuffer), s)
                    )
                    MetricCell(
                        label: "Output",
                        value: GeometryFormat.frames(s.outputFrames(io: previewIoBuffer)),
                        detail: GeometryFormat.frameMilliseconds(s.outputFrames(io: previewIoBuffer), s)
                    )
                }
            }
        }
    }
}
