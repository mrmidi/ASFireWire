import Foundation

enum AudioMeasurementStatus: Sendable, Equatable {
    case unsupported
    case warmingUp
    case valid
    case idle
    case discontinuous
}

struct AudioMeasurement<Value: Sendable & Equatable>: Sendable, Equatable {
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

struct AudioChannelPair: Sendable, Equatable {
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

struct AudioRingGeometry: Sendable, Equatable {
    var sampleRateHz: UInt32
    var channels: UInt32
    var activeFrames: UInt32
    var mappedFrames: UInt64
    var memoryGeneration: UInt64
}

struct AudioFrameToken: Sendable, Equatable {
    var geometry: AudioRingGeometry
    var sessionEpoch: UInt64
    var discontinuityEpoch: UInt64
    var routingGeneration: UInt64
    var startFrame: UInt64
    var endFrame: UInt64
}

struct AudioChannelLevelMetrics: Sendable, Equatable {
    var samplePeak: AudioMeasurement<Float> = .warmingUp
    var rms: AudioMeasurement<Float> = .warmingUp
    var truePeak: AudioMeasurement<Float> = .unsupported
    var overRangeSamples: UInt32 = 0
}

struct AudioLevelMetrics: Sendable, Equatable {
    var left = AudioChannelLevelMetrics()
    var right = AudioChannelLevelMetrics()
    var mid = AudioChannelLevelMetrics()
    var side = AudioChannelLevelMetrics()
}

enum AudioCancellationRisk: Sendable, Equatable {
    case insufficientSignal
    case normal
    case risk
}

struct AudioStereoMetrics: Sendable, Equatable {
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

struct AudioLoudnessMetrics: Sendable, Equatable {
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

enum AudioLoudnessSessionPhase: Sendable, Equatable {
    case idle
    case running
    case paused
    case discontinuous
    case complete
}

struct AudioLoudnessEnergyChunk: Sendable, Equatable {
    var endFrame: UInt64
    var weightedEnergy: Float
    var frameCount: UInt32
}

struct AudioDiagnosticsMetrics: Sendable, Equatable {
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

struct AudioAnalyzerSnapshot: Sendable, Equatable {
    var token: AudioFrameToken?
    var selectedPair = AudioChannelPair()
    var streamStatus: AudioMeasurementStatus = .warmingUp
    var levels = AudioLevelMetrics()
    var stereo = AudioStereoMetrics()
    var loudness = AudioLoudnessMetrics()
    var diagnostics = AudioDiagnosticsMetrics()
}
