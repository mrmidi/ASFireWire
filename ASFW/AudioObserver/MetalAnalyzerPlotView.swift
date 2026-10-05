import MetalKit
import SwiftUI
import QuartzCore

struct AnalyzerPlotParams {
    var mode: UInt32
    var index: UInt32
    var active: UInt32
    var count: UInt32
    var latestFrame: UInt64
    var sampleRate: UInt32
    var isLight: UInt32 = 0
    var value: Float
    var peak: Float
    var width: Float
    var height: Float
}

nonisolated struct AnalyzerHistoryVertex: Sendable {
    var frame: UInt64
    var correlation: Float
    var sideEnergy: Float
    var breakBefore: UInt32
    var integrated: Float = .nan
}

/// Non-observable cross-executor history. Every read/update is protected by
/// the lock; immutable Array snapshots keep storage alive during GPU upload.
nonisolated final class AnalyzerPlotHistoryState: @unchecked Sendable {
    private let lock = NSLock()
    private var data: (stereo: [AudioStereoHistoryPoint], loudness: [AnalyzerHistoryVertex], revision: UInt64) = ([], [], 0)
    var stereo: [AudioStereoHistoryPoint] { read().stereo }
    var loudness: [AnalyzerHistoryVertex] { read().loudness }
    var revision: UInt64 { read().revision }

    func read() -> (stereo: [AudioStereoHistoryPoint], loudness: [AnalyzerHistoryVertex], revision: UInt64) {
        lock.lock()
        defer { lock.unlock() }
        return data
    }

    func update(stereo: [AudioStereoHistoryPoint], loudness: [AnalyzerHistoryVertex]) {
        lock.lock()
        defer { lock.unlock() }
        data = (stereo, loudness, data.revision &+ 1)
    }
}

struct AnalyzerPlotRegion: Equatable {
    /// Readout text slots (AnalyzerMetalText).
    static let textMode: UInt32 = 6
    var mode: UInt32
    var index: UInt32
    var rect: CGRect
    var otherChannel: UInt32 = 0
    var text: AnalyzerTextSpec? = nil

    static func == (a: Self, b: Self) -> Bool {
        a.mode == b.mode && a.index == b.index && a.rect == b.rect &&
            a.otherChannel == b.otherChannel && a.text?.key == b.text?.key
    }
}

