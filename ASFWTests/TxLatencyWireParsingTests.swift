//
//  TxLatencyWireParsingTests.swift
//  ASFWTests
//
//  Created for ASFireWire Project.
//  Tests for TX Latency Session wire deserialization and statistics.
//

import Foundation
import Testing
@testable import ASFW

struct TxLatencyWireParsingTests {

    private func setLE<T: FixedWidthInteger>(_ value: T, at offset: Int, in data: inout Data) {
        var raw = value.littleEndian
        withUnsafeBytes(of: &raw) { bytes in
            data.replaceSubrange(offset..<(offset + bytes.count), with: bytes)
        }
    }

    @Test func distinguishesReadCollisionFromEviction() throws {
        var data = fixture(sampleCount: 1)
        setLE(UInt8(3), at: 208 + 266, in: &data) // unresolved
        setLE(UInt8(9), at: 208 + 267, in: &data) // read collision
        let page = try #require(TxLatencyWireDecoder.decodePage(data))
        #expect(page.samples[0].unresolvedReason == .publicationReadCollision)
        #expect(page.samples[0].unresolvedReason.description == "Publication Read Collision")
        #expect(TxLatencyWireDecoder.decodePage(fixture(version: 4)) == nil)
    }

    private func fixture(version: UInt32 = 5, sampleCount: UInt32 = 2) -> Data {
        var wire = Data(repeating: 0, count: TxLatencyWireDecoder.pageBytes)

        // Header (192 bytes)
        setLE(version, at: 0, in: &wire)
        setLE(UInt32(4), at: 4, in: &wire) // sessionState: 4 = frozen
        setLE(UInt32(2), at: 8, in: &wire) // termReason: 2 = deadlineExpired
        setLE(UInt32(42), at: 12, in: &wire) // sessionId: 42
        setLE(UInt64(101), at: 16, in: &wire) // endpointId
        setLE(UInt64(1), at: 24, in: &wire) // epoch
        setLE(UInt64(1_000_000), at: 32, in: &wire) // startHostTicks
        setLE(UInt64(2_000_000), at: 40, in: &wire) // deadlineHostTicks
        setLE(UInt64(2_000_100), at: 48, in: &wire) // frozenHostTicks
        setLE(UInt32(5), at: 56, in: &wire) // durationSeconds
        setLE(UInt32(8), at: 60, in: &wire) // strataSize
        setLE(UInt32(1337), at: 64, in: &wire) // seed
        setLE(UInt32(100), at: 68, in: &wire) // assumedDriftPpm
        setLE(UInt64(8000), at: 72, in: &wire) // dataPacketsSeen
        setLE(sampleCount, at: 80, in: &wire) // samplesCaptured
        setLE(UInt32(0), at: 84, in: &wire) // stampsMissedCount
        setLE(sampleCount, at: 88, in: &wire) // resolvedCount
        setLE(UInt32(0), at: 92, in: &wire) // unresolvedCount
        setLE(UInt32(0), at: 96, in: &wire) // transmitFailedCount
        setLE(UInt32(0), at: 100, in: &wire) // substitutionCount
        setLE(UInt32(0), at: 104, in: &wire) // invalidCount
        setLE(UInt32(48000), at: 108, in: &wire) // sampleRateHz

        for i in 0..<8 {
            setLE(UInt32(1000), at: 112 + i * 4, in: &wire) // eligibleByPhase
        }

        setLE(UInt32((504 << 16) | 1008), at: 188, in: &wire) // geometryProvenance: hwRing=504, lead=1008

        // Paging fields at offset 192
        setLE(UInt32(0), at: 192, in: &wire) // pageIndex
        setLE(UInt32(1), at: 196, in: &wire) // totalPages
        setLE(sampleCount, at: 200, in: &wire) // samplesInPage

        // Sample 0 at offset 208. Stride is TxLatencySampleWire's size, which
        // the C++ side pins with static_asserts; this fixture is the check
        // from the other side of the seam.
        if sampleCount >= 1 {
            let s0 = 208
            setLE(UInt64(42), at: s0 + 0, in: &wire) // packetIndex
            setLE(UInt64(1000), at: s0 + 8, in: &wire) // startFrame
            setLE(UInt64(1006), at: s0 + 16, in: &wire) // endFrame
            setLE(UInt64(1_008_000), at: s0 + 24, in: &wire) // pubEarliest
            setLE(UInt64(1_010_000), at: s0 + 32, in: &wire) // pubLatest
            setLE(UInt64(1_020_000), at: s0 + 40, in: &wire) // txTicks
            setLE(Int64(350_000), at: s0 + 48, in: &wire) // waitMinNanos
            setLE(Int64(450_000), at: s0 + 56, in: &wire) // waitMaxNanos
            setLE(UInt64(1_009_000), at: s0 + 64, in: &wire) // encodeComplete
            setLE(UInt64(1_011_000), at: s0 + 72, in: &wire) // offerStart
            setLE(UInt64(1_011_500), at: s0 + 80, in: &wire) // offerEnd
            setLE(UInt64(1_013_000), at: s0 + 88, in: &wire) // transExamined
            setLE(UInt64(1_012_500), at: s0 + 96, in: &wire) // descriptorUpdate
            setLE(UInt64(0), at: s0 + 104, in: &wire) // sealStart
            setLE(UInt64(0), at: s0 + 112, in: &wire) // sealEnd
            setLE(UInt64(1_010_500), at: s0 + 120, in: &wire) // lastBeforeOffer
            setLE(UInt64(1_012_000), at: s0 + 128, in: &wire) // firstAfterOffer
            setLE(UInt64(6), at: s0 + 136, in: &wire) // lastBeforeOfferPass
            setLE(UInt64(7), at: s0 + 144, in: &wire) // firstAfterOfferPass
            setLE(UInt64(7), at: s0 + 152, in: &wire) // passId
            setLE(UInt64(37), at: s0 + 160, in: &wire) // liveHwPos
            setLE(Int64(-1_000), at: s0 + 168, in: &wire) // e0ToImageReady
            setLE(Int64(1_500), at: s0 + 176, in: &wire) // offerToExamined min
            setLE(Int64(2_000), at: s0 + 184, in: &wire) // offerToExamined max
            setLE(Int64(500), at: s0 + 192, in: &wire) // offerToFirstService min
            setLE(Int64(1_000), at: s0 + 200, in: &wire) // offerToFirstService max
            setLE(Int64(1_000), at: s0 + 208, in: &wire) // offerToDescUpdate min
            setLE(Int64(1_500), at: s0 + 216, in: &wire) // offerToDescUpdate max
            setLE(Int64(0), at: s0 + 224, in: &wire) // sealRelToOffer min
            setLE(Int64(0), at: s0 + 232, in: &wire) // sealRelToOffer max
            setLE(UInt32(150), at: s0 + 240, in: &wire) // uncertainty
            setLE(UInt32(100), at: s0 + 244, in: &wire) // correlationAgeTicks
            setLE(UInt32(0x7), at: s0 + 248, in: &wire) // producerFlags
            setLE(UInt32(0x6), at: s0 + 252, in: &wire) // transportFlags
            setLE(UInt32(3), at: s0 + 256, in: &wire) // examinationCount
            setLE(Int32(5), at: s0 + 260, in: &wire) // hwDistancePackets
            setLE(UInt16(0x1F), at: s0 + 264, in: &wire) // validityFlags
            wire[s0 + 266] = 1 // outcome = matched
            wire[s0 + 267] = 0 // unresolvedReason = none
            wire[s0 + 268] = 1 // selectedImage = 1
            wire[s0 + 269] = 3 // cyclePhaseMod8 = 3
            wire[s0 + 270] = 2 // packetGeneration = 2
            wire[s0 + 271] = 1 // pcmProven = 1
            wire[s0 + 272] = 0 // acquireResult = Success
            wire[s0 + 273] = 1 // offerResult = won
            wire[s0 + 274] = 1 // bindResult = Bound
            wire[s0 + 275] = 0 // sealResult
            wire[s0 + 276] = 0 // sealReason
            wire[s0 + 277] = 0 // observedArbPhase
            wire[s0 + 278] = 1 // examinedArbPhase
            wire[s0 + 279] = 0 // producerJoinResult = Valid
            wire[s0 + 280] = 0 // transportJoinResult = Valid
            wire[s0 + 281] = 1 // lastAttemptBindResult = Bound
        }

        if sampleCount >= 2 {
            let s1 = 208 + TxLatencyWireDecoder.sampleBytes
            setLE(UInt64(50), at: s1 + 0, in: &wire) // packetIndex
            setLE(UInt64(1048), at: s1 + 8, in: &wire) // startFrame
            setLE(UInt64(1054), at: s1 + 16, in: &wire) // endFrame
            setLE(UInt64(1_028_000), at: s1 + 24, in: &wire) // pubEarliest
            setLE(UInt64(1_030_000), at: s1 + 32, in: &wire) // pubLatest
            setLE(UInt64(1_042_000), at: s1 + 40, in: &wire) // txTicks
            setLE(Int64(400_000), at: s1 + 48, in: &wire) // waitMinNanos
            setLE(Int64(500_000), at: s1 + 56, in: &wire) // waitMaxNanos
            setLE(UInt32(160), at: s1 + 240, in: &wire) // uncertainty
            setLE(UInt32(110), at: s1 + 244, in: &wire) // correlationAgeTicks
            setLE(UInt16(0x1F), at: s1 + 264, in: &wire) // validityFlags
            wire[s1 + 266] = 1 // outcome = matched
            wire[s1 + 267] = 0 // unresolvedReason = none
            wire[s1 + 268] = 0 // selectedImage = 0
            wire[s1 + 269] = 5 // cyclePhaseMod8 = 5
            wire[s1 + 270] = 2 // packetGeneration = 2
            wire[s1 + 271] = 1 // pcmProven = 1
            // Deliberately left as a lane with no decision evidence: join
            // results of 1 (NeverWritten) must survive decoding distinct from
            // a valid join whose fields happen to be zero.
            wire[s1 + 279] = 1 // producerJoinResult = NeverWritten
            wire[s1 + 280] = 1 // transportJoinResult = NeverWritten
        }

        return wire
    }

