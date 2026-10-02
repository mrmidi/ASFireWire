import Metal
import MetalKit
import SwiftUI
import QuartzCore

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
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.preferredFramesPerSecond = 60
        view.clearColor = MTLClearColor(red: 0.025, green: 0.035,
                                        blue: 0.05, alpha: 1)

        configure(view, coordinator: context.coordinator)
        context.coordinator.drawObserver = NotificationCenter.default.addObserver(
            forName: .asfwAnalysisCompleted, object: client.renderState, queue: .main
        ) { [weak view, weak coordinator = context.coordinator] _ in
            MainActor.assumeIsolated {
                guard let coordinator, coordinator.cadence.shouldDraw(now: CACurrentMediaTime(), hz: mode == .phaseScope ? 60 : 30) else { return }
                view?.draw()
            }
        }
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
                mode: mode,
                leftChannel: leftChannel,
                rightChannel: rightChannel,
                renderState: client.renderState,
                submission: client.renderSubmission)
            coordinator.renderer = renderer
            view.delegate = renderer
        } else {
            coordinator.renderer = nil
            view.delegate = nil
        }
    }

    final class Coordinator {
        var cadence = AnalyzerDrawCadence()
        var drawObserver: NSObjectProtocol?
        deinit { if let drawObserver { NotificationCenter.default.removeObserver(drawObserver) } }

        var configurationKey: String?
        var renderer: AudioObserverRenderer?
    }
}

struct ObserverParams {
    var writeEndFrame: UInt64
    var ringFrames: UInt32
    var channels: UInt32
    var windowFrames: UInt32
    var channel: UInt32
    var rightChannel: UInt32
}

final class AudioObserverRenderer: NSObject, MTKViewDelegate {
    private static let waveformWindowFrames: UInt32 = 960

    private let buffer: MTLBuffer?
    private let renderPipeline: MTLRenderPipelineState
    private let leftChannel: UInt32
    private let rightChannel: UInt32
    private let mode: AudioObserverDisplayMode
    private let renderState: AudioObserverRenderState
    private let submission: AnalyzerRenderSubmission
    private let slots = DispatchSemaphore(value: 2)
    private var lastWriteEnd: UInt64?

    init(buffer: MTLBuffer?,
         renderPipeline: MTLRenderPipelineState,
         mode: AudioObserverDisplayMode,
         leftChannel: UInt32,
         rightChannel: UInt32,
         renderState: AudioObserverRenderState,
         submission: AnalyzerRenderSubmission) {
        self.buffer = buffer
        self.renderPipeline = renderPipeline
        self.mode = mode
        self.leftChannel = leftChannel
        self.rightChannel = rightChannel
        self.renderState = renderState
        self.submission = submission
    }

    func draw(in view: MTKView) {
        let snapshot = renderState.read()
        guard snapshot.ioRunning, snapshot.writeEndFrame != lastWriteEnd,
              slots.wait(timeout: .now()) == .success else { return }
        let slots = self.slots
        var submitted = false
        defer { if !submitted { slots.signal() } }
        // Read two thirds of the active ring directly. The unused third is
        // overwrite slack while the GPU consumes this best-effort view.
        let desiredWindow = mode == .phaseScope
            ? AudioAnalyzerGeometry.goniometerWindowFrames(
                activeRingFrames: snapshot.activeRingFrames)
            : Self.waveformWindowFrames
        let validFrames = min(snapshot.validHistoryFrames,
                              snapshot.activeRingFrames,
                              desiredWindow)
        guard let buffer, snapshot.channels >= 2,
              snapshot.activeRingFrames > 0,
              let surface = submission.drawable(for: view),
              let commandBuffer = submission.commandBuffer(for: renderPipeline.device) else {
            return
        }

        var params = ObserverParams(
            writeEndFrame: snapshot.writeEndFrame,
            ringFrames: snapshot.activeRingFrames,
            channels: snapshot.channels,
            windowFrames: validFrames,
            channel: min(leftChannel, snapshot.channels - 1),
                rightChannel: min(rightChannel, snapshot.channels - 1))
        guard let render = commandBuffer.makeRenderCommandEncoder(descriptor: surface.pass) else {
            return
        }

        render.setRenderPipelineState(renderPipeline)
        render.setVertexBuffer(buffer, offset: 0, index: 0)
        if validFrames > 1 {
            let draws = mode == .waveform ? 2 : 1
            for lane in 0..<draws {
                if mode == .waveform {
                    params.channel = lane == 0 ? leftChannel : rightChannel
                    let height = view.drawableSize.height / 2
                    render.setViewport(MTLViewport(originX: 0, originY: Double(lane) * height,
                        width: view.drawableSize.width, height: height, znear: 0, zfar: 1))
                    var color = lane == 0 ? SIMD4<Float>(0.2, 0.91, 0.73, 1) : SIMD4<Float>(1, 0.55, 0.1, 1)
                    render.setFragmentBytes(&color, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
                }
                render.setVertexBytes(&params, length: MemoryLayout<ObserverParams>.stride, index: 1)
                render.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: Int(validFrames))
            }
        }
        render.endEncoding()
        commandBuffer.present(surface.drawable)
        commandBuffer.addCompletedHandler { _ in slots.signal() }
        submitted = true
        lastWriteEnd = snapshot.writeEndFrame
        submission.commit(commandBuffer)
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
}
