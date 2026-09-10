//
//  DriverConnector+TxLatency.swift
//  ASFW
//
//  Created for ASFireWire Project.
//  Typed decoding, models, and connector methods for TX Latency Session wire formats.
//

import Foundation
import IOKit

// MARK: - Enums

enum TxLatencyOutcome: UInt8, Codable, Sendable, CustomStringConvertible {
    case unknown = 0
    case matched = 1
    case substituted = 2
    case unresolved = 3
    case transmitFailed = 4
    case invalid = 5

    var description: String {
        switch self {
        case .unknown: return "Unknown"
        case .matched: return "Matched"
        case .substituted: return "Substituted"
        case .unresolved: return "Unresolved"
        case .transmitFailed: return "Transmit Failed"
        case .invalid: return "Invalid"
        }
    }
}

enum TxLatencyUnresolvedReason: UInt8, Codable, Sendable, CustomStringConvertible {
    case none = 0
    case staleCorrelation = 1
    case coveragePending = 2
    case coverageGap = 3
    case epochMismatch = 4
    case publicationAgedOut = 5
    case provenanceAgedOut = 6
    case unrecognizedEventCode = 7
    case imageUnavailable = 8
    case publicationReadCollision = 9

    var description: String {
        switch self {
        case .none: return "None"
        case .staleCorrelation: return "Stale Correlation (>8s)"
        case .coveragePending: return "Coverage Pending"
        case .coverageGap: return "Coverage Gap"
        case .epochMismatch: return "Epoch Mismatch"
        case .publicationAgedOut: return "Publication Aged Out"
        case .provenanceAgedOut: return "Provenance Aged Out"
        case .unrecognizedEventCode: return "Unrecognized Event Code"
        case .imageUnavailable: return "Image Unavailable"
        case .publicationReadCollision: return "Publication Read Collision"
        }
    }
}

enum TxLatencySessionState: UInt32, Codable, Sendable, CustomStringConvertible {
    case idle = 0
    case arming = 1
    case capturing = 2
    case stopRequested = 3
    case frozen = 4

    var description: String {
        switch self {
        case .idle: return "Idle"
        case .arming: return "Arming"
        case .capturing: return "Capturing"
        case .stopRequested: return "Stop Requested"
        case .frozen: return "Frozen"
        }
    }
}

enum TxLatencyTerminationReason: UInt32, Codable, Sendable, CustomStringConvertible {
    case none = 0
    case userStopped = 1
    case deadlineExpired = 2
    case epochChanged = 3
    case capacityReached = 4
    case streamReset = 5
    case driverTeardown = 6

    var description: String {
        switch self {
        case .none: return "None"
        case .userStopped: return "User Stopped"
        case .deadlineExpired: return "Deadline Expired"
        case .epochChanged: return "Epoch Changed"
        case .capacityReached: return "Capacity Reached"
        case .streamReset: return "Stream Reset"
        case .driverTeardown: return "Driver Teardown"
        }
    }
}

// MARK: - Models

/// Mirrors kTxLatencyFlag* in TxLatencySessionWireFormats.hpp.
enum TxLatencyFlags {
    static let pubValid: UInt16               = 1 << 0
    static let txValid: UInt16                = 1 << 1
    static let waitValid: UInt16              = 1 << 2
    static let provenanceValid: UInt16        = 1 << 3
    static let correlationValid: UInt16       = 1 << 4
    static let producerDecisionValid: UInt16  = 1 << 5
    static let transportDecisionValid: UInt16 = 1 << 6
    static let imageReadyValid: UInt16        = 1 << 7
    static let offerValid: UInt16             = 1 << 8
    static let bindValid: UInt16              = 1 << 9
    static let descriptorUpdateValid: UInt16  = 1 << 10
    static let sealValid: UInt16              = 1 << 11
    static let decisionOverwritten: UInt16    = 1 << 12
    static let terminalPosIsSnapshot: UInt16  = 1 << 13
}

struct TxLatencySample: Identifiable, Sendable, Codable, Equatable {
    var id: UInt64 { packetIndex }

    let packetIndex: UInt64
    let pcmCommittedStartFrame: UInt64
    let pcmCommittedEndFrame: UInt64
    let pubEarliestHostTicks: UInt64
    let pubLatestHostTicks: UInt64
    let txCycleStartHostTicks: UInt64
    let waitMinNanos: Int64
    let waitMaxNanos: Int64
    let uncertaintyHostTicks: UInt32
    let correlationAgeTicks: UInt32
    let outcome: TxLatencyOutcome
    let unresolvedReason: TxLatencyUnresolvedReason
    let selectedImage: UInt8
    let cyclePhaseMod8: UInt8
    let packetGeneration: UInt8
    let pcmIdentityProven: Bool
    let validityFlags: UInt16

