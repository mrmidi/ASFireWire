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

    let endpointId: AudioEndpointID
    let deviceInstanceId: DeviceInstanceID
    let observedGuid: UInt64
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
    // Wire v5 bring-up attribution. Read as a combination: all-zero means no
    // packet reached the consumer at all; packetsSeen == noDataPackets means the
    // device really is sending only CIP NO-DATA; a non-zero reject counter means
    // ASFW rejected packets the device did send (geometryMismatch in particular
    // means our profile and the device disagree on channels/DBS). Wire v5 retains
    // explicit status-only/zero-length completion attribution at the tail.
    let rxPacketsSeen: UInt64
    let rxDataPackets: UInt64
    let rxNoDataPackets: UInt64
    let rxShortPackets: UInt64
    let rxInvalidCipHeaders: UInt64
    let rxZeroDataBlockSize: UInt64
    let rxGeometryMismatch: UInt64
    // Wire v5 TX content ownership. All values are copied by the driver while
    // the endpoint owns its binding; no MCP caller receives a buffer pointer.
    let txPlaybackWriteFrame: UInt64
    let txPlaybackOldestValidFrame: UInt64
    let txContentFinalizedFrameEnd: UInt64
    let txStagingOldestValidFrame: UInt64
    let txStagingWrittenEndFrame: UInt64
    let txTransportCompletionCursor: UInt64
    let txTransportCommittedEnd: UInt64
    let txStagingWrites: UInt64
    let txStagingFrames: UInt64
    let txStagingDiscontinuities: UInt64
    let txStagingOverwrittenFrames: UInt64
    let txStagingReadsReady: UInt64
    let txStagingReadsNotYetWritten: UInt64
    let txStagingReadsStaleOverwritten: UInt64
    let txStagingReadsSnapshotBusy: UInt64
    let txStagingReadsInvalid: UInt64
    let txContentDeferrals: UInt64
    let txContentDeadlineNoData: UInt64
    let txContentStaleXruns: UInt64
    let txContentRebases: UInt64
    let txContentFaultEvents: UInt64
    let txContentFirstFaultPacket: UInt64
    let txContentFirstFaultAudioFrame: UInt64
    let txContentFirstFaultOldestFrame: UInt64
    let txContentFirstFaultWrittenEndFrame: UInt64
    let txContentFirstFaultCompletionCursor: UInt64
    let txContentFirstFaultCommittedEnd: UInt64
    let txContentFirstFaultReason: UInt32
    let txTransportStatus: UInt32
    let rxEmptyCompletions: UInt64

    var id: AudioEndpointID { endpointId }
    var isBindingReady: Bool { (flags & (1 << 0)) != 0 }
    var isStreaming: Bool { (flags & (1 << 1)) != 0 }
    var hasCompletedInterval: Bool { (flags & (1 << 2)) != 0 }
    var hasCompletedRxInterval: Bool { (flags & (1 << 3)) != 0 }
    var isRxCaptureReaderActive: Bool { (flags & (1 << 4)) != 0 }
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
              let data = callStruct(.getAudioTelemetry, initialCap: 8192) else {
            return nil
        }
        return AudioTelemetryWireDecoder.decode(data)
    }
}

enum AudioTelemetryWireDecoder {
    private static let version: UInt16 = 5
    private static let headerBytes = 16
    private static let endpointBytes = 688
    private static let maximumEndpoints = 8
    private static let snapshotBytes = headerBytes + maximumEndpoints * endpointBytes

