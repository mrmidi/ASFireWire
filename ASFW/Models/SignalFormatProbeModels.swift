import Foundation

/// Which plug the signal-format probe asks about.
///
/// On M-Audio "special" firmware the *input* plug is the one that reports the
/// rate actually in effect — Linux's `special_get_rate` comments this as "Input
/// plug shows actual rate"
/// (`references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:302-313`).
/// Ordinary BeBoB firmware answers on the output plug instead.
enum SignalFormatPlugDirection: Sendable, Equatable {
    case input
    case output
}

/// How the probe ended. Mirrors `SignalFormatProbeOutcome` in
/// `ASFWDriver/Protocols/AVC/AVCSignalFormatProbe.hpp`; the raw values are a
/// wire encoding shared with the driver and must not be reordered.
enum SignalFormatProbeOutcome: UInt8, Sendable {
    case decoded = 0
    case malformed = 1
    case notImplemented = 2
    case rejected = 3
    /// The device answered IN TRANSITION, or the sfc field was reserved. Both
    /// mean "ask again", not "unsupported".
    case inTransition = 4
    /// The reply did not echo the request, so it belongs to a different command.
    case mismatched = 5
}

/// Transport-level outcome, mirroring `FCPStatus` in `FCPTransport.hpp`. Carried
/// alongside the AV/C outcome so "the device never answered" stays
/// distinguishable from "the device refused" and from "we declined to send it".
enum SignalFormatProbeTransportStatus: UInt8, Sendable {
    case ok = 0
    case timeout = 1
    case busReset = 2
    case transportError = 3
    case invalidPayload = 4
    case responseMismatch = 5
    case busy = 6
    /// The frame is not in this device's permitted command set. Reaching this
    /// from the probe would be a driver bug — the probe builds a permitted
    /// frame by construction — so it means the table and the builder disagree.
    case refusedByFilter = 7
}

/// Byte-for-byte `AVCSignalFormatProbeResultWire` from
/// `ASFWDriver/UserClient/WireFormats/AVCWireFormats.hpp`.
struct AVCSignalFormatProbeResultWire {
    var outcome: UInt8
    var sfc: UInt8
    var fcpStatus: UInt8
    var plugID: UInt8
    var isInputPlug: UInt8
    var padding0: UInt8
    var padding1: UInt8
    var padding2: UInt8
    var sampleRateHz: UInt32
}

/// A decoded signal-format probe result.
///
/// The AV/C decode happens in the driver, which is where the request lives and
/// therefore the only place the reply's echo can be validated. This type just
/// unpacks the verdict.
struct SignalFormatProbeResult: Sendable, Equatable {
    let outcome: SignalFormatProbeOutcome
    let transportStatus: SignalFormatProbeTransportStatus
    /// Meaningful only when `outcome == .decoded`.
    let sampleRateHz: UInt32
    /// The raw sfc nibble, kept even when reserved.
    let sfc: UInt8
    let plugID: UInt8
    let plugDirection: SignalFormatPlugDirection

    init?(wire data: Data) {
        guard data.count >= MemoryLayout<AVCSignalFormatProbeResultWire>.size else {
            return nil
        }
        let bytes = [UInt8](data)
        outcome = SignalFormatProbeOutcome(rawValue: bytes[0]) ?? .malformed
        sfc = bytes[1]
        transportStatus = SignalFormatProbeTransportStatus(rawValue: bytes[2]) ?? .transportError
        plugID = bytes[3]
        plugDirection = bytes[4] == 1 ? .input : .output
        sampleRateHz = UInt32(bytes[8])
            | UInt32(bytes[9]) << 8
            | UInt32(bytes[10]) << 16
            | UInt32(bytes[11]) << 24
    }

    /// One line suitable for a report or a log.
    var summary: String {
        let plug = "\(plugDirection == .input ? "in" : "out") plug \(plugID)"
        guard transportStatus == .ok else {
            return "\(plug): no answer (\(transportStatus))"
        }
        switch outcome {
        case .decoded:
            return "\(plug): \(sampleRateHz) Hz (sfc \(sfc))"
        case .inTransition:
            return "\(plug): in transition (sfc \(sfc)) — retry"
        case .notImplemented:
            return "\(plug): NOT IMPLEMENTED"
        case .rejected:
            return "\(plug): REJECTED"
        case .mismatched:
            return "\(plug): reply did not echo the request"
        case .malformed:
            return "\(plug): malformed reply"
        }
    }
}
