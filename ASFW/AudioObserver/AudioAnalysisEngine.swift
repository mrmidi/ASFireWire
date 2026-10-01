import Foundation
@preconcurrency import Metal
import QuartzCore

private struct ConsumeRangeParams {
    var startFrame: UInt64
    var frameCount: UInt32
    var ringFrames: UInt32
    var channels: UInt32
    var leftChannel: UInt32
    var rightChannel: UInt32
}

private final class AnalysisScheduledTimestamp: @unchecked Sendable {
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
private final class AnalysisOutputSlot: @unchecked Sendable {
    private let buffer: MTLBuffer

    init(_ buffer: MTLBuffer) { self.buffer = buffer }

    func copyValues(count: Int) -> [UInt32] {
        let pointer = buffer.contents().assumingMemoryBound(to: UInt32.self)
        return (0..<count).map { pointer[$0] }
    }
}

/// Serial, cursor-driven consumer for newly written output frames. It never
/// reads PCM on the CPU and publishes only scalar GPU reductions.
@MainActor
final class AudioAnalysisEngine {
    private let ringBuffer: MTLBuffer
    private let outputBuffer: MTLBuffer
    private let pipeline: MTLComputePipelineState
    private let kWeightPipeline: MTLComputePipelineState
    private var committedFilterState: MTLBuffer
    private var provisionalFilterState: MTLBuffer
    private let queue: MTLCommandQueue
    private let stateReader: AudioObserverStateReader
    private let metrics: AudioObserverMetricsState
    private var cursor: UInt64?
    private var lastSessionEpoch: UInt64?
    private var lastDiscontinuityEpoch: UInt64?
    private var lastMemoryGeneration: UInt64?
    private var lastGeometry: AudioRingGeometry?
    private var pair = AudioChannelPair()
    private var inFlight = false
    private var stopped = false
    private var resetFilterState = true
    private let writerHeadroomFrames: UInt64 = 4_096

    init(device: MTLDevice,
         ringBuffer: MTLBuffer,
         stateReader: AudioObserverStateReader,
         metrics: AudioObserverMetricsState) throws {
        guard device.maxThreadsPerThreadgroup.width >= 256,
              let library = device.makeDefaultLibrary(),
              let function = library.makeFunction(name: "asfwConsumeOutputRange"),
              let kFunction = library.makeFunction(name: "asfwKWeightRange"),
              let queue = device.makeCommandQueue(),
              let output = device.makeBuffer(length: 96 * MemoryLayout<UInt32>.stride,
                                             options: .storageModeShared),
              let committed = device.makeBuffer(length: 20 * MemoryLayout<UInt32>.stride,
                                                options: .storageModeShared),
              let provisional = device.makeBuffer(length: 20 * MemoryLayout<UInt32>.stride,
                                                  options: .storageModeShared) else {
            throw AudioObserverError.pipelineFailed
        }
        self.ringBuffer = ringBuffer
        self.stateReader = stateReader
        self.metrics = metrics
        self.queue = queue
        self.outputBuffer = output
        committedFilterState = committed
        provisionalFilterState = provisional
        committed.contents().initializeMemory(as: UInt8.self, repeating: 0, count: committed.length)
        provisional.contents().initializeMemory(as: UInt8.self, repeating: 0, count: provisional.length)
        do {
            pipeline = try device.makeComputePipelineState(function: function)
            kWeightPipeline = try device.makeComputePipelineState(function: kFunction)
        } catch {
            throw AudioObserverError.pipelineFailed
        }
    }

    func setPair(left: UInt32, right: UInt32, generation: UInt64) {
        let next = AudioChannelPair(leftIndex: left, rightIndex: right, generation: generation)
        guard pair != next else { return }
        pair = next
        cursor = nil
        resetFilterState = true
        metrics.markDiscontinuous()
    }

    func consume(_ snapshot: AudioObserverSnapshot) {
        guard !stopped, snapshot.ioRunning,
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
        let end = snapshot.writeEndFrame
        guard let start = cursor else {
            cursor = end
            return
        }
        guard end > start else { return }

        let ringFrames = UInt64(snapshot.activeRingFrames)
        let distance = end - start
        guard start >= snapshot.oldestValidFrame,
              ringFrames > writerHeadroomFrames,
              distance < ringFrames - writerHeadroomFrames,
              distance <= UInt64(UInt32.max) else {
            cursor = end
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
        guard let encoder = commandBuffer.makeComputeCommandEncoder() else {
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
            guard let kEncoder = commandBuffer.makeComputeCommandEncoder() else {
                cursor = end
                metrics.rejected()
                return
            }
            kEncoder.setComputePipelineState(kWeightPipeline)
            kEncoder.setBuffer(ringBuffer, offset: 0, index: 0)
            kEncoder.setBuffer(provisionalFilterState, offset: 0, index: 1)
            kEncoder.setBuffer(outputBuffer, offset: 0, index: 2)
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
            let outputValues = output.copyValues(count: 96)
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
            Task { @MainActor in
                self.inFlight = false
                guard !self.stopped else {
                    metrics.discardInFlight()
                    return
                }
                guard self.pair == resolvedPair else {
                    self.cursor = postState?.writeEndFrame
                    metrics.discardInFlight()
                    return
                }
                if completed.status == .completed, geometryStillMatches,
                   let postState, outputValues.count == 96 {
                    outputValues.withUnsafeBufferPointer { values in
                        metrics.accept(token: token,
                                       pair: resolvedPair,
                                       result: values,
                                       correlationValid: outputValues[12] != 0,
                                       cpuEncodeMilliseconds: encodeMilliseconds,
                                       scheduledToStartMilliseconds: queueMilliseconds,
                                       gpuMilliseconds: gpuMilliseconds,
                                       completionMilliseconds: (completionTime - encodeStart) * 1_000,
                                       sampleAgeMilliseconds: sampleAge,
                                       overwriteMarginMilliseconds: marginMilliseconds,
                                       meterKey: key)
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
            }
        }
        commandBuffer.commit()
        cursor = end
    }

    func stop() {
        stopped = true
    }
}
