import Foundation
import IOKit
import Metal

struct AudioObserverSnapshot: Sendable, Equatable {
    var writeEndFrame: UInt64 = 0
    var oldestValidFrame: UInt64 = 0
    var sessionEpoch: UInt64 = 0
    var discontinuityEpoch: UInt64 = 0
    var memoryGeneration: UInt64 = 0
    var activeRingFrames: UInt32 = 0
    var channels: UInt32 = 0
    var sampleRateHz: UInt32 = 0
    var mappedFrames: UInt64 = 0
    var validHistoryFrames: UInt32 = 0
    var ioRunning = false
}

struct AudioObserverMetrics: Sendable, Equatable {
    var analysis = AudioAnalyzerSnapshot()
    var stereoHistory: [AudioStereoHistoryPoint] = []
    var cpuEncodeMilliseconds: Double?
    var scheduledToStartMilliseconds: Double?
    var gpuMilliseconds: Double?
    var completionMilliseconds: Double?
    var sampleAgeMilliseconds: Double?
    var overwriteMarginMilliseconds: Double?
    var windowsRendered: UInt64 = 0
    var windowsCrossingWrap: UInt64 = 0
    var unsafeWindows: UInt64 = 0
    var invalidSampleCount: UInt64 = 0
    var inFlight = 0

    // Transitional accessors keep the existing renderer/UI stable while its
    // results are migrated to the shared AudioAnalyzerSnapshot.
    var leftPeak: Float { analysis.levels.left.samplePeak.value ?? 0 }
    var rightPeak: Float { analysis.levels.right.samplePeak.value ?? 0 }
    var correlation: Float { analysis.stereo.correlation.value ?? 0 }
    var correlationAverage: Float { analysis.stereo.rollingCorrelation.value ?? 0 }
    var correlationValid: Bool { analysis.stereo.correlation.status == .valid }
    var meterValues: [Float] {
        [analysis.levels.mid.samplePeak.value ?? 0,
         analysis.levels.side.samplePeak.value ?? 0,
         analysis.levels.left.rms.value ?? 0,
         analysis.levels.right.rms.value ?? 0,
         analysis.levels.mid.rms.value ?? 0,
         analysis.levels.side.rms.value ?? 0,
         analysis.stereo.balance.value ?? 0,
         analysis.stereo.sideEnergyFraction.value ?? 0]
    }
}

struct AudioObserverWireState: Sendable {
    let writeEndFrame: UInt64
    let oldestValidFrame: UInt64
    let sessionEpoch: UInt64
    let discontinuityEpoch: UInt64
    let memoryGeneration: UInt64
    let mappedGeneration: UInt64
    let activeRingFrames: UInt32
    let channels: UInt32
    let sampleRateHz: UInt32
}

/// Keeps synchronous IOKit state reads off SwiftUI's main actor.
actor AudioObserverPoller {
    private let reader: AudioObserverStateReader
    private let mappedSize: UInt64

    init(reader: AudioObserverStateReader, mappedSize: UInt64) {
        self.reader = reader
        self.mappedSize = mappedSize
    }

    func poll() throws -> AudioObserverSnapshot {
        let state = try reader.read()
        guard state.memoryGeneration == state.mappedGeneration else {
            throw AudioObserverError.memoryGenerationChanged
        }
        let bytesPerFrame = UInt64(state.channels) * UInt64(MemoryLayout<Float>.stride)
        guard bytesPerFrame > 0 else { throw AudioObserverError.invalidGeometry }
        let mappedFrames = mappedSize / bytesPerFrame
        guard mappedFrames >= UInt64(state.activeRingFrames) else {
            throw AudioObserverError.invalidGeometry
        }
        let valid = state.writeEndFrame >= state.oldestValidFrame
            ? min(UInt64(state.activeRingFrames), state.writeEndFrame - state.oldestValidFrame)
            : 0
        return AudioObserverSnapshot(writeEndFrame: state.writeEndFrame,
                                      oldestValidFrame: state.oldestValidFrame,
                                      sessionEpoch: state.sessionEpoch,
                                      discontinuityEpoch: state.discontinuityEpoch,
                                      memoryGeneration: state.memoryGeneration,
                                      activeRingFrames: state.activeRingFrames,
                                      channels: state.channels,
                                      sampleRateHz: state.sampleRateHz,
                                      mappedFrames: mappedFrames,
                                      validHistoryFrames: UInt32(valid),
                                      ioRunning: valid > 0)
    }
}

