import Metal

/// One reusable sample buffer per compute pass. The engine has only one batch in flight.
/// Calibrate counter ticks against Metal's CPU/GPU correlation (CPU time is nanoseconds).
nonisolated final class AnalyzerKernelTiming: @unchecked Sendable {
    private let buffers: [MTLCounterSampleBuffer]
    private let millisecondsPerTick: Double

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
        let first = device.sampleTimestamps()
        let second = device.sampleTimestamps()
        guard second.cpu > first.cpu, second.gpu > first.gpu else { return nil }
        millisecondsPerTick = Double(second.cpu - first.cpu) / Double(second.gpu - first.gpu) / 1_000_000
    }

    func encoder(_ command: MTLCommandBuffer, stage: Int) -> MTLComputeCommandEncoder? {
        let pass = MTLComputePassDescriptor()
        let attachment = pass.sampleBufferAttachments[0]!
        attachment.sampleBuffer = buffers[stage]
        attachment.startOfEncoderSampleIndex = 0
        attachment.endOfEncoderSampleIndex = 1
        return command.makeComputeCommandEncoder(descriptor: pass)
    }

    /// Call only after the batch completes, before reusing its sample buffers.
    func milliseconds(stage: Int) -> Double? {
        guard let data = try? buffers[stage].resolveCounterRange(0..<2), data.count == 16 else { return nil }
        return data.withUnsafeBytes { bytes in
            let start = bytes.loadUnaligned(fromByteOffset: 0, as: UInt64.self)
            let end = bytes.loadUnaligned(fromByteOffset: 8, as: UInt64.self)
            guard start != MTLCounterErrorValue, end != MTLCounterErrorValue, end >= start else { return nil }
            return Double(end - start) * millisecondsPerTick
        }
    }
}