    let encodeCompleteHostTicks: UInt64
    let offerStartHostTicks: UInt64
    let offerEndHostTicks: UInt64
    /// When the terminal examination happened, not when its pass started.
    let transExaminedHostTicks: UInt64
    let descriptorUpdateHostTicks: UInt64
    let sealStartHostTicks: UInt64
    let sealEndHostTicks: UInt64
    /// Last transport look that found nothing on offer, and the first that saw
    /// the offer. Their gap brackets when the offer became visible.
    let lastBeforeOfferHostTicks: UInt64
    let firstAfterOfferHostTicks: UInt64
    let lastBeforeOfferPassId: UInt64
    let firstAfterOfferPassId: UInt64
    let passId: UInt64
    let liveHwPos: UInt64
    let e0ToImageReadyNanos: Int64
    // Bound pairs: the offer is a CAS bracket, so an interval measured from it
    // is a range. min < 0 < max means the ordering was not established.
    let offerToExaminedNanosMin: Int64
    let offerToExaminedNanosMax: Int64
    let offerToFirstServiceNanosMin: Int64
    let offerToFirstServiceNanosMax: Int64
    let offerToDescriptorUpdateNanosMin: Int64
    let offerToDescriptorUpdateNanosMax: Int64
    let sealRelativeToOfferNanosMin: Int64
    let sealRelativeToOfferNanosMax: Int64
    let producerFlags: UInt32
    let transportFlags: UInt32
    let examinationCount: UInt32
    let hwDistancePackets: Int32
    let acquireResult: UInt8
    let offerResult: UInt8
    let bindResult: UInt8
    let sealResult: UInt8
    let sealReason: UInt8
    let observedArbPhase: UInt8
    let examinedArbPhase: UInt8
    let producerJoinResult: UInt8
    let transportJoinResult: UInt8
    /// Outcome of the most recent concluded bind attempt. Distinct from
    /// `bindResult`, which comes from the terminal event and is absent when an
    /// attempt failed without deciding the packet.
    let lastAttemptBindResult: UInt8

    var waitMinMicros: Double {
        Double(waitMinNanos) / 1_000.0
    }

    var waitMaxMicros: Double {
        Double(waitMaxNanos) / 1_000.0
    }

    var waitCenterMicros: Double {
        (waitMinMicros + waitMaxMicros) / 2.0
    }

    var uncertaintyMicros: Double {
        (waitMaxMicros - waitMinMicros) / 2.0
    }

    var e0ToImageReadyMicros: Double {
        Double(e0ToImageReadyNanos) / 1_000.0
    }

    /// Midpoint of a bound pair, for display only. Anything that decides
    /// something should use the bounds, not this.
    private static func centerMicros(_ lo: Int64, _ hi: Int64) -> Double {
        (Double(lo) + Double(hi)) / 2_000.0
    }

    var offerToExaminedMicros: Double {
        Self.centerMicros(offerToExaminedNanosMin, offerToExaminedNanosMax)
    }

    var offerToFirstServiceMicros: Double {
        Self.centerMicros(offerToFirstServiceNanosMin, offerToFirstServiceNanosMax)
    }

    var offerToDescriptorUpdateMicros: Double {
        Self.centerMicros(offerToDescriptorUpdateNanosMin, offerToDescriptorUpdateNanosMax)
    }

    var sealRelativeToOfferMicros: Double {
        Self.centerMicros(sealRelativeToOfferNanosMin, sealRelativeToOfferNanosMax)
    }

    /// True when a bound pair straddles zero: the brackets overlap, so the
    /// ordering of the two events was not established by this measurement.
    var offerToFirstServiceOrderEstablished: Bool {
        offerToFirstServiceNanosMin > 0 || offerToFirstServiceNanosMax < 0
    }

    /// Why a lane's decision record could or could not be joined. Mirrors
    /// ASFW::Isoch::TxDecisionJoinResult.
    enum DecisionJoin: UInt8, Sendable, Codable {
        case valid = 0
        case neverWritten = 1
        case captureStartedLater = 2
        case tokenMismatch = 3
        case slotReused = 4
        case snapshotCollision = 5
        case packetMismatch = 6

        var description: String {
            switch self {
            case .valid: return "Valid"
            case .neverWritten: return "No record"
            case .captureStartedLater: return "Capture started later"
            case .tokenMismatch: return "Different capture"
            case .slotReused: return "Overwritten"
            case .snapshotCollision: return "Read collision"
            case .packetMismatch: return "Stale record"
            }
        }
    }

    var producerJoin: DecisionJoin {
        DecisionJoin(rawValue: producerJoinResult) ?? .neverWritten
    }

    var transportJoin: DecisionJoin {
        DecisionJoin(rawValue: transportJoinResult) ?? .neverWritten
    }

    var hasProducerDecision: Bool { producerJoin == .valid }
    var hasTransportDecision: Bool { transportJoin == .valid }

    /// A bound packet is terminal but never sealed, so seal fields are only
    /// meaningful when the driver flagged an actual seal.
    var hasSeal: Bool {
        validityFlags & TxLatencyFlags.sealValid != 0
    }

    /// One line describing what actually happened to this packet's replacement
    /// content, or why we cannot say.
    var decisionEvidenceSummary: String {
        guard hasTransportDecision else { return transportJoin.description }
        var parts: [String] = []
        if hasProducerDecision {
            parts.append("Acq:\(acquireResult)")
            parts.append(offerResult == 1 ? "Offer:won"
                         : (offerResult == 2 ? "Offer:lost" : "Offer:none"))
        } else {
            parts.append("Prod:\(producerJoin.description)")
        }
        if bindResult != 0 {
            parts.append("Bind:\(bindResult)")
        } else if lastAttemptBindResult != 0 {
            // Attempted and failed without deciding the packet -- not the same
            // as never having been serviced.
            parts.append("Attempt:\(lastAttemptBindResult)")
        }
        if hasSeal {
            parts.append("Seal:\(sealResult)/\(sealReason)")
        }
        parts.append("Exams:\(examinationCount)")
        return parts.joined(separator: " ")
    }

