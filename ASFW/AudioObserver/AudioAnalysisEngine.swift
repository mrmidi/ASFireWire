import Foundation
@preconcurrency import Metal
import QuartzCore

nonisolated private struct ConsumeRangeParams {
    var startFrame: UInt64
    var frameCount: UInt32
    var ringFrames: UInt32
    var channels: UInt32
    var leftChannel: UInt32
    var rightChannel: UInt32
}

nonisolated private final class AnalysisScheduledTimestamp: @unchecked Sendable {
    private let lock = NSLock()
    private var timestamp: Double?

    func set(_ value: Double) {
        lock.lock()
        timestamp = value
        lock.unlock()
    }

    func get() -> Double? {
        lock.lock()
        defer { lock.unlock() }
        return timestamp
    }
}

/// The command buffer writes this shared result only on the GPU; the immutable
/// Metal buffer is read after command completion, then copied into Sendable scalars.
nonisolated private final class AnalysisOutputSlot: @unchecked Sendable {
    private let buffer: MTLBuffer

    init(_ buffer: MTLBuffer) { self.buffer = buffer }

    func copyValues(count: Int) -> [UInt32] {
        let pointer = buffer.contents().assumingMemoryBound(to: UInt32.self)
        return Array(UnsafeBufferPointer(start: pointer, count: count))
    }
}

