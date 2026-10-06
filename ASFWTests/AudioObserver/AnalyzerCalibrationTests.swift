import Foundation
import SwiftUI
import Testing
@testable import ASFW

struct AnalyzerCalibrationTests {
    @Test func calibrationConfigGainCalculations() {
        let defaultZero = AnalyzerCalibrationConfig()
        #expect(defaultZero.effectiveOffsetDB == 0.0)
        #expect(defaultZero.linearGain == 1.0)
        #expect(defaultZero.energyScale == 1.0)

        // SoundID Reference standard room correction boost: +3.7 dB
        let soundID = AnalyzerCalibrationConfig(offsetDB: 3.7, isEnabled: true)
        #expect(soundID.effectiveOffsetDB == 3.7)
        let expectedLinear = pow(10.0, 3.7 / 20.0)
        let expectedEnergy = pow(10.0, 3.7 / 10.0)
        #expect(abs(Double(soundID.linearGain) - expectedLinear) < 0.001)
        #expect(abs(Double(soundID.energyScale) - expectedEnergy) < 0.001)

        // When bypassed, effective offset must be 0 dB and gains must be 1.0
        let bypassed = AnalyzerCalibrationConfig(offsetDB: 3.7, isEnabled: false)
        #expect(bypassed.effectiveOffsetDB == 0.0)
        #expect(bypassed.linearGain == 1.0)
        #expect(bypassed.energyScale == 1.0)

        // Attenuation test: -6.0 dB
        let minusSix = AnalyzerCalibrationConfig(offsetDB: -6.0, isEnabled: true)
        #expect(abs(Double(minusSix.linearGain) - 0.501187) < 0.001)
        #expect(abs(Double(minusSix.energyScale) - 0.251189) < 0.001)
    }