    var frameSpan: UInt64 {
        pcmCommittedEndFrame > pcmCommittedStartFrame ? pcmCommittedEndFrame - pcmCommittedStartFrame : 0
    }
}

struct TxLatencySessionHeader: Sendable, Codable, Equatable {
    let version: UInt32
    let sessionState: TxLatencySessionState
    let terminationReason: TxLatencyTerminationReason
    let sessionId: UInt32
    let endpointId: AudioEndpointID
    let epoch: UInt64
    let startHostTicks: UInt64
    let deadlineHostTicks: UInt64
    let frozenHostTicks: UInt64

    let durationSeconds: UInt32
    let strataSize: UInt32
    let seed: UInt32
    let assumedDriftPpm: UInt32

    let dataPacketsSeen: UInt64
    let samplesCaptured: UInt32
    let stampsMissedCount: UInt32

    let resolvedCount: UInt32
    let unresolvedCount: UInt32
    let transmitFailedCount: UInt32
    let substitutionCount: UInt32
    let invalidCount: UInt32
    let sampleRateHz: UInt32

    let eligibleByPhase: [UInt32]

    let reasonStaleCorrelation: UInt32
    let reasonCoveragePending: UInt32
    let reasonCoverageGap: UInt32
    let reasonEpochMismatch: UInt32
    let reasonPublicationAgedOut: UInt32
    let reasonProvenanceAgedOut: UInt32
    let reasonUnrecognizedEventCode: UInt32
    let reasonImageUnavailable: UInt32

    let totalRingRecords: UInt32
    let ringHead: UInt32
    let ringTail: UInt32
    let geometryProvenance: UInt32

    var preparationLeadPackets: UInt16 { UInt16(geometryProvenance & 0xFFFF) }
    var hardwareRingPackets: UInt16 { UInt16(geometryProvenance >> 16) }
}

struct TxLatencyResultsPage: Sendable, Codable {
    let header: TxLatencySessionHeader
    let pageIndex: UInt32
    let totalPages: UInt32
    let samples: [TxLatencySample]
}

struct TxLatencyRangeStats: Sendable, Codable, Equatable {
    let sampleCount: Int
    let minWaitMicros: Double
    let maxWaitMicros: Double
    let medianWaitMicros: Double
    let p95WaitMicros: Double
    let meanWaitMicros: Double
    let meanUncertaintyMicros: Double

    static let empty = TxLatencyRangeStats(
        sampleCount: 0,
        minWaitMicros: 0,
        maxWaitMicros: 0,
        medianWaitMicros: 0,
        p95WaitMicros: 0,
        meanWaitMicros: 0,
        meanUncertaintyMicros: 0
    )
}

struct TxLatencySessionReport: Sendable, Codable {
    let header: TxLatencySessionHeader
    let samples: [TxLatencySample]
    let matchedStats: TxLatencyRangeStats

    init(header: TxLatencySessionHeader, samples: [TxLatencySample]) {
        self.header = header
        self.samples = samples
        self.matchedStats = Self.calculateStats(for: samples.filter { $0.outcome == .matched })
    }

    static func calculateStats(for samples: [TxLatencySample]) -> TxLatencyRangeStats {
        guard !samples.isEmpty else { return .empty }
        let centers = samples.map { $0.waitCenterMicros }.sorted()
        let count = centers.count
        let minWait = samples.map { $0.waitMinMicros }.min() ?? 0
        let maxWait = samples.map { $0.waitMaxMicros }.max() ?? 0
        let sum = centers.reduce(0, +)
        let mean = sum / Double(count)

        let median: Double
        if count % 2 == 0 {
            median = (centers[count / 2 - 1] + centers[count / 2]) / 2.0
        } else {
            median = centers[count / 2]
        }

        let p95Index = min(count - 1, Int(ceil(Double(count) * 0.95)) - 1)
        let p95 = centers[max(0, p95Index)]

        let sumUncertainty = samples.map { $0.uncertaintyMicros }.reduce(0, +)
        let meanUncertainty = sumUncertainty / Double(count)

        return TxLatencyRangeStats(
            sampleCount: count,
            minWaitMicros: minWait,
            maxWaitMicros: maxWait,
            medianWaitMicros: median,
            p95WaitMicros: p95,
            meanWaitMicros: mean,
            meanUncertaintyMicros: meanUncertainty
        )
    }

