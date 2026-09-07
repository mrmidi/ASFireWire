import Foundation
import IOKit

// MARK: - Wire mirrors
//
// Hand-mirrored from ASFWDriver/UserClient/WireFormats/AudioTuningWireFormats.hpp.
// Field order and padding must match exactly; tests check sizes and offsets,
// and the connector rejects incompatible versions or reply sizes.

struct AudioTuningSnapshotWire {
    var version: UInt32 = 0
    var reserved0: UInt32 = 0
    var endpointId: UInt64 = 0

    var txDispatchSlackPackets: UInt32 = 0
    var txOwnershipGuardPackets: UInt32 = 0
    var preparedTargetPackets: UInt32 = 0
    var preparedLeadFrames: UInt32 = 0

    var outputLatencyFrames: UInt32 = 0
    var inputLatencyFrames: UInt32 = 0
    var outputSafetyOffsetFrames: UInt32 = 0
    var inputSafetyOffsetFrames: UInt32 = 0

    var frameRingFrames: UInt32 = 0
    var clientIoBudgetFrames: UInt32 = 0
    var zeroTimestampPeriodFrames: UInt32 = 0
    var sampleRateHz: UInt32 = 0

    var inputChannels: UInt32 = 0
    var outputChannels: UInt32 = 0
    var txPacketsPerGroup: UInt32 = 0
    var txHardwareRingPackets: UInt32 = 0
    var txSharedSlotPackets: UInt32 = 0
    /// Blocking SYT interval (8 / 16 / 32). Zero when no V3 rate is running.
    var framesPerDataPacket: UInt32 = 0

    var streaming: UInt32 = 0
    var pendingGroups: UInt32 = 0
    var appliedSequence: UInt32 = 0
    var lastError: UInt32 = 0
    var lastWarnings: UInt32 = 0
    var requestId: UInt32 = 0
    var requestStatus: UInt32 = 0
    var supportedGroups: UInt32 = 0
    var ready: UInt32 = 0

    // Read-only geometry. Rate-dependent members are zero until a V3 rate runs.
    var txDispatchSlackFloorPackets: UInt32 = 0
    var txDispatchSlackDefaultPackets: UInt32 = 0
    var isochCyclesPerSecond: UInt32 = 0
    var microsecondsPerIsochCycle: UInt32 = 0
    var rxPacketsPerGroup: UInt32 = 0
    var rxHardwareRingPackets: UInt32 = 0
    var txInterruptIntervalMicroseconds: UInt32 = 0
    var rxInterruptIntervalMicroseconds: UInt32 = 0
    var framesPerCompletionGroupTx: UInt32 = 0
    var framesPerCompletionGroupRx: UInt32 = 0
    var minFramesPerRxInterrupt: UInt32 = 0
    var maxFramesPerRxInterrupt: UInt32 = 0
    var cadenceBlockPackets: UInt32 = 0
    var cadenceBlockFrames: UInt32 = 0
    var txContentFreezePackets: UInt32 = 0
    var txRepointGuardPackets: UInt32 = 0
    var schedulingJitterFrames: UInt32 = 0
    var frameAlignmentFrames: UInt32 = 0
    var pcmPublicationCacheFrames: UInt32 = 0
    var maxBlockingFramesPerDataPacket: UInt32 = 0
    var txSafetyOffsetPolicyFrames: UInt32 = 0
    var rxSafetyOffsetPolicyFrames: UInt32 = 0
    var reportedLatencyPolicyFrames: UInt32 = 0
    var completionBatchFrames: UInt32 = 0
    /// Frames in one TX hardware-ring traversal — the step size of the observed
    /// RTL lattice and of the committed-margin minimum.
    var txRingLapFrames: UInt32 = 0
}

struct AudioTuningRequestWire {
    var version: UInt32 = AudioTuningGroup.wireVersion
    var groups: UInt32 = 0
    var endpointId: UInt64 = 0

    var txDispatchSlackPackets: UInt32 = 0
    var txOwnershipGuardPackets: UInt32 = 0
    var outputLatencyFrames: UInt32 = 0
    var inputLatencyFrames: UInt32 = 0
    var outputSafetyOffsetFrames: UInt32 = 0
    var inputSafetyOffsetFrames: UInt32 = 0
    var frameRingFrames: UInt32 = 0
    var clientIoBudgetFrames: UInt32 = 0
    var zeroTimestampPeriodFrames: UInt32 = 0
    var reserved: UInt32 = 0
}

