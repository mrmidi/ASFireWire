import Foundation

// Audio stream health: read-only projection of the driver's per-endpoint RX
// bring-up attribution (AudioTelemetrySnapshot; fields added in wire v3, current v4).
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
        ASFWMCPToolDefinition(name: "asfw_get_audio_telemetry", group: "audio_streams", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Full stable audio telemetry summary (wire v4): TX preparation/margin and RX capture intervals with their durations. No transaction.")
    ]
}

/// One endpoint's RX attribution, plus the verdict derived from it.
struct ASFWMCPAudioStreamHealth: Equatable {
    let guid: UInt64
    let streaming: Bool
    let sampleRateHz: UInt32
    let inputChannels: UInt32
    let outputChannels: UInt32

    let packetsSeen: UInt64
    let dataPackets: UInt64
    let noDataPackets: UInt64
    let shortPackets: UInt64
    let invalidCipHeaders: UInt64
    let zeroDataBlockSize: UInt64
    let geometryMismatch: UInt64
    let replayEntries: UInt64
    let replayEpochResets: UInt64

    var rejectedPackets: UInt64 {
        shortPackets &+ invalidCipHeaders &+ zeroDataBlockSize &+ geometryMismatch
    }

    /// Stable machine-readable cause, so callers do not re-derive the rules.
    ///
    /// Deliberately conservative: it names what the counters prove, never why.
    /// `deviceSendsOnlyNoData` says the device sent nothing but CIP NO-DATA — it
    /// does NOT say the device is waiting on us.
    var verdict: String {
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
        case "noPacketsReceived":
            return "No isochronous packet reached the audio consumer. The IR context is not delivering: check that the channel matches the device's TX ISOC register, that the context started, and that the device stream is enabled."
        case "geometryMismatch":
            return "Packets arrived with a data block shape our stream config does not accept (channels / DBS / AM824 slots). The profile and the device disagree; this is a host-side rejection, not a silent device."
        case "packetsRejected":
            return "Packets arrived but were rejected before decode (runt, undecodable CIP header, or zero data block size). The device may be streaming correctly."
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
            streaming: isStreaming,
            sampleRateHz: sampleRateHz,
            inputChannels: inputChannels,
            outputChannels: outputChannels,
            packetsSeen: rxPacketsSeen,
            dataPackets: rxDataPackets,
            noDataPackets: rxNoDataPackets,
            shortPackets: rxShortPackets,
            invalidCipHeaders: rxInvalidCipHeaders,
            zeroDataBlockSize: rxZeroDataBlockSize,
            geometryMismatch: rxGeometryMismatch,
            replayEntries: rxReplayEntries,
            replayEpochResets: rxReplayEpochResets
        )
    }
}

