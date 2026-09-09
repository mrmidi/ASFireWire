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

    private func fixture(version: UInt32 = 1, sampleCount: UInt32 = 2) -> Data {
        var wire = Data(repeating: 0, count: TxLatencyWireDecoder.pageBytes)

        // Header (192 bytes)
        setLE(version, at: 0, in: &wire)
        setLE(UInt32(4), at: 4, in: &wire) // sessionState: 4 = frozen
        setLE(UInt32(1), at: 8, in: &wire) // termReason: 1 = durationExpired
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

        for i in 0..<8 {
            setLE(UInt32(1000), at: 112 + i * 4, in: &wire) // eligibleByPhase
        }

        // Paging fields at offset 192
        setLE(UInt32(0), at: 192, in: &wire) // pageIndex
        setLE(UInt32(1), at: 196, in: &wire) // totalPages
        setLE(sampleCount, at: 200, in: &wire) // samplesInPage

        // Sample 0 at offset 208
        if sampleCount >= 1 {
            let s0 = 208
            setLE(UInt64(42), at: s0 + 0, in: &wire) // packetIndex
            setLE(UInt64(1000), at: s0 + 8, in: &wire) // startFrame
            setLE(UInt64(1006), at: s0 + 16, in: &wire) // endFrame
            setLE(UInt64(1_010_000), at: s0 + 24, in: &wire) // pubTicks
            setLE(UInt64(1_020_000), at: s0 + 32, in: &wire) // txTicks
            setLE(UInt32(150), at: s0 + 40, in: &wire) // uncertainty
            setLE(UInt32(350_000), at: s0 + 44, in: &wire) // waitMinNanos
            setLE(UInt32(450_000), at: s0 + 48, in: &wire) // waitMaxNanos
            wire[s0 + 52] = 1 // outcome = matched
            wire[s0 + 53] = 0 // unresolvedReason = none
            wire[s0 + 54] = 1 // selectedImage = 1
            wire[s0 + 55] = 3 // arbitrationPhase = 3
            wire[s0 + 56] = 1 // pcmProven = 1
        }

        // Sample 1 at offset 208 + 64 = 272
        if sampleCount >= 2 {
            let s1 = 208 + 64
            setLE(UInt64(50), at: s1 + 0, in: &wire) // packetIndex
            setLE(UInt64(1048), at: s1 + 8, in: &wire) // startFrame
            setLE(UInt64(1054), at: s1 + 16, in: &wire) // endFrame
            setLE(UInt64(1_030_000), at: s1 + 24, in: &wire) // pubTicks
            setLE(UInt64(1_042_000), at: s1 + 32, in: &wire) // txTicks
            setLE(UInt32(160), at: s1 + 40, in: &wire) // uncertainty
            setLE(UInt32(400_000), at: s1 + 44, in: &wire) // waitMinNanos
            setLE(UInt32(500_000), at: s1 + 48, in: &wire) // waitMaxNanos
            wire[s1 + 52] = 1 // outcome = matched
            wire[s1 + 53] = 0 // unresolvedReason = none
            wire[s1 + 54] = 0 // selectedImage = 0
            wire[s1 + 55] = 5 // arbitrationPhase = 5
            wire[s1 + 56] = 1 // pcmProven = 1
        }

        return wire
    }

    @Test func decodesValidTxLatencyPage() throws {
        let wire = fixture()
        let page = try #require(TxLatencyWireDecoder.decodePage(wire))

        #expect(page.header.version == 1)
        #expect(page.header.sessionState == .frozen)
        #expect(page.header.terminationReason == .durationExpired)
        #expect(page.header.endpointId == AudioEndpointID(101))
        #expect(page.header.durationSeconds == 5)
        #expect(page.header.strataSize == 8)
        #expect(page.header.seed == 1337)
        #expect(page.header.assumedDriftPpm == 100)
        #expect(page.header.dataPacketsSeen == 8000)
        #expect(page.header.samplesCaptured == 2)
        #expect(page.header.eligibleByPhase.count == 8)
        #expect(page.header.eligibleByPhase[0] == 1000)

        #expect(page.pageIndex == 0)
        #expect(page.totalPages == 1)
        #expect(page.samples.count == 2)

        let s0 = page.samples[0]
        #expect(s0.packetIndex == 42)
        #expect(s0.pcmCommittedStartFrame == 1000)
        #expect(s0.pcmCommittedEndFrame == 1006)
        #expect(s0.frameSpan == 6)
        #expect(s0.waitMinMicros == 350.0)
        #expect(s0.waitMaxMicros == 450.0)
        #expect(s0.waitCenterMicros == 400.0)
        #expect(s0.uncertaintyMicros == 50.0)
        #expect(s0.outcome == .matched)
        #expect(s0.selectedImage == 1)
        #expect(s0.arbitrationPhase == 3)
        #expect(s0.pcmIdentityProven == true)

        let s1 = page.samples[1]
        #expect(s1.packetIndex == 50)
        #expect(s1.waitCenterMicros == 450.0)
        #expect(s1.selectedImage == 0)
        #expect(s1.arbitrationPhase == 5)
    }

    @Test func rejectsTruncatedOrVersionMismatch() {
        let wire = fixture()
        #expect(TxLatencyWireDecoder.decodePage(wire.dropLast()) == nil)
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
        #expect(csv.contains("# Endpoint ID: 101"))
        #expect(csv.contains("packet_index,pcm_start_frame,pcm_end_frame"))
        #expect(csv.contains("42,1000,1006,6,Matched,None,1,3,1"))
        #expect(csv.contains("50,1048,1054,6,Matched,None,0,5,1"))
    }
}