private final class AudioObserverMappingLifetime: @unchecked Sendable {
    private let connection: io_connect_t
    private let address: mach_vm_address_t

    init(connection: io_connect_t, address: mach_vm_address_t) {
        self.connection = connection
        self.address = address
    }

    deinit {
        IOConnectUnmapMemory64(connection, 2, mach_task_self_, address)
        IOServiceClose(connection)
    }
}

final class AudioObserverStateReader: @unchecked Sendable {
    nonisolated private static let selector: UInt32 = 67
    private let connection: io_connect_t
    private let guid: UInt64
    private let lock = NSLock()

    init(connection: io_connect_t, guid: UInt64) {
        self.connection = connection
        self.guid = guid
    }

    nonisolated func read() throws -> AudioObserverWireState {
        lock.lock()
        defer { lock.unlock() }
        var values = [UInt64](repeating: 0, count: 9)
        var outputCount: UInt32 = UInt32(values.count)
        let result = values.withUnsafeMutableBufferPointer { buffer in
            IOConnectCallScalarMethod(connection, Self.selector, nil, 0,
                                      buffer.baseAddress, &outputCount)
        }
        guard result == KERN_SUCCESS, outputCount == 9 else {
            throw AudioObserverError.stateQuery(result)
        }
        let state = AudioObserverWireState(
            writeEndFrame: values[0],
            oldestValidFrame: values[1],
            sessionEpoch: values[2],
            discontinuityEpoch: values[3],
            memoryGeneration: values[4],
            mappedGeneration: values[5],
            activeRingFrames: UInt32(truncatingIfNeeded: values[6]),
            channels: UInt32(truncatingIfNeeded: values[7]),
            sampleRateHz: UInt32(truncatingIfNeeded: values[8]))
        guard state.activeRingFrames > 0, state.channels >= 2,
              state.sampleRateHz > 0 else {
            throw AudioObserverError.invalidGeometry
        }
        return state
    }
}

enum AudioObserverError: LocalizedError, Sendable {
    case serviceUnavailable
    case openFailed(kern_return_t)
    case selectionFailed(kern_return_t)
    case mappingFailed(kern_return_t)
    case stateQuery(kern_return_t)
    case invalidGeometry
    case memoryGenerationChanged
    case zeroCopyImportFailed
    case metalUnavailable
    case shaderUnavailable
    case pipelineFailed

    var errorDescription: String? {
        switch self {
        case .serviceUnavailable:
            return "ASFWDriver is not available. Start the driver and select an ASFW Core Audio device."
        case .openFailed(let result):
            return String(format: "Opening the ASFW audio observer failed (0x%08x).", result)
        case .selectionFailed(let result):
            return String(format: "Selecting this audio endpoint failed (0x%08x).", result)
        case .mappingFailed(let result):
            return String(format: "Mapping the ASFW output ring failed (0x%08x). The audio graph may not be ready yet.", result)
        case .stateQuery(let result):
            return String(format: "Reading ASFW output-ring state failed (0x%08x).", result)
        case .invalidGeometry:
            return "The ASFW output ring returned invalid geometry."
        case .memoryGenerationChanged:
            return "The audio buffer changed during reconfiguration; reconnecting the observer."
        case .zeroCopyImportFailed:
            return "ZERO-COPY IMPORT FAILED: Metal rejected the mapped ASFW output ring."
        case .metalUnavailable:
            return "No Metal device is available."
        case .shaderUnavailable:
            return "The ASFW audio observer Metal functions are missing."
        case .pipelineFailed:
            return "Metal could not create the ASFW audio observer pipelines."
        }
    }
}

final class AudioObserverRenderState: @unchecked Sendable {
    private let lock = NSLock()
    private var snapshot = AudioObserverSnapshot()

    func update(_ value: AudioObserverSnapshot) {
        lock.lock()
        snapshot = value
        lock.unlock()
    }

    func read() -> AudioObserverSnapshot {
        lock.lock()
        defer { lock.unlock() }
        return snapshot
    }
}

final class AudioObserverMetricsState: @unchecked Sendable {
    private static let stereoHistoryCapacity = 600
    private let lock = NSLock()
    private var value = AudioObserverMetrics()
    private var stereoHistory = [AudioStereoHistoryPoint?](repeating: nil,
                                                           count: stereoHistoryCapacity)
    private var stereoHistoryWriteIndex = 0
    private var stereoHistoryCount = 0
    private var nextStereoHistoryFrame: UInt64?
    private var lastMeterTime = Date.distantPast
    private var meterKey: String?
    private var loudnessEnergyRing = [AudioLoudnessEnergyChunk?](repeating: nil, count: 300)
    private var loudnessWriteIndex = 0
    private var loudnessCount = 0
    private var loudnessSession = AudioLoudnessMeasurementSession()