// Full stable telemetry summary (wire v4). Field names follow the C++ record
// (AudioTelemetrySnapshot.hpp) so a report can be matched to the contract.
// ASFWMCPValue has no floating point, so durations are also given in ns.
extension AudioTelemetrySnapshot {
    func mcpValue() -> ASFWMCPValue {
        func nanos(_ ticks: UInt64) -> ASFWMCPValue {
            guard hostTimebaseDenom != 0 else { return .null }
            let product = ticks.multipliedFullWidth(by: UInt64(hostTimebaseNumer))
            let (quotient, _) = UInt64(hostTimebaseDenom).dividingFullWidth(product)
            return .uint64(quotient)
        }
        func u64s(_ values: [UInt64]) -> ASFWMCPValue { .array(values.map { .uint64($0) }) }
        return .object([
            "wireVersion": .int(Int(AudioTelemetryWireDecoder.minimumVersion)),
            "captureHostTicks": .uint64(captureHostTicks),
            "hostTimebaseNumer": .int(Int(hostTimebaseNumer)),
            "hostTimebaseDenom": .int(Int(hostTimebaseDenom)),
            "endpointCount": .int(endpoints.count),
            "endpoints": .array(endpoints.map { e in
                .object([
                    "guid": .uint64(e.guid),
                    "endpointGeneration": .uint64(e.endpointGeneration),
                    "controlGeneration": .uint64(e.controlGeneration),
                    "flags": .int(Int(e.flags)),
                    "streaming": .bool(e.isStreaming),
                    "sampleRateHz": .int(Int(e.sampleRateHz)),
                    "outputChannels": .int(Int(e.outputChannels)),
                    "inputChannels": .int(Int(e.inputChannels)),
                    "tx": .object([
                        "completedIntervalSequence": .uint64(e.completedIntervalSequence),
                        "completedIntervalDurationTicks": .uint64(e.txCompletedIntervalDurationTicks),
                        "completedIntervalDurationNs": e.txCompletedIntervalDurationTicks == 0 ? .null : nanos(e.txCompletedIntervalDurationTicks),
                        "completedIntervalEndHostTicks": .uint64(e.txCompletedIntervalEndHostTicks),
                        "lastPreparationLatencyTicks": .uint64(e.lastPreparationLatencyTicks),
                        "completedIntervalMaxLatencyTicks": .uint64(e.completedIntervalMaxLatencyTicks),
                        "maxPreparationLatencyTicks": .uint64(e.maxPreparationLatencyTicks),
                        "preparationWakeCount": .uint64(e.preparationWakeCount),
                        "preparationAtMost750Us": .uint64(e.preparationAtMost750Us),
                        "preparationAtLeast1500Us": .uint64(e.preparationAtLeast1500Us),
                        "completedLatencyHistogram": u64s(e.completedLatencyHistogram),
                        "completedMarginHistogram": u64s(e.completedMarginHistogram),
                        "currentCommittedMarginPackets": .int(Int(e.currentCommittedMarginPackets)),
                        "completedIntervalMarginMinPackets": .int(Int(e.completedIntervalMarginMinPackets)),
                        "completedIntervalMarginMaxPackets": .int(Int(e.completedIntervalMarginMaxPackets)),
                        "minimumCommittedMarginPackets": .int(Int(e.minimumCommittedMarginPackets)),
                        "preparationLeadPackets": .int(Int(e.preparationLeadPackets)),
                        "hardwareFloorPackets": .int(Int(e.hardwareFloorPackets))
                    ]),
                    "rx": .object([
                        "completedIntervalSequence": .uint64(e.rxCompletedIntervalSequence),
                        "completedIntervalDurationTicks": .uint64(e.rxCompletedIntervalDurationTicks),
                        "completedIntervalDurationNs": e.rxCompletedIntervalDurationTicks == 0 ? .null : nanos(e.rxCompletedIntervalDurationTicks),
                        "completedIntervalEndHostTicks": .uint64(e.rxCompletedIntervalEndHostTicks),
                        "captureReaderActive": .bool(e.isRxCaptureReaderActive),
                        "currentAvailableFrames": .uint64(e.rxCurrentAvailableFrames),
                        "inputFrameCapacityFrames": .int(Int(e.inputFrameCapacityFrames)),
                        "completedIntervalMinimumAvailableFrames": .uint64(e.rxCompletedIntervalMinimumAvailableFrames),
                        "completedIntervalMaximumAvailableFrames": .uint64(e.rxCompletedIntervalMaximumAvailableFrames),
                        "completedIntervalMinimumFreeHeadroomFrames": .uint64(e.rxCompletedIntervalMinimumFreeHeadroomFrames),
                        "completedIntervalOverrunEvents": .uint64(e.rxCompletedIntervalOverrunEvents),
                        "completedIntervalOverwrittenFrames": .uint64(e.rxCompletedIntervalOverwrittenFrames),
                        "completedIntervalStarvationEvents": .uint64(e.rxCompletedIntervalStarvationEvents),
                        "completedIntervalStarvedFrames": .uint64(e.rxCompletedIntervalStarvedFrames),
                        "completedOccupancyHistogram": u64s(e.rxCompletedOccupancyHistogram),
                        "captureOverrunEvents": .uint64(e.rxCaptureOverrunEvents),
                        "captureStarvationEvents": .uint64(e.rxCaptureStarvationEvents),
                        "totalOverwrittenFrames": .uint64(e.rxTotalOverwrittenFrames),
                        "totalStarvedFrames": .uint64(e.rxTotalStarvedFrames)
                    ]),
                    "rxAttribution": e.mcpStreamHealth.mcpValue()
                ])
            })
        ])
    }
}
