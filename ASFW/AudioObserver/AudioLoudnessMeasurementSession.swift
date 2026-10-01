import Foundation

/// Owns the user-controlled Integrated Loudness interval. It receives only
/// 10 ms GPU-produced energy records, never PCM samples.
struct AudioLoudnessMeasurementSession: Sendable {
    static let supportedSampleRate: UInt32 = 48_000
    static let maximumDurationSeconds: UInt64 = 24 * 60 * 60
    private static let shortTermWindowChunks = 300
    private static let shortTermHopChunks = 10
    private static let chunksPerBlock = 40
    private static let chunksPerHop = 10
    private static let lraMinimumLUFS: Float = -100
    private static let lraBinWidthLU: Float = 0.05
    private static let lraBinCount = 4_401

    private(set) var phase: AudioLoudnessSessionPhase = .idle
    private(set) var measurementID: UInt64 = 0
    private(set) var sampleRateHz: UInt32 = 0
    private(set) var includedFrames: UInt64 = 0
    private(set) var integratedLUFS: Float?
    private(set) var loudnessRangeLU: Float?
    private(set) var loudnessRangeIsProvisional = false
    private(set) var maximumTruePeakDBTP: Float?

    private var blockWindow: [AudioLoudnessEnergyChunk] = []
    private var chunksSinceBlock = 0
    private var integratedBlockEnergies: [Float] = []
    private var lastIntegratedUpdate = Date.distantPast
    private var hasNewIntegratedBlocks = false
    private var shortTermEnergyRing = [Float](repeating: 0, count: shortTermWindowChunks)
    private var shortTermRingCount = 0
    private var shortTermWriteIndex = 0
    private var shortTermEnergySum: Double = 0
    private var chunksSinceShortTerm = 0
    private var lraHistogram = [UInt32](repeating: 0, count: lraBinCount)
    private var lraAbsolutePowerSum: Double = 0
    private var lraAbsoluteCount: UInt64 = 0
    private var hasNewLRAValues = false

    var includedDurationSeconds: Double {
        guard sampleRateHz > 0 else { return 0 }
        return Double(includedFrames) / Double(sampleRateHz)
    }

    @discardableResult
    mutating func start(sampleRateHz: UInt32) -> Bool {
        guard sampleRateHz == Self.supportedSampleRate else { return false }
        clear()
        self.sampleRateHz = sampleRateHz
        integratedBlockEnergies.reserveCapacity(Int(Self.maximumIntegratedBlocks))
        lraHistogram = [UInt32](repeating: 0, count: Self.lraBinCount)
        measurementID &+= 1
        phase = .running
        return true
    }

    mutating func pause() {
        guard phase == .running else { return }
        phase = .paused
        clearBlockWindow()
        recomputeLRAIfNeeded()
        recomputeIntegratedIfNeeded(now: Date())
    }

    mutating func resume() {
        guard phase == .paused else { return }
        phase = .running
        clearBlockWindow()
    }

    mutating func reset() {
        let nextID = measurementID &+ 1
        clear()
        measurementID = nextID
    }

    mutating func markDiscontinuous() {
        guard phase == .running || phase == .paused else { return }
        phase = .discontinuous
        clearBlockWindow()
        recomputeLRAIfNeeded()
        recomputeIntegratedIfNeeded(now: Date())
    }

    mutating func consumeTruePeak(amplitude: Float) {
        guard phase == .running, amplitude.isFinite, amplitude > 0 else { return }
        let dbtp = 20 * log10(amplitude)
        maximumTruePeakDBTP = max(maximumTruePeakDBTP ?? -.infinity, dbtp)
    }

