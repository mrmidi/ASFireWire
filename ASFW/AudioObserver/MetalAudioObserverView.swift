import Metal
import MetalKit
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
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.preferredFramesPerSecond = 60
        view.clearColor = MTLClearColor(red: 0.025, green: 0.035,
                                        blue: 0.05, alpha: 1)

        configure(view, coordinator: context.coordinator)
        context.coordinator.drawObserver = NotificationCenter.default.addObserver(
            forName: .asfwAnalysisCompleted, object: client.renderState, queue: .main
        ) { [weak view] _ in
            MainActor.assumeIsolated { view?.draw() }
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
                renderState: client.renderState)
            coordinator.renderer = renderer
            view.delegate = renderer
        } else {
            coordinator.renderer = nil
            view.delegate = nil
        }
    }

    final class Coordinator {
        var drawObserver: NSObjectProtocol?
        deinit { if let drawObserver { NotificationCenter.default.removeObserver(drawObserver) } }

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

final class AudioObserverRenderer: NSObject, MTKViewDelegate {
    private static let waveformWindowFrames: UInt32 = 960

    private let buffer: MTLBuffer?
    private let renderPipeline: MTLRenderPipelineState
    private let leftChannel: UInt32
    private let rightChannel: UInt32
    private let mode: AudioObserverDisplayMode
    private let renderState: AudioObserverRenderState
    private let commandQueue: MTLCommandQueue?
    private let slots = DispatchSemaphore(value: 2)
    private var lastWriteEnd: UInt64?

    init(buffer: MTLBuffer?,
         renderPipeline: MTLRenderPipelineState,
         mode: AudioObserverDisplayMode,
         leftChannel: UInt32,
         rightChannel: UInt32,
         renderState: AudioObserverRenderState) {
        self.buffer = buffer
        self.renderPipeline = renderPipeline
        self.mode = mode
        self.leftChannel = leftChannel
        self.rightChannel = rightChannel
        self.renderState = renderState
        self.commandQueue = renderPipeline.device.makeCommandQueue()
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
              let pass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let commandBuffer = commandQueue?.makeCommandBuffer() else {
            return
        }

        var params = ObserverParams(
            writeEndFrame: snapshot.writeEndFrame,
            ringFrames: snapshot.activeRingFrames,
            channels: snapshot.channels,
            windowFrames: validFrames,
            channel: min(leftChannel, snapshot.channels - 1),
                rightChannel: min(rightChannel, snapshot.channels - 1))
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
        commandBuffer.addCompletedHandler { _ in slots.signal() }
        submitted = true
        lastWriteEnd = snapshot.writeEndFrame
        commandBuffer.commit()
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
}
