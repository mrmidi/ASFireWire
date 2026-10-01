import MetalKit
import SwiftUI

private struct SpectrumParams {
    var writeEnd: UInt64
    var ringFrames: UInt32
    var channels: UInt32
    var channel: UInt32
    var sampleRate: UInt32
    var otherChannel: UInt32
    var transform: UInt32
    var fftSize: UInt32
    var window: UInt32
}

private struct SmoothingParams { var alpha: Float; var elapsed: Float; var reset: UInt32; var unused: UInt32 = 0 }

struct MetalSpectrumView: NSViewRepresentable {
    let client: ASFWAudioObserverClient
    let channel: UInt32
    var otherChannel: UInt32 = 0
    var transform: UInt32 = 0
    var slow = false
    var peakHold = true
    var fftSize: UInt32 = 2048
    var window: UInt32 = 0

    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> NSView {
        guard let device = client.metalDevice,
              let buffer = client.ringBuffer,
              let renderer = SpectrumRenderer(device: device, ring: buffer,
                                               state: client.renderState, channel: channel, otherChannel: otherChannel, transform: transform, slow: slow, peakHold: peakHold, fftSize: fftSize, window: window) else {
            return NSTextField(labelWithString: "Spectrum Metal pipeline unavailable")
        }
        let view = MTKView(frame: .zero, device: device)
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColor(red: 0.025, green: 0.035, blue: 0.05, alpha: 1)
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.preferredFramesPerSecond = 30
        context.coordinator.renderer = renderer
        view.delegate = renderer
        context.coordinator.drawObserver = NotificationCenter.default.addObserver(
            forName: .asfwAnalysisCompleted, object: client.renderState, queue: .main
        ) { [weak view] _ in
            MainActor.assumeIsolated { view?.draw() }
        }
        return view
    }
    func updateNSView(_ view: NSView, context: Context) {}
    final class Coordinator {
        var renderer: SpectrumRenderer?
        var drawObserver: NSObjectProtocol?
        deinit { if let drawObserver { NotificationCenter.default.removeObserver(drawObserver) } }
    }
}

final class SpectrumRenderer: NSObject, MTKViewDelegate {
    private let ring: MTLBuffer
    private let amplitudes: MTLBuffer
    private let compute: MTLComputePipelineState
    private let render: MTLRenderPipelineState
    private let queue: MTLCommandQueue
    private let state: AudioObserverRenderState
    private let channel: UInt32
    private let otherChannel: UInt32
    private let transform: UInt32
    private let slow: Bool
    private let peakHold: Bool
    private let fftSize: UInt32
    private let window: UInt32
    private let smooth: MTLComputePipelineState
    private let peakRender: MTLRenderPipelineState
    private let history: MTLBuffer
    private let average: MTLBuffer
    private let peaks: MTLBuffer
    private var lastTime: Double?
    private var lastWriteEnd: UInt64?
    private var epoch: String?
    private let slots = DispatchSemaphore(value: 2)

