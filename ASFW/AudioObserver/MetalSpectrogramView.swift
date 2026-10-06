import MetalKit
import SwiftUI
import QuartzCore

struct MetalSpectrogramView: NSViewRepresentable {
    let client: ASFWAudioObserverClient
    let channel: UInt32
    var otherChannel: UInt32 = 0
    var fftSize: UInt32 = 2048
    var window: UInt32 = 0
    var regions: [SpectrumPlotRegion] = []
    var waterfall = false
    var wireOverlay = false

    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> NSView {
        guard let device = client.metalDevice,
              let buffer = client.ringBuffer,
              let renderer = SpectrogramRenderer(device: device, ring: buffer,
                                               state: client.renderState, submission: client.renderSubmission, channel: channel, otherChannel: otherChannel, fftSize: fftSize, window: window, regions: regions) else {
            return NSTextField(labelWithString: "Spectrogram Metal pipeline unavailable")
        }
        let view = MTKView(frame: .zero, device: device)
        view.colorPixelFormat = .bgra8Unorm
        view.depthStencilPixelFormat = .depth32Float
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        view.layer?.isOpaque = false
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.preferredFramesPerSecond = 60
        renderer.waterfall = waterfall
        renderer.wireOverlay = wireOverlay
        context.coordinator.renderer = renderer
        view.delegate = renderer
        context.coordinator.drawObserver = NotificationCenter.default.addObserver(
            forName: .asfwAnalysisCompleted, object: client.renderState, queue: .main
        ) { [weak view, weak coordinator = context.coordinator] _ in
            MainActor.assumeIsolated {
                guard let coordinator, coordinator.cadence.shouldDraw(now: CACurrentMediaTime(), hz: 60) else { return }
                view?.draw()
            }
        }
        return view
    }
    func updateNSView(_ view: NSView, context: Context) {
        context.coordinator.renderer?.regions = regions
        context.coordinator.renderer?.waterfall = waterfall
        context.coordinator.renderer?.wireOverlay = wireOverlay
    }
    final class Coordinator {
        var cadence = AnalyzerDrawCadence()
        var renderer: SpectrogramRenderer?
        var drawObserver: NSObjectProtocol?
        deinit { if let drawObserver { NotificationCenter.default.removeObserver(drawObserver) } }
    }
}

private struct SpectrogramFFTParams {
    var writeEnd: UInt64 = 0
    var ringFrames: UInt32 = 0; var channels: UInt32 = 0; var channel: UInt32 = 0; var sampleRate: UInt32 = 0
    var otherChannel: UInt32 = 0; var transform: UInt32 = 0; var fftSize: UInt32 = 0; var window: UInt32 = 0
    var calibrationOffsetDB: Float = 0
    var padding: UInt32 = 0
}
private struct SpectrogramParams { var fft: SpectrogramFFTParams; var firstSlice: UInt64; var hop: UInt32; var columns: UInt32 = 1024 }
private struct SpectrogramDisplay { var latestSlice: UInt64; var columns: UInt32 = 1024; var fftSize: UInt32; var sampleRate: UInt32; var pixelHeight: UInt32 }
private struct SpectrogramLane { let transform: UInt32; let texture: MTLTexture; let stamps: MTLBuffer; let continuity: MTLBuffer }

final class SpectrogramRenderer: NSObject, MTKViewDelegate {
    private let ring: MTLBuffer
    private let stereoScratch: MTLBuffer
    private let compute: MTLComputePipelineState
    private let render: MTLRenderPipelineState
    private let waterfallRender: MTLRenderPipelineState
    private let surfaceRender: MTLRenderPipelineState
    private let continuityCompute: MTLComputePipelineState
    private let depthWrite: MTLDepthStencilState
    private let depthRead: MTLDepthStencilState
    var waterfall = false
    var wireOverlay = false
    private let submission: AnalyzerRenderSubmission
    private let state: AudioObserverRenderState
    private let channel: UInt32, otherChannel: UInt32, fftSize: UInt32, window: UInt32
    private let lanes: [SpectrogramLane]
    var regions: [SpectrumPlotRegion]
    private var timeline = SpectrogramTimeline()
    private var epoch: [UInt64] = []
    private let slots = DispatchSemaphore(value: 2)