    @Test func decodesValidTxLatencyPage() throws {
        let wire = fixture()
        let page = try #require(TxLatencyWireDecoder.decodePage(wire))

        #expect(page.header.version == 5)
        #expect(page.header.sessionState == .frozen)
        #expect(page.header.terminationReason == .deadlineExpired)
        #expect(page.header.sessionId == 42)
        #expect(page.header.endpointId == AudioEndpointID(101))
        #expect(page.header.durationSeconds == 5)
        #expect(page.header.strataSize == 8)
        #expect(page.header.seed == 1337)
        #expect(page.header.assumedDriftPpm == 100)
        #expect(page.header.dataPacketsSeen == 8000)
        #expect(page.header.samplesCaptured == 2)
        #expect(page.header.eligibleByPhase.count == 8)
        #expect(page.header.eligibleByPhase[0] == 1000)

        #expect(page.header.sampleRateHz == 48000)
        #expect(page.header.geometryProvenance == (504 << 16) | 1008)
        #expect(page.header.preparationLeadPackets == 1008)
        #expect(page.header.hardwareRingPackets == 504)

        #expect(page.pageIndex == 0)
        #expect(page.totalPages == 1)
        #expect(page.samples.count == 2)

        let s0 = page.samples[0]
        #expect(s0.packetIndex == 42)
        #expect(s0.pcmCommittedStartFrame == 1000)
        #expect(s0.pcmCommittedEndFrame == 1006)
        #expect(s0.frameSpan == 6)
        #expect(s0.pubEarliestHostTicks == 1_008_000)
        #expect(s0.pubLatestHostTicks == 1_010_000)
        #expect(s0.waitMinNanos == 350_000)
        #expect(s0.waitMaxNanos == 450_000)
        #expect(s0.waitMinMicros == 350.0)
        #expect(s0.waitMaxMicros == 450.0)
        #expect(s0.waitCenterMicros == 400.0)
        #expect(s0.uncertaintyMicros == 50.0)
        #expect(s0.correlationAgeTicks == 100)
        #expect(s0.outcome == .matched)
        #expect(s0.selectedImage == 1)
        #expect(s0.cyclePhaseMod8 == 3)
        #expect(s0.encodeCompleteHostTicks == 1_009_000)
        #expect(s0.offerStartHostTicks == 1_011_000)
        #expect(s0.offerEndHostTicks == 1_011_500)
        #expect(s0.transExaminedHostTicks == 1_013_000)
        #expect(s0.descriptorUpdateHostTicks == 1_012_500)
        #expect(s0.lastBeforeOfferHostTicks == 1_010_500)
        #expect(s0.firstAfterOfferHostTicks == 1_012_000)
        #expect(s0.lastBeforeOfferPassId == 6)
        #expect(s0.firstAfterOfferPassId == 7)
        #expect(s0.passId == 7)
        #expect(s0.liveHwPos == 37)
        #expect(s0.e0ToImageReadyNanos == -1_000)
        #expect(s0.offerToExaminedNanosMin == 1_500)
        #expect(s0.offerToExaminedNanosMax == 2_000)
        #expect(s0.offerToFirstServiceNanosMin == 500)
        #expect(s0.offerToFirstServiceNanosMax == 1_000)
        #expect(s0.offerToDescriptorUpdateNanosMin == 1_000)
        #expect(s0.offerToDescriptorUpdateNanosMax == 1_500)
        #expect(s0.producerFlags == 0x7)
        #expect(s0.transportFlags == 0x6)
        #expect(s0.examinationCount == 3)
        #expect(s0.hwDistancePackets == 5)
        #expect(s0.acquireResult == 0)
        #expect(s0.offerResult == 1)
        #expect(s0.bindResult == 1)
        #expect(s0.examinedArbPhase == 1)
        #expect(s0.producerJoinResult == 0)
        #expect(s0.transportJoinResult == 0)
        #expect(s0.lastAttemptBindResult == 1)
        // validityFlags 0x1F carries no sealValid bit: this packet was bound,
        // and a bound packet is terminal without ever having been sealed.
        #expect(s0.hasSeal == false)
        // Both bounds strictly positive: the offer provably preceded first
        // service, rather than the two brackets merely overlapping.
        #expect(s0.offerToFirstServiceOrderEstablished == true)
        #expect(s0.packetGeneration == 2)
        #expect(s0.pcmIdentityProven == true)
        #expect(s0.validityFlags == 0x1F)

        let s1 = page.samples[1]
        #expect(s1.packetIndex == 50)
        #expect(s1.waitCenterMicros == 450.0)
        #expect(s1.selectedImage == 0)
        #expect(s1.cyclePhaseMod8 == 5)
        // A lane with no evidence decodes as NeverWritten, not as a valid join
        // whose numbers all happen to be zero.
        #expect(s1.producerJoinResult == 1)
        #expect(s1.transportJoinResult == 1)
        #expect(s1.offerToFirstServiceOrderEstablished == false)
        #expect(s1.packetGeneration == 2)
        #expect(s1.validityFlags == 0x1F)
    }

