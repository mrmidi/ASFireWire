import MetalKit
import SwiftUI
import QuartzCore

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
    var calibrationOffsetDB: Float = 0
    var padding: UInt32 = 0
}

private struct SmoothingParams { var alpha: Float; var elapsed: Float; var reset: UInt32; var binCount: UInt32 }

struct SpectrumPlotRegion: Equatable {
    let transform: UInt32
    let rect: CGRect
}

struct SpectrumPlotAnchors: PreferenceKey {
    static var defaultValue: [UInt32: Anchor<CGRect>] { [:] }
    static func reduce(value: inout [UInt32: Anchor<CGRect>], nextValue: () -> [UInt32: Anchor<CGRect>]) {
        value.merge(nextValue(), uniquingKeysWith: { _, next in next })
    }
}

struct SpectrumCanvasSlot: View {
    let transform: UInt32
    var body: some View {
        Color.clear.anchorPreference(key: SpectrumPlotAnchors.self, value: .bounds) { [transform: $0] }
    }
}

private struct SpectrumLane {
    let transform: UInt32
    let amplitudes: MTLBuffer
    let history: MTLBuffer
    let average: MTLBuffer
    let peaks: MTLBuffer
    init?(device: MTLDevice, transform: UInt32, fftSize: UInt32) {
        let bins = SpectrumFFTLayout.binCount(fftSize)
        guard let amplitudes = device.makeBuffer(length: bins * 4, options: .storageModePrivate),
              let history = device.makeBuffer(length: bins * 16, options: .storageModePrivate),
              let average = device.makeBuffer(length: bins * 4, options: .storageModePrivate),
              let peaks = device.makeBuffer(length: bins * 4, options: .storageModePrivate) else { return nil }
        self.transform = transform; self.amplitudes = amplitudes
        self.history = history; self.average = average; self.peaks = peaks
    }
}

struct MetalSpectrumView: NSViewRepresentable {
    let client: ASFWAudioObserverClient
    let channel: UInt32
    var otherChannel: UInt32 = 0
    var transform: UInt32 = 0
    var slow = false
    var peakHold = true
    var fftSize: UInt32 = 2048
    var window: UInt32 = 0
    var regions: [SpectrumPlotRegion] = []

    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> NSView {
        guard let device = client.metalDevice,
              let buffer = client.ringBuffer,
              let renderer = SpectrumRenderer(device: device, ring: buffer,
                                               state: client.renderState, submission: client.renderSubmission, channel: channel, otherChannel: otherChannel, transform: transform, slow: slow, peakHold: peakHold, fftSize: fftSize, window: window, regions: regions) else {
            return NSTextField(labelWithString: "Spectrum Metal pipeline unavailable")
        }
        let view = MTKView(frame: .zero, device: device)
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        view.layer?.isOpaque = false
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.preferredFramesPerSecond = 30
        context.coordinator.renderer = renderer
        view.delegate = renderer
        context.coordinator.drawObserver = NotificationCenter.default.addObserver(
            forName: .asfwAnalysisCompleted, object: client.renderState, queue: .main
        ) { [weak view, weak coordinator = context.coordinator] _ in
            MainActor.assumeIsolated {
                guard let coordinator, coordinator.cadence.shouldDraw(now: CACurrentMediaTime(), hz: 30) else { return }
                view?.draw()
            }
        }
        return view
    }
    func updateNSView(_ view: NSView, context: Context) { context.coordinator.renderer?.regions = regions }
    final class Coordinator {
        var cadence = AnalyzerDrawCadence()
        var renderer: SpectrumRenderer?
        var drawObserver: NSObjectProtocol?
        deinit { if let drawObserver { NotificationCenter.default.removeObserver(drawObserver) } }
    }
}

final class SpectrumRenderer: NSObject, MTKViewDelegate {
    private let ring: MTLBuffer
    private let compute: MTLComputePipelineState
    private let render: MTLRenderPipelineState
    private let submission: AnalyzerRenderSubmission
    private let state: AudioObserverRenderState
    private let channel: UInt32
    private let otherChannel: UInt32
    private let slow: Bool
    private let peakHold: Bool
    private let fftSize: UInt32
    private let window: UInt32
    private let smooth: MTLComputePipelineState
    private let peakRender: MTLRenderPipelineState
    private let lanes: [SpectrumLane]
    var regions: [SpectrumPlotRegion]
    private var lastTime: Double?
    private var lastWriteEnd: UInt64?
    private var epoch: String?
    private let slots = DispatchSemaphore(value: 2)