    /// Returns true when the cached Integrated value was recomputed.
    @discardableResult
    mutating func consume(_ chunk: AudioLoudnessEnergyChunk,
                          now: Date = Date()) -> Bool {
        guard phase == .running,
              chunk.frameCount == 480,
              chunk.weightedEnergy.isFinite,
              chunk.weightedEnergy >= 0,
              includedFrames < Self.maximumFrames else { return false }

        includedFrames += UInt64(chunk.frameCount)
        blockWindow.append(chunk)
        chunksSinceBlock += 1
        if let shortTerm = appendShortTermChunk(chunk) {
            let power = pow(10, Double(shortTerm) / 10)
            lraAbsolutePowerSum += power
            lraAbsoluteCount &+= 1
            let rawBin = Int(floor((shortTerm - Self.lraMinimumLUFS) / Self.lraBinWidthLU))
            let bin = min(Self.lraBinCount - 1, max(0, rawBin))
            lraHistogram[bin] &+= 1
            hasNewLRAValues = true
        }

        if blockWindow.count == Self.chunksPerBlock && chunksSinceBlock >= Self.chunksPerHop {
            let summedEnergy = blockWindow.reduce(Float.zero) { $0 + $1.weightedEnergy }
            let frameCount = blockWindow.reduce(UInt64.zero) { $0 + UInt64($1.frameCount) }
            if frameCount > 0 {
                integratedBlockEnergies.append(summedEnergy / Float(frameCount))
                hasNewIntegratedBlocks = true
            }
            blockWindow.removeFirst(Self.chunksPerHop)
            chunksSinceBlock = 0
        }

        if includedFrames >= Self.maximumFrames {
            phase = .complete
            clearBlockWindow()
            recomputeLRAIfNeeded()
            recomputeIntegratedIfNeeded(now: now)
        }

        guard now.timeIntervalSince(lastIntegratedUpdate) >= 1 else { return false }
        let integratedUpdated = recomputeIntegratedIfNeeded(now: now)
        recomputeLRAIfNeeded()
        return integratedUpdated
    }

    static func gatedIntegratedLoudness(_ energies: [Float]) -> Float? {
        // EBU Tech 3343-2023 §11.1.1: 400 ms blocks, -70 LUFS absolute gate,
        // then a relative gate 10 LU below the absolute-gated loudness.
        let absoluteGateEnergy = pow(10.0, (-70.0 + 0.691) / 10.0)
        var absoluteEnergySum: Double = 0
        var absoluteBlockCount = 0
        for energy in energies where energy.isFinite && Double(energy) >= absoluteGateEnergy {
            absoluteEnergySum += Double(energy)
            absoluteBlockCount += 1
        }
        guard absoluteBlockCount > 0 else { return nil }
        let absoluteMean = absoluteEnergySum / Double(absoluteBlockCount)
        let relativeGateEnergy = max(absoluteGateEnergy, absoluteMean * 0.1)
        var gatedEnergySum: Double = 0
        var gatedBlockCount = 0
        for energy in energies where energy.isFinite && Double(energy) >= relativeGateEnergy {
            gatedEnergySum += Double(energy)
            gatedBlockCount += 1
        }
        guard gatedBlockCount > 0 else { return nil }
        let mean = Float(gatedEnergySum / Double(gatedBlockCount))
        return lufs(forMeanEnergy: mean)
    }

    private static func lufs(forMeanEnergy energy: Float) -> Float {
        guard energy > 0 else { return -.infinity }
        return -0.691 + 10 * log10(energy)
    }

    private static var maximumFrames: UInt64 {
        maximumDurationSeconds * UInt64(supportedSampleRate)
    }

    private static var maximumIntegratedBlocks: UInt64 {
        maximumDurationSeconds * 10
    }

    @discardableResult
    private mutating func recomputeIntegratedIfNeeded(now: Date) -> Bool {
        guard hasNewIntegratedBlocks else { return false }
        integratedLUFS = Self.gatedIntegratedLoudness(integratedBlockEnergies)
        lastIntegratedUpdate = now
        hasNewIntegratedBlocks = false
        return true
    }

    private mutating func appendShortTermChunk(_ chunk: AudioLoudnessEnergyChunk) -> Float? {
        if shortTermRingCount < Self.shortTermWindowChunks {
            shortTermRingCount += 1
        } else {
            shortTermEnergySum -= Double(shortTermEnergyRing[shortTermWriteIndex])
        }
        shortTermEnergyRing[shortTermWriteIndex] = chunk.weightedEnergy
        shortTermEnergySum += Double(chunk.weightedEnergy)
        shortTermWriteIndex = (shortTermWriteIndex + 1) % Self.shortTermWindowChunks
        chunksSinceShortTerm += 1

        guard shortTermRingCount == Self.shortTermWindowChunks,
              chunksSinceShortTerm >= Self.shortTermHopChunks else { return nil }
        chunksSinceShortTerm = 0
        let frameCount = Self.shortTermWindowChunks * 480
        let meanEnergy = Float(shortTermEnergySum / Double(frameCount))
        let loudness = Self.lufs(forMeanEnergy: meanEnergy)
        return loudness >= -70 ? loudness : nil
    }