/// Serial, cursor-driven consumer for newly written output frames. It never
/// reads PCM on the CPU and publishes only scalar GPU reductions.
actor AudioAnalysisEngine {
    private let ringBuffer: MTLBuffer
    private let outputBuffer: MTLBuffer
    private let pipeline: MTLComputePipelineState
    private let kWeightPipeline: MTLComputePipelineState
    private let truePeakPipeline: MTLComputePipelineState
    private let truePeakBuffer: MTLBuffer
    private let kernelTiming: AnalyzerKernelTiming?
    private var committedFilterState: MTLBuffer
    private var provisionalFilterState: MTLBuffer
    private let queue: MTLCommandQueue
    private let stateReader: any AudioAnalysisStateReading
    private let metrics: AudioObserverMetricsState
    private let plotHistory: AnalyzerPlotHistoryState
    private var loudnessHistory: [AnalyzerHistoryVertex] = []
    private var lastHistoryPublish: Double = -.infinity
    private var lastCompletionPublish: Double = -.infinity
    private var completionNotificationPending = false
    private var onCompletion: (@MainActor @Sendable () -> Void)?
    private var stopWaiters: [CheckedContinuation<Void, Never>] = []

    func setCompletionHandler(_ handler: @escaping @MainActor @Sendable () -> Void) {
        guard !stopped else { return }
        onCompletion = handler
    }
    private var cursor: UInt64?
    private var lastSessionEpoch: UInt64?
    private var lastDiscontinuityEpoch: UInt64?
    private var lastMemoryGeneration: UInt64?
    private var lastGeometry: AudioRingGeometry?
    private var pair = AudioChannelPair()
    private var pendingPair: AudioChannelPair?
    private var inFlight = false
    private var stopped = false
    private var resetFilterState = true
    private let writerHeadroomFrames: UInt64 = 4_096

    /// The acquisition clock belongs to the engine, never to SwiftUI. No PCM is
    /// copied: only the small control snapshot is read on this actor.
    func run(renderState: AudioObserverRenderState,
             onLifecycle: @escaping @MainActor @Sendable (AudioObserverSnapshot) -> Void) async throws {
        var previous: AudioObserverSnapshot?
        var lastWrite: UInt64?
        var lastProgress = CACurrentMediaTime()
        while !Task.isCancelled && !stopped {
            let wire = try stateReader.read()
            guard wire.memoryGeneration == wire.mappedGeneration else {
                throw AudioObserverError.memoryGenerationChanged
            }
            let bytesPerFrame = UInt64(wire.channels) * 4
            guard bytesPerFrame > 0, UInt64(ringBuffer.length) / bytesPerFrame >= UInt64(wire.activeRingFrames) else {
                throw AudioObserverError.invalidGeometry
            }
            let now = CACurrentMediaTime()
            if lastWrite != wire.writeEndFrame { lastWrite = wire.writeEndFrame; lastProgress = now }
            let running = now - lastProgress < 0.5 && wire.writeEndFrame > wire.oldestValidFrame
            let valid = wire.writeEndFrame >= wire.oldestValidFrame
                ? min(UInt64(wire.activeRingFrames), wire.writeEndFrame - wire.oldestValidFrame) : 0
            let snapshot = AudioObserverSnapshot(writeEndFrame: wire.writeEndFrame,
                oldestValidFrame: wire.oldestValidFrame, sessionEpoch: wire.sessionEpoch,
                discontinuityEpoch: wire.discontinuityEpoch, memoryGeneration: wire.memoryGeneration,
                activeRingFrames: wire.activeRingFrames, channels: wire.channels, sampleRateHz: wire.sampleRateHz,
                mappedFrames: UInt64(ringBuffer.length) / bytesPerFrame,
                validHistoryFrames: running ? UInt32(valid) : 0, ioRunning: running)
            renderState.update(snapshot)
            if previous?.ioRunning != snapshot.ioRunning || previous?.memoryGeneration != snapshot.memoryGeneration ||
                previous?.sessionEpoch != snapshot.sessionEpoch || previous?.discontinuityEpoch != snapshot.discontinuityEpoch ||
                previous?.sampleRateHz != snapshot.sampleRateHz || previous?.channels != snapshot.channels ||
                previous?.activeRingFrames != snapshot.activeRingFrames {
                if !running { metrics.markIdle() }
                // Rare lifecycle changes may notify the UI; acquiring the next
                // range never waits for its main executor.
                Task { @MainActor in onLifecycle(snapshot) }
            }
            previous = snapshot
            if running { consume(snapshot) }
            // Twenty milliseconds amortizes submission/completion overhead.
            // consume remains cursor based, so every accepted frame is measured.
            // 25 passes/s: each pass is a command buffer, a completion and two
            // driver state reads. Loudness keeps its 10 ms chunks per batch.
            try await Task.sleep(for: .milliseconds(40), tolerance: .milliseconds(5))
        }
    }

    /// Pipeline setup is also CPU work and must not stall the UI on connection.
    @concurrent
    static func make(device: MTLDevice, ringBuffer: MTLBuffer,
                     stateReader: any AudioAnalysisStateReading,
                     metrics: AudioObserverMetricsState,
                     plotHistory: AnalyzerPlotHistoryState) async throws -> AudioAnalysisEngine {
        try AudioAnalysisEngine(device: device, ringBuffer: ringBuffer,
            stateReader: stateReader, metrics: metrics, plotHistory: plotHistory)
    }

    init(device: MTLDevice,
         ringBuffer: MTLBuffer,
         stateReader: any AudioAnalysisStateReading,
         metrics: AudioObserverMetricsState,
         plotHistory: AnalyzerPlotHistoryState) throws {
        guard device.maxThreadsPerThreadgroup.width >= 256,
              let library = device.makeDefaultLibrary(),
              let function = library.makeFunction(name: "asfwConsumeOutputRange"),
              let kFunction = library.makeFunction(name: "asfwKWeightRange"),
              let tpFunction = library.makeFunction(name: "asfwTruePeakRange"),
              let tpBuffer = device.makeBuffer(length: Int(AudioAnalysisLayout.maximumBatchFrames) * MemoryLayout<SIMD2<Float>>.stride, options: .storageModePrivate),
              let queue = device.makeCommandQueue(),
              let output = device.makeBuffer(length: AudioAnalysisLayout.outputWords * MemoryLayout<UInt32>.stride,
                                             options: .storageModeShared),
              let committed = device.makeBuffer(length: 49 * MemoryLayout<UInt32>.stride,
                                                options: .storageModeShared),
              let provisional = device.makeBuffer(length: 49 * MemoryLayout<UInt32>.stride,
                                                  options: .storageModeShared) else {
            throw AudioObserverError.pipelineFailed
        }
        self.ringBuffer = ringBuffer
        self.stateReader = stateReader
        self.metrics = metrics
        self.plotHistory = plotHistory
        self.queue = queue
        self.outputBuffer = output
        truePeakBuffer = tpBuffer
        kernelTiming = AnalyzerKernelTiming(device: device)
        committedFilterState = committed
        provisionalFilterState = provisional
        committed.contents().initializeMemory(as: UInt8.self, repeating: 0, count: committed.length)
        provisional.contents().initializeMemory(as: UInt8.self, repeating: 0, count: provisional.length)
        do {
            pipeline = try device.makeComputePipelineState(function: function)
            kWeightPipeline = try device.makeComputePipelineState(function: kFunction)
            truePeakPipeline = try device.makeComputePipelineState(function: tpFunction)
        } catch {
            throw AudioObserverError.pipelineFailed
        }
    }

    func setPair(_ next: AudioChannelPair) {
        guard !stopped, next.generation >= (pendingPair ?? pair).generation else { return }
        pendingPair = next
    }

    private func setPair(left: UInt32, right: UInt32, generation: UInt64) {
        let next = AudioChannelPair(leftIndex: left, rightIndex: right, generation: generation)
        guard pair != next else { return }
        pair = next
        cursor = nil
        resetFilterState = true
        metrics.markDiscontinuous()
    }

    func consume(_ snapshot: AudioObserverSnapshot, pair nextPair: AudioChannelPair? = nil) {
        guard !stopped else { return }
        if let nextPair { setPair(nextPair) }
        guard !inFlight else { return }
        if let next = pendingPair {
            pendingPair = nil
            setPair(left: next.leftIndex, right: next.rightIndex, generation: next.generation)
        }
        guard snapshot.ioRunning,
              snapshot.activeRingFrames > 0,
              snapshot.channels >= 2,
              let resolvedPair = pair.resolved(channelCount: snapshot.channels) else { return }

        let geometry = AudioRingGeometry(sampleRateHz: snapshot.sampleRateHz,
                                         channels: snapshot.channels,
                                         activeFrames: snapshot.activeRingFrames,
                                         mappedFrames: snapshot.mappedFrames,
                                         memoryGeneration: snapshot.memoryGeneration)
        let changed = lastSessionEpoch != snapshot.sessionEpoch ||
            lastDiscontinuityEpoch != snapshot.discontinuityEpoch ||
            lastMemoryGeneration != snapshot.memoryGeneration ||
            lastGeometry != geometry
        if changed {
            if lastGeometry != nil {
                resetFilterState = true
                metrics.markDiscontinuous()
            }
            cursor = snapshot.writeEndFrame
            lastSessionEpoch = snapshot.sessionEpoch
            lastDiscontinuityEpoch = snapshot.discontinuityEpoch
            lastMemoryGeneration = snapshot.memoryGeneration
            lastGeometry = geometry
            return
        }
        guard !inFlight else { return }
        let availableEnd = snapshot.writeEndFrame
        guard let start = cursor else {
            cursor = availableEnd
            return
        }
        guard availableEnd > start else { return }

        let ringFrames = UInt64(snapshot.activeRingFrames)
        let backlog = availableEnd - start
        let end = AudioAnalysisLayout.batchEnd(start: start, availableEnd: availableEnd)
        let distance = end - start
        guard start >= snapshot.oldestValidFrame,
              ringFrames > writerHeadroomFrames,
              backlog < ringFrames - writerHeadroomFrames,
              distance <= UInt64(UInt32.max) else {
            cursor = availableEnd
            metrics.rejected()
            return
        }

        guard let commandBuffer = queue.makeCommandBuffer() else {
            cursor = end
            metrics.rejected()
            return
        }
        let usesKWeight = snapshot.sampleRateHz == 48_000
        if usesKWeight, resetFilterState {
            committedFilterState.contents().initializeMemory(as: UInt8.self, repeating: 0,
                                                              count: committedFilterState.length)
        }
        if usesKWeight {
            guard let blit = commandBuffer.makeBlitCommandEncoder() else {
                cursor = end
                metrics.rejected()
                return
            }
            blit.copy(from: committedFilterState, sourceOffset: 0,
                      to: provisionalFilterState, destinationOffset: 0,
                      size: committedFilterState.length)
            blit.endEncoding()
        }
        let timing = metrics.kernelTimingEnabled() ? kernelTiming : nil
        guard let encoder = timing != nil ? timing!.encoder(commandBuffer, stage: 0) : commandBuffer.makeComputeCommandEncoder() else {
            cursor = end
            metrics.rejected()
            return
        }
        var params = ConsumeRangeParams(startFrame: start,
                                        frameCount: UInt32(distance),
                                        ringFrames: snapshot.activeRingFrames,
                                        channels: snapshot.channels,
                                        leftChannel: resolvedPair.leftIndex,
                                        rightChannel: resolvedPair.rightIndex)
        let encodeStart = CACurrentMediaTime()
        encoder.setComputePipelineState(pipeline)
        encoder.setBuffer(ringBuffer, offset: 0, index: 0)
        encoder.setBuffer(outputBuffer, offset: 0, index: 1)
        encoder.setBytes(&params, length: MemoryLayout<ConsumeRangeParams>.stride, index: 2)
        encoder.dispatchThreads(MTLSize(width: 256, height: 1, depth: 1),
                                threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
        encoder.endEncoding()
        if usesKWeight {
            guard let tpEncoder = timing != nil ? timing!.encoder(commandBuffer, stage: 1) : commandBuffer.makeComputeCommandEncoder() else {
                cursor = end; metrics.rejected(); return
            }
            tpEncoder.setComputePipelineState(truePeakPipeline)
            tpEncoder.setBuffer(ringBuffer, offset: 0, index: 0)
            tpEncoder.setBuffer(provisionalFilterState, offset: 0, index: 1)
            tpEncoder.setBuffer(truePeakBuffer, offset: 0, index: 2)
            tpEncoder.setBytes(&params, length: MemoryLayout<ConsumeRangeParams>.stride, index: 3)
            tpEncoder.dispatchThreads(MTLSize(width: Int(distance), height: 1, depth: 1),
                                     threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
            tpEncoder.endEncoding()
            guard let kEncoder = timing != nil ? timing!.encoder(commandBuffer, stage: 2) : commandBuffer.makeComputeCommandEncoder() else {
                cursor = end
                metrics.rejected()
                return
            }
            kEncoder.setComputePipelineState(kWeightPipeline)
            kEncoder.setBuffer(ringBuffer, offset: 0, index: 0)
            kEncoder.setBuffer(provisionalFilterState, offset: 0, index: 1)
            kEncoder.setBuffer(outputBuffer, offset: 0, index: 2)
            kEncoder.setBuffer(truePeakBuffer, offset: 0, index: 4)
            kEncoder.setBytes(&params, length: MemoryLayout<ConsumeRangeParams>.stride, index: 3)
            kEncoder.dispatchThreads(MTLSize(width: 1, height: 1, depth: 1),
                                     threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
            kEncoder.endEncoding()
            resetFilterState = false
        }
        let encodeMilliseconds = (CACurrentMediaTime() - encodeStart) * 1_000

        let token = AudioFrameToken(geometry: geometry,
                                    sessionEpoch: snapshot.sessionEpoch,
                                    discontinuityEpoch: snapshot.discontinuityEpoch,
                                    routingGeneration: resolvedPair.generation,
                                    startFrame: start,
                                    endFrame: end)
        let key = "\(snapshot.sessionEpoch):\(snapshot.discontinuityEpoch):\(resolvedPair.leftIndex):\(resolvedPair.rightIndex):\(resolvedPair.generation)"
        let crossesWrap = start % ringFrames + distance > ringFrames
        let metrics = self.metrics
        let reader = stateReader
        let output = AnalysisOutputSlot(outputBuffer)
        let scheduled = AnalysisScheduledTimestamp()
        inFlight = true
        metrics.submitted(crossesWrap: crossesWrap)
        commandBuffer.addScheduledHandler { _ in scheduled.set(CACurrentMediaTime()) }
        commandBuffer.addCompletedHandler { completed in
            let completionTime = CACurrentMediaTime()
            if completed.status == .completed, let timing {
                metrics.recordKernels(reduction: timing.milliseconds(stage: 0),
                    truePeak: usesKWeight ? timing.milliseconds(stage: 1) : nil,
                    weighting: usesKWeight ? timing.milliseconds(stage: 2) : nil,
                    at: ProcessInfo.processInfo.systemUptime)
            }
            let outputValues = output.copyValues(count: AudioAnalysisLayout.outputWords)
            let gpuStart = completed.gpuStartTime
            let gpuEnd = completed.gpuEndTime
            let gpuMilliseconds = gpuStart > 0 && gpuEnd >= gpuStart
                ? (gpuEnd - gpuStart) * 1_000 : nil
            let queueMilliseconds = gpuStart > 0
                ? scheduled.get().map { max(0, gpuStart - $0) * 1_000 } : nil
            let postState = try? reader.read()
            let geometryStillMatches = postState.map {
                $0.memoryGeneration == token.geometry.memoryGeneration &&
                $0.activeRingFrames == token.geometry.activeFrames &&
                $0.channels == token.geometry.channels &&
                $0.sampleRateHz == token.geometry.sampleRateHz &&
                $0.sessionEpoch == token.sessionEpoch &&
                $0.discontinuityEpoch == token.discontinuityEpoch &&
                $0.mappedGeneration == token.geometry.memoryGeneration &&
                $0.oldestValidFrame <= token.startFrame &&
                $0.writeEndFrame >= token.endFrame &&
                $0.writeEndFrame - token.startFrame < UInt64(token.geometry.activeFrames) - 4_096
            } ?? false
            let advanced = postState.map { $0.writeEndFrame >= token.endFrame
                ? $0.writeEndFrame - token.endFrame : 0 } ?? 0
            let sampleAge = Double(advanced) / Double(token.geometry.sampleRateHz) * 1_000
            let advancedFromStart = postState.map { $0.writeEndFrame >= token.startFrame
                ? $0.writeEndFrame - token.startFrame : UInt64.max } ?? 0
            let marginFrames = Int64(token.geometry.activeFrames) -
                Int64(clamping: advancedFromStart) - Int64(4_096)
            let marginMilliseconds = Double(marginFrames) / Double(token.geometry.sampleRateHz) * 1_000
            let completedSuccessfully = completed.status == .completed
            Task {
                await self.finish(token: token, pair: resolvedPair, postState: postState,
                    values: outputValues, completedSuccessfully: completedSuccessfully,
                    geometryStillMatches: geometryStillMatches, usesKWeight: usesKWeight,
                    encodeMilliseconds: encodeMilliseconds, queueMilliseconds: queueMilliseconds,
                    gpuMilliseconds: gpuMilliseconds, completionMilliseconds: (completionTime - encodeStart) * 1_000, analysisStartedAt: encodeStart,
                    sampleAge: sampleAge, marginMilliseconds: marginMilliseconds, key: key)
            }
        }
        commandBuffer.commit()
        cursor = end
    }

    private func finish(token: AudioFrameToken, pair resolvedPair: AudioChannelPair,
                        postState: AudioObserverWireState?, values outputValues: [UInt32],
                        completedSuccessfully: Bool, geometryStillMatches: Bool, usesKWeight: Bool,
                        encodeMilliseconds: Double, queueMilliseconds: Double?, gpuMilliseconds: Double?,
                        completionMilliseconds: Double, analysisStartedAt: Double, sampleAge: Double, marginMilliseconds: Double,
                        key: String) {
        inFlight = false
        defer {
            let waiters = stopWaiters
            stopWaiters.removeAll()
            for waiter in waiters { waiter.resume() }
        }
        guard !self.stopped else {
            metrics.discardInFlight()
            return
        }
        guard self.pair == resolvedPair else {
            self.cursor = postState?.writeEndFrame
            metrics.discardInFlight()
            return
        }
        if completedSuccessfully, geometryStillMatches,
           let postState, outputValues.count == AudioAnalysisLayout.outputWords {
            outputValues.withUnsafeBufferPointer { values in
                metrics.accept(token: token,
                               pair: resolvedPair,
                               result: values,
                               correlationValid: outputValues[12] != 0,
                               cpuEncodeMilliseconds: encodeMilliseconds,
                               scheduledToStartMilliseconds: queueMilliseconds,
                               gpuMilliseconds: gpuMilliseconds,
                               completionMilliseconds: completionMilliseconds,
                               sampleAgeMilliseconds: sampleAge,
                               overwriteMarginMilliseconds: marginMilliseconds,
                               meterKey: key, analysisStartedAt: analysisStartedAt)
            }
            self.cursor = token.endFrame
            self.lastSessionEpoch = postState.sessionEpoch
            self.lastDiscontinuityEpoch = postState.discontinuityEpoch
            if usesKWeight {
                swap(&self.committedFilterState, &self.provisionalFilterState)
            }
        } else {
            self.cursor = postState?.writeEndFrame
            if usesKWeight { self.resetFilterState = true }
            metrics.rejected()
        }
        publishPlotHistory()
        let now = CACurrentMediaTime()
        // At most one UI notification may be queued. A busy main actor must
        // not accumulate stale completion tasks while GPU analysis continues.
        if let onCompletion, !completionNotificationPending,
           now - lastCompletionPublish >= 1.0 / 60.0 {
            lastCompletionPublish = now
            completionNotificationPending = true
            Task { @MainActor in
                onCompletion()
                await self.didPublishCompletion()
            }
        }
    }

    private func didPublishCompletion() { completionNotificationPending = false }

    private func publishPlotHistory() {
        let now = CACurrentMediaTime()
        guard now - lastHistoryPublish >= 0.1 else { return }
        lastHistoryPublish = now
        let snapshot = metrics.read(includeHistory: false)
        if let token = snapshot.analysis.token {
            let loudness = snapshot.analysis.loudness
            let frame = token.endFrame
            if let last = loudnessHistory.last, frame < last.frame { loudnessHistory.removeAll() }
            if loudnessHistory.last?.frame != frame {
                loudnessHistory.append(AnalyzerHistoryVertex(frame: frame,
                    correlation: loudness.momentaryLUFS.value ?? .nan,
                    sideEnergy: loudness.shortTermLUFS.value ?? .nan,
                    breakBefore: snapshot.analysis.streamStatus == .discontinuous ? 1 : 0,
                    integrated: loudness.integratedLUFS.value ?? .nan))
                let duration = UInt64(token.geometry.sampleRateHz) * 60
                loudnessHistory.removeAll { frame > $0.frame && frame - $0.frame > duration }
            }
        }
        plotHistory.update(stereo: metrics.readStereoHistory(), loudness: loudnessHistory)
    }

    /// Quiesce before the owner closes the mapping. Suspension releases this
    /// actor so the pending completion can discard its result and resume us.
    func stop() async {
        stopped = true
        onCompletion = nil
        guard inFlight else { return }
        await withCheckedContinuation { stopWaiters.append($0) }
    }
}
