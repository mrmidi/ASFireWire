import Foundation
import Metal
import Testing
@testable import ASFW

struct SpectrumKernelTests {
    @Test(arguments: [UInt32(1024), 2048, 4096], [UInt32(0), 1, 2])
    func stereoPowerPreservesOppositePhaseAndWindowCalibration(size: UInt32, window: UInt32) throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let library = try #require(device.makeDefaultLibrary())
        let function = try #require(library.makeFunction(name: "asfwSpectrumFFT"))
        let pipeline = try device.makeComputePipelineState(function: function)
        let queue = try #require(device.makeCommandQueue())
        let samples: [Float] = (0..<Int(size)).flatMap { frame in
            let sample = Float(0.5 * sin(2 * Double.pi * 32 * Double(frame) / Double(size)))
            return [sample, -sample]
        }
        let ring = try #require(samples.withUnsafeBytes {
            device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
        })
        let output = try #require(device.makeBuffer(length: 2049 * 4, options: .storageModeShared))
        for transform in [UInt32(3), 1, 2] {
            let params: [UInt32] = [size, 0, size, 2, 0, 48000, 1, transform, size, window]
            let command = try #require(queue.makeCommandBuffer())
            let encoder = try #require(command.makeComputeCommandEncoder())
            encoder.setComputePipelineState(pipeline)
            encoder.setBuffer(ring, offset: 0, index: 0)
            encoder.setBuffer(output, offset: 0, index: 1)
            params.withUnsafeBytes { encoder.setBytes($0.baseAddress!, length: $0.count, index: 2) }
            encoder.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1),
                                         threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
            encoder.endEncoding()
            command.commit()
            command.waitUntilCompleted()
            #expect(command.status == .completed)
            let amplitude = output.contents().assumingMemoryBound(to: Float.self)[32]
            let expected: Float = transform == 3 ? 0.5 : transform == 1 ? 0 : sqrt(0.5)
            #expect(abs(amplitude - expected) < 0.0001)
        }
    }
}
