import Foundation

nonisolated struct ASFWAudioStreamMetricsTX: Equatable, Sendable {
    let clientWriteEndFrame: UInt64
    let consumedEndFrame: UInt64
    let outputUnderruns: UInt64
    let ringWriteFrame: UInt64
    let ringReadFrame: UInt64
    let ringOldestValidFrame: UInt64
    let ringUnderruns: UInt64
    let ringOverruns: UInt64
    let packets: UInt64
    let dataPackets: UInt64
    let noDataPackets: UInt64
    let silenceSubstitutions: UInt64
    let pcmFramesEncoded: UInt64
    let pcmNonzeroPackets: UInt64
    let pcmAllZeroPackets: UInt64
    let scheduledSampleFrame: UInt64
    let completedSampleFrame: UInt64
    let replayEntries: UInt64
    let replayUnderflows: UInt64
    let replayInvalidSyt: UInt64
    let minimumPreparationDistance: UInt32
    let minimumCommittedMarginPackets: UInt32
    let transferDelayTicks: UInt32
}

nonisolated struct ASFWAudioStreamMetricsRX: Equatable, Sendable {
    let clientReadEndFrame: UInt64
    let producedEndFrame: UInt64
    let inputOverruns: UInt64
    let dbcFrameCount: UInt64
    let ringWriteFrame: UInt64
    let ringReadFrame: UInt64
    let ringOverruns: UInt64
    let ringStarvations: UInt64
    let packets: UInt64
    let decodedFrames: UInt64
    let discontinuities: UInt64
    let replayEntries: UInt64
    let replayEpochResets: UInt64
    let ztsRxAdkPublished: UInt64
    let transferDelayTicks: UInt32
}

nonisolated struct ASFWAudioStreamMetricsSnapshot: Equatable, Sendable {
    static let configAvailableFlag: UInt32 = 1 << 0
    static let controlAvailableFlag: UInt32 = 1 << 1
    static let streamingFlag: UInt32 = 1 << 2
    static let consistentFlag: UInt32 = 1 << 3

    let status: UInt32
    let stateFlags: UInt32
    let guid: UInt64
    let timestampNs: UInt64
    let endpointGeneration: UInt64
    let streamGeneration: UInt64
    let sampleRateHz: UInt32
    let outputChannels: UInt32
    let inputChannels: UInt32
    let ioCallbackGeneration: UInt64
    let ioCallbackErrorGeneration: UInt64
    let ioLastError: UInt32
    let fatalGeneration: UInt64
    let fatalReason: UInt32
    let discontinuities: UInt64
    let tx: ASFWAudioStreamMetricsTX
    let rx: ASFWAudioStreamMetricsRX

    var configAvailable: Bool { stateFlags & Self.configAvailableFlag != 0 }
    var controlAvailable: Bool { stateFlags & Self.controlAvailableFlag != 0 }
    var streaming: Bool { stateFlags & Self.streamingFlag != 0 }
    var consistent: Bool { stateFlags & Self.consistentFlag != 0 }
}

extension ASFWDriverConnector {
    private enum AudioStreamMetricsWire {
        // 1013 is getAudioTelemetry. Must match kMethodDiagGetAudioStreamMetrics
        // in ASFWDriverUserClient.cpp — a stale number here reads the wrong struct.
        static let selector: UInt32 = 1015
    }

    func audioStreamMetricsSnapshot(guid: UInt64) -> ASFWAudioStreamMetricsSnapshot? {
        var request = ASFWAudioStreamMetricsRequestV1()
        request.abiVersion = UInt32(ASFW_AUDIO_STREAM_METRICS_ABI_VERSION)
        request.structSize = UInt32(MemoryLayout.size(ofValue: request))
        request.guid = guid
        let input = withUnsafeBytes(of: &request) { Data($0) }

        guard let data = transport.callStruct(
            selector: AudioStreamMetricsWire.selector,
            input: input,
            initialCap: MemoryLayout<ASFWAudioStreamMetricsSnapshotV1>.size
        ) else {
            return nil
        }
        return Self.decodeAudioStreamMetricsSnapshot(data)
    }