/// Only scalar reductions/history cross the CPU. Geometry and trace drawing
/// are performed by Metal, triggered by completed analysis batches.
@MainActor
struct MetalAnalyzerPlotView: NSViewRepresentable {
    let client: ASFWAudioObserverClient
    let mode: UInt32 // 0: meter, 1: bipolar indicator, 2: stereo history, 3: loudness history, 4: loudness bar
    let index: UInt32
    var points: [AudioStereoHistoryPoint] = []
    var regions: [AnalyzerPlotRegion]? = nil
    var loudnessPoints: [AnalyzerHistoryVertex] = []
    var historyState: AnalyzerPlotHistoryState?
    private static var pipelines: [ObjectIdentifier: MTLRenderPipelineState] = [:]
    private static var glyphPipelines: [ObjectIdentifier: MTLRenderPipelineState] = [:]

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: client.metalDevice)
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColorMake(0, 0, 0, 0)
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.layer?.isOpaque = false
        context.coordinator.regions = regions
        context.coordinator.historyState = historyState
        configure(view, coordinator: context.coordinator, device: client.metalDevice)
        observeCompletions(view, coordinator: context.coordinator) { client.metalDevice }
        return view
    }

    func observeCompletions(_ view: MTKView, coordinator: Coordinator,
                            deviceProvider: @escaping @MainActor () -> MTLDevice?) {
        coordinator.observer = NotificationCenter.default.addObserver(
            forName: .asfwAnalysisCompleted, object: client.renderState, queue: .main
        ) { [weak view, weak coordinator] _ in
            MainActor.assumeIsolated {
                guard let view, let coordinator else { return }
                let hasFastPlots = coordinator.regions?.contains { $0.mode == 0 || $0.mode == 1 || $0.mode == 5 } ?? (mode < 2)
                guard !client.renderState.read().ioRunning ||
                    coordinator.cadence.shouldDraw(now: CACurrentMediaTime(), hz: hasFastPlots ? 60 : 10) else { return }
                // Constant representable inputs can cause SwiftUI to elide
                // updateNSView after client.open(). GPU completion is also
                // responsible for attaching the renderer in that case.
                configure(view, coordinator: coordinator, device: deviceProvider())
                coordinator.renderer?.updateLiveHistory(coordinator.historyState)
                view.draw()
            }
        }
    }

    func updateNSView(_ view: MTKView, context: Context) {
        context.coordinator.regions = regions
        context.coordinator.historyState = historyState
        context.coordinator.renderer?.regions = regions
        configure(view, coordinator: context.coordinator, device: client.metalDevice)
        if historyState == nil {
            context.coordinator.renderer?.updateHistory(points)
            context.coordinator.renderer?.updateLoudnessHistory(loudnessPoints)
        } else { context.coordinator.renderer?.updateLiveHistory(historyState) }
        // GPU completion schedules the next visual frame. A layout change
        // (new readout slots, resize) draws once so readouts appear without audio.
        view.needsDisplay = true
    }

    // SwiftUI creates the panel before client.open() makes Metal available.
    // Retry on updates, and drop the old renderer when the client disconnects.
    func configure(_ view: MTKView, coordinator: Coordinator, device: MTLDevice?) {
        guard let device else {
            view.delegate = nil
            coordinator.renderer = nil
            coordinator.deviceKey = nil
            coordinator.ringKey = nil
            view.device = nil
            return
        }
        let key = ObjectIdentifier(device)
        let ringKey = client.ringBuffer.map { ObjectIdentifier($0) }
        guard coordinator.deviceKey != key || coordinator.ringKey != ringKey || coordinator.renderer == nil else { return }
        guard let pipeline = Self.pipeline(device) else { return }
        view.device = device
        // The view may already have been laid out with a nil device. Assigning
        // a device does not imply a subsequent AppKit resize notification.
        let scale = view.window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 1
        view.drawableSize = CGSize(width: view.bounds.width * scale, height: view.bounds.height * scale)
        let renderer = AnalyzerPlotRenderer(device: device, pipeline: pipeline, glyphPipeline: Self.glyphPipeline(device),
                                            submission: client.renderSubmission,
                                            metrics: client.metrics, state: client.renderState,
                                            mode: mode, index: index, ring: client.ringBuffer,
                                            phasePipeline: client.phaseRenderPipeline)
        renderer.regions = coordinator.regions
        if historyState == nil {
            renderer.updateHistory(points)
            renderer.updateLoudnessHistory(loudnessPoints)
        } else { renderer.updateLiveHistory(historyState) }
        coordinator.renderer = renderer
        coordinator.deviceKey = key
        coordinator.ringKey = ringKey
        view.delegate = renderer
    }

    static func pipeline(_ device: MTLDevice) -> MTLRenderPipelineState? {
        let key = ObjectIdentifier(device)
        if let pipeline = pipelines[key] { return pipeline }
        guard let library = device.makeDefaultLibrary() else { return nil }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "asfwAnalyzerPlotVertex")
        descriptor.fragmentFunction = library.makeFunction(name: "asfwAnalyzerPlotFragment")
        let attachment = descriptor.colorAttachments[0]!
        attachment.pixelFormat = .bgra8Unorm
        attachment.isBlendingEnabled = true
        attachment.sourceRGBBlendFactor = .sourceAlpha
        attachment.destinationRGBBlendFactor = .oneMinusSourceAlpha
        attachment.sourceAlphaBlendFactor = .one
        attachment.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        guard let pipeline = try? device.makeRenderPipelineState(descriptor: descriptor) else { return nil }
        pipelines[key] = pipeline
        return pipeline
    }

    static func glyphPipeline(_ device: MTLDevice) -> MTLRenderPipelineState? {
        let key = ObjectIdentifier(device)
        if let pipeline = glyphPipelines[key] { return pipeline }
        guard let library = device.makeDefaultLibrary() else { return nil }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "asfwAnalyzerGlyphVertex")
        descriptor.fragmentFunction = library.makeFunction(name: "asfwAnalyzerGlyphFragment")
        let attachment = descriptor.colorAttachments[0]!
        attachment.pixelFormat = .bgra8Unorm
        attachment.isBlendingEnabled = true
        attachment.sourceRGBBlendFactor = .sourceAlpha
        attachment.destinationRGBBlendFactor = .oneMinusSourceAlpha
        attachment.sourceAlphaBlendFactor = .one
        attachment.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        guard let pipeline = try? device.makeRenderPipelineState(descriptor: descriptor) else { return nil }
        glyphPipelines[key] = pipeline
        return pipeline
    }

    final class Coordinator {
        var cadence = AnalyzerDrawCadence()
        fileprivate var renderer: AnalyzerPlotRenderer?
        var observer: NSObjectProtocol?
        var deviceKey: ObjectIdentifier?
        var ringKey: ObjectIdentifier?
        var regions: [AnalyzerPlotRegion]?
        var historyState: AnalyzerPlotHistoryState?
        deinit { if let observer { NotificationCenter.default.removeObserver(observer) } }
    }
}

