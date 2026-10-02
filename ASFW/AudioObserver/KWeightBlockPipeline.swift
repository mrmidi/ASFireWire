import Metal

/// ASFW adaptation of zero-state response + homogeneous state correction:
/// H. Zhai and B.-P. Paris, "Parallel Cascaded Recursive Filtering on Multi-Core
/// CPUs and GPUs", arXiv:2607.23763v1 (2026), Sec. II-F, Eqs. (24)-(25).
/// https://arxiv.org/abs/2607.23763v1
/// Contiguous 32-frame blocks and a bounded scan replace the paper's intra-group
/// PH/CR solvers and CUDA decoupled lookback. This is not their full algorithm.
nonisolated final class KWeightBlockPipeline: @unchecked Sendable {
    static let blockFrames = 32
    private let local: MTLComputePipelineState
    private let scan: MTLComputePipelineState
    private let correct: MTLComputePipelineState
    private let reduce: MTLComputePipelineState
    let chunks: MTLBuffer
    private let powers: MTLBuffer
    let shelf: MTLBuffer
    let filtered: MTLBuffer
    private let carries: MTLBuffer
    private let initialStates: MTLBuffer

    init(device: MTLDevice, library: MTLLibrary) throws {
        func pipeline(_ name: String) throws -> MTLComputePipelineState {
            guard let function = library.makeFunction(name: name) else { throw AudioObserverError.pipelineFailed }
            return try device.makeComputePipelineState(function: function)
        }
        local = try pipeline("asfwKWeightBlockLocal")
        scan = try pipeline("asfwKWeightBlockScan")
        correct = try pipeline("asfwKWeightBlockCorrect")
        reduce = try pipeline("asfwKWeightChunkReduce")
        guard let chunkBuffer = device.makeBuffer(length: 13 * 32, options: .storageModePrivate) else {
            throw AudioObserverError.pipelineFailed
        }
        chunks = chunkBuffer
        let maximum = Int(AudioAnalysisLayout.maximumBatchFrames)
        let blockCount = (maximum + Self.blockFrames - 1) / Self.blockFrames
        guard let s = device.makeBuffer(length: maximum * 8, options: .storageModePrivate),
              let f = device.makeBuffer(length: maximum * 8, options: .storageModePrivate),
              let c = device.makeBuffer(length: blockCount * 16, options: .storageModePrivate),
              let initial = device.makeBuffer(length: blockCount * 16, options: .storageModePrivate) else {
            throw AudioObserverError.pipelineFailed
        }
        shelf = s; filtered = f; carries = c; initialStates = initial
        // Use exactly the existing Float coefficients; powers are calculated in Double
        // so the near-unit high-pass poles do not accumulate setup-rounding error.
        var table: [SIMD4<Float>] = []
        for (a, b) in [(Float(1.6906592932), Float(-0.7324807742)),
                       (Float(1.9900474548), Float(-0.9900722504))] {
            var m = SIMD4<Double>(1, 0, 0, 1)
            for _ in 0...maximum {
                table.append(SIMD4(Float(m.x), Float(m.y), Float(m.z), Float(m.w)))
                // State is (y, delta-y), avoiding cancellation of near-equal
                // y[n-1]/y[n-2] in the high-pass homogeneous correction.
                let sum = Double(a) + Double(b)
                m = SIMD4(sum * m.x - Double(b) * m.z,
                          sum * m.y - Double(b) * m.w,
                          (sum - 1) * m.x - Double(b) * m.z,
                          (sum - 1) * m.y - Double(b) * m.w)
            }
        }
        guard let buffer = table.withUnsafeBytes({ device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared) }) else {
            throw AudioObserverError.pipelineFailed
        }
        powers = buffer
    }

    func encode(command: MTLCommandBuffer, ring: MTLBuffer, state: MTLBuffer,
                truePeaks: MTLBuffer, params: UnsafeRawPointer, paramsLength: Int, frames: Int,
                timing: AnalyzerKernelTiming? = nil) throws {
        guard frames > 0, frames <= Int(AudioAnalysisLayout.maximumBatchFrames) else {
            throw AudioObserverError.pipelineFailed
        }
        let blocks = (frames + Self.blockFrames - 1) / Self.blockFrames
        for stage in UInt32(0)...1 {
            let source = stage == 0 ? ring : shelf
            let destination = stage == 0 ? shelf : filtered
            func encoder(_ pipeline: MTLComputePipelineState, first: Bool = false) throws -> MTLComputeCommandEncoder {
                let encoder: MTLComputeCommandEncoder?
                if first, let timing { encoder = timing.encoder(command, stage: 2, start: true, end: false) }
                else { encoder = command.makeComputeCommandEncoder() }
                guard let encoder else { throw AudioObserverError.pipelineFailed }
                encoder.setComputePipelineState(pipeline)
                encoder.setBuffer(source, offset: 0, index: 0)
                encoder.setBuffer(state, offset: 0, index: 1)
                encoder.setBuffer(destination, offset: 0, index: 2)
                encoder.setBuffer(carries, offset: 0, index: 3)
                encoder.setBuffer(initialStates, offset: 0, index: 4)
                encoder.setBuffer(powers, offset: 0, index: 5)
                encoder.setBytes(params, length: paramsLength, index: 6)
                var stage = stage
                encoder.setBytes(&stage, length: 4, index: 7)
                return encoder
            }
            let first = try encoder(local, first: stage == 0)
            first.dispatchThreads(MTLSize(width: blocks, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 32, height: 1, depth: 1))
            first.endEncoding()
            let second = try encoder(scan)
            second.dispatchThreads(MTLSize(width: 256, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
            second.endEncoding()
            let third = try encoder(correct)
            third.dispatchThreads(MTLSize(width: frames, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
            third.endEncoding()
        }
        guard let summary = command.makeComputeCommandEncoder() else { throw AudioObserverError.pipelineFailed }
        summary.setComputePipelineState(reduce)
        summary.setBuffer(ring, offset: 0, index: 0)
        summary.setBuffer(state, offset: 0, index: 1)
        summary.setBuffer(filtered, offset: 0, index: 2)
        summary.setBuffer(truePeaks, offset: 0, index: 3)
        summary.setBuffer(chunks, offset: 0, index: 4)
        summary.setBytes(params, length: paramsLength, index: 5)
        summary.dispatchThreadgroups(MTLSize(width: 13, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
        summary.endEncoding()
    }
}
