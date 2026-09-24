//
//  AudioTelemetryWireTests.swift
//  ASFWTests
//
//  Decodes the golden audio telemetry v4 fixture written by the C++ host test
//  (tests/audio/AudioRuntime/AudioTelemetryWireTests.cpp). Both sides derive
//  every value from the field's byte offset, so a decoder offset that drifts
//  from the driver's struct fails here.
//

import Foundation
import Testing
@testable import ASFW

struct AudioTelemetryWireTests {
    private static let fixtureURL = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()
        .deletingLastPathComponent()
        .appendingPathComponent("tests/fixtures/audio_telemetry_v4.bin")

    // Must match GoldenU64/GoldenU32 in the C++ test.
    private func u64(_ endpoint: Int, _ offset: Int) -> UInt64 { (UInt64(endpoint + 1) << 40) + UInt64(offset) }
    private func u32(_ endpoint: Int, _ offset: Int) -> UInt32 { (UInt32(endpoint + 1) << 16) + UInt32(offset) }

    private func fixture() throws -> Data { try Data(contentsOf: Self.fixtureURL) }

    @Test func decodesHeaderAndEveryField() throws {
        let snapshot = try #require(AudioTelemetryWireDecoder.decode(try fixture()))
        #expect(snapshot.captureHostTicks == 0x0123_4567_89AB_CDEF)
        #expect(snapshot.hostTimebaseNumer == 125)
        #expect(snapshot.hostTimebaseDenom == 3)
        #expect(snapshot.endpoints.count == 2)

        for (e, ep) in snapshot.endpoints.enumerated() {
            #expect(ep.guid == u64(e, 0))
            #expect(ep.endpointGeneration == u64(e, 8))
            #expect(ep.controlGeneration == u64(e, 16))
            #expect(ep.completedIntervalSequence == u64(e, 24))
            #expect(ep.lastPreparationLatencyTicks == u64(e, 32))
            #expect(ep.completedIntervalMaxLatencyTicks == u64(e, 40))
            #expect(ep.maxPreparationLatencyTicks == u64(e, 48))
            #expect(ep.preparationWakeCount == u64(e, 56))
            #expect(ep.preparationAtMost750Us == u64(e, 64))
            #expect(ep.preparationAtLeast1500Us == u64(e, 72))
            #expect(ep.rxReplayEntries == u64(e, 80))
            #expect(ep.rxReplayEpochResets == u64(e, 88))
            #expect(ep.completedLatencyHistogram == (0..<6).map { u64(e, 96 + $0 * 8) })
            #expect(ep.completedMarginHistogram == (0..<5).map { u64(e, 144 + $0 * 8) })
            #expect(ep.flags == 0x7F)
            #expect(ep.sampleRateHz == u32(e, 188))
            #expect(ep.outputChannels == u32(e, 192))
            #expect(ep.inputChannels == u32(e, 196))
            #expect(ep.currentCommittedMarginPackets == u32(e, 200))
            #expect(ep.completedIntervalMarginMinPackets == u32(e, 204))
            #expect(ep.completedIntervalMarginMaxPackets == u32(e, 208))
            #expect(ep.minimumCommittedMarginPackets == u32(e, 212))
            #expect(ep.preparationLeadPackets == u32(e, 216))
            #expect(ep.hardwareFloorPackets == u32(e, 220))
            #expect(ep.rxCurrentAvailableFrames == u64(e, 224))
            #expect(ep.rxCompletedIntervalSequence == u64(e, 232))
            #expect(ep.rxCompletedIntervalMinimumAvailableFrames == u64(e, 240))
            #expect(ep.rxCompletedIntervalMaximumAvailableFrames == u64(e, 248))
            #expect(ep.rxCompletedIntervalMinimumFreeHeadroomFrames == u64(e, 256))
            #expect(ep.rxCompletedIntervalOverrunEvents == u64(e, 264))
            #expect(ep.rxCompletedIntervalOverwrittenFrames == u64(e, 272))
            #expect(ep.rxCompletedIntervalStarvationEvents == u64(e, 280))
            #expect(ep.rxCompletedIntervalStarvedFrames == u64(e, 288))
            #expect(ep.rxCaptureOverrunEvents == u64(e, 296))
            #expect(ep.rxCaptureStarvationEvents == u64(e, 304))
            #expect(ep.rxTotalOverwrittenFrames == u64(e, 312))
            #expect(ep.rxTotalStarvedFrames == u64(e, 320))
            #expect(ep.rxCompletedOccupancyHistogram == (0..<5).map { u64(e, 328 + $0 * 8) })
            #expect(ep.inputFrameCapacityFrames == u32(e, 368))
            #expect(ep.rxPacketsSeen == u64(e, 376))
            #expect(ep.rxDataPackets == u64(e, 384))
            #expect(ep.rxNoDataPackets == u64(e, 392))
            #expect(ep.rxShortPackets == u64(e, 400))
            #expect(ep.rxInvalidCipHeaders == u64(e, 408))
            #expect(ep.rxZeroDataBlockSize == u64(e, 416))
            #expect(ep.rxGeometryMismatch == u64(e, 424))
            #expect(ep.txCompletedIntervalDurationTicks == u64(e, 432))
            #expect(ep.txCompletedIntervalEndHostTicks == u64(e, 440))
            #expect(ep.rxCompletedIntervalDurationTicks == u64(e, 448))
            #expect(ep.rxCompletedIntervalEndHostTicks == u64(e, 456))
            let expectedSeconds = Double(u64(e, 432)) * 125 / 3 / 1_000_000_000
            #expect(abs((ep.txCompletedIntervalSeconds ?? 0) - expectedSeconds) < 1e-9 * expectedSeconds)
        }
    }

    @Test func rejectsOlderVersionsAndInconsistentSizes() throws {
        var data = try fixture()
        data[0] = 3
        #expect(AudioTelemetryWireDecoder.decode(data) == nil)

        let truncated = try fixture().prefix(100)
        #expect(AudioTelemetryWireDecoder.decode(Data(truncated)) == nil)

        var badTotal = try fixture()
        badTotal[4] &+= 1
        #expect(AudioTelemetryWireDecoder.decode(badTotal) == nil)
    }

    /// A newer driver that appends fields to each record still decodes: the
    /// stride comes from the header, not from this app's struct.
    @Test func toleratesLongerRecordsFromANewerDriver() throws {
        let original = try fixture()
        let header = 32, record = 464, extra = 16
        var grown = Data(original.prefix(header))
        for index in 0..<2 {
            grown.append(original.subdata(in: (header + index * record)..<(header + (index + 1) * record)))
            grown.append(Data(repeating: 0xEE, count: extra))
        }
        let newRecord = UInt32(record + extra)
        let newTotal = UInt32(header + 2 * (record + extra))
        grown.replaceSubrange(12..<16, with: withUnsafeBytes(of: newRecord.littleEndian) { Data($0) })
        grown.replaceSubrange(4..<8, with: withUnsafeBytes(of: newTotal.littleEndian) { Data($0) })

        let snapshot = try #require(AudioTelemetryWireDecoder.decode(grown))
        #expect(snapshot.endpoints.map(\.guid) == [u64(0, 0), u64(1, 0)])
        #expect(snapshot.endpoints[1].rxCompletedIntervalEndHostTicks == u64(1, 456))
    }
}
