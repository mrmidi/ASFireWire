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
        setLE(UInt8(3), at: 208 + 72, in: &data) // unresolved
        setLE(UInt8(9), at: 208 + 73, in: &data) // read collision
        let page = try #require(TxLatencyWireDecoder.decodePage(data))
        #expect(page.samples[0].unresolvedReason == .publicationReadCollision)
        #expect(page.samples[0].unresolvedReason.description == "Publication Read Collision")
        #expect(TxLatencyWireDecoder.decodePage(fixture(version: 3)) == nil)
    }

    private func fixture(version: UInt32 = 4, sampleCount: UInt32 = 2) -> Data {
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

        // Sample 0 at offset 208 (80 bytes)
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
            setLE(UInt32(150), at: s0 + 64, in: &wire) // uncertainty
            setLE(UInt32(100), at: s0 + 68, in: &wire) // correlationAgeTicks
            wire[s0 + 72] = 1 // outcome = matched
            wire[s0 + 73] = 0 // unresolvedReason = none
            wire[s0 + 74] = 1 // selectedImage = 1
            wire[s0 + 75] = 3 // arbitrationPhase = 3
            wire[s0 + 76] = 2 // packetGeneration = 2
            wire[s0 + 77] = 1 // pcmProven = 1
            setLE(UInt16(0x1F), at: s0 + 78, in: &wire) // validityFlags
        }

        // Sample 1 at offset 208 + 80 = 288 (80 bytes)
        if sampleCount >= 2 {
            let s1 = 208 + 80
            setLE(UInt64(50), at: s1 + 0, in: &wire) // packetIndex
            setLE(UInt64(1048), at: s1 + 8, in: &wire) // startFrame
            setLE(UInt64(1054), at: s1 + 16, in: &wire) // endFrame
            setLE(UInt64(1_028_000), at: s1 + 24, in: &wire) // pubEarliest
            setLE(UInt64(1_030_000), at: s1 + 32, in: &wire) // pubLatest
            setLE(UInt64(1_042_000), at: s1 + 40, in: &wire) // txTicks
            setLE(Int64(400_000), at: s1 + 48, in: &wire) // waitMinNanos
            setLE(Int64(500_000), at: s1 + 56, in: &wire) // waitMaxNanos
            setLE(UInt32(160), at: s1 + 64, in: &wire) // uncertainty
            setLE(UInt32(110), at: s1 + 68, in: &wire) // correlationAgeTicks
            wire[s1 + 72] = 1 // outcome = matched
            wire[s1 + 73] = 0 // unresolvedReason = none
            wire[s1 + 74] = 0 // selectedImage = 0
            wire[s1 + 75] = 5 // arbitrationPhase = 5
            wire[s1 + 76] = 2 // packetGeneration = 2
            wire[s1 + 77] = 1 // pcmProven = 1
            setLE(UInt16(0x1F), at: s1 + 78, in: &wire) // validityFlags
        }

        return wire
    }

    @Test func decodesValidTxLatencyPage() throws {
        let wire = fixture()
        let page = try #require(TxLatencyWireDecoder.decodePage(wire))

        #expect(page.header.version == 4)
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
        #expect(s0.arbitrationPhase == 3)
        #expect(s0.packetGeneration == 2)
        #expect(s0.pcmIdentityProven == true)
        #expect(s0.validityFlags == 0x1F)

        let s1 = page.samples[1]
        #expect(s1.packetIndex == 50)
        #expect(s1.waitCenterMicros == 450.0)
        #expect(s1.selectedImage == 0)
        #expect(s1.arbitrationPhase == 5)
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
    }
}
