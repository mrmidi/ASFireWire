import Foundation

// Audio stream health: read-only projection of the driver's per-endpoint RX
// bring-up attribution and TX cursor ownership (AudioTelemetrySnapshot v6).
//
// This exists because a stream that never establishes used to be one
// indistinguishable silence. Every RX outcome now lands in exactly one counter,
// so "the device only sends CIP NO-DATA", "we rejected everything it sent" and
// "nothing arrived at all" can be told apart from the control plane, without a
// packet analyser and without adding logging to the isochronous hot path.
//
// No transaction is issued: this reads counters the driver already maintains.

extension ASFWMCPToolCatalog {
    static let audioStreamTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(name: "asfw_get_audio_stream_health", group: "audio_streams", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Per-endpoint RX bring-up attribution: what the device sent and what we did with it. No transaction."),
        ASFWMCPToolDefinition(name: "asfw_get_audio_cursors", group: "audio_streams", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Value-owned TX PCM-publication, planned-frame, and transport cursor snapshots with first-fault attribution. No buffer pointer or transaction.")
    ]
}

/// One endpoint's RX attribution, plus the verdict derived from it.
struct ASFWMCPAudioStreamHealth: Equatable {
    let endpointId: AudioEndpointID
    let deviceInstanceId: DeviceInstanceID
    let observedGuid: UInt64
    let bindingReady: Bool
    let streaming: Bool
    let sampleRateHz: UInt32
    let inputChannels: UInt32
    let outputChannels: UInt32

    let packetsSeen: UInt64
    let dataPackets: UInt64
    let noDataPackets: UInt64
    let emptyCompletions: UInt64
    let shortPackets: UInt64
    let invalidCipHeaders: UInt64
    let zeroDataBlockSize: UInt64
    let geometryMismatch: UInt64
    let replayEntries: UInt64
    let replayEpochResets: UInt64

    // Capture-ring delivery. Every counter above answers "did packets decode?".
    // These answer "did the decoded audio reach the reader?" — the seam where a
    // fully silent capture previously reported nothing but green.
    let captureReaderActive: Bool
    let hasCompletedCaptureInterval: Bool
    let captureAvailableFrames: UInt64
    let captureCapacityFrames: UInt32
    let captureStarvationEvents: UInt64
    let captureTotalStarvedFrames: UInt64
    let captureIntervalStarvationEvents: UInt64
    let captureIntervalStarvedFrames: UInt64
    let captureOverrunEvents: UInt64

    // Transmit. Every field above describes the receive path, which is why a
    // fatally stopped transmit context previously read as `receivingData`: RX
    // was genuinely fine and nothing here asked about TX. `IsochTxQueueStatus`,
    // mirrored by the driver on every preparation pass.
    let txTransportStatus: UInt32

    var txTransportStatusName: String {
        switch txTransportStatus {
        case 0: return "stopped"
        case 1: return "running"
        case 2: return "producerFault"
        case 3: return "deadContext"
        case 4: return "transportProgressStall"
        default: return "unknown"
        }
    }

    /// A transmit context that is not running while the endpoint is streaming.
    /// Kept separate from the RX verdict rather than folded into it: the two
    /// directions fail independently, and collapsing them would reintroduce the
    /// case this exists to catch.
    var txFaulted: Bool {
        streaming && txTransportStatus != 1
    }

    var rejectedPackets: UInt64 {
        emptyCompletions &+ shortPackets &+ invalidCipHeaders &+
            zeroDataBlockSize &+ geometryMismatch
    }

