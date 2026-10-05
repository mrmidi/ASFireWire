//
//  AudioGeometryObserverTests.swift
//  ASFWTests
//
//  Tests for AudioCadenceObserver, AudioMarginObserver, and AudioGeometrySnapshot.
//

import Foundation
import Testing
@testable import ASFW

struct AudioGeometryObserverTests {

    // MARK: - Cadence

    @Test func cadenceIsDerivedFromDifferencesNotAbsoluteValues() {
        var o = AudioCadenceObserver()
        o.sample(txPackets: 0, rxPackets: 0, atUptime: 10)
        o.sample(txPackets: 8000, rxPackets: 8000, atUptime: 11)
        #expect(o.txPacketsPerSecond == 8000)
        #expect(o.rxPacketsPerSecond == 8000)
        #expect(o.samples == 1)

        let irq = o.interruptsPerSecond(packetsPerGroup: 8, packetRate: o.txPacketsPerSecond)
        #expect(irq != nil)
        #expect(abs(irq! - 1000.0) < 0.001)
    }

    @Test func frozenCursorReportsZeroNotNil() {
        var o = AudioCadenceObserver()
        o.sample(txPackets: 4242, rxPackets: 4242, atUptime: 10)
        o.sample(txPackets: 4242, rxPackets: 4242, atUptime: 11)
        #expect(o.txPacketsPerSecond == 0)
        #expect(o.rxPacketsPerSecond == 0)
    }

    @Test func counterResetSuppressesTheRateRatherThanInventingOne() {
        var o = AudioCadenceObserver()
        o.sample(txPackets: 100_000, rxPackets: 100_000, atUptime: 10)
        o.sample(txPackets: 0, rxPackets: 0, atUptime: 11)
        #expect(o.txPacketsPerSecond == nil)
        #expect(o.rxPacketsPerSecond == nil)

        // Re-baselines cleanly from the new origin
        o.sample(txPackets: 8000, rxPackets: 8000, atUptime: 12)
        #expect(o.txPacketsPerSecond == 8000)
    }

    @Test func unusableIntervalsAreRefused() {
        var tooLong = AudioCadenceObserver()
        tooLong.sample(txPackets: 0, rxPackets: 0, atUptime: 10)
        tooLong.sample(txPackets: 8_000_000, rxPackets: 8_000_000, atUptime: 1010)
        #expect(tooLong.txPacketsPerSecond == nil)
        #expect(tooLong.samples == 0)

        var tooShort = AudioCadenceObserver()
        tooShort.sample(txPackets: 0, rxPackets: 0, atUptime: 10)
        tooShort.sample(txPackets: 8, rxPackets: 8, atUptime: 10.01)
        #expect(tooShort.txPacketsPerSecond == nil)
    }

    @Test func directionsAreReportedIndependently() {
        var o = AudioCadenceObserver()
        o.sample(txPackets: 0, rxPackets: 0, atUptime: 10)
        o.sample(txPackets: 0, rxPackets: 8000, atUptime: 11)
        #expect(o.txPacketsPerSecond == 0)
        #expect(o.rxPacketsPerSecond == 8000)
    }

    @Test func interruptRateNeedsAGroupSize() {
        let o = AudioCadenceObserver()
        #expect(o.interruptsPerSecond(packetsPerGroup: 0, packetRate: 8000) == nil)
        #expect(o.interruptsPerSecond(packetsPerGroup: 8, packetRate: nil) == nil)
    }

    // MARK: - Margin