    func read(includeHistory: Bool = true) -> AudioObserverMetrics {
        lock.lock()
        defer { lock.unlock() }
        var snapshot = value
        snapshot.stereoHistory = includeHistory ? orderedStereoHistory() : []
        return snapshot
    }

    func readStereoHistory() -> [AudioStereoHistoryPoint] {
        lock.lock()
        defer { lock.unlock() }
        return orderedStereoHistory()
    }

    // Read only current scalar reductions; no history allocation on render events.
    func plotValues(mode: UInt32, index: UInt32) -> (Float, Float, Bool) {
        lock.lock()
        defer { lock.unlock() }
        if mode == 4 {
            let loudness = value.analysis.loudness
            let measurement = [loudness.momentaryLUFS, loudness.shortTermLUFS, loudness.integratedLUFS][Int(index)]
            return (measurement.value ?? -60, 0, measurement.value?.isFinite == true)
        }
        if mode == 0 {
            let channel = [value.analysis.levels.left, value.analysis.levels.right,
                           value.analysis.levels.mid, value.analysis.levels.side][Int(index)]
            return (channel.rms.value ?? 0, channel.samplePeak.value ?? 0,
                    channel.rms.status == .valid)
        }
        if index == 0 { return (value.correlation, 0, value.correlationValid) }
        if index == 1 { return (value.analysis.stereo.balance.value ?? 0, 0, true) }
        return (2 * (value.analysis.stereo.sideEnergyFraction.value ?? 0) - 1, 0, true)
    }

    func submitted(crossesWrap: Bool) {
        lock.lock()
        value.inFlight += 1
        if crossesWrap { value.windowsCrossingWrap += 1 }
        lock.unlock()
    }

