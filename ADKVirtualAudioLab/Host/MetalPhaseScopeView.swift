import Metal
import MetalKit
import QuartzCore
import SwiftUI

@MainActor
struct MetalPhaseScopeView: NSViewRepresentable {
    let client: AudioRingClient
    let buffer: MTLBuffer
    let renderPipeline: MTLRenderPipelineState
    let analysisPipeline: MTLComputePipelineState
    let analysisBuffer: MTLBuffer

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
        let renderer = PhaseScopeRenderer(
            buffer: buffer,
            renderPipeline: renderPipeline,
            analysisPipeline: analysisPipeline,
            analysisBuffer: analysisBuffer,
            renderState: client.renderState,
            diagnostics: client.diagnostics,
            stateReader: client.stateReader,
            validationSamples: { [weak client] snapshot in
                client?.cpuValidationBits(snapshot: snapshot,
                                          windowFrames: PhaseScopeRenderer.windowFrames) ?? []
            })
        context.coordinator.renderer = renderer
        view.delegate = renderer
        return view
    }

    func updateNSView(_ view: MTKView, context: Context) {}

    final class Coordinator { var renderer: PhaseScopeRenderer? }
}

private struct ScopeParams {
    var writeEndFrame: UInt64
    var ringFrames: UInt32
    var channels: UInt32
    var windowFrames: UInt32
    var validateSamples: UInt32
}

private final class ScheduledTimestamp: @unchecked Sendable {
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

final class PhaseScopeRenderer: NSObject, MTKViewDelegate {
    static let windowFrames: UInt32 = 1024

    private let buffer: MTLBuffer
    private let renderPipeline: MTLRenderPipelineState
    private let analysisPipeline: MTLComputePipelineState
    private let analysisBuffer: MTLBuffer
    private let renderState: WaveformRenderState
    private let diagnostics: ScopeDiagnostics
    private let stateReader: AudioRingStateReader?
    private let commandQueue: MTLCommandQueue?
    private let validationSamples: (AudioViewSnapshot) -> [UInt32]
    private var lastValidationTime = CACurrentMediaTime()

    init(buffer: MTLBuffer,
         renderPipeline: MTLRenderPipelineState,
         analysisPipeline: MTLComputePipelineState,
         analysisBuffer: MTLBuffer,
         renderState: WaveformRenderState,
         diagnostics: ScopeDiagnostics,
         stateReader: AudioRingStateReader?,
         validationSamples: @escaping (AudioViewSnapshot) -> [UInt32]) {
        self.buffer = buffer
        self.renderPipeline = renderPipeline
        self.analysisPipeline = analysisPipeline
        self.analysisBuffer = analysisBuffer
        self.renderState = renderState
        self.diagnostics = diagnostics
        self.stateReader = stateReader
        self.validationSamples = validationSamples
        self.commandQueue = renderPipeline.device.makeCommandQueue()
    }