    /// Stable machine-readable cause, so callers do not re-derive the rules.
    ///
    /// Deliberately conservative: it names what the counters prove, never why.
    /// `deviceSendsOnlyNoData` says the device sent nothing but CIP NO-DATA — it
    /// does NOT say the device is waiting on us.
    var verdict: String {
        if !bindingReady {
            return "bindingNotReady"
        }
        // Asked before any receive counter. On 2026-09-06 an IT FATAL STOP left
        // the transmit context stopped for minutes while RX kept decoding
        // perfectly, and this tool answered `receivingData` the whole time --
        // which is true of the receive path and worthless as a health verdict.
        if txFaulted {
            return "transmitNotRunning"
        }
        if packetsSeen == 0 {
            return "noPacketsReceived"
        }
        if geometryMismatch > 0 {
            return "geometryMismatch"
        }
        if rejectedPackets > 0 {
            return "packetsRejected"
        }
        if dataPackets == 0 && noDataPackets > 0 {
            return "deviceSendsOnlyNoData"
        }
        if dataPackets > 0 && replayEntries == 0 {
            return "dataNotAccepted"
        }
        // Cross-layer invariant. Each layer below reports its own job succeeding,
        // so "packets decoded" and "audio reached CoreAudio" have to be asked
        // separately: an RX write cursor that shares no origin with the HAL's
        // read cursor zero-fills every frame while all the counters above stay
        // perfect. Only claimed when a reader is actually active and a full
        // interval has completed, so warm-up and idle never trip it.
        if captureReaderActive && hasCompletedCaptureInterval &&
            captureIntervalStarvedFrames > 0 {
            return "framesNotReachingReader"
        }
        return "receivingData"
    }

    var explanation: String {
        switch verdict {
        case "bindingNotReady":
            return "The audio endpoint is registered, but its value-owned telemetry binding is not complete. Inspect endpoint memory/configuration setup before interpreting zero stream counters."
        case "noPacketsReceived":
            return "No isochronous packet reached the audio consumer. The IR context is not delivering: check that the channel matches the device's TX ISOC register, that the context started, and that the device stream is enabled."
        case "geometryMismatch":
            return "Packets arrived with a data block shape our stream config does not accept (channels / DBS / AM824 slots). The profile and the device disagree; this is a host-side rejection, not a silent device."
        case "packetsRejected":
            return "A receive cycle completed without a decodable audio packet (status-only/zero-length completion, runt packet, undecodable CIP header, or zero data block size). The counters identify a host-side receive/replay discontinuity; inspect the individual rejection fields."
        case "deviceSendsOnlyNoData":
            return "Valid CIP headers arrived carrying SYT 0xFFFF and no audio frames. The device is in NO-DATA. This states what the device sent; it is not evidence about what the device is waiting for."
        case "dataNotAccepted":
            return "Data-bearing packets with valid SYT arrived but no replay entry was published. Inspect the SYT cadence detector rather than the device."
        case "transmitNotRunning":
            return "The endpoint is streaming but its transmit context is not running (\(txTransportStatusName)). Receive counters below may look perfectly healthy and say nothing about this: the two directions fail independently. Check the Isoch ring for IT FATAL STOP and for TX-IRQ-001 interrupt silence."
        case "framesNotReachingReader":
            return "Packets are decoding and being accepted, but the reader is being zero-filled: decoded audio is not landing where CoreAudio reads. Compare the RX write coordinate against the hardware-derived HAL sample coordinate before suspecting the device or the wire."
        default:
            return "Data-bearing packets are arriving and being accepted."
        }
    }

