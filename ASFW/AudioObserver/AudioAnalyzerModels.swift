import Foundation

extension Notification.Name {
    static let asfwAnalysisCompleted = Notification.Name("ASFWAnalysisCompleted")
}

// Matches the GPU result layout in AudioObserver.metal. Header words 0...94
// never overlap the fixed-capacity loudness chunk region.
nonisolated enum AudioAnalysisLayout {
    static let chunkOffset = 96
    static let chunkWords = 8
    /// 12 chunks = 5760 frames: 120 ms at 48 kHz, 60 ms at 96 kHz. A 40 ms
    /// acquisition interval then drains a late wakeup's backlog in one pass.
    static let chunkCapacity = 12
    static let outputWords = chunkOffset + chunkWords * chunkCapacity
    static let maximumBatchFrames: UInt64 = 480 * UInt64(chunkCapacity)

    static func batchEnd(start: UInt64, availableEnd: UInt64) -> UInt64 {
        start + min(availableEnd - start, maximumBatchFrames)
    }
}

nonisolated enum AudioAnalyzerGeometry {
    /// Keep one third of the active ring between the oldest plotted sample
    /// and the writer, leaving overwrite slack for GPU execution.
    static func goniometerWindowFrames(activeRingFrames: UInt32) -> UInt32 {
        activeRingFrames - activeRingFrames / 3
    }
}

nonisolated enum AudioMeasurementStatus: Sendable, Equatable {
    case unsupported
    case warmingUp
    case valid
    case idle
    case discontinuous
}

nonisolated struct AudioMeasurement<Value: Sendable & Equatable>: Sendable, Equatable {
    var value: Value?
    var status: AudioMeasurementStatus

    static var warmingUp: Self { Self(value: nil, status: .warmingUp) }
    static var unsupported: Self { Self(value: nil, status: .unsupported) }
    static var idle: Self { Self(value: nil, status: .idle) }
    static func valid(_ value: Value) -> Self { Self(value: value, status: .valid) }
    static func discontinuous(_ lastValue: Value? = nil) -> Self {
        Self(value: lastValue, status: .discontinuous)
    }
}

nonisolated struct AudioChannelPair: Sendable, Equatable {
    /// Zero-based indices into the mapped interleaved CoreAudio output ring.
    var leftIndex: UInt32 = 0
    var rightIndex: UInt32 = 1
    var generation: UInt64 = 0

    func resolved(channelCount: UInt32) -> Self? {
        guard channelCount > 0, leftIndex < channelCount,
              rightIndex < channelCount else { return nil }
        return self
    }
}

nonisolated struct AudioRingGeometry: Sendable, Equatable {
    var sampleRateHz: UInt32
    var channels: UInt32
    var activeFrames: UInt32
    var mappedFrames: UInt64
    var memoryGeneration: UInt64
}

nonisolated struct AudioFrameToken: Sendable, Equatable {
    var geometry: AudioRingGeometry
    var sessionEpoch: UInt64
    var discontinuityEpoch: UInt64
    var routingGeneration: UInt64
    var startFrame: UInt64
    var endFrame: UInt64
}

nonisolated struct AudioChannelLevelMetrics: Sendable, Equatable {
    var samplePeak: AudioMeasurement<Float> = .warmingUp
    var rms: AudioMeasurement<Float> = .warmingUp
    var truePeak: AudioMeasurement<Float> = .unsupported
    var overRangeSamples: UInt32 = 0
}

nonisolated struct AudioLevelMetrics: Sendable, Equatable {
    var left = AudioChannelLevelMetrics()
    var right = AudioChannelLevelMetrics()
    var mid = AudioChannelLevelMetrics()
    var side = AudioChannelLevelMetrics()
}

nonisolated enum AudioCancellationRisk: Sendable, Equatable {
    case insufficientSignal
    case normal
    case risk
}

nonisolated struct AudioStereoMetrics: Sendable, Equatable {
    var correlation: AudioMeasurement<Float> = .warmingUp
    var rollingCorrelation: AudioMeasurement<Float> = .warmingUp
    /// Normalized energy imbalance: -1 is left-only and +1 is right-only.
    var balance: AudioMeasurement<Float> = .warmingUp
    /// Fraction of pair energy in Side, in [0, 1].
    var sideEnergyFraction: AudioMeasurement<Float> = .warmingUp
    /// 10*log10(E_mid / E_stereo); zero for identical in-phase channels.
    var monoEnergyRetentionDB: AudioMeasurement<Float> = .warmingUp
    var cancellationRisk: AudioCancellationRisk = .insufficientSignal
}

nonisolated struct AudioStereoHistoryPoint: Sendable, Equatable {
    var endFrame: UInt64
    var correlation: Float
    var sideEnergyFraction: Float
    var breakBefore = false
}

nonisolated struct AudioLoudnessMetrics: Sendable, Equatable {
    var momentaryLUFS: AudioMeasurement<Float> = .unsupported
    var shortTermLUFS: AudioMeasurement<Float> = .unsupported
    var integratedLUFS: AudioMeasurement<Float> = .unsupported
    var loudnessRangeLU: AudioMeasurement<Float> = .unsupported
    var maximumMomentaryLUFS: AudioMeasurement<Float> = .unsupported
    var maximumShortTermLUFS: AudioMeasurement<Float> = .unsupported
    var maximumTruePeakDBTP: AudioMeasurement<Float> = .unsupported
    var plrDB: AudioMeasurement<Float> = .unsupported
    var crestFactorDB: AudioMeasurement<Float> = .warmingUp
    var integratedMeasurementID: UInt64 = 0
    var acceptedAudioFrames: UInt64 = 0
    var includedAudioFrames: UInt64 = 0
    var sessionPhase: AudioLoudnessSessionPhase = .idle
    var loudnessRangeIsProvisional = false
}

nonisolated enum AudioLoudnessSessionPhase: Sendable, Equatable {
    case idle
    case running
    case paused
    case discontinuous
    case complete
}

nonisolated struct AudioLoudnessEnergyChunk: Sendable, Equatable {
    var endFrame: UInt64
    var weightedEnergy: Float
    /// Sum of squared, unweighted L/R samples for the same 10 ms interval.
    var rawSampleEnergy: Float = 0
    var samplePeak: Float = 0
    var truePeakLeft: Float = 0
    var truePeakRight: Float = 0
    var frameCount: UInt32
}

nonisolated struct AudioDiagnosticsMetrics: Sendable, Equatable {
    var cursor: AudioMeasurement<UInt64> = .warmingUp
    var sampleAgeMilliseconds: AudioMeasurement<Double> = .warmingUp
    var overwriteMarginMilliseconds: AudioMeasurement<Double> = .warmingUp
    var cpuSubmissionMilliseconds: AudioMeasurement<Double> = .warmingUp
    var gpuMilliseconds: AudioMeasurement<Double> = .warmingUp
    var completionMilliseconds: AudioMeasurement<Double> = .warmingUp
    var inFlight: UInt32 = 0
    var rejectedRanges: UInt64 = 0
    var wrapWindows: UInt64 = 0
    var unsafeRanges: UInt64 = 0
    var invalidSamples: UInt64 = 0
}

nonisolated struct AudioAnalyzerSnapshot: Sendable, Equatable {
    var token: AudioFrameToken?
    var selectedPair = AudioChannelPair()
    var streamStatus: AudioMeasurementStatus = .warmingUp
    var levels = AudioLevelMetrics()
    var stereo = AudioStereoMetrics()
    var loudness = AudioLoudnessMetrics()
    var diagnostics = AudioDiagnosticsMetrics()
    var calibrationOffsetDB: Double = 0.0
}
