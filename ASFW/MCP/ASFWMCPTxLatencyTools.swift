//
//  ASFWMCPTxLatencyTools.swift
//  ASFW
//
//  Created for ASFireWire Project.
//  MCP tool definitions and serializers for FireWire TX Latency Sessions (E0 -> E2).
//

import Foundation

extension ASFWMCPToolCatalog {
    static let txLatencyTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(
            name: "asfw_start_tx_latency_session",
            group: "tx_latency",
            visibility: .developerWrite,
            readOnly: false,
            idempotent: false,
            summary: "Start or arm a TX latency metering session (E0 -> E2) on an audio endpoint."
        ),
        ASFWMCPToolDefinition(
            name: "asfw_stop_tx_latency_session",
            group: "tx_latency",
            visibility: .developerWrite,
            readOnly: false,
            idempotent: false,
            summary: "Stop an active TX latency metering session on an audio endpoint early."
        ),
        ASFWMCPToolDefinition(
            name: "asfw_get_tx_latency_results",
            group: "tx_latency",
            visibility: .readOnly,
            readOnly: true,
            idempotent: true,
            summary: "Query paged or consolidated TX latency metering results (E0 -> E2) with statistics and population classification."
        )
    ]
}

// MARK: - MCP Value Serialization

extension TxLatencySample {
    var mcpValue: ASFWMCPValue {
        var obj: [String: ASFWMCPValue] = [:]
        obj["packetIndex"] = .uint64(packetIndex)
        obj["pcmCommittedStartFrame"] = .uint64(pcmCommittedStartFrame)
        obj["pcmCommittedEndFrame"] = .uint64(pcmCommittedEndFrame)
        obj["frameSpan"] = .uint64(frameSpan)
        obj["pubEarliestHostTicks"] = .uint64(pubEarliestHostTicks)
        obj["pubLatestHostTicks"] = .uint64(pubLatestHostTicks)
        obj["txCycleStartHostTicks"] = .uint64(txCycleStartHostTicks)
        obj["uncertaintyHostTicks"] = .int(Int(uncertaintyHostTicks))
        obj["waitMinNanos"] = .int(Int(waitMinNanos))
        obj["waitMaxNanos"] = .int(Int(waitMaxNanos))
        obj["waitMinMicros"] = .string(String(format: "%.3f", waitMinMicros))
        obj["waitMaxMicros"] = .string(String(format: "%.3f", waitMaxMicros))
        obj["waitCenterMicros"] = .string(String(format: "%.3f", waitCenterMicros))
        obj["uncertaintyMicros"] = .string(String(format: "%.3f", uncertaintyMicros))
        obj["outcome"] = .string(outcome.description)
        obj["unresolvedReason"] = .string(unresolvedReason.description)
        obj["selectedImage"] = .int(Int(selectedImage))
        obj["arbitrationPhase"] = .int(Int(arbitrationPhase))
        obj["packetGeneration"] = .int(Int(packetGeneration))
        obj["pcmIdentityProven"] = .bool(pcmIdentityProven)
        obj["validityFlags"] = .int(Int(validityFlags))
        obj["correlationAgeTicks"] = .int(Int(correlationAgeTicks))
        return .object(obj)
    }
}

extension TxLatencySessionHeader {
    var mcpValue: ASFWMCPValue {
        var obj: [String: ASFWMCPValue] = [:]
        obj["version"] = .int(Int(version))
        obj["sessionState"] = .string(sessionState.description)
        obj["terminationReason"] = .string(terminationReason.description)
        obj["sessionId"] = .int(Int(sessionId))
        obj["endpointId"] = .uint64(endpointId.rawValue)
        obj["epoch"] = .uint64(epoch)
        obj["sampleRateHz"] = .int(Int(sampleRateHz))
        obj["startHostTicks"] = .uint64(startHostTicks)
        obj["deadlineHostTicks"] = .uint64(deadlineHostTicks)
        obj["frozenHostTicks"] = .uint64(frozenHostTicks)
        obj["durationSeconds"] = .int(Int(durationSeconds))
        obj["strataSize"] = .int(Int(strataSize))
        obj["seed"] = .int(Int(seed))
        obj["assumedDriftPpm"] = .int(Int(assumedDriftPpm))
        obj["dataPacketsSeen"] = .uint64(dataPacketsSeen)
        obj["samplesCaptured"] = .int(Int(samplesCaptured))
        obj["stampsMissedCount"] = .int(Int(stampsMissedCount))
        obj["resolvedCount"] = .int(Int(resolvedCount))
        obj["unresolvedCount"] = .int(Int(unresolvedCount))
        obj["transmitFailedCount"] = .int(Int(transmitFailedCount))
        obj["substitutionCount"] = .int(Int(substitutionCount))
        obj["invalidCount"] = .int(Int(invalidCount))
        obj["eligibleByPhase"] = .array(eligibleByPhase.map { .int(Int($0)) })
        obj["unresolvedReasons"] = .object([
            "staleCorrelation": .int(Int(reasonStaleCorrelation)),
            "coveragePending": .int(Int(reasonCoveragePending)),
            "coverageGap": .int(Int(reasonCoverageGap)),
            "epochMismatch": .int(Int(reasonEpochMismatch)),
            "publicationAgedOut": .int(Int(reasonPublicationAgedOut)),
            "provenanceAgedOut": .int(Int(reasonProvenanceAgedOut)),
            "unrecognizedEventCode": .int(Int(reasonUnrecognizedEventCode)),
            "imageUnavailable": .int(Int(reasonImageUnavailable))
        ])
        obj["ring"] = .object([
            "totalRecords": .int(Int(totalRingRecords)),
            "head": .int(Int(ringHead)),
            "tail": .int(Int(ringTail))
        ])
        obj["geometryProvenance"] = .object([
            "raw": .int(Int(geometryProvenance)),
            "preparationLeadPackets": .int(Int(preparationLeadPackets)),
            "hardwareRingPackets": .int(Int(hardwareRingPackets))
        ])
        return .object(obj)
    }
}

extension TxLatencyRangeStats {
    var mcpValue: ASFWMCPValue {
        .object([
            "sampleCount": .int(sampleCount),
            "minWaitMicros": .string(String(format: "%.3f", minWaitMicros)),
            "maxWaitMicros": .string(String(format: "%.3f", maxWaitMicros)),
            "medianWaitMicros": .string(String(format: "%.3f", medianWaitMicros)),
            "p95WaitMicros": .string(String(format: "%.3f", p95WaitMicros)),
            "meanWaitMicros": .string(String(format: "%.3f", meanWaitMicros)),
            "meanUncertaintyMicros": .string(String(format: "%.3f", meanUncertaintyMicros))
        ])
    }
}

extension TxLatencyResultsPage {
    var mcpValue: ASFWMCPValue {
        .object([
            "header": header.mcpValue,
            "pageIndex": .int(Int(pageIndex)),
            "totalPages": .int(Int(totalPages)),
            "sampleCount": .int(samples.count),
            "samples": .array(samples.map(\.mcpValue))
        ])
    }
}

extension TxLatencySessionReport {
    var mcpValue: ASFWMCPValue {
        .object([
            "header": header.mcpValue,
            "matchedStats": matchedStats.mcpValue,
            "totalSamples": .int(samples.count),
            "samples": .array(samples.map(\.mcpValue))
        ])
    }
}
