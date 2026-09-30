import Metal
import MetalKit
import QuartzCore
import SwiftUI

enum AudioObserverDisplayMode: Sendable, Equatable {
    case phaseScope
    case waveform
}

@MainActor
struct MetalAudioObserverView: NSViewRepresentable {
    let client: ASFWAudioObserverClient
    let mode: AudioObserverDisplayMode
    var leftChannel: UInt32 = 0
    var rightChannel: UInt32 = 1

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: client.metalDevice)
        view.colorPixelFormat = .bgra8Unorm
        view.framebufferOnly = true
        view.isPaused = false
        view.enableSetNeedsDisplay = false
        view.preferredFramesPerSecond = 60
        view.clearColor = MTLClearColor(red: 0.025, green: 0.035,
                                        blue: 0.05, alpha: 1)

        configure(view, coordinator: context.coordinator)
        return view
    }

    func updateNSView(_ view: MTKView, context: Context) {
        configure(view, coordinator: context.coordinator)
    }

    private func configure(_ view: MTKView, coordinator: Coordinator) {
        let key = "\(client.ringBuffer.map { ObjectIdentifier($0).hashValue } ?? 0)-\(mode)-\(leftChannel)-\(rightChannel)"
        guard coordinator.configurationKey != key else { return }
        coordinator.configurationKey = key
        let pipeline = mode == .phaseScope ? client.phaseRenderPipeline : client.waveformRenderPipeline
        if let pipeline {
            let renderer = AudioObserverRenderer(
                buffer: client.ringBuffer,
                renderPipeline: pipeline,
                analysisPipeline: mode == .phaseScope ? client.analysisPipeline : nil,
                analysisBuffer: client.analysisBuffer,
                mode: mode,
                leftChannel: leftChannel,
                rightChannel: rightChannel,
                renderState: client.renderState,
                metrics: client.metrics,
                stateReader: client.stateReader)
            coordinator.renderer = renderer
            view.delegate = renderer
        } else {
            coordinator.renderer = nil
            view.delegate = nil
        }
    }

    final class Coordinator {
        var configurationKey: String?
        var renderer: AudioObserverRenderer?
    }
}

private struct ObserverParams {
    var writeEndFrame: UInt64
    var ringFrames: UInt32
    var channels: UInt32
    var windowFrames: UInt32
    var channel: UInt32
    var rightChannel: UInt32
}

private final class ObserverScheduledTimestamp: @unchecked Sendable {
    private let lock = NSLock()
    private var value: Double?

    func set(_ timestamp: Double) {
        lock.lock()
        value = timestamp
        lock.unlock()
    }

    func read() -> Double? {
        lock.lock()
        defer { lock.unlock() }
        return value
    }
}

final class AudioObserverRenderer: NSObject, MTKViewDelegate {
    private static let phaseWindowFrames: UInt32 = 1024
    private static let waveformWindowFrames: UInt32 = 960

    private let buffer: MTLBuffer?
    private let renderPipeline: MTLRenderPipelineState
    private let analysisPipeline: MTLComputePipelineState?
    private let analysisBuffer: MTLBuffer?
    private let leftChannel: UInt32
    private let rightChannel: UInt32
    private let mode: AudioObserverDisplayMode
    private let renderState: AudioObserverRenderState
    private let metrics: AudioObserverMetricsState
    private let stateReader: AudioObserverStateReader?
    private let commandQueue: MTLCommandQueue?

    init(buffer: MTLBuffer?,
         renderPipeline: MTLRenderPipelineState,
         analysisPipeline: MTLComputePipelineState?,
         analysisBuffer: MTLBuffer?,
         mode: AudioObserverDisplayMode,
         leftChannel: UInt32,
         rightChannel: UInt32,
         renderState: AudioObserverRenderState,
         metrics: AudioObserverMetricsState,
         stateReader: AudioObserverStateReader?) {
        self.buffer = buffer
        self.renderPipeline = renderPipeline
        self.analysisPipeline = analysisPipeline
        self.analysisBuffer = analysisBuffer
        self.mode = mode
        self.leftChannel = leftChannel
        self.rightChannel = rightChannel
        self.renderState = renderState
        self.metrics = metrics
        self.stateReader = stateReader
        self.commandQueue = renderPipeline.device.makeCommandQueue()
    }