    func toCSV() -> String {
        var lines: [String] = []
        // Metadata header
        lines.append("# ASFW FireWire TX Latency Session Report (E0 -> E2)")
        lines.append("# Session ID: \(header.sessionId)")
        lines.append("# Endpoint ID: \(header.endpointId.rawValue)")
        lines.append("# Session State: \(header.sessionState)")
        lines.append("# Termination Reason: \(header.terminationReason)")
        lines.append("# Rate: \(header.sampleRateHz) Hz, Geometry: lead=\(header.preparationLeadPackets)pkts, hwRing=\(header.hardwareRingPackets)pkts")
        lines.append("# Duration: \(header.durationSeconds)s, Strata: \(header.strataSize), Seed: \(header.seed), Drift: \(header.assumedDriftPpm) ppm")
        lines.append("# Packets Seen: \(header.dataPacketsSeen), Samples: \(header.samplesCaptured), Missed Stamps: \(header.stampsMissedCount)")
        lines.append("# Resolved: \(header.resolvedCount), Unresolved: \(header.unresolvedCount), Failed: \(header.transmitFailedCount), Substituted: \(header.substitutionCount), Invalid: \(header.invalidCount)")
        lines.append("# Range Stats (Matched): count=\(matchedStats.sampleCount), min=\(String(format: "%.2f", matchedStats.minWaitMicros))us, med=\(String(format: "%.2f", matchedStats.medianWaitMicros))us, p95=\(String(format: "%.2f", matchedStats.p95WaitMicros))us, max=\(String(format: "%.2f", matchedStats.maxWaitMicros))us, meanUncertainty=±\(String(format: "%.2f", matchedStats.meanUncertaintyMicros))us")
        lines.append("")
        // CSV columns
        lines.append("packet_index,pcm_start_frame,pcm_end_frame,frame_span,outcome,unresolved_reason,selected_image,cycle_phase_mod8,packet_generation,pcm_proven,validity_flags,correlation_age_ticks,pub_earliest_host_ticks,pub_latest_host_ticks,tx_host_ticks,uncertainty_ticks,wait_min_ns,wait_max_ns,wait_min_us,wait_max_us,wait_center_us,uncertainty_us,encode_host_ticks,offer_start_host_ticks,offer_end_host_ticks,exam_host_ticks,desc_update_host_ticks,seal_start_host_ticks,seal_end_host_ticks,last_before_offer_ticks,first_after_offer_ticks,last_before_offer_pass,first_after_offer_pass,e0_to_image_ready_us,offer_to_examined_us_min,offer_to_examined_us_max,offer_to_first_service_us_min,offer_to_first_service_us_max,offer_to_desc_update_us_min,offer_to_desc_update_us_max,seal_relative_to_offer_us_min,seal_relative_to_offer_us_max,pass_id,live_hw_pos,hw_distance_pkts,exam_count,producer_flags,transport_flags,acquire_result,offer_result,bind_result,seal_result,seal_reason,observed_arb_phase,examined_arb_phase,producer_join,transport_join")

        for s in samples {
            let row = [
                "\(s.packetIndex)",
                "\(s.pcmCommittedStartFrame)",
                "\(s.pcmCommittedEndFrame)",
                "\(s.frameSpan)",
                "\(s.outcome)",
                "\(s.unresolvedReason)",
                "\(s.selectedImage)",
                "\(s.cyclePhaseMod8)",
                "\(s.packetGeneration)",
                "\(s.pcmIdentityProven ? 1 : 0)",
                "\(s.validityFlags)",
                "\(s.correlationAgeTicks)",
                "\(s.pubEarliestHostTicks)",
                "\(s.pubLatestHostTicks)",
                "\(s.txCycleStartHostTicks)",
                "\(s.uncertaintyHostTicks)",
                "\(s.waitMinNanos)",
                "\(s.waitMaxNanos)",
                String(format: "%.3f", s.waitMinMicros),
                String(format: "%.3f", s.waitMaxMicros),
                String(format: "%.3f", s.waitCenterMicros),
                String(format: "%.3f", s.uncertaintyMicros),
                "\(s.encodeCompleteHostTicks)",
                "\(s.offerStartHostTicks)",
                "\(s.offerEndHostTicks)",
                "\(s.transExaminedHostTicks)",
                "\(s.descriptorUpdateHostTicks)",
                "\(s.sealStartHostTicks)",
                "\(s.sealEndHostTicks)",
                "\(s.lastBeforeOfferHostTicks)",
                "\(s.firstAfterOfferHostTicks)",
                "\(s.lastBeforeOfferPassId)",
                "\(s.firstAfterOfferPassId)",
                String(format: "%.3f", s.e0ToImageReadyMicros),
                // Bounds, not a single delta: exporting a midpoint alone would
                // let an overlapping pair read as a confident ordering.
                String(format: "%.3f", Double(s.offerToExaminedNanosMin) / 1_000.0),
                String(format: "%.3f", Double(s.offerToExaminedNanosMax) / 1_000.0),
                String(format: "%.3f", Double(s.offerToFirstServiceNanosMin) / 1_000.0),
                String(format: "%.3f", Double(s.offerToFirstServiceNanosMax) / 1_000.0),
                String(format: "%.3f", Double(s.offerToDescriptorUpdateNanosMin) / 1_000.0),
                String(format: "%.3f", Double(s.offerToDescriptorUpdateNanosMax) / 1_000.0),
                String(format: "%.3f", Double(s.sealRelativeToOfferNanosMin) / 1_000.0),
                String(format: "%.3f", Double(s.sealRelativeToOfferNanosMax) / 1_000.0),
                "\(s.passId)",
                "\(s.liveHwPos)",
                "\(s.hwDistancePackets)",
                "\(s.examinationCount)",
                "\(s.producerFlags)",
                "\(s.transportFlags)",
                "\(s.acquireResult)",
                "\(s.offerResult)",
                "\(s.bindResult)",
                "\(s.sealResult)",
                "\(s.sealReason)",
                "\(s.observedArbPhase)",
                "\(s.examinedArbPhase)",
                "\(s.producerJoinResult)",
                "\(s.transportJoinResult)"
            ].joined(separator: ",")
            lines.append(row)
        }
        return lines.joined(separator: "\n")
    }
}