    @Test func decodesSignedNegativeWaitTime() throws {
        var wire = fixture(sampleCount: 1)
        let s0 = 208
        setLE(Int64(-25_000), at: s0 + 48, in: &wire) // waitMinNanos negative
        setLE(Int64(75_000), at: s0 + 56, in: &wire)  // waitMaxNanos positive
        let page = try #require(TxLatencyWireDecoder.decodePage(wire))
        let s = page.samples[0]
        #expect(s.waitMinNanos == -25_000)
        #expect(s.waitMaxNanos == 75_000)
        #expect(s.waitMinMicros == -25.0)
        #expect(s.waitMaxMicros == 75.0)
        #expect(s.waitCenterMicros == 25.0)
        #expect(s.uncertaintyMicros == 50.0)
    }

    @Test func rejectsTruncatedOrVersionMismatch() {
        let wire = fixture()
        #expect(TxLatencyWireDecoder.decodePage(wire.dropLast()) == nil)
        #expect(TxLatencyWireDecoder.decodePage(fixture(version: 2)) == nil)
        #expect(TxLatencyWireDecoder.decodePage(fixture(version: 4)) == nil)
        #expect(TxLatencyWireDecoder.decodePage(fixture(version: 99)) == nil)
    }

    @Test func percentileAndStatsCalculation() throws {
        let wire = fixture()
        let page = try #require(TxLatencyWireDecoder.decodePage(wire))
        let report = TxLatencySessionReport(header: page.header, samples: page.samples)

        let stats = report.matchedStats
        #expect(stats.sampleCount == 2)
        #expect(stats.minWaitMicros == 350.0)
        #expect(stats.maxWaitMicros == 500.0)
        // Two samples: 400.0 and 450.0 -> median is 425.0
        #expect(stats.medianWaitMicros == 425.0)
        #expect(stats.meanWaitMicros == 425.0)
        #expect(stats.meanUncertaintyMicros == 50.0)
    }