    func mcpValue() -> ASFWMCPValue {
        .object([
            "endpointId": .uint64(endpointId.rawValue),
            "deviceInstanceId": .uint64(deviceInstanceId.rawValue),
            "observedGuid": .string(String(format: "0x%016llX", observedGuid)),
            "bindingReady": .bool(bindingReady),
            "streaming": .bool(streaming),
            "sampleRateHz": .int(Int(sampleRateHz)),
            "inputChannels": .int(Int(inputChannels)),
            "outputChannels": .int(Int(outputChannels)),
            "verdict": .string(verdict),
            "explanation": .string(explanation),
            "transmit": .object([
                "status": .string(txTransportStatusName),
                "faulted": .bool(txFaulted)
            ]),
            "counters": .object([
                "packetsSeen": .uint64(packetsSeen),
                "dataPackets": .uint64(dataPackets),
                "noDataPackets": .uint64(noDataPackets),
                "emptyCompletions": .uint64(emptyCompletions),
                "shortPackets": .uint64(shortPackets),
                "invalidCipHeaders": .uint64(invalidCipHeaders),
                "zeroDataBlockSize": .uint64(zeroDataBlockSize),
                "geometryMismatch": .uint64(geometryMismatch),
                "rejectedPackets": .uint64(rejectedPackets),
                "replayEntries": .uint64(replayEntries),
                "replayEpochResets": .uint64(replayEpochResets)
            ]),
            "capture": .object([
                "readerActive": .bool(captureReaderActive),
                "hasCompletedInterval": .bool(hasCompletedCaptureInterval),
                "availableFrames": .uint64(captureAvailableFrames),
                "capacityFrames": .int(Int(captureCapacityFrames)),
                "starvationEvents": .uint64(captureStarvationEvents),
                "totalStarvedFrames": .uint64(captureTotalStarvedFrames),
                "intervalStarvationEvents": .uint64(captureIntervalStarvationEvents),
                "intervalStarvedFrames": .uint64(captureIntervalStarvedFrames),
                "overrunEvents": .uint64(captureOverrunEvents)
            ])
        ])
    }
}

extension AudioTelemetryEndpoint {
    var mcpStreamHealth: ASFWMCPAudioStreamHealth {
        ASFWMCPAudioStreamHealth(
            endpointId: endpointId,
            deviceInstanceId: deviceInstanceId,
            observedGuid: observedGuid,
            bindingReady: isBindingReady,
            streaming: isStreaming,
            sampleRateHz: sampleRateHz,
            inputChannels: inputChannels,
            outputChannels: outputChannels,
            packetsSeen: rxPacketsSeen,
            dataPackets: rxDataPackets,
            noDataPackets: rxNoDataPackets,
            emptyCompletions: rxEmptyCompletions,
            shortPackets: rxShortPackets,
            invalidCipHeaders: rxInvalidCipHeaders,
            zeroDataBlockSize: rxZeroDataBlockSize,
            geometryMismatch: rxGeometryMismatch,
            replayEntries: rxReplayEntries,
            replayEpochResets: rxReplayEpochResets,
            captureReaderActive: isRxCaptureReaderActive,
            hasCompletedCaptureInterval: hasCompletedRxInterval,
            captureAvailableFrames: rxCurrentAvailableFrames,
            captureCapacityFrames: inputFrameCapacityFrames,
            captureStarvationEvents: rxCaptureStarvationEvents,
            captureTotalStarvedFrames: rxTotalStarvedFrames,
            captureIntervalStarvationEvents: rxCompletedIntervalStarvationEvents,
            captureIntervalStarvedFrames: rxCompletedIntervalStarvedFrames,
            captureOverrunEvents: rxCaptureOverrunEvents,
            txTransportStatus: txTransportStatus
        )
    }
}

/// One value-owned TX ownership snapshot. The three domains intentionally stay
/// separate: CoreAudio publishes immutable frames, audio plans packet content,
/// and transport owns packet completion. Their units are named in the wire
/// response so a caller cannot accidentally subtract frames from packets.
struct ASFWMCPAudioCursorSnapshot: Equatable {
    let endpointId: AudioEndpointID
    let deviceInstanceId: DeviceInstanceID
    let observedGuid: UInt64
    let bindingReady: Bool
    let streaming: Bool
    let sampleRateHz: UInt32
    let outputChannels: UInt32
    let pcmOldestValidFrame: UInt64
    let pcmPublishedEndFrame: UInt64
    let scheduledFrameEnd: UInt64
    let completionPacket: UInt64
    let committedPacketEnd: UInt64
    let transportStatus: UInt32
    let pcmPublications: UInt64
    let pcmFramesPublished: UInt64
    let pcmDiscontinuities: UInt64
    let pcmExpiredFrames: UInt64
    let copiesReady: UInt64
    let copiesNotYetPublished: UInt64
    let copiesExpired: UInt64
    let copiesConcurrentRewrite: UInt64
    let copiesInvalid: UInt64
    let deferrals: UInt64
    let deadlineNoData: UInt64
    let copiesWrongEpoch: UInt64
    let missedFrames: UInt64
    let faultEvents: UInt64
    let firstFaultReason: UInt32
    let firstFaultPacket: UInt64
    let firstFaultAudioFrame: UInt64
    let firstFaultOldestFrame: UInt64
    let firstFaultWrittenEndFrame: UInt64
    let firstFaultCompletionPacket: UInt64
    let firstFaultCommittedPacketEnd: UInt64