enum AudioTuningGroup {
    static let wireVersion: UInt32 = 3
    static let transmitDepth: UInt32 = 1 << 0
    static let declarations: UInt32 = 1 << 1
    static let halGeometry: UInt32 = 1 << 2
}

/// Mirrors `ASFW::Audio::Shared::TuningWarning`. A warning means the driver
/// applied the request and is telling the operator what it now costs; it is not
/// a rejection, and it is not the panel's own guess about the value it offered.
enum AudioTuningWarning {
    static let dispatchSlackBelowFloor: UInt32 = 1 << 0
    static let sharedSlotRingOverProvisioned: UInt32 = 1 << 1
    static let declaredLatencyBelowDefault: UInt32 = 1 << 2

    static func labels(_ mask: UInt32) -> [String] {
        var out: [String] = []
        if mask & dispatchSlackBelowFloor != 0 {
            out.append("Dispatch slack is below the asserted floor. A coalesced "
                       + "completion can exhaust the prepared window and hole the "
                       + "descriptor ring — a transport failure, which silence "
                       + "substitution cannot cover.")
        }
        if mask & sharedSlotRingOverProvisioned != 0 {
            out.append("The shared slot ring is larger than the prepared target "
                       + "needs. Harmless: capacity is not latency.")
        }
        if mask & declaredLatencyBelowDefault != 0 {
            out.append("Declared latency is below the profile default. Recordings "
                       + "will be misaligned by the difference.")
        }
        let known = dispatchSlackBelowFloor | sharedSlotRingOverProvisioned
            | declaredLatencyBelowDefault
        if mask & ~known != 0 {
            out.append(String(format: "Driver reported warnings this build does "
                              + "not recognise (0x%08x).", mask & ~known))
        }
        return out
    }
}

// Wide intermediates keep display arithmetic safe for every wire value.
//
// Everything here is presentation of a number the driver published. The panel
// derives nothing structural of its own: the floor, the default, the group
// size and the cycle grid all arrive on the wire, because a second copy in the
// app disagrees with the driver the moment a constant moves.
enum AudioTuningPresentation {
    static func outputFrames(_ s: AudioTuningSnapshotWire, io: UInt32) -> UInt64 {
        UInt64(io) + UInt64(s.outputLatencyFrames) + UInt64(s.outputSafetyOffsetFrames)
    }
    static func roundTripFrames(_ s: AudioTuningSnapshotWire, io: UInt32) -> UInt64 {
        outputFrames(s, io: io) + UInt64(io)
            + UInt64(s.inputLatencyFrames) + UInt64(s.inputSafetyOffsetFrames)
    }
    /// Mirrors `ASFW::Audio::Shared::TuningRequestStatus`. Only the terminal
    /// failures are named: the panel needs them to stop presenting a rejected
    /// value as though it were the setting in force.
    enum Status {
        static let rejected: UInt32 = 5
        static let aborted: UInt32 = 6
        static func isTerminalFailure(_ value: UInt32) -> Bool {
            value == rejected || value == aborted
        }
    }

    static func status(_ s: AudioTuningSnapshotWire) -> String {
        let labels = ["Idle", "Queued", "Waiting for host", "Applying", "Applied",
                      "Rejected", "Aborted", "Unchanged"]
        let label = labels.indices.contains(Int(s.requestStatus))
            ? labels[Int(s.requestStatus)] : "Unknown status"
        return "Request \(s.requestId): \(label)"
            + (s.lastError == 0 ? "" : String(format: " (0x%08x)", s.lastError))
    }

    // MARK: - Unit conversions

    /// Packets to milliseconds. Legitimate at any sample rate because an
    /// isochronous cycle is a fixed 125 us; the grid arrives on the wire so the
    /// app never spells 125 or 8000 itself.
    static func milliseconds(packets: UInt64, _ s: AudioTuningSnapshotWire) -> Double? {
        guard s.microsecondsPerIsochCycle != 0 else { return nil }
        return Double(packets) * Double(s.microsecondsPerIsochCycle) / 1000
    }

    static func milliseconds(frames: UInt64, rate: UInt32) -> Double? {
        guard rate != 0 else { return nil }
        return Double(frames) * 1000 / Double(rate)
    }

    /// Interrupts per second for one context. Deliberately a `Double`: at six
    /// packets a group this is 8000/6 = 1333.33, and rounding it to an integer
    /// misstates the cadence the panel exists to show.
    static func interruptsPerSecond(intervalMicroseconds: UInt32) -> Double? {
        guard intervalMicroseconds != 0 else { return nil }
        return 1_000_000 / Double(intervalMicroseconds)
    }