    func accept(token: AudioFrameToken,
                pair: AudioChannelPair,
                result: UnsafeBufferPointer<UInt32>,
                correlationValid: Bool,
                cpuEncodeMilliseconds: Double,
                scheduledToStartMilliseconds: Double?,
                gpuMilliseconds: Double?,
                completionMilliseconds: Double,
                sampleAgeMilliseconds: Double,
                overwriteMarginMilliseconds: Double,
                meterKey: String) {
        guard result.count >= AudioAnalysisLayout.outputWords else { return }
        let floats = result
        lock.lock()
        value.inFlight = max(0, value.inFlight - 1)
        value.windowsRendered += 1
        let meterValues = (4...11).map { Float(bitPattern: floats[$0]) }
        value.analysis.levels.left.samplePeak = .valid(Float(bitPattern: floats[0]))
        value.analysis.levels.right.samplePeak = .valid(Float(bitPattern: floats[1]))
        value.analysis.levels.mid.samplePeak = .valid(meterValues[0])
        value.analysis.levels.side.samplePeak = .valid(meterValues[1])
        value.analysis.levels.left.rms = .valid(meterValues[2])
        value.analysis.levels.right.rms = .valid(meterValues[3])
        value.analysis.levels.mid.rms = .valid(meterValues[4])
        value.analysis.levels.side.rms = .valid(meterValues[5])
        if token.geometry.sampleRateHz == 48_000 {
            if floats[94] != 0 {
                let truePeakLeft = Float(bitPattern: floats[92])
                let truePeakRight = Float(bitPattern: floats[93])
                value.analysis.levels.left.truePeak = .valid(truePeakLeft)
                value.analysis.levels.right.truePeak = .valid(truePeakRight)
            } else {
                value.analysis.levels.left.truePeak = .warmingUp
                value.analysis.levels.right.truePeak = .warmingUp
            }
        } else {
            value.analysis.levels.left.truePeak = .unsupported
            value.analysis.levels.right.truePeak = .unsupported
        }
        if correlationValid {
            let now = Date()
            let alpha = self.meterKey == meterKey
                ? Float(exp(-now.timeIntervalSince(lastMeterTime))) : 0
            let previousAverage = value.analysis.stereo.rollingCorrelation.value ?? 0
            value.analysis.stereo.rollingCorrelation = .valid(
                alpha * previousAverage + (1 - alpha) * Float(bitPattern: floats[2]))
            lastMeterTime = now
            self.meterKey = meterKey
            value.analysis.stereo.correlation = .valid(Float(bitPattern: floats[2]))
        } else {
            self.meterKey = nil
            value.analysis.stereo.correlation = .warmingUp
            value.analysis.stereo.rollingCorrelation = .warmingUp
        }
        value.analysis.stereo.balance = .valid(meterValues[6])
        value.analysis.stereo.sideEnergyFraction = .valid(meterValues[7])
        value.analysis.token = token
        value.analysis.selectedPair = pair
        value.analysis.streamStatus = .valid
        value.analysis.levels.left.overRangeSamples = floats[13]
        value.analysis.levels.right.overRangeSamples = floats[14]
        value.invalidSampleCount &+= UInt64(floats[90]) + UInt64(floats[91])
        value.analysis.diagnostics.invalidSamples = value.invalidSampleCount
        value.analysis.stereo.monoEnergyRetentionDB = .valid(Float(bitPattern: floats[15]))
        value.analysis.stereo.cancellationRisk = correlationValid && Float(bitPattern: floats[2]) < 0
            ? .risk : (correlationValid ? .normal : .insufficientSignal)
        appendStereoHistory(token: token, correlationValid: correlationValid)
        value.analysis.diagnostics.cursor = .valid(token.endFrame)
        value.analysis.diagnostics.sampleAgeMilliseconds = .valid(sampleAgeMilliseconds)
        value.analysis.diagnostics.overwriteMarginMilliseconds = .valid(overwriteMarginMilliseconds)
        value.analysis.diagnostics.cpuSubmissionMilliseconds = .valid(cpuEncodeMilliseconds)
        if let gpuMilliseconds {
            value.analysis.diagnostics.gpuMilliseconds = .valid(gpuMilliseconds)
        } else {
            value.analysis.diagnostics.gpuMilliseconds = .warmingUp
        }
        value.analysis.diagnostics.completionMilliseconds = .valid(completionMilliseconds)
        value.analysis.diagnostics.inFlight = UInt32(max(0, value.inFlight))
        value.analysis.diagnostics.wrapWindows = value.windowsCrossingWrap
        value.analysis.diagnostics.unsafeRanges = value.unsafeWindows
        value.analysis.diagnostics.rejectedRanges = value.unsafeWindows
        value.cpuEncodeMilliseconds = cpuEncodeMilliseconds
        value.scheduledToStartMilliseconds = scheduledToStartMilliseconds
        value.gpuMilliseconds = gpuMilliseconds
        value.completionMilliseconds = completionMilliseconds
        value.sampleAgeMilliseconds = sampleAgeMilliseconds
        value.overwriteMarginMilliseconds = overwriteMarginMilliseconds
        if token.geometry.sampleRateHz == 48_000 {
            let chunkCount = min(Int(floats[16]), AudioAnalysisLayout.chunkCapacity)
            for index in 0..<chunkCount {
                let word = AudioAnalysisLayout.chunkOffset + index * AudioAnalysisLayout.chunkWords
                let endFrame = UInt64(floats[word]) | (UInt64(floats[word + 1]) << 32)
                let energy = Float(bitPattern: floats[word + 2]) + Float(bitPattern: floats[word + 3])
                loudnessEnergyRing[loudnessWriteIndex] = AudioLoudnessEnergyChunk(
                    endFrame: endFrame,
                    weightedEnergy: energy,
                    rawSampleEnergy: Float(bitPattern: floats[word + 4]),
                    samplePeak: Float(bitPattern: floats[word + 5]),
                    truePeakLeft: Float(bitPattern: floats[word + 6]),
                    truePeakRight: Float(bitPattern: floats[word + 7]),
                    frameCount: 480)
                loudnessWriteIndex = (loudnessWriteIndex + 1) % loudnessEnergyRing.count
                loudnessCount = min(loudnessEnergyRing.count, loudnessCount + 1)
                value.analysis.loudness.acceptedAudioFrames &+= 480
                if let momentary = loudnessValue(forLastChunks: 40) {
                    value.analysis.loudness.momentaryLUFS = .valid(momentary)
                } else {
                    value.analysis.loudness.momentaryLUFS = .warmingUp
                }
                if let shortTerm = loudnessValue(forLastChunks: 300) {
                    value.analysis.loudness.shortTermLUFS = .valid(shortTerm)
                } else {
                    value.analysis.loudness.shortTermLUFS = .warmingUp
                }
                loudnessSession.consume(AudioLoudnessEnergyChunk(
                    endFrame: endFrame,
                    weightedEnergy: energy,
                    rawSampleEnergy: Float(bitPattern: floats[word + 4]),
                    samplePeak: Float(bitPattern: floats[word + 5]),
                    truePeakLeft: Float(bitPattern: floats[word + 6]),
                    truePeakRight: Float(bitPattern: floats[word + 7]),
                    frameCount: 480))
            }
            publishLoudnessSessionState()
        } else {
            value.analysis.loudness.momentaryLUFS = .unsupported
            value.analysis.loudness.shortTermLUFS = .unsupported
        }
        lock.unlock()
    }