    static func decodeAudioStreamMetricsSnapshot(
        _ data: Data
    ) -> ASFWAudioStreamMetricsSnapshot? {
        var wire = ASFWAudioStreamMetricsSnapshotV1()
        let wireSize = MemoryLayout.size(ofValue: wire)
        guard data.count >= wireSize else { return nil }
        withUnsafeMutableBytes(of: &wire) { destination in
            _ = data.copyBytes(to: destination)
        }
        guard wire.abiVersion == UInt32(ASFW_AUDIO_STREAM_METRICS_ABI_VERSION),
              wire.structSize >= UInt32(wireSize),
              wire.guid != 0 else {
            return nil
        }

        return ASFWAudioStreamMetricsSnapshot(
            status: wire.status,
            stateFlags: wire.stateFlags,
            guid: wire.guid,
            timestampNs: wire.timestampNs,
            endpointGeneration: wire.endpointGeneration,
            streamGeneration: wire.streamGeneration,
            sampleRateHz: wire.sampleRateHz,
            outputChannels: wire.outputChannels,
            inputChannels: wire.inputChannels,
            ioCallbackGeneration: wire.ioCallbackGeneration,
            ioCallbackErrorGeneration: wire.ioCallbackErrorGeneration,
            ioLastError: wire.ioLastError,
            fatalGeneration: wire.fatalGeneration,
            fatalReason: wire.fatalReason,
            discontinuities: wire.discontinuities,
            tx: ASFWAudioStreamMetricsTX(
                clientWriteEndFrame: wire.outputClientWriteEndFrame,
                consumedEndFrame: wire.outputConsumedEndFrame,
                outputUnderruns: wire.outputUnderruns,
                ringWriteFrame: wire.playbackRingWriteFrame,
                ringReadFrame: wire.playbackRingReadFrame,
                ringOldestValidFrame: wire.playbackRingOldestValidFrame,
                ringUnderruns: wire.playbackRingUnderruns,
                ringOverruns: wire.playbackRingOverruns,
                packets: wire.txPackets,
                dataPackets: wire.txDataPackets,
                noDataPackets: wire.txNoDataPackets,
                silenceSubstitutions: wire.txSilenceSubstitutions,
                pcmFramesEncoded: wire.txPcmFramesEncoded,
                pcmNonzeroPackets: wire.txPcmNonzeroPackets,
                pcmAllZeroPackets: wire.txPcmAllZeroPackets,
                scheduledSampleFrame: wire.txScheduledSampleFrame,
                completedSampleFrame: wire.txCompletedSampleFrame,
                replayEntries: wire.txReplayEntries,
                replayUnderflows: wire.txReplayUnderflows,
                replayInvalidSyt: wire.txReplayInvalidSyt,
                minimumPreparationDistance: wire.txMinimumPreparationDistance,
                minimumCommittedMarginPackets: wire.txMinimumCommittedMarginPackets,
                transferDelayTicks: wire.txTransferDelayTicks
            ),
            rx: ASFWAudioStreamMetricsRX(
                clientReadEndFrame: wire.inputClientReadEndFrame,
                producedEndFrame: wire.inputProducedEndFrame,
                inputOverruns: wire.inputOverruns,
                dbcFrameCount: wire.rxDbcFrameCount,
                ringWriteFrame: wire.captureRingWriteFrame,
                ringReadFrame: wire.captureRingReadFrame,
                ringOverruns: wire.captureRingOverruns,
                ringStarvations: wire.captureRingStarvations,
                packets: wire.rxPackets,
                decodedFrames: wire.rxDecodedFrames,
                discontinuities: wire.rxDiscontinuities,
                replayEntries: wire.rxReplayEntries,
                replayEpochResets: wire.rxReplayEpochResets,
                ztsRxAdkPublished: wire.ztsRxAdkPublished,
                transferDelayTicks: wire.rxTransferDelayTicks
            )
        )
    }
}