    static func decode(_ data: Data) -> AudioTelemetrySnapshot? {
        guard data.count == snapshotBytes,
              let wireVersion: UInt16 = data.readInteger(at: 0),
              wireVersion == version,
              let encodedHeaderBytes: UInt16 = data.readInteger(at: 2),
              encodedHeaderBytes == UInt16(headerBytes),
              let encodedByteSize: UInt32 = data.readInteger(at: 4),
              encodedByteSize == UInt32(data.count),
              let endpointCount: UInt32 = data.readInteger(at: 8),
              let encodedEndpointBytes: UInt32 = data.readInteger(at: 12),
              encodedEndpointBytes == UInt32(endpointBytes),
              endpointCount <= maximumEndpoints else {
            return nil
        }

        var endpoints: [AudioTelemetryEndpoint] = []
        endpoints.reserveCapacity(Int(endpointCount))
        for index in 0..<Int(endpointCount) {
            let base = headerBytes + index * endpointBytes
            guard let endpoint = decodeEndpoint(data, base: base) else { return nil }
            endpoints.append(endpoint)
        }
        return AudioTelemetrySnapshot(endpoints: endpoints.sorted { $0.endpointId.rawValue < $1.endpointId.rawValue })
    }

    private static func decodeEndpoint(_ data: Data, base: Int) -> AudioTelemetryEndpoint? {
        func u64(_ offset: Int) -> UInt64? { data.readInteger(at: base + offset) }
        func u32(_ offset: Int) -> UInt32? { data.readInteger(at: base + offset) }
        guard let recordVersion: UInt16 = data.readInteger(at: base),
              recordVersion == version,
              let recordBytes: UInt16 = data.readInteger(at: base + 2),
              recordBytes == UInt16(endpointBytes),
              let reserved = u32(4), reserved == 0,
              let endpointRaw = u64(8), endpointRaw != 0,
              let deviceInstanceRaw = u64(16), deviceInstanceRaw != 0,
              let observedGuid = u64(24),
              let endpointGeneration = u64(32),
              let controlGeneration = u64(40),
              let completedIntervalSequence = u64(48),
              let lastLatency = u64(56),
              let intervalMaxLatency = u64(64),
              let maxLatency = u64(72),
              let wakeCount = u64(80),
              let fast750 = u64(88),
              let late1500 = u64(96),
              let rxReplayEntries = u64(104),
              let rxReplayEpochResets = u64(112),
              let flags = u32(208),
              let sampleRateHz = u32(212),
              let outputChannels = u32(216),
              let inputChannels = u32(220),
              let currentMargin = u32(224),
              let intervalMarginMin = u32(228),
              let intervalMarginMax = u32(232),
              let minimumMargin = u32(236),
              let preparationLead = u32(240),
              let hardwareFloor = u32(244),
              let rxCurrentAvailable = u64(248),
              let rxCompletedIntervalSequence = u64(256),
              let rxIntervalMinimumAvailable = u64(264),
              let rxIntervalMaximumAvailable = u64(272),
              let rxIntervalMinimumFreeHeadroom = u64(280),
              let rxIntervalOverrunEvents = u64(288),
              let rxIntervalOverwrittenFrames = u64(296),
              let rxIntervalStarvationEvents = u64(304),
              let rxIntervalStarvedFrames = u64(312),
              let rxOverrunEvents = u64(320),
              let rxStarvationEvents = u64(328),
              let rxTotalOverwrittenFrames = u64(336),
              let rxTotalStarvedFrames = u64(344),
              let inputFrameCapacityFrames = u32(392),
              let rxPacketsSeen = u64(400),
              let rxDataPackets = u64(408),
              let rxNoDataPackets = u64(416),
              let rxShortPackets = u64(424),
              let rxInvalidCipHeaders = u64(432),
              let rxZeroDataBlockSize = u64(440),
              let rxGeometryMismatch = u64(448),
              let txPlaybackWriteFrame = u64(456),
              let txPlaybackOldestValidFrame = u64(464),
              let txContentFinalizedFrameEnd = u64(472),
              let txStagingOldestValidFrame = u64(480),
              let txStagingWrittenEndFrame = u64(488),
              let txTransportCompletionCursor = u64(496),
              let txTransportCommittedEnd = u64(504),
              let txStagingWrites = u64(512),
              let txStagingFrames = u64(520),
              let txStagingDiscontinuities = u64(528),
              let txStagingOverwrittenFrames = u64(536),
              let txStagingReadsReady = u64(544),
              let txStagingReadsNotYetWritten = u64(552),
              let txStagingReadsStaleOverwritten = u64(560),
              let txStagingReadsSnapshotBusy = u64(568),
              let txStagingReadsInvalid = u64(576),
              let txContentDeferrals = u64(584),
              let txContentDeadlineNoData = u64(592),
              let txContentStaleXruns = u64(600),
              let txContentRebases = u64(608),
              let txContentFaultEvents = u64(616),
              let txContentFirstFaultPacket = u64(624),
              let txContentFirstFaultAudioFrame = u64(632),
              let txContentFirstFaultOldestFrame = u64(640),
              let txContentFirstFaultWrittenEndFrame = u64(648),
              let txContentFirstFaultCompletionCursor = u64(656),
              let txContentFirstFaultCommittedEnd = u64(664),
              let txContentFirstFaultReason = u32(672),
              let txTransportStatus = u32(676),
              let rxEmptyCompletions = u64(680) else {
            return nil
        }
        let latencyHistogram = (0..<6).compactMap { u64(120 + $0 * 8) }
        let marginHistogram = (0..<5).compactMap { u64(168 + $0 * 8) }
        let rxOccupancyHistogram = (0..<5).compactMap { u64(352 + $0 * 8) }
        guard latencyHistogram.count == 6,
              marginHistogram.count == 5,
              rxOccupancyHistogram.count == 5 else { return nil }
        return AudioTelemetryEndpoint(
            endpointId: AudioEndpointID(rawValue: endpointRaw),
            deviceInstanceId: DeviceInstanceID(rawValue: deviceInstanceRaw),
            observedGuid: observedGuid,
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
            inputFrameCapacityFrames: inputFrameCapacityFrames,
            rxPacketsSeen: rxPacketsSeen,
            rxDataPackets: rxDataPackets,
            rxNoDataPackets: rxNoDataPackets,
            rxShortPackets: rxShortPackets,
            rxInvalidCipHeaders: rxInvalidCipHeaders,
            rxZeroDataBlockSize: rxZeroDataBlockSize,
            rxGeometryMismatch: rxGeometryMismatch,
            txPlaybackWriteFrame: txPlaybackWriteFrame,
            txPlaybackOldestValidFrame: txPlaybackOldestValidFrame,
            txContentFinalizedFrameEnd: txContentFinalizedFrameEnd,
            txStagingOldestValidFrame: txStagingOldestValidFrame,
            txStagingWrittenEndFrame: txStagingWrittenEndFrame,
            txTransportCompletionCursor: txTransportCompletionCursor,
            txTransportCommittedEnd: txTransportCommittedEnd,
            txStagingWrites: txStagingWrites,
            txStagingFrames: txStagingFrames,
            txStagingDiscontinuities: txStagingDiscontinuities,
            txStagingOverwrittenFrames: txStagingOverwrittenFrames,
            txStagingReadsReady: txStagingReadsReady,
            txStagingReadsNotYetWritten: txStagingReadsNotYetWritten,
            txStagingReadsStaleOverwritten: txStagingReadsStaleOverwritten,
            txStagingReadsSnapshotBusy: txStagingReadsSnapshotBusy,
            txStagingReadsInvalid: txStagingReadsInvalid,
            txContentDeferrals: txContentDeferrals,
            txContentDeadlineNoData: txContentDeadlineNoData,
            txContentStaleXruns: txContentStaleXruns,
            txContentRebases: txContentRebases,
            txContentFaultEvents: txContentFaultEvents,
            txContentFirstFaultPacket: txContentFirstFaultPacket,
            txContentFirstFaultAudioFrame: txContentFirstFaultAudioFrame,
            txContentFirstFaultOldestFrame: txContentFirstFaultOldestFrame,
            txContentFirstFaultWrittenEndFrame: txContentFirstFaultWrittenEndFrame,
            txContentFirstFaultCompletionCursor: txContentFirstFaultCompletionCursor,
            txContentFirstFaultCommittedEnd: txContentFirstFaultCommittedEnd,
            txContentFirstFaultReason: txContentFirstFaultReason,
            txTransportStatus: txTransportStatus,
            rxEmptyCompletions: rxEmptyCompletions
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
