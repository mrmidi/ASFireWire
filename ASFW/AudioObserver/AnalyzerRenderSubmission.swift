import MetalKit
import QuartzCore

/// All synchronous plot callbacks for one analysis notification encode into
/// one command buffer. SwiftUI updates configure views; they don't submit work.
@MainActor
final class AnalyzerRenderSubmission {
    private let metrics: AudioObserverMetricsState?
    private var frameStartedAt: Double?
    private var frameOrigin: Double?

    init(metrics: AudioObserverMetricsState? = nil) { self.metrics = metrics }

    private var queue: MTLCommandQueue?
    private var deviceKey: ObjectIdentifier?
    private var frameCommand: MTLCommandBuffer?
    private var batching = false
    private var drawableMilliseconds = 0.0
    private let frames = DispatchSemaphore(value: 2)
    private(set) var submittedCommandBuffers: UInt64 = 0

    func withFrame(_ body: () -> Void) {
        precondition(!batching)
        guard frames.wait(timeout: .now()) == .success else { return }
        drawableMilliseconds = 0
        frameStartedAt = CACurrentMediaTime()
        frameOrigin = metrics?.visualOrigin()
        batching = true
        defer {
            batching = false
            if let command = frameCommand {
                let frames = self.frames
                command.addCompletedHandler { _ in frames.signal() }
                submittedCommandBuffers &+= 1
                measureAndCommit(command, cpuStart: frameStartedAt, origin: frameOrigin)
            } else {
                frames.signal()
            }
            frameCommand = nil
            frameStartedAt = nil; frameOrigin = nil
        }
        body()
    }

    /// MTKView may wait here for a drawable. Separate this wall time from encode work.
    func drawable(for view: MTKView) -> (pass: MTLRenderPassDescriptor, drawable: CAMetalDrawable)? {
        let start = CACurrentMediaTime()
        defer { if batching { drawableMilliseconds += (CACurrentMediaTime() - start) * 1_000 } }
        guard let pass = view.currentRenderPassDescriptor, let drawable = view.currentDrawable else { return nil }
        return (pass, drawable)
    }

    func commandBuffer(for device: MTLDevice) -> MTLCommandBuffer? {
        if batching, let frameCommand { return frameCommand }
        let key = ObjectIdentifier(device)
        if deviceKey != key {
            queue = device.makeCommandQueue()
            deviceKey = key
        }
        guard let command = queue?.makeCommandBuffer() else { return nil }
        command.label = "ASFW analyzer visual frame"
        if batching { frameCommand = command }
        return command
    }

    func commit(_ command: MTLCommandBuffer) {
        // The outer frame owns submission; each renderer still registers its
        // completion handler and presents its own drawable on this buffer.
        guard !batching else { return }
        submittedCommandBuffers &+= 1
        measureAndCommit(command, cpuStart: nil, origin: nil)
    }

    private func measureAndCommit(_ command: MTLCommandBuffer, cpuStart: Double?, origin: Double?) {
        let committedAt = CACurrentMediaTime()
        let cpu = cpuStart.map { (committedAt - $0) * 1_000 }
        let metrics = self.metrics
        let drawable = cpuStart == nil ? nil : drawableMilliseconds
        command.addCompletedHandler { completed in
            guard completed.status == .completed else { return }
            let start = completed.gpuStartTime, end = completed.gpuEndTime
            guard start > 0, end >= start else { return }
            // Use GPU finish time for the chain, excluding CPU completion-callback delay.
            let chain = origin.flatMap { end >= $0 ? (end - $0) * 1_000 : nil }
            metrics?.recordVisual(gpu: (end - start) * 1_000, cpu: cpu,
                queue: max(0, start - committedAt) * 1_000, chain: chain, drawable: drawable,
                at: ProcessInfo.processInfo.systemUptime)
        }
        command.commit()
    }
}

/// Completion-driven throttling. MTKView's preferred FPS does not throttle
/// explicit draw() calls; each surface therefore owns this inexpensive gate.
@MainActor
struct AnalyzerDrawCadence {
    private var lastDraw = -Double.infinity
    mutating func shouldDraw(now: Double, hz: Double) -> Bool {
        guard now - lastDraw >= 1 / hz else { return false }
        // Keep a stable deadline when completions arrive at a different rate
        // (e.g. 50 Hz analysis driving a 30 Hz spectrum), without catch-up bursts.
        let interval = 1 / hz
        lastDraw = lastDraw.isFinite && now - lastDraw < 2 * interval ? lastDraw + interval : now
        return true
    }
}
