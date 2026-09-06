import Foundation
import IOKit

// MARK: - Wire mirrors
//
// Hand-mirrored from ASFWDriver/UserClient/WireFormats/AudioTuningWireFormats.hpp.
// Field order and padding must match exactly; the sizes are asserted below so a
// divergence is a build-time failure in the app rather than a garbled panel.

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
    var framesPerPacketAverage: UInt32 = 0

    var streaming: UInt32 = 0
    var pendingGroups: UInt32 = 0
    var appliedSequence: UInt32 = 0
    var lastRejection: UInt32 = 0
    var lastWarnings: UInt32 = 0
    var reserved1: UInt32 = 0
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
    static let wireVersion: UInt32 = 1
    static let transmitDepth: UInt32 = 1 << 0
    static let declarations: UInt32 = 1 << 1
    static let halGeometry: UInt32 = 1 << 2
}

/// What applying a request costs. Mirrors `ASFW::Audio::Shared::ApplyCost`, and
/// exists so the panel can warn *before* the operator commits rather than after
/// the device has vanished from every running app.
enum AudioTuningApplyCost {
    case nothing
    case streamRearm
    case deviceRepublish

    var label: String {
        switch self {
        case .nothing: return "No change"
        case .streamRearm: return "Restarts audio streams"
        case .deviceRepublish: return "Republishes the CoreAudio device"
        }
    }

    var isDisruptive: Bool { self != .nothing }
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

    /// Submits a candidate. Success means the driver accepted it and opened a
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