private final class AnalyzerPlotRenderer: NSObject, MTKViewDelegate {
    private let device: MTLDevice
    private let pipeline: MTLRenderPipelineState
    private let glyphPipeline: MTLRenderPipelineState?
    private var glyphAtlases: [AnalyzerTextStyle: (scale: CGFloat, atlas: AnalyzerGlyphAtlas)] = [:]
    private var readoutText = AnalyzerTextCache()
    private let readoutBatch = AnalyzerReadoutBatch()
    private var readoutPrimary = SIMD4<Float>(1, 1, 1, 1)
    private var readoutSecondary = SIMD4<Float>(1, 1, 1, 0.55)
    private let submission: AnalyzerRenderSubmission
    private let metrics: AudioObserverMetricsState
    private let state: AudioObserverRenderState
    private let mode: UInt32
    private let index: UInt32
    private let slots = DispatchSemaphore(value: 2)
    private var stereoHistory: MTLBuffer?
    private var loudnessHistory: MTLBuffer?
    private var stereoCount = 0
    private var loudnessCount = 0
    private var stereoFrame: UInt64 = 0
    private var loudnessFrame: UInt64 = 0
    private let ring: MTLBuffer?
    private let phasePipeline: MTLRenderPipelineState?
    private var historyRevision: UInt64?
    private var displaySmoothers: [UInt64: AnalyzerDisplaySmoother] = [:]
    private var displayEpoch: (UInt64, UInt64, UInt64)?
    var regions: [AnalyzerPlotRegion]?

    init(device: MTLDevice, pipeline: MTLRenderPipelineState, glyphPipeline: MTLRenderPipelineState?,
         submission: AnalyzerRenderSubmission,
         metrics: AudioObserverMetricsState, state: AudioObserverRenderState,
         mode: UInt32, index: UInt32, ring: MTLBuffer?, phasePipeline: MTLRenderPipelineState?) {
        self.device = device; self.pipeline = pipeline; self.glyphPipeline = glyphPipeline
        self.submission = submission
        self.metrics = metrics; self.state = state; self.mode = mode; self.index = index
        self.ring = ring; self.phasePipeline = phasePipeline
    }

    @MainActor
    func updateLiveHistory(_ state: AnalyzerPlotHistoryState?) {
        guard let state else { return }
        let snapshot = state.read()
        guard historyRevision != snapshot.revision else { return }
        historyRevision = snapshot.revision
        let plots = regions ?? [AnalyzerPlotRegion(mode: mode, index: index, rect: .zero)]
        if plots.contains(where: { $0.mode == 2 }) { updateHistory(snapshot.stereo) }
        if plots.contains(where: { $0.mode == 3 }) { updateLoudnessHistory(snapshot.loudness) }
    }

    func updateHistory(_ points: [AudioStereoHistoryPoint]) {
        stereoCount = points.count
        stereoFrame = points.last?.endFrame ?? 0
        guard !points.isEmpty else { stereoHistory = nil; return }
        let vertices = points.map { AnalyzerHistoryVertex(frame: $0.endFrame,
            correlation: $0.correlation, sideEnergy: $0.sideEnergyFraction,
            breakBefore: $0.breakBefore ? 1 : 0) }
        stereoHistory = vertices.withUnsafeBufferPointer { buffer in
            device.makeBuffer(bytes: buffer.baseAddress!,
                              length: buffer.count * MemoryLayout<AnalyzerHistoryVertex>.stride,
                              options: .storageModeShared)
        }
    }