    func rejected() {
        lock.lock()
        value.inFlight = max(0, value.inFlight - 1)
        value.unsafeWindows += 1
        value.analysis.streamStatus = .discontinuous
        value.analysis.levels = AudioLevelMetrics()
        value.analysis.stereo = AudioStereoMetrics()
        clearLoudnessHistory(status: .discontinuous)
        loudnessSession.markDiscontinuous()
        publishLoudnessSessionState()
        value.analysis.diagnostics.inFlight = UInt32(max(0, value.inFlight))
        value.analysis.diagnostics.unsafeRanges = value.unsafeWindows
        value.analysis.diagnostics.rejectedRanges = value.unsafeWindows
        meterKey = nil
        clearStereoHistory()
        lock.unlock()
    }

    func discardInFlight() {
        lock.lock()
        value.inFlight = max(0, value.inFlight - 1)
        value.analysis.diagnostics.inFlight = UInt32(max(0, value.inFlight))
        lock.unlock()
    }

    func markDiscontinuous() {
        lock.lock()
        value.analysis.token = nil
        value.analysis.streamStatus = .discontinuous
        value.analysis.levels = AudioLevelMetrics()
        value.analysis.stereo = AudioStereoMetrics()
        clearLoudnessHistory(status: .discontinuous)
        loudnessSession.markDiscontinuous()
        publishLoudnessSessionState()
        meterKey = nil
        clearStereoHistory()
        lock.unlock()
    }

    func markIdle() {
        lock.lock()
        guard value.analysis.streamStatus != .idle else {
            lock.unlock()
            return
        }
        value.analysis.streamStatus = .idle
        value.analysis.levels = AudioLevelMetrics()
        value.analysis.stereo = AudioStereoMetrics()
        clearLoudnessHistory(status: .idle)
        loudnessSession.markDiscontinuous()
        publishLoudnessSessionState()
        meterKey = nil
        clearStereoHistory()
        lock.unlock()
    }

    private func appendStereoHistory(token: AudioFrameToken, correlationValid: Bool) {
        guard correlationValid,
              let correlation = value.analysis.stereo.rollingCorrelation.value,
              let sideEnergy = value.analysis.stereo.sideEnergyFraction.value else {
            return
        }
        let interval = max(UInt64(1), UInt64(token.geometry.sampleRateHz) / 10)
        let endFrame = token.endFrame
        let nextFrame = nextStereoHistoryFrame ?? endFrame
        guard endFrame >= nextFrame else { return }
        let breakBefore: Bool
        if let last = latestStereoHistoryPoint {
            breakBefore = endFrame - last.endFrame > interval * 2
        } else {
            breakBefore = false
        }
        stereoHistory[stereoHistoryWriteIndex] = AudioStereoHistoryPoint(
            endFrame: endFrame,
            correlation: correlation,
            sideEnergyFraction: sideEnergy,
            breakBefore: breakBefore)
        stereoHistoryWriteIndex = (stereoHistoryWriteIndex + 1) % stereoHistory.count
        stereoHistoryCount = min(stereoHistory.count, stereoHistoryCount + 1)
        nextStereoHistoryFrame = endFrame &+ interval
    }

    private var latestStereoHistoryPoint: AudioStereoHistoryPoint? {
        guard stereoHistoryCount > 0 else { return nil }
        let index = (stereoHistoryWriteIndex + stereoHistory.count - 1) % stereoHistory.count
        return stereoHistory[index]
    }

    private func orderedStereoHistory() -> [AudioStereoHistoryPoint] {
        guard stereoHistoryCount > 0 else { return [] }
        let first = (stereoHistoryWriteIndex + stereoHistory.count - stereoHistoryCount)
            % stereoHistory.count
        return (0..<stereoHistoryCount).compactMap { offset in
            stereoHistory[(first + offset) % stereoHistory.count]
        }
    }

