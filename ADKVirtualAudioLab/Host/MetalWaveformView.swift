import Metal
import MetalKit
import SwiftUI

@MainActor
struct MetalWaveformView: NSViewRepresentable {
    let client: AudioRingClient
    let buffer: MTLBuffer
    let pipeline: MTLRenderPipelineState

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    func makeNSView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: client.metalDevice)
        view.colorPixelFormat = .bgra8Unorm
        view.framebufferOnly = true
        view.isPaused = false
        view.enableSetNeedsDisplay = false
        view.preferredFramesPerSecond = 60
        view.clearColor = MTLClearColor(red: 0.025, green: 0.035,
                                        blue: 0.05, alpha: 1)

        let renderer = WaveformRenderer(
            buffer: buffer,
            pipeline: pipeline,
            renderState: client.renderState)
        context.coordinator.renderer = renderer
        view.delegate = renderer
        return view
    }

    func updateNSView(_ view: MTKView, context: Context) {}

    final class Coordinator {
        var renderer: WaveformRenderer?
    }
}

private struct WaveformParams {
    var writeEndFrame: UInt64
    var ringFrames: UInt32
    var channels: UInt32
    var windowFrames: UInt32
    var channel: UInt32
}

final class WaveformRenderer: NSObject, MTKViewDelegate {
    private static let windowFrames: UInt32 = 960

    private let buffer: MTLBuffer
    private let pipeline: MTLRenderPipelineState
    private let renderState: WaveformRenderState
    private let commandQueue: MTLCommandQueue?

    init(buffer: MTLBuffer,
         pipeline: MTLRenderPipelineState,
         renderState: WaveformRenderState) {
        self.buffer = buffer
        self.pipeline = pipeline
        self.renderState = renderState
        self.commandQueue = pipeline.device.makeCommandQueue()
    }

    func draw(in view: MTKView) {
        guard let pass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let commandBuffer = commandQueue?.makeCommandBuffer(),
              let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else {
            return
        }

        let state = renderState.read()
        let windowFrames = min(Self.windowFrames, state.validHistoryFrames)
        guard windowFrames > 1 else {
            encoder.endEncoding()
            commandBuffer.present(drawable)
            commandBuffer.commit()
            return
        }
        var params = WaveformParams(
            writeEndFrame: state.writeEndFrame,
            ringFrames: state.activeRingFrames,
            channels: state.channels,
            windowFrames: windowFrames,
            channel: 0)

        encoder.setRenderPipelineState(pipeline)
        encoder.setVertexBuffer(buffer, offset: 0, index: 0)
        encoder.setVertexBytes(&params,
                               length: MemoryLayout<WaveformParams>.stride,
                               index: 1)
        encoder.drawPrimitives(type: .lineStrip,
                               vertexStart: 0,
                               vertexCount: Int(windowFrames))
        encoder.endEncoding()
        commandBuffer.present(drawable)
        commandBuffer.commit()
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
}