    init?(device: MTLDevice, ring: MTLBuffer, state: AudioObserverRenderState, submission: AnalyzerRenderSubmission, channel: UInt32, otherChannel: UInt32, transform: UInt32, slow: Bool, peakHold: Bool, fftSize: UInt32 = 2048, window: UInt32 = 0, regions: [SpectrumPlotRegion] = []) {
        guard let library = device.makeDefaultLibrary(),
              let smoothing = library.makeFunction(name: "asfwSpectrumSmooth"),
              let smooth = try? device.makeComputePipelineState(function: smoothing),
              let peakFragment = library.makeFunction(name: "asfwSpectrumPeakFragment"),
              let fft = library.makeFunction(name: "asfwSpectrumFFT"),
              let vertex = library.makeFunction(name: "asfwSpectrumVertex"),
              let fragment = library.makeFunction(name: "asfwAudioFragment"),
              let compute = try? device.makeComputePipelineState(function: fft),
              compute.maxTotalThreadsPerThreadgroup >= 256,
              SpectrumFFTLayout.sizes.contains(fftSize),
              compute.staticThreadgroupMemoryLength + SpectrumFFTLayout.scratchBytes(fftSize) <= device.maxThreadgroupMemoryLength else { return nil }
        let transforms = regions.isEmpty ? [transform] : regions.map(\.transform)
        let lanes = transforms.compactMap { SpectrumLane(device: device, transform: $0, fftSize: fftSize) }
        guard lanes.count == transforms.count else { return nil }
        self.lanes = lanes; self.regions = regions
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = vertex
        descriptor.fragmentFunction = fragment
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        guard let render = try? device.makeRenderPipelineState(descriptor: descriptor) else { return nil }
        descriptor.fragmentFunction = peakFragment
        guard let peakRender = try? device.makeRenderPipelineState(descriptor: descriptor) else { return nil }
        self.smooth = smooth; self.peakRender = peakRender
        self.otherChannel = otherChannel
        self.slow = slow; self.peakHold = peakHold
        self.fftSize = fftSize; self.window = window
        self.ring = ring
        self.compute = compute
        self.render = render
        self.submission = submission
        self.state = state
        self.channel = channel
    }

    func draw(in view: MTKView) {
        let snapshot = state.read()
        guard snapshot.ioRunning, snapshot.writeEndFrame != lastWriteEnd,
              snapshot.validHistoryFrames >= UInt64(fftSize), snapshot.sampleRateHz > 40,
              channel < snapshot.channels, otherChannel < snapshot.channels,
              let surface = submission.drawable(for: view),
              slots.wait(timeout: .now()) == .success else { return }
        let slots = self.slots
        guard let command = submission.commandBuffer(for: compute.device) else { slots.signal(); return }
        let now = CACurrentMediaTime()
        let key = "\(snapshot.sessionEpoch)-\(snapshot.discontinuityEpoch)"
        let elapsed = Float(min(0.25, max(0, now - (lastTime ?? now))))
        var smoothing = SmoothingParams(alpha: exp(-elapsed / (slow ? 1.0 : 0.15)), elapsed: elapsed,
            reset: epoch == key && now - (lastTime ?? now) < 0.5 ? 0 : 1, binCount: fftSize / 2 + 1)
        for lane in lanes {
            guard let encoder = command.makeComputeCommandEncoder() else { slots.signal(); return }
            var params = parameters(snapshot, transform: lane.transform)
            encoder.setComputePipelineState(compute)
            encoder.setThreadgroupMemoryLength(SpectrumFFTLayout.scratchBytes(fftSize), index: 0)
            encoder.setBuffer(ring, offset: 0, index: 0)
            encoder.setBuffer(lane.amplitudes, offset: 0, index: 1)
            encoder.setBytes(&params, length: MemoryLayout<SpectrumParams>.stride, index: 2)
            encoder.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
            encoder.endEncoding()
            guard let filtering = command.makeComputeCommandEncoder() else { slots.signal(); return }
            filtering.setComputePipelineState(smooth)
            filtering.setBuffer(lane.amplitudes, offset: 0, index: 0)
            filtering.setBuffer(lane.history, offset: 0, index: 1)
            filtering.setBuffer(lane.average, offset: 0, index: 2)
            filtering.setBuffer(lane.peaks, offset: 0, index: 3)
            filtering.setBytes(&smoothing, length: MemoryLayout<SmoothingParams>.stride, index: 4)
            filtering.dispatchThreads(MTLSize(width: Int(fftSize / 2 + 1), height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
            filtering.endEncoding()
        }
        guard let drawing = command.makeRenderCommandEncoder(descriptor: surface.pass) else { slots.signal(); return }
        let scale = view.window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 1
        for lane in lanes {
            let rect = (regions.first { $0.transform == lane.transform }?.rect ?? view.bounds).intersection(view.bounds)
            guard rect.width > 0, rect.height > 0 else { continue }
            drawing.setViewport(MTLViewport(originX: rect.minX * scale, originY: rect.minY * scale,
                width: rect.width * scale, height: rect.height * scale, znear: 0, zfar: 1))
            drawing.setScissorRect(MTLScissorRect(x: Int(rect.minX * scale), y: Int(rect.minY * scale),
                width: max(1, Int(rect.width * scale)), height: max(1, Int(rect.height * scale))))
            var params = parameters(snapshot, transform: lane.transform)
            drawing.setRenderPipelineState(render)
            drawing.setVertexBuffer(lane.average, offset: 0, index: 0)
            drawing.setVertexBytes(&params, length: MemoryLayout<SpectrumParams>.stride, index: 1)
            drawing.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: 512)
            if peakHold {
                drawing.setRenderPipelineState(peakRender)
                drawing.setVertexBuffer(lane.peaks, offset: 0, index: 0)
                drawing.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: 512)
            }
        }
        drawing.endEncoding()
        command.present(surface.drawable)
        command.addCompletedHandler { _ in slots.signal() }
        lastTime = now; epoch = key; lastWriteEnd = snapshot.writeEndFrame
        submission.commit(command)
    }
    private func parameters(_ snapshot: AudioObserverSnapshot, transform: UInt32) -> SpectrumParams {
        let offset = Float(AnalyzerCalibrationState.shared.config.effectiveOffsetDB)
        return SpectrumParams(writeEnd: snapshot.writeEndFrame, ringFrames: snapshot.activeRingFrames,
            channels: snapshot.channels, channel: channel, sampleRate: snapshot.sampleRateHz,
            otherChannel: otherChannel, transform: transform, fftSize: fftSize, window: window,
            calibrationOffsetDB: offset, padding: 0)
    }
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
}