// MARK: - Binary Wire Decoder

enum TxLatencyWireDecoder {
    static let wireVersion: UInt32 = 5
    static let maxSamplesPerPage: Int = 32
    static let sampleBytes: Int = 288
    static let headerBytes: Int = 192
    static let pageBytes: Int = 192 + 16 + (32 * 288) // 9424 bytes

    static func decodePage(_ data: Data) -> TxLatencyResultsPage? {
        guard data.count >= pageBytes else { return nil }

        return data.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return nil }

            // Validate header
            let version = base.loadUnaligned(fromByteOffset: 0, as: UInt32.self)
            guard version == wireVersion else { return nil }

            let stateRaw = base.loadUnaligned(fromByteOffset: 4, as: UInt32.self)
            let termRaw = base.loadUnaligned(fromByteOffset: 8, as: UInt32.self)
            let sessionId = base.loadUnaligned(fromByteOffset: 12, as: UInt32.self)
            let endpointIdRaw = base.loadUnaligned(fromByteOffset: 16, as: UInt64.self)
            let epoch = base.loadUnaligned(fromByteOffset: 24, as: UInt64.self)
            let startHostTicks = base.loadUnaligned(fromByteOffset: 32, as: UInt64.self)
            let deadlineHostTicks = base.loadUnaligned(fromByteOffset: 40, as: UInt64.self)
            let frozenHostTicks = base.loadUnaligned(fromByteOffset: 48, as: UInt64.self)
            let durationSeconds = base.loadUnaligned(fromByteOffset: 56, as: UInt32.self)
            let strataSize = base.loadUnaligned(fromByteOffset: 60, as: UInt32.self)
            let seed = base.loadUnaligned(fromByteOffset: 64, as: UInt32.self)
            let assumedDriftPpm = base.loadUnaligned(fromByteOffset: 68, as: UInt32.self)
            let dataPacketsSeen = base.loadUnaligned(fromByteOffset: 72, as: UInt64.self)
            let samplesCaptured = base.loadUnaligned(fromByteOffset: 80, as: UInt32.self)
            let stampsMissedCount = base.loadUnaligned(fromByteOffset: 84, as: UInt32.self)
            let resolvedCount = base.loadUnaligned(fromByteOffset: 88, as: UInt32.self)
            let unresolvedCount = base.loadUnaligned(fromByteOffset: 92, as: UInt32.self)
            let transmitFailedCount = base.loadUnaligned(fromByteOffset: 96, as: UInt32.self)
            let substitutionCount = base.loadUnaligned(fromByteOffset: 100, as: UInt32.self)
            let invalidCount = base.loadUnaligned(fromByteOffset: 104, as: UInt32.self)
            let sampleRateHz = base.loadUnaligned(fromByteOffset: 108, as: UInt32.self)

            var eligibleByPhase: [UInt32] = []
            eligibleByPhase.reserveCapacity(8)
            for i in 0..<8 {
                let count = base.loadUnaligned(fromByteOffset: 112 + i * 4, as: UInt32.self)
                eligibleByPhase.append(count)
            }

            let reasonStale = base.loadUnaligned(fromByteOffset: 144, as: UInt32.self)
            let reasonPending = base.loadUnaligned(fromByteOffset: 148, as: UInt32.self)
            let reasonGap = base.loadUnaligned(fromByteOffset: 152, as: UInt32.self)
            let reasonEpoch = base.loadUnaligned(fromByteOffset: 156, as: UInt32.self)
            let reasonPubAged = base.loadUnaligned(fromByteOffset: 160, as: UInt32.self)
            let reasonProvAged = base.loadUnaligned(fromByteOffset: 164, as: UInt32.self)
            let reasonUnrec = base.loadUnaligned(fromByteOffset: 168, as: UInt32.self)
            let reasonImgUnavail = base.loadUnaligned(fromByteOffset: 172, as: UInt32.self)
            let totalRingRecords = base.loadUnaligned(fromByteOffset: 176, as: UInt32.self)
            let ringHead = base.loadUnaligned(fromByteOffset: 180, as: UInt32.self)
            let ringTail = base.loadUnaligned(fromByteOffset: 184, as: UInt32.self)
            let geometryProvenance = base.loadUnaligned(fromByteOffset: 188, as: UInt32.self)

