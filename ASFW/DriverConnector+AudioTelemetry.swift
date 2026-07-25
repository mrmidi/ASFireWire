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
    static let notMeasured = UInt32.max

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

    var id: UInt64 { guid }
    var isStreaming: Bool { (flags & (1 << 1)) != 0 }
    var hasCompletedInterval: Bool { (flags & (1 << 2)) != 0 }
    var intervalMinimum: UInt32? {
        completedIntervalMarginMinPackets == Self.notMeasured ? nil : completedIntervalMarginMinPackets
    }
    var lifetimeMinimum: UInt32? {
        minimumCommittedMarginPackets == Self.notMeasured ? nil : minimumCommittedMarginPackets
    }
}

struct AudioTelemetrySnapshot {
    let endpoints: [AudioTelemetryEndpoint]
}

extension ASFWDriverConnector {
    func getAudioTelemetry() -> AudioTelemetrySnapshot? {
        guard isConnected, connection != 0,
              let data = callStruct(.getAudioTelemetry, initialCap: 2048) else {
            return nil
        }
        return AudioTelemetryWireDecoder.decode(data)
    }
}

private enum AudioTelemetryWireDecoder {
    private static let version = 1
    private static let headerBytes = 8
    private static let endpointBytes = 224
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
              let hardwareFloor = u32(220) else {
            return nil
        }
        let latencyHistogram = (0..<6).compactMap { u64(96 + $0 * 8) }
        let marginHistogram = (0..<5).compactMap { u64(144 + $0 * 8) }
        guard latencyHistogram.count == 6, marginHistogram.count == 5 else { return nil }
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
            hardwareFloorPackets: hardwareFloor
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
