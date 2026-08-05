import Foundation

// Audio stream health: read-only projection of the driver's per-endpoint RX
// bring-up attribution (AudioTelemetrySnapshot wire v3).
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
        ASFWMCPToolDefinition(name: "asfw_get_audio_stream_health", group: "audio_streams", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Per-endpoint RX bring-up attribution: what the device sent and what we did with it. No transaction.")
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
