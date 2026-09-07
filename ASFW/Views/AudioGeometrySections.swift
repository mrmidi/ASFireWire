import SwiftUI

// Read-only halves of the Audio Geometry panel, laid out as an instrument
// readout rather than a settings list.
//
// The layout rule: a fixed value is a *cell*, not a row. Label above value in a
// narrow column, so twenty-eight of them reflow into three or four columns
// instead of twenty-eight full-width lines with a thousand points of nothing in
// the middle. Only quantities that move — the observed cadence, the margin
// verdict — carry weight or colour, so the eye lands on the number that is wrong.
//
// The prose that used to sit under every group is not gone, it is on `.help`.
// It explains a value you are already looking at, which is what a tooltip is
// for; as body text it was most of the page height.
//
// Every number still arrives on the wire from AudioGeometryReport. The app
// derives no structure of its own — no packet-group size, no cycle rate, no
// floor — because a constant duplicated in the app silently disagrees with the
// driver the moment the driver's copy moves.

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

/// A titled surface. One radius and one fill for every card on the page: this
/// is a uniform data surface, and varying it would imply a hierarchy the
/// content does not have.
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
    /// Packets carry their millisecond equivalent on the sub-line rather than
    /// joined onto the value, which keeps the figure column scannable.
    static func packets(_ count: UInt64) -> String { "\(count) packets" }

    static func packetMilliseconds(_ count: UInt64,
                                   _ s: AudioTuningSnapshotWire) -> String? {
        guard let ms = AudioTuningPresentation.milliseconds(packets: count, s) else {
            return nil
        }
        return String(format: "%.2f ms", ms)
    }

    static func frames(_ count: UInt64) -> String { "\(count) frames" }

    static func frameMilliseconds(_ count: UInt64,
                                  _ s: AudioTuningSnapshotWire) -> String? {
        guard let ms = AudioTuningPresentation.milliseconds(
            frames: count, rate: s.sampleRateHz) else { return nil }
        return String(format: "%.2f ms", ms)
    }

    static func rate(_ perSecond: Double) -> String {
        String(format: "%.1f/s", perSecond)
    }
}

// MARK: - Stream identity

/// The running configuration as a single strip. Every frame count on the page
/// is expressed in this rate, so it leads — but it is a few short facts, not a
/// card of its own.
struct StreamIdentityStrip: View {
    let s: AudioTuningSnapshotWire

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
                           note: "No supported rate is running, so the driver "
                               + "reports no cadence rather than the 48 kHz answer.")
            }
            MetricCell(label: "Transport",
                       value: s.streaming != 0 ? "IO running" : "IO stopped",
                       emphasis: s.streaming != 0 ? nil : Color.secondary)
        }
    }

    private var averageLine: String? {
        guard let avg = AudioTuningPresentation.framesPerCycle(s) else { return nil }
        return String(format: "%.2f per cycle avg", avg)
    }
}

// MARK: - Interrupt cadence