    @Test func csvExportFormatting() throws {
        let wire = fixture()
        let page = try #require(TxLatencyWireDecoder.decodePage(wire))
        let report = TxLatencySessionReport(header: page.header, samples: page.samples)
        let csv = report.toCSV()

        #expect(csv.contains("# ASFW FireWire TX Latency Session Report (E0 -> E2)"))
        #expect(csv.contains("# Session ID: 42"))
        #expect(csv.contains("# Endpoint ID: 101"))
        #expect(csv.contains("# Rate: 48000 Hz, Geometry: lead=1008pkts, hwRing=504pkts"))
        #expect(csv.contains("packet_index,pcm_start_frame,pcm_end_frame"))
        #expect(csv.contains("42,1000,1006,6,Matched,None,1,3,2,1,31,100,1008000,1010000,1020000,150,350000,450000,350.000,450.000,400.000,50.000"))
        // Bounds are exported as a pair, never as one signed delta.
        #expect(csv.contains("offer_to_first_service_us_min,offer_to_first_service_us_max"))
        #expect(csv.contains("producer_join,transport_join"))
    }

    @Test func boundPacketDoesNotReportASeal() throws {
        // A successful bind produces a terminal event and no seal. The decoder
        // must take that from the sealValid flag, not from the presence of
        // decision evidence, or every working packet reports a seal whose
        // timestamps are zero.
        var wire = fixture(sampleCount: 1)
        let s0 = 208
        setLE(UInt16(TxLatencyFlags.pubValid | TxLatencyFlags.txValid
                     | TxLatencyFlags.waitValid | TxLatencyFlags.bindValid
                     | TxLatencyFlags.transportDecisionValid),
              at: s0 + 264, in: &wire)
        let page = try #require(TxLatencyWireDecoder.decodePage(wire))
        let s = page.samples[0]
        #expect(s.hasSeal == false)
        #expect(s.hasTransportDecision == true)
        #expect(s.decisionEvidenceSummary.contains("Seal:") == false)
    }