    var publishedAheadFrames: UInt64 {
        pcmPublishedEndFrame >= scheduledFrameEnd
            ? pcmPublishedEndFrame - scheduledFrameEnd
            : 0
    }

    var committedMarginPackets: UInt64 {
        committedPacketEnd >= completionPacket
            ? committedPacketEnd - completionPacket
            : 0
    }

    var scheduledRangeExpired: Bool {
        scheduledFrameEnd < pcmOldestValidFrame
    }

    var firstFaultName: String {
        switch firstFaultReason {
        case 0: return "none"
        case 1: return "notYetPublishedAtDeadline"
        case 2: return "expired"
        case 3: return "concurrentRewriteAtDeadline"
        case 4: return "invalidSource"
        case 5: return "secondaryStreamFailure"
        default: return "unknown"
        }
    }

    var transportStatusName: String {
        switch transportStatus {
        case 0: return "stopped"
        case 1: return "running"
        case 2: return "producerFault"
        case 3: return "deadContext"
        case 4: return "transportProgressStall"
        default: return "unknown"
        }
    }

    /// Names only states that the snapshot proves. Published PCM normally leads
    /// the next range selected by the physical presentation planner.
    var verdict: String {
        if !bindingReady { return "bindingNotReady" }
        if !streaming { return "idle" }
        if transportStatus == 2 || transportStatus == 3 || transportStatus == 4 ||
            firstFaultReason == 4 || firstFaultReason == 5 || copiesInvalid > 0 {
            return "fatal"
        }
        if copiesWrongEpoch > 0 {
            return "wrongEpoch"
        }
        if deadlineNoData > 0 {
            return "deadlineNoData"
        }
        if pcmPublishedEndFrame == 0 {
            return "awaitingHostWrite"
        }
        if scheduledRangeExpired {
            return "expiredRange"
        }
        if publishedAheadFrames > 0 {
            return "healthyPendingContent"
        }
        return "healthy"
    }