    /// Average audio frames per isochronous cycle — 6.0 at 48 kHz. Exact for
    /// every rate, unlike the truncating integer field this replaced.
    static func framesPerCycle(_ s: AudioTuningSnapshotWire) -> Double? {
        guard s.isochCyclesPerSecond != 0, s.sampleRateHz != 0 else { return nil }
        return Double(s.sampleRateHz) / Double(s.isochCyclesPerSecond)
    }

    // MARK: - Transmit depth candidates

    /// The dispatch-slack ladder, in whole completion groups, anchored on the
    /// driver's own default rather than on a constant compiled into the app.
    /// Whatever is currently in force is always selectable, even if some other
    /// client set a value off the ladder.
    static func slackPresets(_ s: AudioTuningSnapshotWire) -> [UInt32] {
        let group = max(s.txPacketsPerGroup, 1)
        let defaultGroups = s.txDispatchSlackDefaultPackets / group
        var packets: Set<UInt32> = [s.txDispatchSlackPackets]
        if s.txDispatchSlackDefaultPackets != 0 {
            packets.insert(s.txDispatchSlackDefaultPackets)
        }
        // Sixths of the default: 12, 8, 6, 4 and 2 groups at the shipping value.
        for numerator: UInt32 in [6, 4, 3, 2, 1] {
            let groups = defaultGroups * numerator / 6
            if groups != 0 { packets.insert(groups * group) }
        }
        return packets.sorted(by: >)
    }

    static func isBelowFloor(_ packets: UInt32, _ s: AudioTuningSnapshotWire) -> Bool {
        s.txDispatchSlackFloorPackets != 0 && packets < s.txDispatchSlackFloorPackets
    }

    /// Mirrors `AudioRuntimeTuning::PreparedTargetPackets()`.
    static func preparedTargetPackets(
        slack: UInt32, _ s: AudioTuningSnapshotWire) -> UInt64 {
        UInt64(slack) + UInt64(s.txOwnershipGuardPackets)
    }

    static func groupsLabel(_ packets: UInt32, _ s: AudioTuningSnapshotWire) -> String {
        let group = s.txPacketsPerGroup
        guard group != 0, packets % group == 0 else { return "\(packets) packets" }
        let groups = packets / group
        return "\(packets) packets · \(groups) group\(groups == 1 ? "" : "s")"
    }
}

extension ASFWDriverConnector {

    func getAudioRuntimeTuning(endpointID: AudioEndpointID) -> AudioTuningSnapshotWire? {
        guard connection != 0, endpointID.rawValue != 0 else { return nil }
        var scalarInput = [endpointID.rawValue]
        var wire = AudioTuningSnapshotWire()
        var byteCount = MemoryLayout<AudioTuningSnapshotWire>.size
        let result = withUnsafeMutableBytes(of: &wire) { output in
            IOConnectCallMethod(
                connection,
                Method.getAudioRuntimeTuning.rawValue,
                &scalarInput,
                UInt32(scalarInput.count),
                nil, 0, nil, nil,
                output.baseAddress,
                &byteCount
            )
        }
        guard result == KERN_SUCCESS else {
            lastError = "getAudioRuntimeTuning failed: \(interpretIOReturn(result))"
            return nil
        }
        // A short reply is a version skew between app and dext, not a value to
        // interpret: reading it would show the operator numbers from the wrong
        // fields, which is worse than showing nothing.
        guard byteCount == MemoryLayout<AudioTuningSnapshotWire>.size,
              wire.version == AudioTuningGroup.wireVersion else {
            lastError = "getAudioRuntimeTuning: wire mismatch "
                + "(\(byteCount) bytes, version \(wire.version)) -- "
                + "the installed driver is a different build than this app"
            return nil
        }
        return wire
    }

    /// Submits a candidate. Success means the driver queued it for a
    /// configuration-change window; the geometry is in force only once a
    /// subsequent snapshot reports it, so callers must re-read rather than
    /// assume.
    @discardableResult
    func requestAudioRuntimeTuning(_ request: AudioTuningRequestWire) -> kern_return_t {
        guard connection != 0 else { return kIOReturnNotOpen }
        var payload = request
        let result = withUnsafeBytes(of: &payload) { input in
            IOConnectCallMethod(
                connection,
                Method.requestAudioRuntimeTuning.rawValue,
                nil, 0,
                input.baseAddress,
                MemoryLayout<AudioTuningRequestWire>.size,
                nil, nil, nil, nil
            )
        }
        if result != KERN_SUCCESS {
            lastError = "requestAudioRuntimeTuning failed: \(interpretIOReturn(result))"
        }
        return result
    }
}