            let header = TxLatencySessionHeader(
                version: version,
                sessionState: TxLatencySessionState(rawValue: stateRaw) ?? .idle,
                terminationReason: TxLatencyTerminationReason(rawValue: termRaw) ?? .none,
                sessionId: sessionId,
                endpointId: AudioEndpointID(endpointIdRaw),
                epoch: epoch,
                startHostTicks: startHostTicks,
                deadlineHostTicks: deadlineHostTicks,
                frozenHostTicks: frozenHostTicks,
                durationSeconds: durationSeconds,
                strataSize: strataSize,
                seed: seed,
                assumedDriftPpm: assumedDriftPpm,
                dataPacketsSeen: dataPacketsSeen,
                samplesCaptured: samplesCaptured,
                stampsMissedCount: stampsMissedCount,
                resolvedCount: resolvedCount,
                unresolvedCount: unresolvedCount,
                transmitFailedCount: transmitFailedCount,
                substitutionCount: substitutionCount,
                invalidCount: invalidCount,
                sampleRateHz: sampleRateHz,
                eligibleByPhase: eligibleByPhase,
                reasonStaleCorrelation: reasonStale,
                reasonCoveragePending: reasonPending,
                reasonCoverageGap: reasonGap,
                reasonEpochMismatch: reasonEpoch,
                reasonPublicationAgedOut: reasonPubAged,
                reasonProvenanceAgedOut: reasonProvAged,
                reasonUnrecognizedEventCode: reasonUnrec,
                reasonImageUnavailable: reasonImgUnavail,
                totalRingRecords: totalRingRecords,
                ringHead: ringHead,
                ringTail: ringTail,
                geometryProvenance: geometryProvenance
            )

            let pageIndex = base.loadUnaligned(fromByteOffset: 192, as: UInt32.self)
            let totalPages = base.loadUnaligned(fromByteOffset: 196, as: UInt32.self)
            let samplesInPage = base.loadUnaligned(fromByteOffset: 200, as: UInt32.self)

            var samples: [TxLatencySample] = []
            let validSamples = min(Int(samplesInPage), maxSamplesPerPage)
            samples.reserveCapacity(validSamples)

