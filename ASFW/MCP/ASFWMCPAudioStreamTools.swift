import Foundation

// Audio stream health: read-only projection of the driver's per-endpoint RX
// bring-up attribution and TX cursor ownership (AudioTelemetrySnapshot v4).
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
        ASFWMCPToolDefinition(name: "asfw_get_audio_cursors", group: "audio_streams", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Value-owned TX staging, finalized-content, and transport cursor snapshots with first-fault attribution. No buffer pointer or transaction.")
    ]
}

/// One endpoint's RX attribution, plus the verdict derived from it.
struct ASFWMCPAudioStreamHealth: Equatable {
    let guid: UInt64
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
        default:
            return "Data-bearing packets are arriving and being accepted."
        }
    }

    func mcpValue() -> ASFWMCPValue {
        .object([
            "guid": .uint64(guid),
            "bindingReady": .bool(bindingReady),
            "streaming": .bool(streaming),
            "sampleRateHz": .int(Int(sampleRateHz)),
            "inputChannels": .int(Int(inputChannels)),
            "outputChannels": .int(Int(outputChannels)),
            "verdict": .string(verdict),
            "explanation": .string(explanation),
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
            ])
        ])
    }
}

extension AudioTelemetryEndpoint {
    var mcpStreamHealth: ASFWMCPAudioStreamHealth {
        ASFWMCPAudioStreamHealth(
            guid: guid,
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
            replayEpochResets: rxReplayEpochResets
        )
    }
}

/// One value-owned TX ownership snapshot. The three domains intentionally stay
/// separate: CoreAudio stages frames, audio finalizes immutable packet content,
/// and transport owns packet completion. Their units are named in the wire
/// response so a caller cannot accidentally subtract frames from packets.
struct ASFWMCPAudioCursorSnapshot: Equatable {
    let guid: UInt64
    let bindingReady: Bool
    let streaming: Bool
    let sampleRateHz: UInt32
    let outputChannels: UInt32
    let stagedOldestFrame: UInt64
    let stagedWrittenEndFrame: UInt64
    let finalizedFrameEnd: UInt64
    let completionPacket: UInt64
    let committedPacketEnd: UInt64
    let transportStatus: UInt32
    let stagingWrites: UInt64
    let stagingFrames: UInt64
    let stagingDiscontinuities: UInt64
    let stagingOverwrittenFrames: UInt64
    let readsReady: UInt64
    let readsNotYetWritten: UInt64
    let readsStaleOverwritten: UInt64
    let readsSnapshotBusy: UInt64
    let readsInvalid: UInt64
    let deferrals: UInt64
    let deadlineNoData: UInt64
    let staleXruns: UInt64
    let rebases: UInt64
    let faultEvents: UInt64
    let firstFaultReason: UInt32
    let firstFaultPacket: UInt64
    let firstFaultAudioFrame: UInt64
    let firstFaultOldestFrame: UInt64
    let firstFaultWrittenEndFrame: UInt64
    let firstFaultCompletionPacket: UInt64
    let firstFaultCommittedPacketEnd: UInt64

    var pendingStagedFrames: UInt64 {
        stagedWrittenEndFrame >= finalizedFrameEnd
            ? stagedWrittenEndFrame - finalizedFrameEnd
            : 0
    }

    var committedMarginPackets: UInt64 {
        committedPacketEnd >= completionPacket
            ? committedPacketEnd - completionPacket
            : 0
    }

    var finalizedCursorIsStale: Bool {
        finalizedFrameEnd < stagedOldestFrame
    }