    private func clearStereoHistory() {
        stereoHistory = [AudioStereoHistoryPoint?](repeating: nil, count: Self.stereoHistoryCapacity)
        stereoHistoryWriteIndex = 0
        stereoHistoryCount = 0
        nextStereoHistoryFrame = nil
        value.stereoHistory = []
    }

    @discardableResult
    func startLoudnessMeasurement(sampleRateHz: UInt32) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard loudnessSession.start(sampleRateHz: sampleRateHz) else { return false }
        publishLoudnessSessionState()
        return true
    }

    func pauseLoudnessMeasurement() {
        lock.lock()
        loudnessSession.pause()
        publishLoudnessSessionState()
        lock.unlock()
    }

    func resumeLoudnessMeasurement() {
        lock.lock()
        loudnessSession.resume()
        publishLoudnessSessionState()
        lock.unlock()
    }

    func resetLoudnessMeasurement() {
        lock.lock()
        loudnessSession.reset()
        publishLoudnessSessionState()
        lock.unlock()
    }

    private func publishLoudnessSessionState() {
        value.analysis.loudness.sessionPhase = loudnessSession.phase
        value.analysis.loudness.integratedMeasurementID = loudnessSession.measurementID
        value.analysis.loudness.includedAudioFrames = loudnessSession.includedFrames
        let sessionStatus: AudioMeasurementStatus
        switch loudnessSession.phase {
        case .idle: sessionStatus = .idle
        case .running, .paused, .complete: sessionStatus = .warmingUp
        case .discontinuous: sessionStatus = .discontinuous
        }
        value.analysis.loudness.maximumMomentaryLUFS = measurement(
            loudnessSession.maximumMomentaryLUFS, status: sessionStatus)
        value.analysis.loudness.maximumShortTermLUFS = measurement(
            loudnessSession.maximumShortTermLUFS, status: sessionStatus)
        value.analysis.loudness.crestFactorDB = measurement(
            loudnessSession.crestFactorDB, status: sessionStatus)
        let maximumTruePeak = [loudnessSession.maximumTruePeakLeftDBTP,
                               loudnessSession.maximumTruePeakRightDBTP]
            .compactMap { $0 }.max()
        value.analysis.loudness.maximumTruePeakDBTP = measurement(
            maximumTruePeak, status: sessionStatus)
        if let integrated = loudnessSession.integratedLUFS, let maximumTruePeak {
            let plr = maximumTruePeak - integrated
            value.analysis.loudness.plrDB = loudnessSession.phase == .discontinuous
                ? .discontinuous(plr) : .valid(plr)
        } else {
            value.analysis.loudness.plrDB = AudioMeasurement(value: nil, status: sessionStatus)
        }
        if let integrated = loudnessSession.integratedLUFS {
            value.analysis.loudness.integratedLUFS = loudnessSession.phase == .discontinuous
                ? .discontinuous(integrated) : .valid(integrated)
        } else {
            let status: AudioMeasurementStatus
            switch loudnessSession.phase {
            case .idle: status = .idle
            case .running, .paused, .complete: status = .warmingUp
            case .discontinuous: status = .discontinuous
            }
            value.analysis.loudness.integratedLUFS = AudioMeasurement(value: nil, status: status)
        }
        if let range = loudnessSession.loudnessRangeLU {
            value.analysis.loudness.loudnessRangeLU = loudnessSession.phase == .discontinuous
                ? .discontinuous(range) : .valid(range)
        } else {
            let status: AudioMeasurementStatus
            switch loudnessSession.phase {
            case .idle: status = .idle
            case .running, .paused, .complete: status = .warmingUp
            case .discontinuous: status = .discontinuous
            }
            value.analysis.loudness.loudnessRangeLU = AudioMeasurement(value: nil, status: status)
        }
        value.analysis.loudness.loudnessRangeIsProvisional = loudnessSession.loudnessRangeIsProvisional
    }

    private func measurement(_ value: Float?, status: AudioMeasurementStatus) -> AudioMeasurement<Float> {
        guard let value else { return AudioMeasurement(value: nil, status: status) }
        return status == .discontinuous ? .discontinuous(value) : .valid(value)
    }

    private func loudnessValue(forLastChunks count: Int) -> Float? {
        guard loudnessCount >= count else { return nil }
        let first = (loudnessWriteIndex - count + loudnessEnergyRing.count) % loudnessEnergyRing.count
        var energy: Float = 0
        var frames: UInt32 = 0
        for offset in 0..<count {
            guard let chunk = loudnessEnergyRing[(first + offset) % loudnessEnergyRing.count] else {
                return nil
            }
            energy += chunk.weightedEnergy
            frames += chunk.frameCount
        }
        guard energy > 0, frames > 0 else { return -.infinity }
        return -0.691 + 10 * log10(energy / Float(frames))
    }

    private func clearLoudnessHistory(status: AudioMeasurementStatus) {
        loudnessEnergyRing = [AudioLoudnessEnergyChunk?](repeating: nil, count: 300)
        loudnessWriteIndex = 0
        loudnessCount = 0
        value.analysis.loudness.momentaryLUFS = AudioMeasurement(value: nil, status: status)
        value.analysis.loudness.shortTermLUFS = AudioMeasurement(value: nil, status: status)
        value.analysis.loudness.maximumMomentaryLUFS = .warmingUp
        value.analysis.loudness.maximumShortTermLUFS = .warmingUp
        value.analysis.loudness.acceptedAudioFrames = 0
    }
}