    @Test func metricsStateAppliesCalibrationNonDestructively() {
        let metricsState = AudioObserverMetricsState()
        let config = AnalyzerCalibrationConfig(offsetDB: 3.7, isEnabled: true)
        metricsState.setCalibrationConfig(config)
        #expect(metricsState.getCalibrationConfig() == config)

        let geometry = AudioRingGeometry(sampleRateHz: 48000, channels: 2, activeFrames: 4096, mappedFrames: 8192, memoryGeneration: 1)
        let token = AudioFrameToken(geometry: geometry, sessionEpoch: 1, discontinuityEpoch: 1, routingGeneration: 1, startFrame: 0, endFrame: 480)
        let pair = AudioChannelPair(leftIndex: 0, rightIndex: 1, generation: 1)

        let rawPeak: Float = 0.5
        let rawRms: Float = 0.25
        let rawTruePeak: Float = 0.55
        let rawChunkEnergy: Float = 0.1

        var words = [UInt32](repeating: 0, count: AudioAnalysisLayout.outputWords)
        words[0] = rawPeak.bitPattern
        words[1] = rawPeak.bitPattern
        words[2] = Float(0.85).bitPattern // Correlation
        words[4] = Float(0.4).bitPattern // Mid peak
        words[5] = Float(0.1).bitPattern // Side peak
        words[6] = rawRms.bitPattern // Left RMS
        words[7] = rawRms.bitPattern // Right RMS
        words[8] = Float(0.2).bitPattern
        words[9] = Float(0.05).bitPattern
        words[10] = Float(0.0).bitPattern // Balance
        words[11] = Float(0.12).bitPattern // Side energy fraction
        words[15] = Float(-0.5).bitPattern // Mono energy retention
        words[92] = rawTruePeak.bitPattern // Left true peak
        words[93] = rawTruePeak.bitPattern // Right true peak
        words[94] = 1 // True peak valid
        words[95] = rawChunkEnergy.bitPattern // Raw sample energy

        // 1 loudness energy chunk
        words[16] = 1
        let chunkOffset = AudioAnalysisLayout.chunkOffset
        words[chunkOffset] = 480
        words[chunkOffset + 1] = 0
        words[chunkOffset + 2] = (rawChunkEnergy * 0.5).bitPattern
        words[chunkOffset + 3] = (rawChunkEnergy * 0.5).bitPattern

        metricsState.startLoudnessMeasurement(sampleRateHz: 48000)

        for chunkIdx in 0..<40 {
            words[chunkOffset] = UInt32((chunkIdx + 1) * 480)
            words.withUnsafeBufferPointer { buffer in
                metricsState.accept(
                    token: token,
                    pair: pair,
                    result: buffer,
                    correlationValid: true,
                    cpuEncodeMilliseconds: 0.1,
                    scheduledToStartMilliseconds: 0.1,
                    gpuMilliseconds: 0.2,
                    completionMilliseconds: 0.3,
                    sampleAgeMilliseconds: 1.0,
                    overwriteMarginMilliseconds: 10.0,
                    meterKey: "test"
                )
            }
        }

        let read = metricsState.read(includeHistory: false)

        // 1. Calibration offset must match configured value
        #expect(read.analysis.calibrationOffsetDB == 3.7)

        // 2. Peak, RMS, True Peak should scale by linearGain
        let linear = config.linearGain
        #expect(abs((read.analysis.levels.left.samplePeak.value ?? 0) - (rawPeak * linear)) < 0.001)
        #expect(abs((read.analysis.levels.left.rms.value ?? 0) - (rawRms * linear)) < 0.001)
        #expect(abs((read.analysis.levels.left.truePeak.value ?? 0) - (rawTruePeak * linear)) < 0.001)

        // 3. Correlation and balance must be scale-invariant
        #expect(read.analysis.stereo.correlation.value == 0.85)
        #expect(read.analysis.stereo.balance.value == 0.0)

        // 4. Momentary LUFS must be shifted by +3.7 LUFS
        // BS.1770 LUFS formula: -0.691 + 10 * log10(energy / frames)
        let uncalibratedLUFS = -0.691 + 10.0 * log10(Double(rawChunkEnergy) / 480.0)
        if let calibratedLUFS = read.analysis.loudness.momentaryLUFS.value {
            #expect(abs(Double(calibratedLUFS) - (uncalibratedLUFS + 3.7)) < 0.01)
        } else {
            #expect(Bool(false), "Momentary LUFS should be valid")
        }

        // 5. Test bypass mode resets scaling
        metricsState.setCalibrationConfig(AnalyzerCalibrationConfig(offsetDB: 3.7, isEnabled: false))
        words.withUnsafeBufferPointer { buffer in
            metricsState.accept(
                token: token,
                pair: pair,
                result: buffer,
                correlationValid: true,
                cpuEncodeMilliseconds: 0.1,
                scheduledToStartMilliseconds: 0.1,
                gpuMilliseconds: 0.2,
                completionMilliseconds: 0.3,
                sampleAgeMilliseconds: 1.0,
                overwriteMarginMilliseconds: 10.0,
                meterKey: "test"
            )
        }
        let unscaledRead = metricsState.read(includeHistory: false)
        #expect(unscaledRead.analysis.calibrationOffsetDB == 0.0)
        #expect(abs((unscaledRead.analysis.levels.left.samplePeak.value ?? 0) - rawPeak) < 0.001)
        #expect(abs((unscaledRead.analysis.levels.left.rms.value ?? 0) - rawRms) < 0.001)
        #expect(abs((unscaledRead.analysis.levels.left.truePeak.value ?? 0) - rawTruePeak) < 0.001)
    }

    @Test func themeModeVisualTokens() {
        let modes = AnalyzerThemeMode.allCases
        #expect(modes.count == 4)

        let dark = AnalyzerThemeMode.studioDark
        #expect(!dark.isLight)
        #expect(dark.readoutPrimary.x > 0.9) // White/bright text
        #expect(dark.readoutPrimary.w == 1.0)

        let light = AnalyzerThemeMode.studioLight
        #expect(light.isLight)
        #expect(light.readoutPrimary.x < 0.2) // Charcoal/dark text for light cards
        #expect(light.readoutPrimary.w == 1.0)

        let contrast = AnalyzerThemeMode.highContrast
        #expect(!contrast.isLight)
        #expect(contrast.readoutPrimary == SIMD4<Float>(1.0, 1.0, 1.0, 1.0))

        let oled = AnalyzerThemeMode.oledBlack
        #expect(!oled.isLight)
        #expect(oled.cardBackgroundTop == Color.black)
        #expect(oled.plotBackground == Color.black)

        // Verify plotBackground adapts: studioLight is off-white, dark modes are dark CRT/black
        #expect(dark.plotBackground == Color(red: 0.025, green: 0.035, blue: 0.05))
        #expect(light.plotBackground == Color(red: 0.94, green: 0.955, blue: 0.97))
    }
}