            for i in 0..<validSamples {
                let sOffset = 208 + i * sampleBytes
                let packetIndex = base.loadUnaligned(fromByteOffset: sOffset + 0, as: UInt64.self)
                let startFrame = base.loadUnaligned(fromByteOffset: sOffset + 8, as: UInt64.self)
                let endFrame = base.loadUnaligned(fromByteOffset: sOffset + 16, as: UInt64.self)
                let pubEarliest = base.loadUnaligned(fromByteOffset: sOffset + 24, as: UInt64.self)
                let pubLatest = base.loadUnaligned(fromByteOffset: sOffset + 32, as: UInt64.self)
                let txTicks = base.loadUnaligned(fromByteOffset: sOffset + 40, as: UInt64.self)
                let waitMin = base.loadUnaligned(fromByteOffset: sOffset + 48, as: Int64.self)
                let waitMax = base.loadUnaligned(fromByteOffset: sOffset + 56, as: Int64.self)
                let encodeCompleteTicks = base.loadUnaligned(fromByteOffset: sOffset + 64, as: UInt64.self)
                let offerStartTicks = base.loadUnaligned(fromByteOffset: sOffset + 72, as: UInt64.self)
                let offerEndTicks = base.loadUnaligned(fromByteOffset: sOffset + 80, as: UInt64.self)
                let transExaminedTicks = base.loadUnaligned(fromByteOffset: sOffset + 88, as: UInt64.self)
                let descriptorUpdateTicks = base.loadUnaligned(fromByteOffset: sOffset + 96, as: UInt64.self)
                let sealStartTicks = base.loadUnaligned(fromByteOffset: sOffset + 104, as: UInt64.self)
                let sealEndTicks = base.loadUnaligned(fromByteOffset: sOffset + 112, as: UInt64.self)
                let lastBeforeOfferTicks = base.loadUnaligned(fromByteOffset: sOffset + 120, as: UInt64.self)
                let firstAfterOfferTicks = base.loadUnaligned(fromByteOffset: sOffset + 128, as: UInt64.self)
                let lastBeforeOfferPass = base.loadUnaligned(fromByteOffset: sOffset + 136, as: UInt64.self)
                let firstAfterOfferPass = base.loadUnaligned(fromByteOffset: sOffset + 144, as: UInt64.self)
                let passId = base.loadUnaligned(fromByteOffset: sOffset + 152, as: UInt64.self)
                let liveHwPos = base.loadUnaligned(fromByteOffset: sOffset + 160, as: UInt64.self)
                let e0ToImageReadyNs = base.loadUnaligned(fromByteOffset: sOffset + 168, as: Int64.self)
                let offerToExaminedNsMin = base.loadUnaligned(fromByteOffset: sOffset + 176, as: Int64.self)
                let offerToExaminedNsMax = base.loadUnaligned(fromByteOffset: sOffset + 184, as: Int64.self)
                let offerToFirstServiceNsMin = base.loadUnaligned(fromByteOffset: sOffset + 192, as: Int64.self)
                let offerToFirstServiceNsMax = base.loadUnaligned(fromByteOffset: sOffset + 200, as: Int64.self)
                let offerToDescUpdateNsMin = base.loadUnaligned(fromByteOffset: sOffset + 208, as: Int64.self)
                let offerToDescUpdateNsMax = base.loadUnaligned(fromByteOffset: sOffset + 216, as: Int64.self)
                let sealRelToOfferNsMin = base.loadUnaligned(fromByteOffset: sOffset + 224, as: Int64.self)
                let sealRelToOfferNsMax = base.loadUnaligned(fromByteOffset: sOffset + 232, as: Int64.self)
                let uncertainty = base.loadUnaligned(fromByteOffset: sOffset + 240, as: UInt32.self)
                let correlationAgeTicks = base.loadUnaligned(fromByteOffset: sOffset + 244, as: UInt32.self)
                let prodFlags = base.loadUnaligned(fromByteOffset: sOffset + 248, as: UInt32.self)
                let transFlags = base.loadUnaligned(fromByteOffset: sOffset + 252, as: UInt32.self)
                let examCount = base.loadUnaligned(fromByteOffset: sOffset + 256, as: UInt32.self)
                let hwDistPackets = base.loadUnaligned(fromByteOffset: sOffset + 260, as: Int32.self)
                let validityFlags = base.loadUnaligned(fromByteOffset: sOffset + 264, as: UInt16.self)
                let outcomeRaw = base.loadUnaligned(fromByteOffset: sOffset + 266, as: UInt8.self)
                let unresRaw = base.loadUnaligned(fromByteOffset: sOffset + 267, as: UInt8.self)
                let selImg = base.loadUnaligned(fromByteOffset: sOffset + 268, as: UInt8.self)
                let phase = base.loadUnaligned(fromByteOffset: sOffset + 269, as: UInt8.self)
                let packetGen = base.loadUnaligned(fromByteOffset: sOffset + 270, as: UInt8.self)
                let proven = base.loadUnaligned(fromByteOffset: sOffset + 271, as: UInt8.self)
                let acqRes = base.loadUnaligned(fromByteOffset: sOffset + 272, as: UInt8.self)
                let offRes = base.loadUnaligned(fromByteOffset: sOffset + 273, as: UInt8.self)
                let bndRes = base.loadUnaligned(fromByteOffset: sOffset + 274, as: UInt8.self)
                let slRes = base.loadUnaligned(fromByteOffset: sOffset + 275, as: UInt8.self)
                let slRsn = base.loadUnaligned(fromByteOffset: sOffset + 276, as: UInt8.self)
                let obsArb = base.loadUnaligned(fromByteOffset: sOffset + 277, as: UInt8.self)
                let exmArb = base.loadUnaligned(fromByteOffset: sOffset + 278, as: UInt8.self)
                let prodJoin = base.loadUnaligned(fromByteOffset: sOffset + 279, as: UInt8.self)
                let transJoin = base.loadUnaligned(fromByteOffset: sOffset + 280, as: UInt8.self)
                let lastAttempt = base.loadUnaligned(fromByteOffset: sOffset + 281, as: UInt8.self)

                samples.append(TxLatencySample(
                    packetIndex: packetIndex,
                    pcmCommittedStartFrame: startFrame,
                    pcmCommittedEndFrame: endFrame,
                    pubEarliestHostTicks: pubEarliest,
                    pubLatestHostTicks: pubLatest,
                    txCycleStartHostTicks: txTicks,
                    waitMinNanos: waitMin,
                    waitMaxNanos: waitMax,
                    uncertaintyHostTicks: uncertainty,
                    correlationAgeTicks: correlationAgeTicks,
                    outcome: TxLatencyOutcome(rawValue: outcomeRaw) ?? .unknown,
                    unresolvedReason: TxLatencyUnresolvedReason(rawValue: unresRaw) ?? .none,
                    selectedImage: selImg,
                    cyclePhaseMod8: phase,
                    packetGeneration: packetGen,
                    pcmIdentityProven: proven != 0,
                    validityFlags: validityFlags,
                    encodeCompleteHostTicks: encodeCompleteTicks,
                    offerStartHostTicks: offerStartTicks,
                    offerEndHostTicks: offerEndTicks,
                    transExaminedHostTicks: transExaminedTicks,
                    descriptorUpdateHostTicks: descriptorUpdateTicks,
                    sealStartHostTicks: sealStartTicks,
                    sealEndHostTicks: sealEndTicks,
                    lastBeforeOfferHostTicks: lastBeforeOfferTicks,
                    firstAfterOfferHostTicks: firstAfterOfferTicks,
                    lastBeforeOfferPassId: lastBeforeOfferPass,
                    firstAfterOfferPassId: firstAfterOfferPass,
                    passId: passId,
                    liveHwPos: liveHwPos,
                    e0ToImageReadyNanos: e0ToImageReadyNs,
                    offerToExaminedNanosMin: offerToExaminedNsMin,
                    offerToExaminedNanosMax: offerToExaminedNsMax,
                    offerToFirstServiceNanosMin: offerToFirstServiceNsMin,
                    offerToFirstServiceNanosMax: offerToFirstServiceNsMax,
                    offerToDescriptorUpdateNanosMin: offerToDescUpdateNsMin,
                    offerToDescriptorUpdateNanosMax: offerToDescUpdateNsMax,
                    sealRelativeToOfferNanosMin: sealRelToOfferNsMin,
                    sealRelativeToOfferNanosMax: sealRelToOfferNsMax,
                    producerFlags: prodFlags,
                    transportFlags: transFlags,
                    examinationCount: examCount,
                    hwDistancePackets: hwDistPackets,
                    acquireResult: acqRes,
                    offerResult: offRes,
                    bindResult: bndRes,
                    sealResult: slRes,
                    sealReason: slRsn,
                    observedArbPhase: obsArb,
                    examinedArbPhase: exmArb,
                    producerJoinResult: prodJoin,
                    transportJoinResult: transJoin,
                    lastAttemptBindResult: lastAttempt
                ))
            }