@MainActor
final class ASFWAudioObserverClient {
    private let guid: UInt64
    private var connection: io_connect_t = IO_OBJECT_NULL
    private var mappedAddress: mach_vm_address_t = 0
    private var mappedSize: mach_vm_size_t = 0
    private var mappingLifetime: AudioObserverMappingLifetime?
    private(set) var stateReader: AudioObserverStateReader?
    private var poller: AudioObserverPoller?
    private(set) var snapshot = AudioObserverSnapshot()

    let renderState = AudioObserverRenderState()
    let metrics = AudioObserverMetricsState()
    private(set) var metalDevice: MTLDevice?
    private(set) var ringBuffer: MTLBuffer?
    private(set) var phaseRenderPipeline: MTLRenderPipelineState?
    let plotHistory = AnalyzerPlotHistoryState()
    private(set) var waveformRenderPipeline: MTLRenderPipelineState?

    init(guid: UInt64) {
        self.guid = guid
    }

    func open() throws {
        guard connection == IO_OBJECT_NULL else { return }
        let service = IOServiceGetMatchingService(
            kIOMainPortDefault, IOServiceNameMatching("ASFWDriver"))
        guard service != IO_OBJECT_NULL else {
            throw AudioObserverError.serviceUnavailable
        }
        defer { IOObjectRelease(service) }

        let openResult = IOServiceOpen(service, mach_task_self_, 0, &connection)
        guard openResult == KERN_SUCCESS, connection != IO_OBJECT_NULL else {
            connection = IO_OBJECT_NULL
            throw AudioObserverError.openFailed(openResult)
        }

        let input = [guid]
        let selectResult = input.withUnsafeBufferPointer { buffer in
            IOConnectCallScalarMethod(connection, 66, buffer.baseAddress, 1,
                                      nil, nil)
        }
        guard selectResult == KERN_SUCCESS else {
            closeConnection()
            throw AudioObserverError.selectionFailed(selectResult)
        }

        stateReader = AudioObserverStateReader(connection: connection, guid: guid)
        var address: mach_vm_address_t = 0
        var length: mach_vm_size_t = 0
        let mapResult = IOConnectMapMemory64(
            connection, 2, mach_task_self_, &address, &length,
            UInt32(kIOMapAnywhere | kIOMapDefaultCache))
        guard mapResult == KERN_SUCCESS else {
            closeConnection()
            throw AudioObserverError.mappingFailed(mapResult)
        }
        guard address % UInt64(vm_page_size) == 0, length > 0,
              length % UInt64(vm_page_size) == 0, length <= UInt64(Int.max),
              let rawPointer = UnsafeMutableRawPointer(bitPattern: UInt(address)) else {
            closeConnection(mappedAddress: address, mappedSize: length)
            throw AudioObserverError.invalidGeometry
        }
        mappedAddress = address
        mappedSize = length
        let lifetime = AudioObserverMappingLifetime(connection: connection,
                                                    address: address)
        mappingLifetime = lifetime

        guard let stateReader, let initialState = try? stateReader.read() else {
            closeConnection()
            throw AudioObserverError.invalidGeometry
        }
        guard initialState.memoryGeneration == initialState.mappedGeneration else {
            closeConnection()
            throw AudioObserverError.memoryGenerationChanged
        }
        let bytesPerFrame = UInt64(initialState.channels) * UInt64(MemoryLayout<Float>.stride)
        let mappedFrames = UInt64(length) / bytesPerFrame
        guard mappedFrames >= UInt64(initialState.activeRingFrames) else {
            closeConnection()
            throw AudioObserverError.invalidGeometry
        }
        poller = AudioObserverPoller(reader: stateReader, mappedSize: UInt64(length))

        guard let device = MTLCreateSystemDefaultDevice() else {
            closeConnection()
            throw AudioObserverError.metalUnavailable
        }
        metalDevice = device
        guard let ring = device.makeBuffer(
            bytesNoCopy: rawPointer,
            length: Int(length),
            options: .storageModeShared,
            deallocator: { [lifetime] _, _ in withExtendedLifetime(lifetime) {} }) else {
            closeConnection()
            throw AudioObserverError.zeroCopyImportFailed
        }
        ringBuffer = ring

        guard let library = device.makeDefaultLibrary(),
              let phaseVertex = library.makeFunction(name: "asfwPhaseVertex"),
              let waveformVertex = library.makeFunction(name: "asfwWaveformVertex"),
              let fragment = library.makeFunction(name: "asfwAudioFragment"),
              let waveformFragment = library.makeFunction(name: "asfwWaveformFragment") else {
            closeConnection()
            throw AudioObserverError.shaderUnavailable
        }
        let phaseDescriptor = Self.renderDescriptor(vertex: phaseVertex,
                                                    fragment: fragment)
        let waveformDescriptor = Self.renderDescriptor(vertex: waveformVertex,
                                                       fragment: waveformFragment)
        do {
            phaseRenderPipeline = try device.makeRenderPipelineState(descriptor: phaseDescriptor)
            waveformRenderPipeline = try device.makeRenderPipelineState(descriptor: waveformDescriptor)
        } catch {
            closeConnection()
            throw AudioObserverError.pipelineFailed
        }
        snapshot = AudioObserverSnapshot(writeEndFrame: initialState.writeEndFrame,
                                          oldestValidFrame: initialState.oldestValidFrame,
                                          sessionEpoch: initialState.sessionEpoch,
                                          discontinuityEpoch: initialState.discontinuityEpoch,
                                          memoryGeneration: initialState.memoryGeneration,
                                          activeRingFrames: initialState.activeRingFrames,
                                          channels: initialState.channels,
                                          sampleRateHz: initialState.sampleRateHz,
                                          mappedFrames: mappedFrames,
                                          validHistoryFrames: UInt32(min(UInt64(initialState.activeRingFrames),
                                              initialState.writeEndFrame >= initialState.oldestValidFrame
                                                ? initialState.writeEndFrame - initialState.oldestValidFrame : 0)),
                                          ioRunning: initialState.writeEndFrame > initialState.oldestValidFrame)
        renderState.update(snapshot)
    }