    func draw(in view: MTKView) {
        // One shared analysis result buffer is reused, so keep at most one
        // command in flight. A skipped display tick never delays audio.
        guard diagnostics.read().inFlight == 0 else { return }
        let snapshot = renderState.read()
        let validFrames = min(snapshot.validHistoryFrames,
                              snapshot.activeRingFrames,
                              Self.windowFrames)
        guard snapshot.channels >= 2, snapshot.activeRingFrames > 0,
              let pass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let commandBuffer = commandQueue?.makeCommandBuffer(),
              let compute = commandBuffer.makeComputeCommandEncoder(),
              let render = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else {
            return
        }

        let doValidation = CACurrentMediaTime() - lastValidationTime >= 1.0 &&
            validFrames > 1
        let expectedBits = doValidation
            ? validationSamples(snapshot)
            : []
        if doValidation { lastValidationTime = CACurrentMediaTime() }

        var params = ScopeParams(
            writeEndFrame: snapshot.writeEndFrame,
            ringFrames: snapshot.activeRingFrames,
            channels: snapshot.channels,
            windowFrames: validFrames,
            validateSamples: doValidation && expectedBits.count == 32 ? 1 : 0)
        let crossingWrap = validFrames > 0 &&
            ((snapshot.writeEndFrame - UInt64(validFrames)) % UInt64(snapshot.activeRingFrames)) +
            UInt64(validFrames) > UInt64(snapshot.activeRingFrames)
        let encodeStart = CACurrentMediaTime()

        compute.setComputePipelineState(analysisPipeline)
        compute.setBuffer(buffer, offset: 0, index: 0)
        compute.setBuffer(analysisBuffer, offset: 0, index: 1)
        compute.setBytes(&params, length: MemoryLayout<ScopeParams>.stride, index: 2)
        compute.dispatchThreads(MTLSize(width: 1, height: 1, depth: 1),
                                threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
        compute.endEncoding()

        render.setRenderPipelineState(renderPipeline)
        render.setVertexBuffer(buffer, offset: 0, index: 0)
        render.setVertexBytes(&params, length: MemoryLayout<ScopeParams>.stride, index: 1)
        if validFrames > 1 {
            render.drawPrimitives(type: .lineStrip,
                                  vertexStart: 0,
                                  vertexCount: Int(validFrames))
        }
        render.endEncoding()
        commandBuffer.present(drawable)
        let encodeMs = (CACurrentMediaTime() - encodeStart) * 1_000
        diagnostics.submitted(crossesWrap: crossingWrap)

        let diagnostics = self.diagnostics
        let stateReader = self.stateReader
        let analysisBuffer = self.analysisBuffer
        let scheduledTimestamp = ScheduledTimestamp()
        commandBuffer.addScheduledHandler { scheduledBuffer in
            scheduledTimestamp.set(CACurrentMediaTime())
            _ = scheduledBuffer
        }
        commandBuffer.addCompletedHandler { completedBuffer in
            let completionTime = CACurrentMediaTime()
            let output = analysisBuffer.contents().assumingMemoryBound(to: UInt32.self)
            let leftPeak = Float(bitPattern: output[0])
            let rightPeak = Float(bitPattern: output[1])
            let correlation = Float(bitPattern: output[2])
            let gpuStart = completedBuffer.gpuStartTime
            let gpuEnd = completedBuffer.gpuEndTime
            let gpuMs = gpuEnd >= gpuStart && gpuStart > 0
                ? (gpuEnd - gpuStart) * 1_000
                : nil
            let scheduledToStartMs = gpuStart > 0
                ? scheduledTimestamp.read().map { max(0, gpuStart - $0) * 1_000 }
                : nil

            var ageMs: Double?
            var marginMs: Double?
            var safe = false
            if let stateReader, let current = try? stateReader.read(),
               current.epoch == snapshot.epoch,
               current.writeEndFrame >= snapshot.writeEndFrame,
               snapshot.sampleRate > 0 {
                let advanced = current.writeEndFrame - snapshot.writeEndFrame
                let rate = Double(snapshot.sampleRate)
                ageMs = Double(advanced) / rate * 1_000
                let margin = Int64(snapshot.activeRingFrames) - Int64(validFrames) - Int64(advanced)
                marginMs = Double(margin) / rate * 1_000
                safe = margin > 0
            }

            var validationPassed: Bool?
            if params.validateSamples != 0 && expectedBits.count == 32 {
                validationPassed = (0..<32).allSatisfy {
                    output[4 + $0] == expectedBits[$0]
                }
            }
            diagnostics.completed(
                cpuEncodeMs: encodeMs,
                scheduledToStartMs: scheduledToStartMs,
                gpuMs: gpuMs,
                completionMs: (completionTime - encodeStart) * 1_000,
                sampleAgeMs: ageMs,
                overwriteMarginMs: marginMs,
                leftPeak: leftPeak,
                rightPeak: rightPeak,
                correlation: correlation,
                validationPassed: validationPassed,
                windowWasSafe: safe)
        }
        commandBuffer.commit()
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
}