    var firstFaultName: String {
        switch firstFaultReason {
        case 0: return "none"
        case 1: return "notYetWrittenAtDeadline"
        case 2: return "staleOverwritten"
        case 3: return "snapshotBusyAtDeadline"
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

    /// Names only states that the snapshot proves. A positive pending frame
    /// count is normal because CoreAudio is expected to lead finalization.
    var verdict: String {
        if !bindingReady { return "bindingNotReady" }
        if !streaming { return "idle" }
        if transportStatus == 2 || transportStatus == 3 || transportStatus == 4 ||
            firstFaultReason == 4 || firstFaultReason == 5 || readsInvalid > 0 {
            return "fatal"
        }
        if staleXruns > 0 || finalizedCursorIsStale {
            return "staleXrun"
        }
        if deadlineNoData > 0 {
            return "deadlineNoData"
        }
        if stagedWrittenEndFrame == 0 {
            return "awaitingHostWrite"
        }
        if pendingStagedFrames > 0 {
            return "healthyPendingContent"
        }
        return "healthy"
    }

    func mcpValue() -> ASFWMCPValue {
        .object([
            "guid": .uint64(guid),
            "bindingReady": .bool(bindingReady),
            "streaming": .bool(streaming),
            "sampleRateHz": .int(Int(sampleRateHz)),
            "outputChannels": .int(Int(outputChannels)),
            "snapshotKind": .string("valueOwned"),
            "verdict": .string(verdict),
            "frameCursors": .object([
                "units": .string("absoluteHostFrames"),
                "stagedOldest": .uint64(stagedOldestFrame),
                "stagedWrittenEnd": .uint64(stagedWrittenEndFrame),
                "finalizedEnd": .uint64(finalizedFrameEnd),
                "pendingStagedFrames": .uint64(pendingStagedFrames),
                "finalizedCursorIsStale": .bool(finalizedCursorIsStale)
            ]),
            "transportCursors": .object([
                "units": .string("absoluteIsochPackets"),
                "completion": .uint64(completionPacket),
                "committedEnd": .uint64(committedPacketEnd),
                "committedMargin": .uint64(committedMarginPackets),
                "status": .string(transportStatusName)
            ]),
            "counters": .object([
                "stagingWrites": .uint64(stagingWrites),
                "stagingFrames": .uint64(stagingFrames),
                "stagingDiscontinuities": .uint64(stagingDiscontinuities),
                "stagingOverwrittenFrames": .uint64(stagingOverwrittenFrames),
                "readsReady": .uint64(readsReady),
                "readsNotYetWritten": .uint64(readsNotYetWritten),
                "readsStaleOverwritten": .uint64(readsStaleOverwritten),
                "readsSnapshotBusy": .uint64(readsSnapshotBusy),
                "readsInvalid": .uint64(readsInvalid),
                "deferrals": .uint64(deferrals),
                "deadlineNoData": .uint64(deadlineNoData),
                "staleXruns": .uint64(staleXruns),
                "rebases": .uint64(rebases),
                "faultEvents": .uint64(faultEvents)
            ]),
            "firstFault": .object([
                "reason": .string(firstFaultName),
                "packet": .uint64(firstFaultPacket),
                "audioFrame": .uint64(firstFaultAudioFrame),
                "stagedOldest": .uint64(firstFaultOldestFrame),
                "stagedWrittenEnd": .uint64(firstFaultWrittenEndFrame),
                "completionPacket": .uint64(firstFaultCompletionPacket),
                "committedPacketEnd": .uint64(firstFaultCommittedPacketEnd)
            ])
        ])
    }
}

extension AudioTelemetryEndpoint {
    var mcpAudioCursors: ASFWMCPAudioCursorSnapshot {
        ASFWMCPAudioCursorSnapshot(
            guid: guid,
            bindingReady: isBindingReady,
            streaming: isStreaming,
            sampleRateHz: sampleRateHz,
            outputChannels: outputChannels,
            stagedOldestFrame: txStagingOldestValidFrame,
            stagedWrittenEndFrame: txStagingWrittenEndFrame,
            finalizedFrameEnd: txContentFinalizedFrameEnd,
            completionPacket: txTransportCompletionCursor,
            committedPacketEnd: txTransportCommittedEnd,
            transportStatus: txTransportStatus,
            stagingWrites: txStagingWrites,
            stagingFrames: txStagingFrames,
            stagingDiscontinuities: txStagingDiscontinuities,
            stagingOverwrittenFrames: txStagingOverwrittenFrames,
            readsReady: txStagingReadsReady,
            readsNotYetWritten: txStagingReadsNotYetWritten,
            readsStaleOverwritten: txStagingReadsStaleOverwritten,
            readsSnapshotBusy: txStagingReadsSnapshotBusy,
            readsInvalid: txStagingReadsInvalid,
            deferrals: txContentDeferrals,
            deadlineNoData: txContentDeadlineNoData,
            staleXruns: txContentStaleXruns,
            rebases: txContentRebases,
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