    func poll() async throws -> AudioObserverSnapshot {
        guard mappedAddress != 0, let poller else {
            throw AudioObserverError.serviceUnavailable
        }
        snapshot = try await poller.poll()
        renderState.update(snapshot)
        return snapshot
    }

    func close() {
        closeConnection()
    }

    private static func renderDescriptor(vertex: MTLFunction,
                                         fragment: MTLFunction) -> MTLRenderPipelineDescriptor {
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = vertex
        descriptor.fragmentFunction = fragment
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        return descriptor
    }

    private func closeConnection(mappedAddress address: mach_vm_address_t? = nil,
                                 mappedSize size: mach_vm_size_t? = nil) {
        ringBuffer = nil
        phaseRenderPipeline = nil
        waveformRenderPipeline = nil
        metalDevice = nil
        stateReader = nil
        poller = nil
        if mappingLifetime != nil {
            mappingLifetime = nil
            connection = IO_OBJECT_NULL
            mappedAddress = 0
            mappedSize = 0
            return
        }
        let address = address ?? mappedAddress
        let size = size ?? mappedSize
        if connection != IO_OBJECT_NULL {
            if size != 0, address != 0 {
                IOConnectUnmapMemory64(connection, 2, mach_task_self_, address)
            }
            IOServiceClose(connection)
        }
        connection = IO_OBJECT_NULL
        mappedAddress = 0
        mappedSize = 0
    }
}

extension ASFWAudioObserverClient {
    static func guid(fromDeviceUID uid: String) -> UInt64? {
        guard uid.hasPrefix("ASFW-") else { return nil }
        return UInt64(uid.dropFirst(5), radix: 16)
    }
}