    init?(device: MTLDevice, ring: MTLBuffer, state: AudioObserverRenderState,
          submission: AnalyzerRenderSubmission, channel: UInt32, otherChannel: UInt32,
          fftSize: UInt32, window: UInt32, regions: [SpectrumPlotRegion]) {
        guard let library = device.makeDefaultLibrary(),
              let fft = library.makeFunction(name: "asfwSpectrogramSTFT"),
              let vertex = library.makeFunction(name: "asfwSpectrogramVertex"),
              let fragment = library.makeFunction(name: "asfwSpectrogramFragment"),
              let compute = try? device.makeComputePipelineState(function: fft),
              compute.maxTotalThreadsPerThreadgroup >= 256,
              SpectrumFFTLayout.sizes.contains(fftSize),
              compute.staticThreadgroupMemoryLength + SpectrumFFTLayout.scratchBytes(fftSize) <= device.maxThreadgroupMemoryLength,
              let stereoScratch = device.makeBuffer(length: SpectrumFFTLayout.binCount(fftSize) * SpectrogramTimeline.maximumSlicesPerUpdate * 4, options: .storageModePrivate) else { return nil }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = vertex; descriptor.fragmentFunction = fragment
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        descriptor.depthAttachmentPixelFormat = .depth32Float
        guard let render = try? device.makeRenderPipelineState(descriptor: descriptor) else { return nil }
        descriptor.vertexFunction = library.makeFunction(name: "asfwWaterfallVertex")
        descriptor.fragmentFunction = library.makeFunction(name: "asfwWaterfallWireFragment")
        let attachment = descriptor.colorAttachments[0]!
        attachment.isBlendingEnabled = true
        attachment.sourceRGBBlendFactor = .sourceAlpha
        attachment.destinationRGBBlendFactor = .oneMinusSourceAlpha
        attachment.sourceAlphaBlendFactor = .one
        attachment.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        guard let waterfallRender = try? device.makeRenderPipelineState(descriptor: descriptor) else { return nil }
        descriptor.vertexFunction = library.makeFunction(name: "asfwWaterfallSurfaceVertex")
        descriptor.fragmentFunction = library.makeFunction(name: "asfwWaterfallFragment")
        attachment.isBlendingEnabled = false
        guard let surfaceRender = try? device.makeRenderPipelineState(descriptor: descriptor),
              let continuityFunction = library.makeFunction(name: "asfwWaterfallContinuity"),
              let continuityCompute = try? device.makeComputePipelineState(function: continuityFunction) else { return nil }
        let depthDescriptor = MTLDepthStencilDescriptor()
        depthDescriptor.depthCompareFunction = .lessEqual
        depthDescriptor.isDepthWriteEnabled = true
        guard let depthWrite = device.makeDepthStencilState(descriptor: depthDescriptor) else { return nil }
        depthDescriptor.isDepthWriteEnabled = false
        guard let depthRead = device.makeDepthStencilState(descriptor: depthDescriptor) else { return nil }
        let textureDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float,
            width: SpectrogramTimeline.columns, height: Int(fftSize / 2 + 1), mipmapped: false)
        textureDescriptor.storageMode = .private
        textureDescriptor.hazardTrackingMode = .tracked
        textureDescriptor.usage = [.shaderRead, .shaderWrite]
        var lanes: [SpectrogramLane] = []
        for region in regions {
            guard let texture = device.makeTexture(descriptor: textureDescriptor),
                  let stamps = device.makeBuffer(length: SpectrogramTimeline.columns * 8, options: [.storageModePrivate, .hazardTrackingModeTracked]),
                  let continuity = device.makeBuffer(length: (WaterfallProjection.historyRows - 1) * 4, options: [.storageModePrivate, .hazardTrackingModeTracked]) else { return nil }
            lanes.append(SpectrogramLane(transform: region.transform, texture: texture, stamps: stamps, continuity: continuity))
        }
        guard !lanes.isEmpty else { return nil }
        self.stereoScratch = stereoScratch
        self.lanes = lanes; self.regions = regions; self.ring = ring; self.state = state
        self.submission = submission; self.compute = compute; self.render = render
        self.waterfallRender = waterfallRender
        self.surfaceRender = surfaceRender; self.continuityCompute = continuityCompute; self.depthWrite = depthWrite; self.depthRead = depthRead
        self.channel = channel; self.otherChannel = otherChannel; self.fftSize = fftSize; self.window = window
    }
    func draw(in view: MTKView) {
        let snapshot = state.read()
        guard snapshot.ioRunning, snapshot.sampleRateHz > 40,
              snapshot.activeRingFrames >= fftSize, snapshot.validHistoryFrames >= UInt64(fftSize),
              channel < snapshot.channels, otherChannel < snapshot.channels,
              let surface = submission.drawable(for: view),
              slots.wait(timeout: .now()) == .success else { return }
        let slots = self.slots
        let key = [snapshot.sessionEpoch, snapshot.discontinuityEpoch, UInt64(snapshot.sampleRateHz)]
        var nextTimeline = epoch == key ? timeline : SpectrogramTimeline()
        guard let slices = nextTimeline.append(writeEnd: snapshot.writeEndFrame, oldest: snapshot.oldestValidFrame,
                    fftSize: fftSize, sampleRate: snapshot.sampleRateHz),
              let command = submission.commandBuffer(for: compute.device) else { slots.signal(); return }
        if epoch != key {
            guard let clear = command.makeBlitCommandEncoder() else { slots.signal(); return }
            for lane in lanes { clear.fill(buffer: lane.stamps, range: 0..<lane.stamps.length, value: 0) }
            clear.endEncoding()
        }
        guard let encoder = command.makeComputeCommandEncoder() else { slots.signal(); return }
        encoder.setComputePipelineState(compute)
        encoder.setThreadgroupMemoryLength(SpectrumFFTLayout.scratchBytes(fftSize), index: 0)
        encoder.setBuffer(stereoScratch, offset: 0, index: 3)
        encoder.setBuffer(ring, offset: 0, index: 0)
        let cal = AnalyzerCalibrationState.shared.config
        for lane in lanes {
            var params = SpectrogramParams(fft: SpectrogramFFTParams(ringFrames: snapshot.activeRingFrames,
                channels: snapshot.channels, channel: channel, sampleRate: snapshot.sampleRateHz,
                otherChannel: otherChannel, transform: lane.transform, fftSize: fftSize, window: window,
                calibrationOffsetDB: Float(cal.effectiveOffsetDB), padding: 0),
                firstSlice: slices.lowerBound, hop: UInt32(SpectrogramTimeline.hop(sampleRate: snapshot.sampleRateHz)))
            encoder.setBuffer(lane.stamps, offset: 0, index: 1)
            encoder.setTexture(lane.texture, index: 0)
            encoder.setBytes(&params, length: MemoryLayout<SpectrogramParams>.stride, index: 2)
            encoder.dispatchThreadgroups(MTLSize(width: Int(slices.upperBound - slices.lowerBound + 1), height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
        }
        encoder.endEncoding()
        if waterfall {
            guard let validity = command.makeComputeCommandEncoder() else { slots.signal(); return }
            validity.setComputePipelineState(continuityCompute)
            var camera = WaterfallProjection.uniforms
            var display = SpectrogramDisplay(latestSlice: slices.upperBound, fftSize: fftSize,
                sampleRate: snapshot.sampleRateHz, pixelHeight: 0)
            validity.setBytes(&display, length: MemoryLayout<SpectrogramDisplay>.stride, index: 2)
            validity.setBytes(&camera, length: MemoryLayout<WaterfallCameraUniforms>.stride, index: 3)
            for lane in lanes {
                validity.setBuffer(lane.stamps, offset: 0, index: 0)
                validity.setBuffer(lane.continuity, offset: 0, index: 1)
                validity.dispatchThreads(MTLSize(width: WaterfallProjection.historyRows - 1, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: min(64, continuityCompute.maxTotalThreadsPerThreadgroup), height: 1, depth: 1))
            }
            validity.endEncoding()
        }
        surface.pass.depthAttachment.loadAction = .clear
        surface.pass.depthAttachment.clearDepth = 1
        surface.pass.depthAttachment.storeAction = .dontCare
        guard let drawing = command.makeRenderCommandEncoder(descriptor: surface.pass) else { slots.signal(); return }
        drawing.setRenderPipelineState(waterfall ? waterfallRender : render)
        let scale = view.window?.backingScaleFactor ?? 1
        for lane in lanes {
            let rect = (regions.first { $0.transform == lane.transform }?.rect ?? .zero).intersection(view.bounds)
            guard rect.width > 0, rect.height > 0 else { continue }
            drawing.setViewport(MTLViewport(originX: rect.minX * scale, originY: rect.minY * scale,
                width: rect.width * scale, height: rect.height * scale, znear: 0, zfar: 1))
            drawing.setScissorRect(MTLScissorRect(x: Int(rect.minX * scale), y: Int(rect.minY * scale),
                width: max(1, Int(rect.width * scale)), height: max(1, Int(rect.height * scale))))
            var display = SpectrogramDisplay(latestSlice: slices.upperBound, fftSize: fftSize, sampleRate: snapshot.sampleRateHz, pixelHeight: UInt32(max(1, rect.height * scale)))
            if waterfall {
                drawing.setVertexTexture(lane.texture, index: 0)
                drawing.setVertexBuffer(lane.stamps, offset: 0, index: 0)
                drawing.setVertexBytes(&display, length: MemoryLayout<SpectrogramDisplay>.stride, index: 1)
                var camera = WaterfallProjection.uniforms
                drawing.setVertexBytes(&camera, length: MemoryLayout<WaterfallCameraUniforms>.stride, index: 2)
                drawing.setRenderPipelineState(surfaceRender)
                drawing.setDepthStencilState(depthWrite)
                drawing.setDepthBias(0, slopeScale: 0, clamp: 0)
                drawing.setVertexBuffer(lane.continuity, offset: 0, index: 3)
                drawing.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 512, instanceCount: WaterfallProjection.historyRows - 1)
                if wireOverlay {
                    drawing.setRenderPipelineState(waterfallRender)
                    drawing.setDepthStencilState(depthRead)
                    drawing.setDepthBias(-0.00001, slopeScale: 0, clamp: 0)
                    drawing.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: 256, instanceCount: WaterfallProjection.historyRows)
                }
            } else {
                drawing.setFragmentTexture(lane.texture, index: 0)
                drawing.setFragmentBuffer(lane.stamps, offset: 0, index: 0)
                drawing.setFragmentBytes(&display, length: MemoryLayout<SpectrogramDisplay>.stride, index: 1)
                drawing.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            }
        }
        drawing.endEncoding(); command.present(surface.drawable)
        command.addCompletedHandler { _ in slots.signal() }
        timeline = nextTimeline; epoch = key
        submission.commit(command)
    }
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
}
