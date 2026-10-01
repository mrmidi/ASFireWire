import Metal

/// All synchronous plot callbacks for one analysis notification encode into
/// one command buffer. SwiftUI updates configure views; they don't submit work.
@MainActor
final class AnalyzerRenderSubmission {
    private var queue: MTLCommandQueue?
    private var deviceKey: ObjectIdentifier?
    private var frameCommand: MTLCommandBuffer?
    private var batching = false
    private let frames = DispatchSemaphore(value: 2)
    private(set) var submittedCommandBuffers: UInt64 = 0

    func withFrame(_ body: () -> Void) {
        precondition(!batching)
        guard frames.wait(timeout: .now()) == .success else { return }
        batching = true
        defer {
            batching = false
            if let command = frameCommand {
                let frames = self.frames
                command.addCompletedHandler { _ in frames.signal() }
                submittedCommandBuffers &+= 1
                command.commit()
            } else {
                frames.signal()
            }
            frameCommand = nil
        }
        body()
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