/// The one card where numbers move, so the one card that carries colour.
///
/// Derived cadence is a statement about the descriptor program; only the
/// counters say the interrupts are arriving. A transmit row at 0/s while the
/// stream is nominally running is the signature of an interrupt-path stall,
/// which has previously been found only in post-mortem.
struct InterruptCadenceSection: View {
    let s: AudioTuningSnapshotWire
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
        .help(rowNote(observed))
    }

    private func derived(_ micros: UInt32) -> String {
        guard let hz = AudioTuningPresentation.interruptsPerSecond(
            intervalMicroseconds: micros) else { return "—" }
        return GeometryFormat.rate(hz)
    }

    // Each direction converts with its OWN group size. They are equal today and
    // published separately so this code does not have to assume it.
    private func observedText(_ packetRate: Double?, packetsPerGroup: UInt32) -> String {
        guard let packetRate else {
            return cadence.samples == 0 ? "sampling" : "—"
        }
        guard let irq = cadence.interruptsPerSecond(
            packetsPerGroup: packetsPerGroup, packetRate: packetRate) else {
            return String(format: "%.0f pkt/s", packetRate)
        }
        return GeometryFormat.rate(irq)
    }

    /// Colour is an assertion, not decoration: that the packet cursor is
    /// advancing on the cycle grid. Nil means not enough samples to say.
    private func observedTint(_ packetRate: Double?) -> Color? {
        guard let packetRate, s.isochCyclesPerSecond != 0 else { return nil }
        let expected = Double(s.isochCyclesPerSecond)
        if packetRate == 0 { return .red }
        return abs(packetRate - expected) / expected > 0.1 ? .orange : .green
    }

    private func rowNote(_ packetRate: Double?) -> String {
        guard let packetRate else {
            return "Measured by differencing the driver's monotonic packet "
                + "cursors between polls. Two samples are needed before a rate "
                + "exists."
        }
        return String(
            format: "%.0f packets/s observed against a %u packets/s cycle grid. "
                + "Transmit counts packets the transport has completed; receive "
                + "counts packets the master stream decoded.",
            packetRate, s.isochCyclesPerSecond)
    }

    private var combinedRate: Double? {
        guard let tx = AudioTuningPresentation.interruptsPerSecond(
                intervalMicroseconds: s.txInterruptIntervalMicroseconds),
              let rx = AudioTuningPresentation.interruptsPerSecond(
                intervalMicroseconds: s.rxInterruptIntervalMicroseconds) else { return nil }
        return tx + rx
    }

    private var healthNote: String? {
        guard cadence.samples > 0, s.streaming != 0,
              s.isochCyclesPerSecond != 0 else { return nil }
        let expected = Double(s.isochCyclesPerSecond)
        for (name, rate) in [("Transmit", cadence.txPacketsPerSecond),
                             ("Receive", cadence.rxPacketsPerSecond)] {
            guard let rate else { continue }
            if rate == 0 {
                return "\(name) is not advancing while IO is running."
            }
            if abs(rate - expected) / expected > 0.1 {
                return String(format: "%@ is %.0f packets/s against a %.0f "
                              + "packets/s grid.", name, rate, expected)
            }
        }
        return nil
    }

    private var mathNote: String {
        guard s.isochCyclesPerSecond != 0, s.txPacketsPerGroup != 0,
              let hz = AudioTuningPresentation.interruptsPerSecond(
                intervalMicroseconds: s.txInterruptIntervalMicroseconds) else {
            return "Cadence unavailable."
        }
        return String(
            format: "%u cycles/s ÷ %u packets per completion group = %.1f "
                + "interrupts/s per context. A cycle is %u µs at every sample "
                + "rate, so the cadence is rate-independent; only the frames it "
                + "carries scale. Derived values come from geometry; observed "
                + "values are measured. Completion latency distributions live in "
                + "Audio Telemetry.",
            s.isochCyclesPerSecond, s.txPacketsPerGroup, hz,
            s.microsecondsPerIsochCycle)
    }
}

// MARK: - Reference geometry

struct IsochRingSection: View {
    let s: AudioTuningSnapshotWire

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
                           note: "Never rebound: the CommandPtr packet and its "
                               + "successor.")
                if s.txRingLapFrames != 0 {
                    MetricCell(
                        label: "TX ring lap",
                        value: GeometryFormat.frames(UInt64(s.txRingLapFrames)),
                        detail: GeometryFormat.frameMilliseconds(
                            UInt64(s.txRingLapFrames), s),
                        note: "One traversal of the TX hardware ring. Measured "
                            + "round-trip latency lands on a lattice with this "
                            + "spacing, and the committed-margin minimum moves "
                            + "in the same step — a jump of exactly "
                            + "\(s.txRingLapFrames) frames is one lap, not drift.")
                }
            }
        }
    }

    private func packetCell(_ label: String, _ packets: UInt32,
                            note: String) -> MetricCell {
        MetricCell(label: label,
                   value: GeometryFormat.packets(UInt64(packets)),
                   detail: GeometryFormat.packetMilliseconds(UInt64(packets), s),
                   note: note)
    }
}

