//
//  DriverConnector+AudioTelemetry.swift
//  ASFW
//
//  Typed decoding for the read-only AudioTelemetrySnapshot wire contract.
//

import Foundation

struct AudioTelemetryEndpoint: Identifiable, Equatable {
    static let latencyBucketLabels = ["<250 µs", "250–500 µs", "500–750 µs", "750–1000 µs", "1–1.5 ms", "≥1.5 ms"]
    static let marginBucketLabels = ["<2× floor", "2–4×", "4–8×", "8–16×", "≥16×"]
    static let rxOccupancyBucketLabels = ["0–20%", "20–40%", "40–60%", "60–80%", "80–100%"]
    static let notMeasured = UInt32.max
    static let notMeasuredFrames = UInt64.max

    let guid: UInt64
    let endpointGeneration: UInt64
    let controlGeneration: UInt64
    let completedIntervalSequence: UInt64
    let lastPreparationLatencyTicks: UInt64
    let completedIntervalMaxLatencyTicks: UInt64
    let maxPreparationLatencyTicks: UInt64
    let preparationWakeCount: UInt64
    let preparationAtMost750Us: UInt64
    let preparationAtLeast1500Us: UInt64
    let rxReplayEntries: UInt64
    let rxReplayEpochResets: UInt64
    let completedLatencyHistogram: [UInt64]
    let completedMarginHistogram: [UInt64]
    let flags: UInt32
    let sampleRateHz: UInt32
    let outputChannels: UInt32
    let inputChannels: UInt32
    let currentCommittedMarginPackets: UInt32
    let completedIntervalMarginMinPackets: UInt32
    let completedIntervalMarginMaxPackets: UInt32
    let minimumCommittedMarginPackets: UInt32
    let preparationLeadPackets: UInt32
    let hardwareFloorPackets: UInt32
    let rxCurrentAvailableFrames: UInt64
    let rxCompletedIntervalSequence: UInt64
    let rxCompletedIntervalMinimumAvailableFrames: UInt64
    let rxCompletedIntervalMaximumAvailableFrames: UInt64
    let rxCompletedIntervalMinimumFreeHeadroomFrames: UInt64
    let rxCompletedIntervalOverrunEvents: UInt64
    let rxCompletedIntervalOverwrittenFrames: UInt64
    let rxCompletedIntervalStarvationEvents: UInt64
    let rxCompletedIntervalStarvedFrames: UInt64
    let rxCaptureOverrunEvents: UInt64
    let rxCaptureStarvationEvents: UInt64
    let rxTotalOverwrittenFrames: UInt64
    let rxTotalStarvedFrames: UInt64
    let rxCompletedOccupancyHistogram: [UInt64]
    let inputFrameCapacityFrames: UInt32

    var id: UInt64 { guid }
    var isStreaming: Bool { (flags & (1 << 1)) != 0 }
    var hasCompletedInterval: Bool { (flags & (1 << 2)) != 0 }
    var hasCompletedRxInterval: Bool { (flags & (1 << 3)) != 0 }
    var intervalMinimum: UInt32? {
        completedIntervalMarginMinPackets == Self.notMeasured ? nil : completedIntervalMarginMinPackets
    }
    var lifetimeMinimum: UInt32? {
        minimumCommittedMarginPackets == Self.notMeasured ? nil : minimumCommittedMarginPackets
    }
    var rxIntervalMinimumAvailable: UInt64? {
        rxCompletedIntervalMinimumAvailableFrames == Self.notMeasuredFrames ? nil : rxCompletedIntervalMinimumAvailableFrames
    }
    var rxIntervalMinimumFreeHeadroom: UInt64? {
        rxCompletedIntervalMinimumFreeHeadroomFrames == Self.notMeasuredFrames ? nil : rxCompletedIntervalMinimumFreeHeadroomFrames
    }
}

struct AudioTelemetrySnapshot {
    let endpoints: [AudioTelemetryEndpoint]
}

extension ASFWDriverConnector {
    func getAudioTelemetry() -> AudioTelemetrySnapshot? {
        guard isConnected, connection != 0,
              let data = callStruct(.getAudioTelemetry, initialCap: 4096) else {
            return nil
        }
        return AudioTelemetryWireDecoder.decode(data)
    }
}

private enum AudioTelemetryWireDecoder {
    private static let version = 2
    private static let headerBytes = 8
    private static let endpointBytes = 376
    private static let maximumEndpoints = 8

    static func decode(_ data: Data) -> AudioTelemetrySnapshot? {
        guard data.count >= headerBytes,
              let wireVersion: UInt32 = data.readInteger(at: 0),
              wireVersion == version,
              let endpointCount: UInt32 = data.readInteger(at: 4),
              endpointCount <= maximumEndpoints,
              data.count >= headerBytes + Int(endpointCount) * endpointBytes else {
            return nil
        }

        let endpoints = (0..<Int(endpointCount)).compactMap { index -> AudioTelemetryEndpoint? in
            let base = headerBytes + index * endpointBytes
            return decodeEndpoint(data, base: base)
        }
        return AudioTelemetrySnapshot(endpoints: endpoints.sorted { $0.guid < $1.guid })
    }

