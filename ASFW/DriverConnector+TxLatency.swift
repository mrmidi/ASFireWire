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
    let arbitrationPhase: UInt8
    let packetGeneration: UInt8
    let pcmIdentityProven: Bool
    let validityFlags: UInt16

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
        lines.append("packet_index,pcm_start_frame,pcm_end_frame,frame_span,outcome,unresolved_reason,selected_image,arbitration_phase,packet_generation,pcm_proven,validity_flags,correlation_age_ticks,pub_earliest_host_ticks,pub_latest_host_ticks,tx_host_ticks,uncertainty_ticks,wait_min_ns,wait_max_ns,wait_min_us,wait_max_us,wait_center_us,uncertainty_us")

        for s in samples {
            let row = [
                "\(s.packetIndex)",
                "\(s.pcmCommittedStartFrame)",
                "\(s.pcmCommittedEndFrame)",
                "\(s.frameSpan)",
                "\(s.outcome)",
                "\(s.unresolvedReason)",
                "\(s.selectedImage)",
                "\(s.arbitrationPhase)",
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
                String(format: "%.3f", s.uncertaintyMicros)
            ].joined(separator: ",")
            lines.append(row)
        }
        return lines.joined(separator: "\n")
    }
}

// MARK: - Binary Wire Decoder

enum TxLatencyWireDecoder {
    static let wireVersion: UInt32 = 4
    static let maxSamplesPerPage: Int = 32
    static let sampleBytes: Int = 80
    static let headerBytes: Int = 192
    static let pageBytes: Int = 192 + 16 + (32 * 80) // 2768 bytes

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
                let uncertainty = base.loadUnaligned(fromByteOffset: sOffset + 64, as: UInt32.self)
                let correlationAgeTicks = base.loadUnaligned(fromByteOffset: sOffset + 68, as: UInt32.self)
                let outcomeRaw = base.loadUnaligned(fromByteOffset: sOffset + 72, as: UInt8.self)
                let unresRaw = base.loadUnaligned(fromByteOffset: sOffset + 73, as: UInt8.self)
                let selImg = base.loadUnaligned(fromByteOffset: sOffset + 74, as: UInt8.self)
                let phase = base.loadUnaligned(fromByteOffset: sOffset + 75, as: UInt8.self)
                let packetGen = base.loadUnaligned(fromByteOffset: sOffset + 76, as: UInt8.self)
                let proven = base.loadUnaligned(fromByteOffset: sOffset + 77, as: UInt8.self)
                let validityFlags = base.loadUnaligned(fromByteOffset: sOffset + 78, as: UInt16.self)

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
                    arbitrationPhase: phase,
                    packetGeneration: packetGen,
                    pcmIdentityProven: proven != 0,
                    validityFlags: validityFlags
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
