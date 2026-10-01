import Foundation
import Testing
@testable import ASFW

struct AudioLoudnessMeasurementSessionTests {
    @Test func integratedGateUses400MillisecondBlocksAndRelativeTenLoudnessGate() {
        func energy(forLUFS value: Float) -> Float {
            pow(10, (value + 0.691) / 10)
        }

        let loud = energy(forLUFS: -20)
        let quiet = energy(forLUFS: -35)
        let belowAbsoluteGate = energy(forLUFS: -75)
        let measured = AudioLoudnessMeasurementSession.gatedIntegratedLoudness(
            [loud, loud, quiet, belowAbsoluteGate])

        #expect(measured != nil)
        #expect(abs((measured ?? 0) - (-20)) < 0.02)
    }

    @Test func pauseResumeAndDiscontinuityDoNotBridgeMeasurementWindows() {
        var session = AudioLoudnessMeasurementSession()
        let unsupportedStart = session.start(sampleRateHz: 44_100)
        #expect(!unsupportedStart)
        let supportedStart = session.start(sampleRateHz: 48_000)
        #expect(supportedStart)

        let chunk = AudioLoudnessEnergyChunk(endFrame: 0,
                                             weightedEnergy: 0.01,
                                             frameCount: 480)
        let now = Date(timeIntervalSince1970: 1_000)
        for index in 0..<40 {
            var next = chunk
            next.endFrame = UInt64(index + 1) * 480
            session.consume(next, now: now)
        }
        #expect(session.includedFrames == 19_200)
        #expect(session.integratedLUFS != nil)

        session.pause()
        var pausedChunk = chunk
        pausedChunk.endFrame = 20_000
        session.consume(pausedChunk, now: now.addingTimeInterval(1))
        #expect(session.phase == .paused)
        #expect(session.includedFrames == 19_200)

        session.resume()
        for index in 0..<40 {
            var next = chunk
            next.endFrame = 30_000 + UInt64(index + 1) * 480
            session.consume(next, now: now.addingTimeInterval(2))
        }
        #expect(session.includedFrames == 38_400)

        session.markDiscontinuous()
        var discontinuousChunk = chunk
        discontinuousChunk.endFrame = 60_000
        session.consume(discontinuousChunk, now: now.addingTimeInterval(3))
        #expect(session.phase == .discontinuous)
        #expect(session.includedFrames == 38_400)
        #expect(session.integratedLUFS != nil)

        session.reset()
        #expect(session.phase == .idle)
        #expect(session.includedFrames == 0)
        #expect(session.integratedLUFS == nil)
    }

    @Test func loudnessRangeTracksTenHertzShortTermLevels() {
        var session = AudioLoudnessMeasurementSession()
        let started = session.start(sampleRateHz: 48_000)
        #expect(started)

        let quietEnergy = Float(pow(10, (-30.691) / 10) * 480)
        let loudEnergy = Float(pow(10, (-20.691) / 10) * 480)
        for index in 0..<4_000 {
            let energy = index < 2_000 ? loudEnergy : quietEnergy
            let chunk = AudioLoudnessEnergyChunk(endFrame: UInt64(index + 1) * 480,
                                                 weightedEnergy: energy,
                                                 frameCount: 480)
            session.consume(chunk,
                            now: Date(timeIntervalSince1970: 10_000 + Double(index) / 100))
        }

        #expect(session.loudnessRangeLU != nil)
        #expect(abs((session.loudnessRangeLU ?? 0) - 10) < 1)
        #expect(session.loudnessRangeIsProvisional)
    }
}