    func updateLoudnessHistory(_ vertices: [AnalyzerHistoryVertex]) {
        loudnessCount = vertices.count
        loudnessFrame = vertices.last?.frame ?? 0
        guard !vertices.isEmpty else { loudnessHistory = nil; return }
        loudnessHistory = vertices.withUnsafeBufferPointer {
            device.makeBuffer(bytes: $0.baseAddress!, length: $0.count * MemoryLayout<AnalyzerHistoryVertex>.stride,
                              options: .storageModeShared)
        }
    }

    func draw(in view: MTKView) {
        guard view.bounds.width > 0, view.bounds.height > 0 else { return }
        let scale = view.window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 1
        let size = CGSize(width: view.bounds.width * scale, height: view.bounds.height * scale)
        if view.drawableSize != size { view.drawableSize = size }
        guard slots.wait(timeout: .now()) == .success else { return }
        let slots = self.slots
        var submitted = false
        defer { if !submitted { slots.signal() } }
        guard let surface = submission.drawable(for: view),
              let command = submission.commandBuffer(for: device),
              let encoder = command.makeRenderCommandEncoder(descriptor: surface.pass) else { return }
        let snapshot = state.read()
        let now = CACurrentMediaTime()
        let epoch = (snapshot.sessionEpoch, snapshot.discontinuityEpoch, snapshot.memoryGeneration)
        if displayEpoch == nil || displayEpoch! != epoch {
            displaySmoothers.removeAll(keepingCapacity: true)
            displayEpoch = epoch
        }
        encoder.setRenderPipelineState(pipeline)
        let plots = regions ?? [AnalyzerPlotRegion(mode: mode, index: index, rect: view.bounds)]
        for plot in plots where plot.mode != AnalyzerPlotRegion.textMode {
            let rect = plot.rect.intersection(view.bounds)
            guard rect.width > 0, rect.height > 0 else { continue }
            encoder.setViewport(MTLViewport(originX: rect.minX * scale, originY: rect.minY * scale,
                width: rect.width * scale, height: rect.height * scale, znear: 0, zfar: 1))
            encoder.setScissorRect(MTLScissorRect(x: Int(rect.minX * scale), y: Int(rect.minY * scale),
                width: max(1, Int(rect.width * scale)), height: max(1, Int(rect.height * scale))))
            if plot.mode == 5 {
                guard snapshot.ioRunning, snapshot.channels >= 2, snapshot.activeRingFrames > 0,
                      let ring, let phasePipeline else { continue }
                let frames = min(snapshot.validHistoryFrames,
                    AudioAnalyzerGeometry.goniometerWindowFrames(activeRingFrames: snapshot.activeRingFrames))
                guard frames > 1 else { continue }
                var phase = ObserverParams(writeEndFrame: snapshot.writeEndFrame,
                    ringFrames: snapshot.activeRingFrames, channels: snapshot.channels, windowFrames: frames,
                    channel: min(plot.index, snapshot.channels - 1),
                    rightChannel: min(plot.otherChannel, snapshot.channels - 1))
                encoder.setRenderPipelineState(phasePipeline)
                encoder.setVertexBuffer(ring, offset: 0, index: 0)
                encoder.setVertexBytes(&phase, length: MemoryLayout<ObserverParams>.stride, index: 1)
                encoder.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: Int(frames))
                continue
            }
            encoder.setRenderPipelineState(pipeline)
            let count = plot.mode == 2 ? stereoCount : loudnessCount
            let latestFrame = plot.mode == 2 ? stereoFrame : loudnessFrame
            let history = plot.mode == 2 ? stereoHistory : loudnessHistory
            let (rawValue, rawPeak, valid) = metrics.plotValues(mode: plot.mode, index: plot.index)
            let active = snapshot.ioRunning && valid
            var value = rawValue
            var peak = rawPeak
            if plot.mode == 0 || plot.mode == 1 || plot.mode == 4 {
                let key = UInt64(plot.mode) << 32 | UInt64(plot.index)
                var smoother = displaySmoothers[key] ?? AnalyzerDisplaySmoother()
                let smoothed = smoother.update(value: rawValue, peak: rawPeak, mode: plot.mode, now: now, active: active)
                displaySmoothers[key] = smoother
                value = smoothed.value; peak = smoothed.peak
            }
            let isLight: UInt32 = AnalyzerThemeState.shared.mode.isLight ? 1 : 0
            var params = AnalyzerPlotParams(mode: plot.mode, index: plot.index,
                active: snapshot.ioRunning && valid ? 1 : 0, count: UInt32(count),
                latestFrame: latestFrame, sampleRate: snapshot.sampleRateHz, isLight: isLight,
                value: value, peak: peak, width: Float(rect.width * scale), height: Float(rect.height * scale))
            encoder.setVertexBytes(&params, length: MemoryLayout<AnalyzerPlotParams>.stride, index: 0)
            var emptyPoint = AnalyzerHistoryVertex(frame: 0, correlation: 0, sideEnergy: 0, breakBefore: 0)
            encoder.setVertexBytes(&emptyPoint, length: MemoryLayout<AnalyzerHistoryVertex>.stride, index: 1)
            if plot.mode == 2 || plot.mode == 3 {
                if let history, count > 1 {
                    encoder.setVertexBuffer(history, offset: 0, index: 1)
                    encoder.drawPrimitives(type: .line, vertexStart: 0, vertexCount: (count - 1) * 2)
                }
            } else {
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 12)
            }
        }
        drawReadouts(plots.filter { $0.text != nil }, encoder: encoder,
                     view: view, scale: scale, now: now)
        encoder.endEncoding()
        command.present(surface.drawable)
        command.addCompletedHandler { _ in slots.signal() }
        submitted = true
        submission.commit(command)
    }
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    /// Numeric readouts drawn as glyph quads, so a changing value never goes
    /// through SwiftUI text layout. Text is re-formatted at the SwiftUI
    /// readouts' 250 ms cadence; every frame redraws the cached strings.
    private func drawReadouts(_ slots: [AnalyzerPlotRegion], encoder: MTLRenderCommandEncoder,
                              view: MTKView, scale: CGFloat, now: Double) {
        guard !slots.isEmpty, let glyphPipeline else { return }
        let theme = AnalyzerThemeState.shared.mode
        readoutPrimary = theme.readoutPrimary
        readoutSecondary = theme.readoutSecondary
        var metricsSnapshot: AudioObserverMetrics?
        let readMetrics = { [metrics] () -> AudioObserverMetrics in
            if let metricsSnapshot { return metricsSnapshot }
            let value = metrics.read(includeHistory: false)
            metricsSnapshot = value
            return value
        }
        let stateSnapshot = state.read()
        var drawableSize = SIMD2<Float>(Float(view.drawableSize.width), Float(view.drawableSize.height))
        encoder.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(drawableSize.x),
                                        height: Double(drawableSize.y), znear: 0, zfar: 1))
        encoder.setScissorRect(MTLScissorRect(x: 0, y: 0, width: Int(drawableSize.x), height: Int(drawableSize.y)))
        encoder.setRenderPipelineState(glyphPipeline)
        encoder.setVertexBytes(&drawableSize, length: MemoryLayout<SIMD2<Float>>.stride, index: 1)
        var items: [AnalyzerReadoutBatch.Item] = []
        items.reserveCapacity(slots.count)
        for slot in slots {
            guard let spec = slot.text, let atlas = atlas(for: spec.style, scale: scale) else { continue }
            let text = readoutText.text(for: spec, now: now, metrics: readMetrics, snapshot: { stateSnapshot })
            let rect = CGRect(x: slot.rect.minX * scale, y: slot.rect.minY * scale,
                              width: slot.rect.width * scale, height: slot.rect.height * scale)
            items.append(.init(key: spec.key, text: text, rect: rect, atlas: atlas,
                               alignment: spec.alignment, tone: spec.tone))
        }
        readoutBatch.update(items, device: device)
        guard let buffer = readoutBatch.buffer else { return }
        encoder.setVertexBuffer(buffer, offset: 0, index: 0)
        for draw in readoutBatch.draws {
            var color = draw.tone == .primary ? readoutPrimary : readoutSecondary
            encoder.setFragmentBytes(&color, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            encoder.setFragmentTexture(draw.texture, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: draw.start, vertexCount: draw.count)
        }
    }

    private func atlas(for style: AnalyzerTextStyle, scale: CGFloat) -> AnalyzerGlyphAtlas? {
        if let cached = glyphAtlases[style], cached.scale == scale { return cached.atlas }
        let atlas = AnalyzerGlyphAtlas(font: style.font(scale: scale), device: device)
        glyphAtlases[style] = (scale, atlas)
        return atlas
    }
}