struct HalGeometrySection: View {
    let s: AudioTuningSnapshotWire

    var body: some View {
        GeometryCard(
            title: "HAL buffers",
            note: "Changing any of these republishes the device: the "
                + "zero-timestamp period is fixed when the HAL device is "
                + "created, and the ring sizes cross shared memory. Neither can "
                + "move under a live device, which is why this build does not "
                + "offer them."
        ) {
            MetricGrid {
                frameCell("Frame ring", s.frameRingFrames,
                          note: "Cross-process shared memory.")
                frameCell("Client IO budget", s.clientIoBudgetFrames,
                          note: "Maximum span a client may ask for.")
                frameCell("Zero-timestamp period", s.zeroTimestampPeriodFrames,
                          note: "Fixed at device init.")
                frameCell("Scheduling jitter", s.schedulingJitterFrames,
                          note: "Publication retention, not a safety floor.")
                frameCell("PCM publication cache", s.pcmPublicationCacheFrames,
                          note: "Byte retention only; no scheduling meaning.")
                MetricCell(label: "Frame alignment",
                           value: "\(s.frameAlignmentFrames) frames")
                MetricCell(label: "Max blocking frames",
                           value: "\(s.maxBlockingFramesPerDataPacket) frames",
                           note: "The divisor the zero-timestamp period must "
                               + "respect.")
            }
        }
    }

    private func frameCell(_ label: String, _ frames: UInt32,
                           note: String? = nil) -> MetricCell {
        MetricCell(label: label,
                   value: GeometryFormat.frames(UInt64(frames)),
                   detail: GeometryFormat.frameMilliseconds(UInt64(frames), s),
                   note: note)
    }
}

struct DeclarationsSection: View {
    let s: AudioTuningSnapshotWire

    var body: some View {
        GeometryCard(
            title: "CoreAudio declarations",
            note: "Runtime declaration and HAL geometry changes are unavailable "
                + "in this build."
        ) {
            MetricGrid {
                MetricCell(
                    label: "Output",
                    value: "\(s.outputLatencyFrames) + \(s.outputSafetyOffsetFrames)",
                    detail: GeometryFormat.frameMilliseconds(
                        UInt64(s.outputLatencyFrames)
                            + UInt64(s.outputSafetyOffsetFrames), s),
                    note: "Declared latency plus safety offset, in frames.")
                MetricCell(
                    label: "Input",
                    value: "\(s.inputLatencyFrames) + \(s.inputSafetyOffsetFrames)",
                    detail: GeometryFormat.frameMilliseconds(
                        UInt64(s.inputLatencyFrames)
                            + UInt64(s.inputSafetyOffsetFrames), s),
                    note: "Declared latency plus safety offset, in frames.")
                if s.completionBatchFrames != 0 {
                    MetricCell(
                        label: "Completion batch",
                        value: GeometryFormat.frames(UInt64(s.completionBatchFrames)),
                        detail: GeometryFormat.frameMilliseconds(
                            UInt64(s.completionBatchFrames), s),
                        note: "One RX completion with every packet counted as "
                            + "DATA. Input safety below this does not cover the "
                            + "interval it exists to cover.",
                        emphasis: s.inputSafetyOffsetFrames < s.completionBatchFrames
                            ? .orange : nil)
                }
                if s.txSafetyOffsetPolicyFrames != 0 {
                    MetricCell(
                        label: "Policy would derive",
                        value: "out \(s.txSafetyOffsetPolicyFrames) · in "
                            + "\(s.rxSafetyOffsetPolicyFrames)",
                        detail: "latency \(s.reportedLatencyPolicyFrames) frames",
                        note: "What the rate ladder alone implies. The device "
                            + "profile declares its own floor and wins where it "
                            + "is larger, so a difference here is not by itself "
                            + "a fault.")
                }
            }
        }
    }
}
