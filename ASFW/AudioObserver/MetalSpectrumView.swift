import MetalKit
import SwiftUI

private struct SpectrumParams {
    var writeEnd: UInt64
    var ringFrames: UInt32
    var channels: UInt32
    var channel: UInt32
    var sampleRate: UInt32
}

struct MetalSpectrumView: NSViewRepresentable {
    let client: ASFWAudioObserverClient
    let channel: UInt32

    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> NSView {
        guard let device = client.metalDevice,
              let buffer = client.ringBuffer,
              let renderer = SpectrumRenderer(device: device, ring: buffer,
                                               state: client.renderState, channel: channel) else {
            return NSTextField(labelWithString: "Spectrum Metal pipeline unavailable")
        }
        let view = MTKView(frame: .zero, device: device)
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColor(red: 0.025, green: 0.035, blue: 0.05, alpha: 1)
        view.preferredFramesPerSecond = 30
        context.coordinator.renderer = renderer
        view.delegate = renderer
        return view
    }
    func updateNSView(_ view: NSView, context: Context) {}
    final class Coordinator { var renderer: SpectrumRenderer? }
}

final class SpectrumRenderer: NSObject, MTKViewDelegate {
    private let ring: MTLBuffer
    private let amplitudes: MTLBuffer
    private let compute: MTLComputePipelineState
    private let render: MTLRenderPipelineState
    private let queue: MTLCommandQueue
    private let state: AudioObserverRenderState
    private let channel: UInt32
    private let slots = DispatchSemaphore(value: 2)

    init?(device: MTLDevice, ring: MTLBuffer, state: AudioObserverRenderState, channel: UInt32) {
        guard let library = device.makeDefaultLibrary(),
              let fft = library.makeFunction(name: "asfwSpectrumFFT"),
              let vertex = library.makeFunction(name: "asfwSpectrumVertex"),
              let fragment = library.makeFunction(name: "asfwAudioFragment"),
              let compute = try? device.makeComputePipelineState(function: fft),
              compute.maxTotalThreadsPerThreadgroup >= 256,
              let amplitudes = device.makeBuffer(length: 1025 * 4, options: .storageModePrivate),
              let queue = device.makeCommandQueue() else { return nil }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = vertex
        descriptor.fragmentFunction = fragment
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        guard let render = try? device.makeRenderPipelineState(descriptor: descriptor) else { return nil }
        self.ring = ring
        self.amplitudes = amplitudes
        self.compute = compute
        self.render = render
        self.queue = queue
        self.state = state
        self.channel = channel
    }

    func draw(in view: MTKView) {
        let snapshot = state.read()
        guard snapshot.validHistoryFrames >= 2048, snapshot.sampleRateHz > 40,
              channel < snapshot.channels,
              let pass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              slots.wait(timeout: .now()) == .success else { return }
        let slots = self.slots
        guard let command = queue.makeCommandBuffer(),
              let encoder = command.makeComputeCommandEncoder() else { slots.signal(); return }
        var params = SpectrumParams(writeEnd: snapshot.writeEndFrame,
                                    ringFrames: snapshot.activeRingFrames,
                                    channels: snapshot.channels,
                                    channel: channel, sampleRate: snapshot.sampleRateHz)
        encoder.setComputePipelineState(compute)
        encoder.setBuffer(ring, offset: 0, index: 0)
        encoder.setBuffer(amplitudes, offset: 0, index: 1)
        encoder.setBytes(&params, length: MemoryLayout<SpectrumParams>.stride, index: 2)
        encoder.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1),
                                     threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
        encoder.endEncoding()
        guard let drawing = command.makeRenderCommandEncoder(descriptor: pass) else { slots.signal(); return }
        drawing.setRenderPipelineState(render)
        drawing.setVertexBuffer(amplitudes, offset: 0, index: 0)
        drawing.setVertexBytes(&params, length: MemoryLayout<SpectrumParams>.stride, index: 1)
        drawing.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: 512)
        drawing.endEncoding()
        command.present(drawable)
        command.addCompletedHandler { _ in slots.signal() }
        command.commit()
    }
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
}