    @Test func firstContactSkipsTheIntervalAlreadyOnTheWire() {
        var o = AudioMarginObserver()
        o.sample(appliedSequence: 1, intervalSequence: 2, intervalMinimum: 7)
        #expect(o.didResetOnLastSample)
        #expect(o.worstIntervalMarginPackets == nil)
        #expect(o.intervalsObserved == 0)

        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 7)
        #expect(!o.didResetOnLastSample)
        #expect(o.worstIntervalMarginPackets == 7)
        #expect(o.intervalsObserved == 1)
    }

    @Test func applyingNewGeometryDiscardsTheOldHistory() {
        var o = AudioMarginObserver()
        o.sample(appliedSequence: 1, intervalSequence: 2, intervalMinimum: 40)
        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 40)
        o.sample(appliedSequence: 1, intervalSequence: 6, intervalMinimum: 12)
        #expect(o.worstIntervalMarginPackets == 12)
        #expect(o.intervalsObserved == 2)

        o.sample(appliedSequence: 2, intervalSequence: 8, intervalMinimum: 90)
        #expect(o.didResetOnLastSample)
        #expect(o.worstIntervalMarginPackets == nil)
        #expect(o.intervalsObserved == 0)

        o.sample(appliedSequence: 2, intervalSequence: 10, intervalMinimum: 90)
        #expect(o.worstIntervalMarginPackets == 90)
        #expect(o.intervalsObserved == 1)
    }

    @Test func repeatedPollsOfOneIntervalCountOnce() {
        var o = AudioMarginObserver()
        o.sample(appliedSequence: 1, intervalSequence: 2, intervalMinimum: 30)
        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 30)
        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 30)
        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 30)
        #expect(o.intervalsObserved == 1)
        #expect(o.worstIntervalMarginPackets == 30)
    }

    @Test func worstCaseSurvivesLaterHealthyIntervals() {
        var o = AudioMarginObserver()
        o.sample(appliedSequence: 1, intervalSequence: 2, intervalMinimum: 96)
        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 96)
        o.sample(appliedSequence: 1, intervalSequence: 6, intervalMinimum: 14)
        o.sample(appliedSequence: 1, intervalSequence: 8, intervalMinimum: 96)
        #expect(o.worstIntervalMarginPackets == 14)
        #expect(o.intervalsObserved == 3)
    }

    @Test func unmeasuredIntervalsDoNotCountAsZeroMargin() {
        var o = AudioMarginObserver()
        o.sample(appliedSequence: 1, intervalSequence: 2, intervalMinimum: nil)
        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: nil)
        #expect(o.worstIntervalMarginPackets == nil)
        #expect(o.intervalsObserved == 0)

        o.sample(appliedSequence: 1, intervalSequence: 6, intervalMinimum: 50)
        #expect(o.worstIntervalMarginPackets == 50)
        #expect(o.intervalsObserved == 1)
    }

    @Test func marginRatioAgainstTheFatalFloor() {
        var o = AudioMarginObserver()
        o.sample(appliedSequence: 1, intervalSequence: 2, intervalMinimum: 96)
        #expect(o.marginOverFloor(hardwareFloorPackets: 48) == nil)

        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 96)
        #expect(o.marginOverFloor(hardwareFloorPackets: 48) == 2.0)

        o.sample(appliedSequence: 1, intervalSequence: 6, intervalMinimum: 24)
        #expect(o.marginOverFloor(hardwareFloorPackets: 48) == 0.5)

        #expect(o.marginOverFloor(hardwareFloorPackets: 0) == nil)
    }

    // MARK: - Snapshot Resolution

    @Test func resolves48kHzStandardGeometry() {
        let s = AudioGeometrySnapshot.resolve(sampleRateHz: 48000, channelsIn: 2, channelsOut: 2)
        #expect(s.sampleRateHz == 48000)
        #expect(s.framesPerDataPacket == 8)
        #expect(s.cadenceBlockPackets == 4)
        #expect(s.cadenceBlockFrames == 24)
        #expect(s.framesPerCompletionGroupTx == 48)
        #expect(s.framesPerCompletionGroupRx == 48)
        #expect(s.minFramesPerRxInterrupt == 40)
        #expect(s.maxFramesPerRxInterrupt == 48)
        #expect(s.zeroTimestampPeriodFrames == 12288)
        #expect(s.frameRingFrames == 12288)
        #expect(s.clientIoBudgetFrames == 1024)
        #expect(s.txRingLapFrames == 3024)
        #expect(s.txTransferDelayTicks == 12800)
        #expect(s.rxTransferDelayTicks == 12800)
    }

    @Test func resolves96kHzDoubleRateGeometry() {
        let s = AudioGeometrySnapshot.resolve(sampleRateHz: 96000, channelsIn: 4, channelsOut: 4)
        #expect(s.sampleRateHz == 96000)
        #expect(s.framesPerDataPacket == 16)
        #expect(s.cadenceBlockPackets == 4)
        #expect(s.cadenceBlockFrames == 48)
        #expect(s.framesPerCompletionGroupTx == 96)
        #expect(s.framesPerCompletionGroupRx == 96)
        #expect(s.minFramesPerRxInterrupt == 80)
        #expect(s.maxFramesPerRxInterrupt == 96)
        #expect(s.zeroTimestampPeriodFrames == 24576)
        #expect(s.frameRingFrames == 24576)
        #expect(s.txRingLapFrames == 6048)
        #expect(s.txTransferDelayTicks == 12800)
        #expect(s.rxTransferDelayTicks == 12800)
    }

    @Test func resolves441kHzGeometryWithTransferDelay() {
        let s = AudioGeometrySnapshot.resolve(sampleRateHz: 44100)
        #expect(s.sampleRateHz == 44100)
        #expect(s.framesPerDataPacket == 8)
        #expect(s.zeroTimestampPeriodFrames == 12288)
        #expect(s.txTransferDelayTicks == 13162)
        #expect(s.rxTransferDelayTicks == 13162)
        #expect(s.minFramesPerRxInterrupt == 40)
        #expect(s.maxFramesPerRxInterrupt == 48)
    }

    @Test func latencyCalculationsMatchFormula() {
        let s = AudioGeometrySnapshot.resolve(sampleRateHz: 48000)
        // Output latency: clientIo + outLatency + outSafety
        // 128 + 52 + 128 = 308 frames
        #expect(s.outputFrames(io: 128) == 308)
        // Round trip: outputFrames + clientIo + inLatency + inSafety
        // 308 + 128 + 53 + 128 = 617 frames
        #expect(s.roundTripFrames(io: 128) == 617)
    }
}