    init?(device: MTLDevice, ring: MTLBuffer, state: AudioObserverRenderState, channel: UInt32, otherChannel: UInt32, transform: UInt32, slow: Bool, peakHold: Bool, fftSize: UInt32 = 2048, window: UInt32 = 0) {
        guard let library = device.makeDefaultLibrary(),
              let smoothing = library.makeFunction(name: "asfwSpectrumSmooth"),
              let smooth = try? device.makeComputePipelineState(function: smoothing),
              let peakFragment = library.makeFunction(name: "asfwSpectrumPeakFragment"),
              let history = device.makeBuffer(length: 2049 * 16, options: .storageModePrivate),
              let average = device.makeBuffer(length: 2049 * 4, options: .storageModePrivate),
              let peaks = device.makeBuffer(length: 2049 * 4, options: .storageModePrivate),
              let fft = library.makeFunction(name: "asfwSpectrumFFT"),
              let vertex = library.makeFunction(name: "asfwSpectrumVertex"),
              let fragment = library.makeFunction(name: "asfwAudioFragment"),
              let compute = try? device.makeComputePipelineState(function: fft),
              compute.maxTotalThreadsPerThreadgroup >= 256,
              let amplitudes = device.makeBuffer(length: 2049 * 4, options: .storageModePrivate),
              let queue = device.makeCommandQueue() else { return nil }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = vertex
        descriptor.fragmentFunction = fragment
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        guard let render = try? device.makeRenderPipelineState(descriptor: descriptor) else { return nil }
        descriptor.fragmentFunction = peakFragment
        guard let peakRender = try? device.makeRenderPipelineState(descriptor: descriptor) else { return nil }
        self.smooth = smooth; self.peakRender = peakRender
        self.history = history; self.average = average; self.peaks = peaks
        self.otherChannel = otherChannel; self.transform = transform
        self.slow = slow; self.peakHold = peakHold
        self.fftSize = fftSize; self.window = window
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
        guard snapshot.ioRunning, snapshot.writeEndFrame != lastWriteEnd,
              snapshot.validHistoryFrames >= UInt64(fftSize), snapshot.sampleRateHz > 40,
              channel < snapshot.channels, otherChannel < snapshot.channels,
              let pass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              slots.wait(timeout: .now()) == .success else { return }
        let slots = self.slots
        guard let command = queue.makeCommandBuffer(),
              let encoder = command.makeComputeCommandEncoder() else { slots.signal(); return }
        var params = SpectrumParams(writeEnd: snapshot.writeEndFrame,
                                    ringFrames: snapshot.activeRingFrames,
                                    channels: snapshot.channels,
                                    channel: channel, sampleRate: snapshot.sampleRateHz, otherChannel: otherChannel, transform: transform, fftSize: fftSize, window: window)
        encoder.setComputePipelineState(compute)
        encoder.setBuffer(ring, offset: 0, index: 0)
        encoder.setBuffer(amplitudes, offset: 0, index: 1)
        encoder.setBytes(&params, length: MemoryLayout<SpectrumParams>.stride, index: 2)
        encoder.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1),
                                     threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
        encoder.endEncoding()
        guard let filtering = command.makeComputeCommandEncoder() else { slots.signal(); return }
        let now = CACurrentMediaTime()
        let key = "\(snapshot.sessionEpoch)-\(snapshot.discontinuityEpoch)"
        let elapsed = Float(min(0.25, max(0, now - (lastTime ?? now))))
        var smoothing = SmoothingParams(alpha: exp(-elapsed / (slow ? 1.0 : 0.15)),
                                        elapsed: elapsed, reset: epoch == key && now - (lastTime ?? now) < 0.5 ? 0 : 1)
        filtering.setComputePipelineState(smooth)
        filtering.setBuffer(amplitudes, offset: 0, index: 0)
        filtering.setBuffer(history, offset: 0, index: 1)
        filtering.setBuffer(average, offset: 0, index: 2)
        filtering.setBuffer(peaks, offset: 0, index: 3)
        filtering.setBytes(&smoothing, length: MemoryLayout<SmoothingParams>.stride, index: 4)
        filtering.dispatchThreads(MTLSize(width: Int(fftSize / 2 + 1), height: 1, depth: 1),
                                  threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
        filtering.endEncoding()
        guard let drawing = command.makeRenderCommandEncoder(descriptor: pass) else { slots.signal(); return }
        drawing.setRenderPipelineState(render)
        drawing.setVertexBuffer(average, offset: 0, index: 0)
        drawing.setVertexBytes(&params, length: MemoryLayout<SpectrumParams>.stride, index: 1)
        drawing.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: 512)
        if peakHold {
            drawing.setRenderPipelineState(peakRender)
            drawing.setVertexBuffer(peaks, offset: 0, index: 0)
            drawing.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: 512)
        }
        drawing.endEncoding()
        command.present(drawable)
        command.addCompletedHandler { _ in slots.signal() }
        lastTime = now; epoch = key; lastWriteEnd = snapshot.writeEndFrame
        command.commit()
    }
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
}
