import MetalKit
import SwiftUI

struct AnalyzerPlotParams {
    var mode: UInt32
    var index: UInt32
    var active: UInt32
    var count: UInt32
    var latestFrame: UInt64
    var sampleRate: UInt32
    var padding: UInt32 = 0
    var value: Float
    var peak: Float
    var width: Float
    var height: Float
}

struct AnalyzerHistoryVertex {
    var frame: UInt64
    var correlation: Float
    var sideEnergy: Float
    var breakBefore: UInt32
    var integrated: Float = .nan
}

@MainActor
final class AnalyzerPlotHistoryState {
    var stereo: [AudioStereoHistoryPoint] = []
    var loudness: [AnalyzerHistoryVertex] = []
    var revision: UInt64 = 0
}

struct AnalyzerPlotRegion: Equatable {
    var mode: UInt32
    var index: UInt32
    var rect: CGRect
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
    private static var queues: [ObjectIdentifier: MTLCommandQueue] = [:]
    private static var pipelines: [ObjectIdentifier: MTLRenderPipelineState] = [:]

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
        view.draw()
    }

    // SwiftUI creates the panel before client.open() makes Metal available.
    // Retry on updates, and drop the old renderer when the client disconnects.
    func configure(_ view: MTKView, coordinator: Coordinator, device: MTLDevice?) {
        guard let device else {
            view.delegate = nil
            coordinator.renderer = nil
            coordinator.deviceKey = nil
            view.device = nil
            return
        }
        let key = ObjectIdentifier(device)
        guard coordinator.deviceKey != key || coordinator.renderer == nil else { return }
        guard let pipeline = Self.pipeline(device), let queue = Self.commandQueue(device) else { return }
        view.device = device
        // The view may already have been laid out with a nil device. Assigning
        // a device does not imply a subsequent AppKit resize notification.
        let scale = view.window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 1
        view.drawableSize = CGSize(width: view.bounds.width * scale, height: view.bounds.height * scale)
        let renderer = AnalyzerPlotRenderer(device: device, pipeline: pipeline, queue: queue,
                                            metrics: client.metrics, state: client.renderState,
                                            mode: mode, index: index)
        if historyState == nil {
            renderer.updateHistory(points)
            renderer.updateLoudnessHistory(loudnessPoints)
        } else { renderer.updateLiveHistory(historyState) }
        renderer.regions = coordinator.regions
        coordinator.renderer = renderer
        coordinator.deviceKey = key
        view.delegate = renderer
    }

    private static func commandQueue(_ device: MTLDevice) -> MTLCommandQueue? {
        let key = ObjectIdentifier(device)
        if let queue = queues[key] { return queue }
        guard let queue = device.makeCommandQueue() else { return nil }
        queues[key] = queue
        return queue
    }

    private static func pipeline(_ device: MTLDevice) -> MTLRenderPipelineState? {
        let key = ObjectIdentifier(device)
        if let pipeline = pipelines[key] { return pipeline }
        guard let library = device.makeDefaultLibrary() else { return nil }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "asfwAnalyzerPlotVertex")
        descriptor.fragmentFunction = library.makeFunction(name: "asfwAnalyzerPlotFragment")
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        guard let pipeline = try? device.makeRenderPipelineState(descriptor: descriptor) else { return nil }
        pipelines[key] = pipeline
        return pipeline
    }

    final class Coordinator {
        fileprivate var renderer: AnalyzerPlotRenderer?
        var observer: NSObjectProtocol?
        var deviceKey: ObjectIdentifier?
        var regions: [AnalyzerPlotRegion]?
        var historyState: AnalyzerPlotHistoryState?
        deinit { if let observer { NotificationCenter.default.removeObserver(observer) } }
    }
}

private final class AnalyzerPlotRenderer: NSObject, MTKViewDelegate {
    private let device: MTLDevice
    private let pipeline: MTLRenderPipelineState
    private let queue: MTLCommandQueue?
    private let metrics: AudioObserverMetricsState
    private let state: AudioObserverRenderState
    private let mode: UInt32
    private let index: UInt32
    private let slots = DispatchSemaphore(value: 2)
    private var history: MTLBuffer?
    private var count = 0
    private var latestFrame: UInt64 = 0
    private var historyRevision: UInt64?
    var regions: [AnalyzerPlotRegion]?

    init(device: MTLDevice, pipeline: MTLRenderPipelineState, queue: MTLCommandQueue,
         metrics: AudioObserverMetricsState, state: AudioObserverRenderState,
         mode: UInt32, index: UInt32) {
        self.device = device; self.pipeline = pipeline; self.queue = queue
        self.metrics = metrics; self.state = state; self.mode = mode; self.index = index
    }

    @MainActor
    func updateLiveHistory(_ state: AnalyzerPlotHistoryState?) {
        guard let state, historyRevision != state.revision else { return }
        historyRevision = state.revision
        if mode == 2 { updateHistory(state.stereo) }
        if mode == 3 { updateLoudnessHistory(state.loudness) }
    }

    func updateHistory(_ points: [AudioStereoHistoryPoint]) {
        guard mode == 2 else { return }
        count = points.count
        latestFrame = points.last?.endFrame ?? 0
        guard !points.isEmpty else { history = nil; return }
        let vertices = points.map { AnalyzerHistoryVertex(frame: $0.endFrame,
            correlation: $0.correlation, sideEnergy: $0.sideEnergyFraction,
            breakBefore: $0.breakBefore ? 1 : 0) }
        history = vertices.withUnsafeBufferPointer { buffer in
            device.makeBuffer(bytes: buffer.baseAddress!,
                              length: buffer.count * MemoryLayout<AnalyzerHistoryVertex>.stride,
                              options: .storageModeShared)
        }
    }

    func updateLoudnessHistory(_ vertices: [AnalyzerHistoryVertex]) {
        guard mode == 3 else { return }
        count = vertices.count
        latestFrame = vertices.last?.frame ?? 0
        guard !vertices.isEmpty else { history = nil; return }
        history = vertices.withUnsafeBufferPointer {
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
        guard let pass = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
              let command = queue?.makeCommandBuffer(),
              let encoder = command.makeRenderCommandEncoder(descriptor: pass) else { return }
        let snapshot = state.read()
        encoder.setRenderPipelineState(pipeline)
        let plots = regions ?? [AnalyzerPlotRegion(mode: mode, index: index, rect: view.bounds)]
        for plot in plots {
            let rect = plot.rect.intersection(view.bounds)
            guard rect.width > 0, rect.height > 0 else { continue }
            encoder.setViewport(MTLViewport(originX: rect.minX * scale, originY: rect.minY * scale,
                width: rect.width * scale, height: rect.height * scale, znear: 0, zfar: 1))
            encoder.setScissorRect(MTLScissorRect(x: Int(rect.minX * scale), y: Int(rect.minY * scale),
                width: max(1, Int(rect.width * scale)), height: max(1, Int(rect.height * scale))))
            let (value, peak, valid) = metrics.plotValues(mode: plot.mode, index: plot.index)
            var params = AnalyzerPlotParams(mode: plot.mode, index: plot.index,
                active: snapshot.ioRunning && valid ? 1 : 0, count: UInt32(count),
                latestFrame: latestFrame, sampleRate: snapshot.sampleRateHz,
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
        encoder.endEncoding()
        command.present(drawable)
        command.addCompletedHandler { _ in slots.signal() }
        submitted = true
        command.commit()
    }
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
}