    func draw(in view: MTKView) {
        let snapshot = renderState.read()
        let desiredWindow = mode == .phaseScope
            ? Self.phaseWindowFrames
            : Self.waveformWindowFrames
        let validFrames = min(snapshot.validHistoryFrames,
                              snapshot.activeRingFrames,
                              desiredWindow)
        guard let buffer, snapshot.channels >= 2,
              snapshot.activeRingFrames > 0,
              let pass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let commandBuffer = commandQueue?.makeCommandBuffer() else {
            return
        }

        let phaseScope = mode == .phaseScope
        if phaseScope, metrics.read().inFlight > 0 { return }
        let encodeStart = CACurrentMediaTime()

        if phaseScope {
            guard let analysisPipeline,
                  let analysisBuffer,
                  let compute = commandBuffer.makeComputeCommandEncoder() else {
                return
            }
            var params = ObserverParams(
                writeEndFrame: snapshot.writeEndFrame,
                ringFrames: snapshot.activeRingFrames,
                channels: snapshot.channels,
                windowFrames: validFrames,
                channel: min(leftChannel, snapshot.channels - 1),
                rightChannel: min(rightChannel, snapshot.channels - 1))
            compute.setComputePipelineState(analysisPipeline)
            compute.setBuffer(buffer, offset: 0, index: 0)
            compute.setBuffer(analysisBuffer, offset: 0, index: 1)
            compute.setBytes(&params, length: MemoryLayout<ObserverParams>.stride, index: 2)
            compute.dispatchThreads(MTLSize(width: 1, height: 1, depth: 1),
                                    threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
            compute.endEncoding()
        }

        var params = ObserverParams(
            writeEndFrame: snapshot.writeEndFrame,
            ringFrames: snapshot.activeRingFrames,
            channels: snapshot.channels,
            windowFrames: validFrames,
            channel: min(leftChannel, snapshot.channels - 1),
                rightChannel: min(rightChannel, snapshot.channels - 1))
        let crossesWrap = validFrames > 0 &&
            ((snapshot.writeEndFrame - UInt64(validFrames)) % UInt64(snapshot.activeRingFrames)) +
            UInt64(validFrames) > UInt64(snapshot.activeRingFrames)
        // The analysis encoder has ended before this render encoder is made.
        guard let render = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else {
            return
        }

        render.setRenderPipelineState(renderPipeline)
        render.setVertexBuffer(buffer, offset: 0, index: 0)
        render.setVertexBytes(&params, length: MemoryLayout<ObserverParams>.stride, index: 1)
        if validFrames > 1 {
            render.drawPrimitives(type: .lineStrip,
                                  vertexStart: 0,
                                  vertexCount: Int(validFrames))
        }
        render.endEncoding()
        commandBuffer.present(drawable)
        let encodeMilliseconds = (CACurrentMediaTime() - encodeStart) * 1_000
        if phaseScope { metrics.submitted(crossesWrap: crossesWrap) }

        if phaseScope, let analysisBuffer, let stateReader {
            let metrics = self.metrics
            let scheduledTimestamp = ObserverScheduledTimestamp()
            commandBuffer.addScheduledHandler { _ in
                scheduledTimestamp.set(CACurrentMediaTime())
            }
            commandBuffer.addCompletedHandler { completedBuffer in
                let completionTime = CACurrentMediaTime()
                let output = analysisBuffer.contents().assumingMemoryBound(to: UInt32.self)
                let leftPeak = Float(bitPattern: output[0])
                let rightPeak = Float(bitPattern: output[1])
                let correlation = Float(bitPattern: output[2])
                let gpuStart = completedBuffer.gpuStartTime
                let gpuEnd = completedBuffer.gpuEndTime
                let gpuMilliseconds = gpuStart > 0 && gpuEnd >= gpuStart
                    ? (gpuEnd - gpuStart) * 1_000
                    : nil
                let scheduledToStartMilliseconds = gpuStart > 0
                    ? scheduledTimestamp.read().map { max(0, gpuStart - $0) * 1_000 }
                    : nil
                var ageMilliseconds: Double?
                var marginMilliseconds: Double?
                var safe = false
                if let current = try? stateReader.read(),
                   current.sessionEpoch == snapshot.sessionEpoch,
                   current.discontinuityEpoch == snapshot.discontinuityEpoch,
                   current.memoryGeneration == snapshot.memoryGeneration,
                   current.writeEndFrame >= snapshot.writeEndFrame {
                    let advanced = current.writeEndFrame - snapshot.writeEndFrame
                    let rate = Double(snapshot.sampleRateHz)
                    ageMilliseconds = Double(advanced) / rate * 1_000
                    let margin = Int64(snapshot.activeRingFrames) -
                        Int64(validFrames) - Int64(advanced)
                    marginMilliseconds = Double(margin) / rate * 1_000
                    safe = margin > 0
                }
                metrics.completed(leftPeak: leftPeak,
                                  rightPeak: rightPeak,
                                  correlation: correlation,
                                  cpuEncodeMilliseconds: encodeMilliseconds,
                                  scheduledToStartMilliseconds: scheduledToStartMilliseconds,
                                  gpuMilliseconds: gpuMilliseconds,
                                  completionMilliseconds: (completionTime - encodeStart) * 1_000,
                                  sampleAgeMilliseconds: ageMilliseconds,
                                  overwriteMarginMilliseconds: marginMilliseconds,
                                  safe: safe)
            }
        }
        commandBuffer.commit()
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
}