    private mutating func recomputeLRAIfNeeded() {
        // EBU Tech 3342-2023: 3 s short-term windows, -70 LUFS absolute gate,
        // -20 LU relative gate, and the P95−P10 percentile range.
        guard hasNewLRAValues else { return }
        guard lraAbsoluteCount > 0 else {
            loudnessRangeLU = nil
            loudnessRangeIsProvisional = includedDurationSeconds < 60
            hasNewLRAValues = false
            return
        }

        let meanPower = lraAbsolutePowerSum / Double(lraAbsoluteCount)
        let relativeGate = max(-70, -0.691 + 10 * log10(meanPower) - 20)
        let firstBin = max(0, min(Self.lraBinCount - 1,
            Int(ceil((relativeGate - Double(Self.lraMinimumLUFS)) /
                     Double(Self.lraBinWidthLU)))))
        let gatedCount = lraHistogram[firstBin...].reduce(UInt64.zero) { $0 + UInt64($1) }
        guard gatedCount > 0 else {
            loudnessRangeLU = nil
            loudnessRangeIsProvisional = includedDurationSeconds < 60
            hasNewLRAValues = false
            return
        }

        let low = Self.histogramPercentile(0.10, firstBin: firstBin,
                                           count: gatedCount, histogram: lraHistogram)
        let high = Self.histogramPercentile(0.95, firstBin: firstBin,
                                            count: gatedCount, histogram: lraHistogram)
        loudnessRangeLU = max(0, high - low)
        loudnessRangeIsProvisional = includedDurationSeconds < 60
        hasNewLRAValues = false
    }

    private static func histogramPercentile(_ percentile: Double,
                                            firstBin: Int,
                                            count: UInt64,
                                            histogram: [UInt32]) -> Float {
        let rank = Double(count - 1) * percentile
        let lowerRank = UInt64(floor(rank))
        let upperRank = min(count - 1, lowerRank + 1)
        let fraction = Float(rank - Double(lowerRank))
        let lower = histogramValue(at: lowerRank, firstBin: firstBin, histogram: histogram)
        let upper = histogramValue(at: upperRank, firstBin: firstBin, histogram: histogram)
        return lower + (upper - lower) * fraction
    }

    private static func histogramValue(at rank: UInt64,
                                       firstBin: Int,
                                       histogram: [UInt32]) -> Float {
        var cumulative: UInt64 = 0
        for bin in firstBin..<histogram.count {
            cumulative += UInt64(histogram[bin])
            if cumulative > rank {
                return lraMinimumLUFS + (Float(bin) + 0.5) * lraBinWidthLU
            }
        }
        return lraMinimumLUFS + (Float(histogram.count - 1) + 0.5) * lraBinWidthLU
    }

    private mutating func clear() {
        phase = .idle
        sampleRateHz = 0
        includedFrames = 0
        integratedLUFS = nil
        loudnessRangeLU = nil
        loudnessRangeIsProvisional = false
        maximumTruePeakDBTP = nil
        integratedBlockEnergies.removeAll(keepingCapacity: false)
        clearBlockWindow()
        lraHistogram = [UInt32](repeating: 0, count: Self.lraBinCount)
        lraAbsolutePowerSum = 0
        lraAbsoluteCount = 0
        hasNewLRAValues = false
        lastIntegratedUpdate = .distantPast
        hasNewIntegratedBlocks = false
    }

    private mutating func clearBlockWindow() {
        blockWindow.removeAll(keepingCapacity: true)
        chunksSinceBlock = 0
        shortTermEnergyRing = [Float](repeating: 0, count: Self.shortTermWindowChunks)
        shortTermRingCount = 0
        shortTermWriteIndex = 0
        shortTermEnergySum = 0
        chunksSinceShortTerm = 0
    }
}