            return TxLatencyResultsPage(
                header: header,
                pageIndex: pageIndex,
                totalPages: totalPages,
                samples: samples
            )
        }
    }
}

// MARK: - ASFWDriverConnector Extension

extension ASFWDriverConnector {

    /// Start or arm a TX latency metering session for the given audio endpoint.
    @discardableResult
    func startTxLatencySession(
        endpointID: AudioEndpointID,
        durationSeconds: UInt32 = 5,
        strataSize: UInt32 = 8,
        seed: UInt32 = 0,
        assumedDriftPpm: UInt32 = 100
    ) -> (result: kern_return_t, sessionId: UInt32) {
        guard connection != 0, endpointID.rawValue != 0 else { return (kIOReturnNotOpen, 0) }

        var scalarInput: [UInt64] = [
            endpointID.rawValue,
            UInt64(durationSeconds),
            UInt64(strataSize),
            UInt64(seed),
            UInt64(assumedDriftPpm),
            0 // action: 0 = Start
        ]
        var scalarOutput: [UInt64] = [0]
        var scalarOutputCount: UInt32 = 1

        let result = IOConnectCallScalarMethod(
            connection,
            Method.startTxLatencySession.rawValue,
            &scalarInput,
            UInt32(scalarInput.count),
            &scalarOutput,
            &scalarOutputCount
        )

        let sessionId = (result == KERN_SUCCESS && scalarOutputCount >= 1) ? UInt32(scalarOutput[0]) : 0
        if result != KERN_SUCCESS {
            lastError = "startTxLatencySession failed: \(interpretIOReturn(result))"
        }
        return (result, sessionId)
    }

    /// Stop an active TX latency metering session early.
    @discardableResult
    func stopTxLatencySession(endpointID: AudioEndpointID, sessionId: UInt32 = 0) -> kern_return_t {
        guard connection != 0, endpointID.rawValue != 0 else { return kIOReturnNotOpen }

        var scalarInput: [UInt64] = [
            endpointID.rawValue,
            0, // duration
            0, // strata
            0, // seed
            0, // drift
            1, // action: 1 = Stop
            UInt64(sessionId) // targetSessionId: 0 = active session
        ]

        let result = IOConnectCallScalarMethod(
            connection,
            Method.startTxLatencySession.rawValue,
            &scalarInput,
            UInt32(scalarInput.count),
            nil,
            nil
        )

        if result != KERN_SUCCESS {
            lastError = "stopTxLatencySession failed: \(interpretIOReturn(result))"
        }
        return result
    }

    /// Query one page of TX latency session results.
    func getTxLatencyResultsPage(
        endpointID: AudioEndpointID,
        pageIndex: UInt32,
        samplesPerPage: UInt32 = UInt32(TxLatencyWireDecoder.maxSamplesPerPage),
        sessionId: UInt32 = 0
    ) -> TxLatencyResultsPage? {
        guard connection != 0, endpointID.rawValue != 0 else { return nil }

        var scalarInput: [UInt64] = [
            endpointID.rawValue,
            UInt64(pageIndex),
            UInt64(samplesPerPage),
            UInt64(sessionId)
        ]

        var outData = Data(count: TxLatencyWireDecoder.pageBytes)
        var outSize = outData.count

        let result = outData.withUnsafeMutableBytes { outPtr in
            IOConnectCallMethod(
                connection,
                Method.getTxLatencyResults.rawValue,
                &scalarInput,
                UInt32(scalarInput.count),
                nil, 0, // No input struct
                nil, nil, // No scalar output
                outPtr.baseAddress,
                &outSize
            )
        }

        guard result == KERN_SUCCESS else {
            lastError = "getTxLatencyResultsPage failed: \(interpretIOReturn(result))"
            return nil
        }

        guard outSize == TxLatencyWireDecoder.pageBytes else {
            lastError = "getTxLatencyResultsPage size mismatch: expected \(TxLatencyWireDecoder.pageBytes), got \(outSize)"
            return nil
        }

        return TxLatencyWireDecoder.decodePage(outData)
    }

    /// Fetch all pages of a TX latency session and return a consolidated report.
    func fetchTxLatencySession(endpointID: AudioEndpointID, sessionId: UInt32 = 0) -> TxLatencySessionReport? {
        guard let firstPage = getTxLatencyResultsPage(endpointID: endpointID, pageIndex: 0, sessionId: sessionId) else {
            return nil
        }

        let session = firstPage.header.sessionId
        var allSamples = firstPage.samples
        let totalPages = firstPage.totalPages

        if totalPages > 1 {
            for page in 1..<totalPages {
                if let nextPage = getTxLatencyResultsPage(endpointID: endpointID, pageIndex: page, sessionId: session) {
                    guard nextPage.header.sessionId == session else {
                        lastError = "fetchTxLatencySession session ID mismatch across pages: expected \(session), got \(nextPage.header.sessionId)"
                        return nil
                    }
                    allSamples.append(contentsOf: nextPage.samples)
                } else {
                    break
                }
            }
        }

        return TxLatencySessionReport(header: firstPage.header, samples: allSamples)
    }
}
