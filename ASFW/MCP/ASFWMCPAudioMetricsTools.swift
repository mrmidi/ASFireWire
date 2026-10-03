import Foundation

extension ASFWMCPToolCatalog {
    static let audioMetricsTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(
            name: "asfw_audio_stream_snapshot",
            group: "audio",
            visibility: .readOnly,
            readOnly: true,
            idempotent: true,
            summary: "Read one synchronized TX/RX direct-audio metrics snapshot for a device GUID."
        ),
        ASFWMCPToolDefinition(name: "asfw_audio_stream_capture_start", group: "audio", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Start a bounded 2 Hz read-only audio metrics capture for a device GUID."),
        ASFWMCPToolDefinition(name: "asfw_audio_stream_capture_mark", group: "audio", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Record a manual misframe marker with a fresh snapshot and log-ring cursor."),
        ASFWMCPToolDefinition(name: "asfw_audio_stream_capture_status", group: "audio", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return capture-session status without its sample window."),
        ASFWMCPToolDefinition(name: "asfw_audio_stream_capture_stop", group: "audio", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Stop the local read-only audio metrics capture and retain its bounded window."),
        ASFWMCPToolDefinition(name: "asfw_audio_stream_capture_export", group: "audio", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Export retained audio metrics samples and manual markers."),
    ]
}

extension ASFWAudioStreamMetricsSnapshot {
    private var statusName: String {
        switch status {
        case 0: return "ok"
        case 1: return "unavailable"
        case 2: return "busy"
        default: return "unknown"
        }
    }

    var mcpValue: ASFWMCPValue {
        .object([
            "abiVersion": .int(Int(ASFW_AUDIO_STREAM_METRICS_ABI_VERSION)),
            "status": .string(statusName),
            "statusCode": .int(Int(status)),
            "guid": .string(String(format: "0x%016llX", guid)),
            "timestampNs": .uint64(timestampNs),
            "endpointGeneration": .uint64(endpointGeneration),
            "streamGeneration": .uint64(streamGeneration),
            "state": .object([
                "configAvailable": .bool(configAvailable),
                "controlAvailable": .bool(controlAvailable),
                "streaming": .bool(streaming),
                "consistent": .bool(consistent),
                "sampleRateHz": .int(Int(sampleRateHz)),
                "outputChannels": .int(Int(outputChannels)),
                "inputChannels": .int(Int(inputChannels)),
            ]),
            "io": .object([
                "callbackGeneration": .uint64(ioCallbackGeneration),
                "callbackErrorGeneration": .uint64(ioCallbackErrorGeneration),
                "lastError": .int(Int(ioLastError)),
                "fatalGeneration": .uint64(fatalGeneration),
                "fatalReason": .int(Int(fatalReason)),
                "discontinuities": .uint64(discontinuities),
            ]),
            "tx": .object([
                "clientWriteEndFrame": .uint64(tx.clientWriteEndFrame),
                "consumedEndFrame": .uint64(tx.consumedEndFrame),
                "outputUnderruns": .uint64(tx.outputUnderruns),
                "ringWriteFrame": .uint64(tx.ringWriteFrame),
                "ringReadFrame": .uint64(tx.ringReadFrame),
                "ringOldestValidFrame": .uint64(tx.ringOldestValidFrame),
                "ringUnderruns": .uint64(tx.ringUnderruns),
                "ringOverruns": .uint64(tx.ringOverruns),
                "packets": .uint64(tx.packets),
                "dataPackets": .uint64(tx.dataPackets),
                "noDataPackets": .uint64(tx.noDataPackets),
                "silenceSubstitutions": .uint64(tx.silenceSubstitutions),
                "pcmFramesEncoded": .uint64(tx.pcmFramesEncoded),
                "pcmNonzeroPackets": .uint64(tx.pcmNonzeroPackets),
                "pcmAllZeroPackets": .uint64(tx.pcmAllZeroPackets),
                "scheduledSampleFrame": .uint64(tx.scheduledSampleFrame),
                "completedSampleFrame": .uint64(tx.completedSampleFrame),
                "replayEntries": .uint64(tx.replayEntries),
                "replayUnderflows": .uint64(tx.replayUnderflows),
                "replayInvalidSyt": .uint64(tx.replayInvalidSyt),
                "minimumPreparationDistance": .uint64(
                    UInt64(tx.minimumPreparationDistance)),
                "minimumCommittedMarginPackets": .uint64(
                    UInt64(tx.minimumCommittedMarginPackets)),
                "transferDelayTicks": .uint64(UInt64(tx.transferDelayTicks)),
            ]),
            "rx": .object([
                "clientReadEndFrame": .uint64(rx.clientReadEndFrame),
                "producedEndFrame": .uint64(rx.producedEndFrame),
                "inputOverruns": .uint64(rx.inputOverruns),
                "dbcFrameCount": .uint64(rx.dbcFrameCount),
                "ringWriteFrame": .uint64(rx.ringWriteFrame),
                "ringReadFrame": .uint64(rx.ringReadFrame),
                "ringOverruns": .uint64(rx.ringOverruns),
                "ringStarvations": .uint64(rx.ringStarvations),
                "packets": .uint64(rx.packets),
                "decodedFrames": .uint64(rx.decodedFrames),
                "discontinuities": .uint64(rx.discontinuities),
                "replayEntries": .uint64(rx.replayEntries),
                "replayEpochResets": .uint64(rx.replayEpochResets),
                "ztsRxAdkPublished": .uint64(rx.ztsRxAdkPublished),
                "transferDelayTicks": .uint64(UInt64(rx.transferDelayTicks)),
            ]),
        ])
    }

    // Explicitly `nonisolated`, because the ASFW target sets SWIFT_DEFAULT_ACTOR_ISOLATION:
    // MainActor. Without it the factory inherits MainActor isolation although it touches
    // none of its state: it only builds a value of a type that is itself `nonisolated` and
    // `Sendable`. That false isolation broke Swift 6 mode when called from
    // `actor MockASFWDriverControl`. An `await` at the call site would suggest a suspension
    // that does not exist, and would return with every further caller outside MainActor.
    nonisolated static func mock(guid: UInt64) -> ASFWAudioStreamMetricsSnapshot {
        ASFWAudioStreamMetricsSnapshot(
            status: 0,
            stateFlags: configAvailableFlag | controlAvailableFlag |
                streamingFlag | consistentFlag,
            guid: guid,
            timestampNs: 123_456_789_000,
            endpointGeneration: 4,
            streamGeneration: 9,
            sampleRateHz: 48_000,
            outputChannels: 28,
            inputChannels: 28,
            ioCallbackGeneration: 1_024,
            ioCallbackErrorGeneration: 0,
            ioLastError: 0,
            fatalGeneration: 0,
            fatalReason: 0,
            discontinuities: 0,
            tx: ASFWAudioStreamMetricsTX(
                clientWriteEndFrame: 48_000,
                consumedEndFrame: 47_744,
                outputUnderruns: 0,
                ringWriteFrame: 48_000,
                ringReadFrame: 47_744,
                ringOldestValidFrame: 39_808,
                ringUnderruns: 0,
                ringOverruns: 0,
                packets: 8_000,
                dataPackets: 7_998,
                noDataPackets: 2,
                silenceSubstitutions: 0,
                pcmFramesEncoded: 48_000,
                pcmNonzeroPackets: 7_900,
                pcmAllZeroPackets: 98,
                scheduledSampleFrame: 48_256,
                completedSampleFrame: 47_744,
                replayEntries: 8_000,
                replayUnderflows: 0,
                replayInvalidSyt: 0,
                minimumPreparationDistance: 32,
                minimumCommittedMarginPackets: 16,
                transferDelayTicks: 12_800
            ),
            rx: ASFWAudioStreamMetricsRX(
                clientReadEndFrame: 47_488,
                producedEndFrame: 48_000,
                inputOverruns: 0,
                dbcFrameCount: 48_000,
                ringWriteFrame: 48_000,
                ringReadFrame: 47_488,
                ringOverruns: 0,
                ringStarvations: 0,
                packets: 8_000,
                decodedFrames: 48_000,
                discontinuities: 0,
                replayEntries: 8_000,
                replayEpochResets: 0,
                ztsRxAdkPublished: 1_024,
                transferDelayTicks: 12_800
            )
        )
    }
}