    private static func decodeEndpoint(_ data: Data, base: Int) -> AudioTelemetryEndpoint? {
        func u64(_ offset: Int) -> UInt64? { data.readInteger(at: base + offset) }
        func u32(_ offset: Int) -> UInt32? { data.readInteger(at: base + offset) }
        guard let guid = u64(0),
              let endpointGeneration = u64(8),
              let controlGeneration = u64(16),
              let completedIntervalSequence = u64(24),
              let lastLatency = u64(32),
              let intervalMaxLatency = u64(40),
              let maxLatency = u64(48),
              let wakeCount = u64(56),
              let fast750 = u64(64),
              let late1500 = u64(72),
              let rxReplayEntries = u64(80),
              let rxReplayEpochResets = u64(88),
              let flags = u32(184),
              let sampleRateHz = u32(188),
              let outputChannels = u32(192),
              let inputChannels = u32(196),
              let currentMargin = u32(200),
              let intervalMarginMin = u32(204),
              let intervalMarginMax = u32(208),
              let minimumMargin = u32(212),
              let preparationLead = u32(216),
              let hardwareFloor = u32(220),
              let rxCurrentAvailable = u64(224),
              let rxCompletedIntervalSequence = u64(232),
              let rxIntervalMinimumAvailable = u64(240),
              let rxIntervalMaximumAvailable = u64(248),
              let rxIntervalMinimumFreeHeadroom = u64(256),
              let rxIntervalOverrunEvents = u64(264),
              let rxIntervalOverwrittenFrames = u64(272),
              let rxIntervalStarvationEvents = u64(280),
              let rxIntervalStarvedFrames = u64(288),
              let rxOverrunEvents = u64(296),
              let rxStarvationEvents = u64(304),
              let rxTotalOverwrittenFrames = u64(312),
              let rxTotalStarvedFrames = u64(320),
              let inputFrameCapacityFrames = u32(368) else {
            return nil
        }
        let latencyHistogram = (0..<6).compactMap { u64(96 + $0 * 8) }
        let marginHistogram = (0..<5).compactMap { u64(144 + $0 * 8) }
        let rxOccupancyHistogram = (0..<5).compactMap { u64(328 + $0 * 8) }
        guard latencyHistogram.count == 6,
              marginHistogram.count == 5,
              rxOccupancyHistogram.count == 5 else { return nil }
        return AudioTelemetryEndpoint(
            guid: guid,
            endpointGeneration: endpointGeneration,
            controlGeneration: controlGeneration,
            completedIntervalSequence: completedIntervalSequence,
            lastPreparationLatencyTicks: lastLatency,
            completedIntervalMaxLatencyTicks: intervalMaxLatency,
            maxPreparationLatencyTicks: maxLatency,
            preparationWakeCount: wakeCount,
            preparationAtMost750Us: fast750,
            preparationAtLeast1500Us: late1500,
            rxReplayEntries: rxReplayEntries,
            rxReplayEpochResets: rxReplayEpochResets,
            completedLatencyHistogram: latencyHistogram,
            completedMarginHistogram: marginHistogram,
            flags: flags,
            sampleRateHz: sampleRateHz,
            outputChannels: outputChannels,
            inputChannels: inputChannels,
            currentCommittedMarginPackets: currentMargin,
            completedIntervalMarginMinPackets: intervalMarginMin,
            completedIntervalMarginMaxPackets: intervalMarginMax,
            minimumCommittedMarginPackets: minimumMargin,
            preparationLeadPackets: preparationLead,
            hardwareFloorPackets: hardwareFloor,
            rxCurrentAvailableFrames: rxCurrentAvailable,
            rxCompletedIntervalSequence: rxCompletedIntervalSequence,
            rxCompletedIntervalMinimumAvailableFrames: rxIntervalMinimumAvailable,
            rxCompletedIntervalMaximumAvailableFrames: rxIntervalMaximumAvailable,
            rxCompletedIntervalMinimumFreeHeadroomFrames: rxIntervalMinimumFreeHeadroom,
            rxCompletedIntervalOverrunEvents: rxIntervalOverrunEvents,
            rxCompletedIntervalOverwrittenFrames: rxIntervalOverwrittenFrames,
            rxCompletedIntervalStarvationEvents: rxIntervalStarvationEvents,
            rxCompletedIntervalStarvedFrames: rxIntervalStarvedFrames,
            rxCaptureOverrunEvents: rxOverrunEvents,
            rxCaptureStarvationEvents: rxStarvationEvents,
            rxTotalOverwrittenFrames: rxTotalOverwrittenFrames,
            rxTotalStarvedFrames: rxTotalStarvedFrames,
            rxCompletedOccupancyHistogram: rxOccupancyHistogram,
            inputFrameCapacityFrames: inputFrameCapacityFrames
        )
    }
}

private extension Data {
    func readInteger<T: FixedWidthInteger>(at offset: Int) -> T? {
        guard offset >= 0, count >= offset + MemoryLayout<T>.size else { return nil }
        return withUnsafeBytes { bytes in
            bytes.loadUnaligned(fromByteOffset: offset, as: T.self)
        }
    }
}