    func mcpValue() -> ASFWMCPValue {
        .object([
            "endpointId": .uint64(endpointId.rawValue),
            "deviceInstanceId": .uint64(deviceInstanceId.rawValue),
            "observedGuid": .string(String(format: "0x%016llX", observedGuid)),
            "bindingReady": .bool(bindingReady),
            "streaming": .bool(streaming),
            "sampleRateHz": .int(Int(sampleRateHz)),
            "outputChannels": .int(Int(outputChannels)),
            "snapshotKind": .string("valueOwned"),
            "verdict": .string(verdict),
            "frameCursors": .object([
                "units": .string("absoluteHostFrames"),
                "pcmOldestValid": .uint64(pcmOldestValidFrame),
                "pcmPublishedEnd": .uint64(pcmPublishedEndFrame),
                "scheduledFrameEnd": .uint64(scheduledFrameEnd),
                "publishedAheadFrames": .uint64(publishedAheadFrames),
                "scheduledRangeExpired": .bool(scheduledRangeExpired)
            ]),
            "transportCursors": .object([
                "units": .string("absoluteIsochPackets"),
                "completion": .uint64(completionPacket),
                "committedEnd": .uint64(committedPacketEnd),
                "committedMargin": .uint64(committedMarginPackets),
                "status": .string(transportStatusName)
            ]),
            "counters": .object([
                "pcmPublications": .uint64(pcmPublications),
                "pcmFramesPublished": .uint64(pcmFramesPublished),
                "pcmDiscontinuities": .uint64(pcmDiscontinuities),
                "pcmExpiredFrames": .uint64(pcmExpiredFrames),
                "copiesReady": .uint64(copiesReady),
                "copiesNotYetPublished": .uint64(copiesNotYetPublished),
                "copiesExpired": .uint64(copiesExpired),
                "copiesConcurrentRewrite": .uint64(copiesConcurrentRewrite),
                "copiesInvalid": .uint64(copiesInvalid),
                "deferrals": .uint64(deferrals),
                "deadlineNoData": .uint64(deadlineNoData),
                "copiesWrongEpoch": .uint64(copiesWrongEpoch),
                "missedFrames": .uint64(missedFrames),
                "faultEvents": .uint64(faultEvents)
            ]),
            "firstFault": .object([
                "reason": .string(firstFaultName),
                "packet": .uint64(firstFaultPacket),
                "audioFrame": .uint64(firstFaultAudioFrame),
                "pcmOldestValid": .uint64(firstFaultOldestFrame),
                "pcmPublishedEnd": .uint64(firstFaultWrittenEndFrame),
                "completionPacket": .uint64(firstFaultCompletionPacket),
                "committedPacketEnd": .uint64(firstFaultCommittedPacketEnd)
            ])
        ])
    }
}

extension AudioTelemetryEndpoint {
    var mcpAudioCursors: ASFWMCPAudioCursorSnapshot {
        ASFWMCPAudioCursorSnapshot(
            endpointId: endpointId,
            deviceInstanceId: deviceInstanceId,
            observedGuid: observedGuid,
            bindingReady: isBindingReady,
            streaming: isStreaming,
            sampleRateHz: sampleRateHz,
            outputChannels: outputChannels,
            pcmOldestValidFrame: txPcmOldestValidFrame,
            pcmPublishedEndFrame: txPcmPublishedEndFrame,
            scheduledFrameEnd: txScheduledFrameEnd,
            completionPacket: txTransportCompletionCursor,
            committedPacketEnd: txTransportCommittedEnd,
            transportStatus: txTransportStatus,
            pcmPublications: txPcmPublications,
            pcmFramesPublished: txPcmFramesPublished,
            pcmDiscontinuities: txPcmDiscontinuities,
            pcmExpiredFrames: txPcmExpiredFrames,
            copiesReady: txPcmCopiesReady,
            copiesNotYetPublished: txPcmCopiesNotYetPublished,
            copiesExpired: txPcmCopiesExpired,
            copiesConcurrentRewrite: txPcmCopiesConcurrentRewrite,
            copiesInvalid: txPcmCopiesInvalid,
            deferrals: txContentDeferrals,
            deadlineNoData: txContentDeadlineNoData,
            copiesWrongEpoch: txPcmCopiesWrongEpoch,
            missedFrames: txMissedFrames,
            faultEvents: txContentFaultEvents,
            firstFaultReason: txContentFirstFaultReason,
            firstFaultPacket: txContentFirstFaultPacket,
            firstFaultAudioFrame: txContentFirstFaultAudioFrame,
            firstFaultOldestFrame: txContentFirstFaultOldestFrame,
            firstFaultWrittenEndFrame: txContentFirstFaultWrittenEndFrame,
            firstFaultCompletionPacket: txContentFirstFaultCompletionCursor,
            firstFaultCommittedPacketEnd: txContentFirstFaultCommittedEnd
        )
    }
}
