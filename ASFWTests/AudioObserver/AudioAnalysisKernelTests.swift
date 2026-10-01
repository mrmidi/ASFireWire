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

private func scalarTruePeak(_ interleaved: [Float], channel: Int) -> Float {
    let phases: [[Float]] = [
        [0.001708984375, 0.010986328125, -0.0196533203125, 0.033203125,
         -0.0594482421875, 0.1373291015625, 0.97216796875, -0.102294921875,
         0.047607421875, -0.026611328125, 0.014892578125, -0.00830078125],
        [-0.0291748046875, 0.029296875, -0.0517578125, 0.089111328125,
         -0.16650390625, 0.465087890625, 0.77978515625, -0.2003173828125,
         0.1015625, -0.0582275390625, 0.0330810546875, -0.0189208984375],
        [-0.0189208984375, 0.0330810546875, -0.0582275390625, 0.1015625,
         -0.2003173828125, 0.77978515625, 0.465087890625, -0.16650390625,
         0.089111328125, -0.0517578125, 0.029296875, -0.0291748046875],
        [-0.00830078125, 0.014892578125, -0.026611328125, 0.047607421875,
         -0.102294921875, 0.97216796875, 0.1373291015625, -0.0594482421875,
         0.033203125, -0.0196533203125, 0.010986328125, 0.001708984375]
    ]
    var history = [Float](repeating: 0, count: 12)
    var peak: Float = 0
    for frame in 0..<(interleaved.count / 2) {
        history.insert(interleaved[frame * 2 + channel], at: 0)
        history.removeLast()
        guard frame >= 11 else { continue }
        for phase in phases {
            let output = zip(phase, history).reduce(Float.zero) { $0 + $1.0 * $1.1 }
            peak = max(peak, abs(output))
        }
    }
    return peak
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
            let phase = 2 * Double.pi * 1_000 * Double(frame) / 48_000
            let sample = peak * Float(sin(phase))
            return [sample, sample]
        }
        let source = try #require(device.makeBuffer(bytes: samples,
                                                    length: samples.count * MemoryLayout<Float>.stride,
                                                    options: .storageModeShared))
        let state = try #require(device.makeBuffer(length: 45 * MemoryLayout<UInt32>.stride,
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
        #expect(values[94] == 1)
        #expect(abs(Float(bitPattern: values[92]) - scalarTruePeak(samples, channel: 0)) < 1e-5)
        #expect(abs(Float(bitPattern: values[93]) - scalarTruePeak(samples, channel: 1)) < 1e-5)
        #expect(state.contents().assumingMemoryBound(to: UInt32.self)[18] == 0)
    }

    @Test func truePeakDetectsInterSampleOvershootAcrossAnalysisRanges() throws {
        let device = try #require(MTLCreateSystemDefaultDevice(), "Metal device is required")
        let library = try #require(device.makeDefaultLibrary(), "ASFW default Metal library is required")
        let function = try #require(library.makeFunction(name: "asfwKWeightRange"))
        let pipeline = try device.makeComputePipelineState(function: function)
        let phase = 323 * Double.pi / 180
        let samples: [Float] = (0..<480).flatMap { frame in
            let signalPhase = 2 * Double.pi * 10_000 * Double(frame) / 48_000 + phase
            let value = 0.99 * Float(sin(signalPhase))
            return [value, value]
        }
        let source = try #require(device.makeBuffer(bytes: samples,
                                                    length: samples.count * MemoryLayout<Float>.stride,
                                                    options: .storageModeShared))
        let state = try #require(device.makeBuffer(length: 45 * MemoryLayout<UInt32>.stride,
                                                   options: .storageModeShared))
        state.contents().initializeMemory(as: UInt8.self, repeating: 0, count: state.length)
        let output = try #require(device.makeBuffer(length: 96 * MemoryLayout<UInt32>.stride,
                                                    options: .storageModeShared))
        let queue = try #require(device.makeCommandQueue())

        func runRange(start: UInt64, count: UInt32) throws -> Float {
            let command = try #require(queue.makeCommandBuffer())
            let encoder = try #require(command.makeComputeCommandEncoder())
            var params = makeLoudnessTestParams(startFrame: start, frameCount: count,
                                                ringFrames: 480, channels: 2)
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
            try #require(command.status == .completed, "True-peak range must complete")
            let words = output.contents().assumingMemoryBound(to: UInt32.self)
            #expect(words[94] == 1)
            return Float(bitPattern: words[92])
        }

        let firstPeak = try runRange(start: 0, count: 200)
        let secondPeak = try runRange(start: 200, count: 280)
        let expected = scalarTruePeak(samples, channel: 0)
        let samplePeak = stride(from: 0, to: samples.count, by: 2)
            .map { abs(samples[$0]) }.max() ?? 0

        #expect(abs(max(firstPeak, secondPeak) - expected) < 1e-5)
        // This phase of a 10 kHz sine produces a modest, but measurable,
        // inter-sample overshoot with the BS.1770 four-phase FIR.
        #expect(expected > samplePeak * 1.005)
    }
}
