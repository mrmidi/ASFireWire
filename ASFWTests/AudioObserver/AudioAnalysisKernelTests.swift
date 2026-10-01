import Foundation
import Metal
import Testing

private struct ConsumeRangeTestParams {
    var startFrame: UInt64
    var frameCount: UInt32
    var ringFrames: UInt32
    var channels: UInt32
    var leftChannel: UInt32
    var rightChannel: UInt32
}

private func makeLoudnessTestParams(startFrame: UInt64, frameCount: UInt32,
                                    ringFrames: UInt32, channels: UInt32) -> ConsumeRangeTestParams {
    ConsumeRangeTestParams(startFrame: startFrame, frameCount: frameCount,
                           ringFrames: ringFrames, channels: channels,
                           leftChannel: 0, rightChannel: 1)
}

struct AudioAnalysisKernelTests {
    @Test func reductionReadsSelectedInterleavedChannelsAcrossRingWrap() throws {
        let device = try #require(MTLCreateSystemDefaultDevice(), "Metal device is required")
        let library = try #require(device.makeDefaultLibrary(), "ASFW default Metal library is required")
        let function = try #require(library.makeFunction(name: "asfwConsumeOutputRange"))
        let pipeline = try device.makeComputePipelineState(function: function)

        // Three interleaved channels; channel 1 is intentionally irrelevant.
        let samples: [Float] = [
            .nan, 99.0, 0.0,    // frame 0: invalid left sample is reported and excluded
            0.5, 99.0, 0.5,    // frame 1: in phase
            0.5, 99.0, -0.5,   // frame 2: opposite phase
            1.2, 99.0, 1.2     // frame 3: over range
        ]
        let source = try #require(device.makeBuffer(bytes: samples,
                                                    length: samples.count * MemoryLayout<Float>.stride,
                                                    options: .storageModeShared))
        let output = try #require(device.makeBuffer(length: 16 * MemoryLayout<UInt32>.stride,
                                                    options: .storageModeShared))
        let queue = try #require(device.makeCommandQueue())
        let command = try #require(queue.makeCommandBuffer())
        let encoder = try #require(command.makeComputeCommandEncoder())
        var params = ConsumeRangeTestParams(startFrame: 3,
                                            frameCount: 4,
                                            ringFrames: 4,
                                            channels: 3,
                                            leftChannel: 0,
                                            rightChannel: 2)
        encoder.setComputePipelineState(pipeline)
        encoder.setBuffer(source, offset: 0, index: 0)
        encoder.setBuffer(output, offset: 0, index: 1)
        encoder.setBytes(&params, length: MemoryLayout<ConsumeRangeTestParams>.stride, index: 2)
        encoder.dispatchThreads(MTLSize(width: 256, height: 1, depth: 1),
                                threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
        encoder.endEncoding()
        command.commit()
        command.waitUntilCompleted()
        try #require(command.status == .completed, "GPU reduction command must complete")

        let values = output.contents().assumingMemoryBound(to: UInt32.self)
        func float(_ index: Int) -> Float { Float(bitPattern: values[index]) }
        #expect(abs(float(0) - 1.2) < 1e-6)
        #expect(abs(float(1) - 1.2) < 1e-6)
        #expect(abs(float(2) - Float(1.44 / 1.94)) < 1e-5)
        #expect(values[3] == 4)
        #expect(abs(float(6) - sqrt(Float(1.94 / 4))) < 1e-5)
        #expect(abs(float(7) - sqrt(Float(1.94 / 4))) < 1e-5)
        #expect(abs(float(11) - Float(0.5 / 3.88)) < 1e-5)
        #expect(values[12] == 1)
        #expect(values[13] == 1)
        #expect(values[14] == 1)
        #expect(abs(float(15) - Float(10 * log10(3.38 / 3.88))) < 1e-4)
        #expect(values[90] == 1)
        #expect(values[91] == 0)
    }

    @Test func kWeightingProducesTheExpectedStereoOneKilohertzLoudness() throws {
        let device = try #require(MTLCreateSystemDefaultDevice(), "Metal device is required")
        let library = try #require(device.makeDefaultLibrary(), "ASFW default Metal library is required")
        let function = try #require(library.makeFunction(name: "asfwKWeightRange"))
        let pipeline = try device.makeComputePipelineState(function: function)
        let sampleCount = 480
        let peak = pow(10.0, Float(-23) / 20)
        let samples = (0..<sampleCount).flatMap { frame -> [Float] in
            let sample = peak * sin(2 * Float.pi * 1_000 * Float(frame) / 48_000)
            return [sample, sample]
        }
        let source = try #require(device.makeBuffer(bytes: samples,
                                                    length: samples.count * MemoryLayout<Float>.stride,
                                                    options: .storageModeShared))
        let state = try #require(device.makeBuffer(length: 20 * MemoryLayout<UInt32>.stride,
                                                   options: .storageModeShared))
        state.contents().initializeMemory(as: UInt8.self, repeating: 0, count: state.length)
        let output = try #require(device.makeBuffer(length: 96 * MemoryLayout<UInt32>.stride,
                                                    options: .storageModeShared))
        let queue = try #require(device.makeCommandQueue())
        let command = try #require(queue.makeCommandBuffer())
        let encoder = try #require(command.makeComputeCommandEncoder())
        var params = makeLoudnessTestParams(startFrame: 0, frameCount: UInt32(sampleCount),
                                            ringFrames: UInt32(sampleCount), channels: 2)
        encoder.setComputePipelineState(pipeline)
        encoder.setBuffer(source, offset: 0, index: 0)
        encoder.setBuffer(state, offset: 0, index: 1)
        encoder.setBuffer(output, offset: 0, index: 2)
        encoder.setBytes(&params, length: MemoryLayout<ConsumeRangeTestParams>.stride, index: 3)
        encoder.dispatchThreads(MTLSize(width: 1, height: 1, depth: 1),
                                threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
        encoder.endEncoding()
        command.commit()
        command.waitUntilCompleted()
        try #require(command.status == .completed, "K-weighting command must complete")

        let values = output.contents().assumingMemoryBound(to: UInt32.self)
        #expect(values[16] == 1)
        #expect(values[17] == UInt32(sampleCount))
        let energyLeft = Float(bitPattern: values[19])
        let energyRight = Float(bitPattern: values[20])
        let lufs = -0.691 + 10 * log10((energyLeft + energyRight) / Float(sampleCount))
        #expect(abs(lufs + 23) < 0.7)
        #expect(state.contents().assumingMemoryBound(to: UInt32.self)[18] == 0)
    }
}
