import Metal

/// One reusable sample buffer per compute pass. The engine has only one batch in flight.
/// Calibrate counter ticks against Metal's CPU/GPU correlation (CPU time is nanoseconds).
/// Apple recommends paired CPU/GPU samples before GPU work and after completion,
/// and sampling sparingly because sampleTimestamps() can trap to the kernel.
/// https://developer.apple.com/documentation/metal/converting-gpu-timestamps-into-cpu-time
nonisolated final class AnalyzerKernelTiming: @unchecked Sendable {
    private let buffers: [MTLCounterSampleBuffer]
    private let device: MTLDevice
    private let baseline: (cpu: UInt64, gpu: UInt64)
    private var millisecondsPerTick: Double?

    init?(device: MTLDevice) {
        guard device.supportsFamily(.apple1), device.supportsCounterSampling(.atStageBoundary),
              let set = device.counterSets?.first(where: { $0.name == MTLCommonCounterSet.timestamp.rawValue }),
              set.counters.contains(where: { $0.name == MTLCommonCounter.timestamp.rawValue }) else { return nil }
        var created: [MTLCounterSampleBuffer] = []
        for label in ["Reduction", "True peak", "K-weighting"] {
            let descriptor = MTLCounterSampleBufferDescriptor()
            descriptor.counterSet = set; descriptor.storageMode = .shared
            descriptor.sampleCount = 2; descriptor.label = "ASFW \(label) timestamps"
            guard let buffer = try? device.makeCounterSampleBuffer(descriptor: descriptor) else { return nil }
            created.append(buffer)
        }
        buffers = created
        self.device = device
        baseline = device.sampleTimestamps()
        // Finish calibration after a real command completes. Two back-to-back
        // samples can return the same GPU tick and must not disable profiling.
        // The baseline may be captured at setup; the second sample is deferred
        // to completion as described in Apple's reference linked above.
    }

    static func scale(cpuStart: UInt64, gpuStart: UInt64, cpuEnd: UInt64, gpuEnd: UInt64) -> Double? {
        guard cpuEnd > cpuStart, gpuEnd > gpuStart else { return nil }
        return Double(cpuEnd - cpuStart) / Double(gpuEnd - gpuStart) / 1_000_000
    }

    func encoder(_ command: MTLCommandBuffer, stage: Int, start: Bool = true, end: Bool = true) -> MTLComputeCommandEncoder? {
        let pass = MTLComputePassDescriptor()
        let attachment = pass.sampleBufferAttachments[0]!
        attachment.sampleBuffer = buffers[stage]
        attachment.startOfEncoderSampleIndex = start ? 0 : MTLCounterDontSample
        attachment.endOfEncoderSampleIndex = end ? 1 : MTLCounterDontSample
        return command.makeComputeCommandEncoder(descriptor: pass)
    }

    /// Call only after the batch completes, before reusing its sample buffers.
    func milliseconds(stage: Int) -> Double? {
        if millisecondsPerTick == nil {
            let end = device.sampleTimestamps()
            millisecondsPerTick = Self.scale(cpuStart: baseline.cpu, gpuStart: baseline.gpu,
                                             cpuEnd: end.cpu, gpuEnd: end.gpu)
        }
        // A temporarily stationary/unavailable clock is retried next completion.
        guard let millisecondsPerTick else { return nil }
        guard let data = try? buffers[stage].resolveCounterRange(0..<2), data.count == 16 else { return nil }
        return data.withUnsafeBytes { bytes in
            let start = bytes.loadUnaligned(fromByteOffset: 0, as: UInt64.self)
            let end = bytes.loadUnaligned(fromByteOffset: 8, as: UInt64.self)
            guard start != MTLCounterErrorValue, end != MTLCounterErrorValue, end >= start else { return nil }
            return Double(end - start) * millisecondsPerTick
        }
    }
}