    @Test func failedAttemptIsDistinctFromNoService() throws {
        // bindResult 0 (no terminal decision) but a concluded attempt: the
        // packet was reached and the attempt failed, which must not read the
        // same as never having been serviced.
        var wire = fixture(sampleCount: 1)
        let s0 = 208
        setLE(UInt16(TxLatencyFlags.transportDecisionValid), at: s0 + 264, in: &wire)
        wire[s0 + 274] = 0 // bindResult = NotExamined (no terminal decision)
        wire[s0 + 281] = 3 // lastAttemptBindResult = RejectedUnavailableHwPos
        let page = try #require(TxLatencyWireDecoder.decodePage(wire))
        let s = page.samples[0]
        #expect(s.bindResult == 0)
        #expect(s.lastAttemptBindResult == 3)
        #expect(s.decisionEvidenceSummary.contains("Attempt:3"))

        var quiet = fixture(sampleCount: 1)
        setLE(UInt16(TxLatencyFlags.transportDecisionValid), at: s0 + 264, in: &quiet)
        quiet[s0 + 274] = 0
        quiet[s0 + 281] = 0
        let quietPage = try #require(TxLatencyWireDecoder.decodePage(quiet))
        #expect(quietPage.samples[0].decisionEvidenceSummary.contains("Attempt:") == false)
    }
}
