import Foundation
import Metal
import Testing
@testable import ASFW

struct SpectrumKernelTests {
    @Test(arguments: [UInt32(1024), 2048, 4096, 8192], [UInt32(0), 1, 2])
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
        let output = try #require(device.makeBuffer(length: SpectrumFFTLayout.binCount(size) * 4, options: .storageModeShared))
        for transform in [UInt32(3), 1, 2] {
            let params: [UInt32] = [size, 0, size, 2, 0, 48000, 1, transform, size, window]
            let command = try #require(queue.makeCommandBuffer())
            let encoder = try #require(command.makeComputeCommandEncoder())
            encoder.setComputePipelineState(pipeline)
            encoder.setThreadgroupMemoryLength(SpectrumFFTLayout.scratchBytes(size), index: 0)
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
    @Test(arguments: [UInt32(1024), 8192], [UInt32(0), 1, 2])
    func realPackingMatchesIndependentDFTWithDCNyquistAndRingWrap(size: UInt32, window: UInt32) throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let library = try #require(device.makeDefaultLibrary())
        let pipeline = try device.makeComputePipelineState(function: #require(library.makeFunction(name: "asfwSpectrumFFT")))
        let smooth = try device.makeComputePipelineState(function: #require(library.makeFunction(name: "asfwSpectrumSmooth")))
        let bytes = SpectrumFFTLayout.scratchBytes(size)
        #expect(bytes + pipeline.staticThreadgroupMemoryLength <= device.maxThreadgroupMemoryLength)
        let queue = try #require(device.makeCommandQueue())
        let count = Int(size), capacity = count + 257, start = capacity - 123
        var seed: UInt32 = 1234
        var left: [Float] = [], right: [Float] = []
        var physical = [Float](repeating: 0, count: capacity * 2)
        for i in 0..<count {
            seed = seed &* 1664525 &+ 1013904223
            let noise = Float(Int32(bitPattern: seed)) / Float(Int32.max) * 0.08
            let l = Float(0.2 + 0.12 * cos(Double.pi * Double(i))) + noise
            let r = Float(-0.07 + 0.23 * sin(2 * Double.pi * 17.25 * Double(i) / Double(size))) - noise
            left.append(l); right.append(r)
            physical[((start + i) % capacity) * 2] = l
            physical[((start + i) % capacity) * 2 + 1] = r
        }
        let ring = try #require(physical.withUnsafeBytes { device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared) })
        let bins = SpectrumFFTLayout.binCount(size)
        let output = try #require(device.makeBuffer(length: bins * 4, options: .storageModeShared))
        let history = try #require(device.makeBuffer(length: bins * 16, options: .storageModePrivate))
        let average = try #require(device.makeBuffer(length: bins * 4, options: .storageModeShared))
        let peak = try #require(device.makeBuffer(length: bins * 4, options: .storageModeShared))
        for transform in UInt32(0)...3 {
            let params: [UInt32] = [UInt32(start + count), 0, UInt32(capacity), 2, 0, 48000, 1, transform, size, window]
            let command = try #require(queue.makeCommandBuffer())
            let encoder = try #require(command.makeComputeCommandEncoder())
            encoder.setComputePipelineState(pipeline)
            encoder.setThreadgroupMemoryLength(bytes, index: 0)
            encoder.setBuffer(ring, offset: 0, index: 0); encoder.setBuffer(output, offset: 0, index: 1)
            params.withUnsafeBytes { encoder.setBytes($0.baseAddress!, length: $0.count, index: 2) }
            encoder.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
            encoder.endEncoding()
            let filtering = try #require(command.makeComputeCommandEncoder())
            filtering.setComputePipelineState(smooth)
            filtering.setBuffer(output, offset: 0, index: 0); filtering.setBuffer(history, offset: 0, index: 1)
            filtering.setBuffer(average, offset: 0, index: 2); filtering.setBuffer(peak, offset: 0, index: 3)
            let smoothing: [UInt32] = [Float(0.5).bitPattern, Float(0.04).bitPattern, 1, UInt32(bins)]
            smoothing.withUnsafeBytes { filtering.setBytes($0.baseAddress!, length: $0.count, index: 4) }
            filtering.dispatchThreads(MTLSize(width: bins, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
            filtering.endEncoding(); command.commit(); command.waitUntilCompleted()
            try #require(command.status == .completed)
            let result = output.contents().assumingMemoryBound(to: Float.self)
            let smoothed = average.contents().assumingMemoryBound(to: Float.self)
            let peaks = peak.contents().assumingMemoryBound(to: Float.self)
            for bin in 0..<bins {
                #expect(result[bin].isFinite)
                #expect(abs(smoothed[bin] - result[bin]) < 1e-6)
                #expect(abs(peaks[bin] - result[bin]) < 1e-6)
            }
            let gain = window == 1 ? 0.54 : window == 2 ? 0.42 : 0.5
            for bin in [0, 1, 17, 18, count / 4, count / 2 - 1, count / 2] {
                var lr = 0.0, li = 0.0, rr = 0.0, ri = 0.0
                for i in 0..<count {
                    let angle = 2 * Double.pi * Double(i) / Double(count)
                    let w = window == 1 ? 0.54 - 0.46 * cos(angle)
                        : window == 2 ? 0.42 - 0.5 * cos(angle) + 0.08 * cos(2 * angle)
                        : 0.5 - 0.5 * cos(angle)
                    let l: Double, r: Double
                    if transform == 1 { l = Double((left[i] + right[i]) * Float(0.70710678118)); r = 0 }
                    else if transform == 2 { l = Double((left[i] - right[i]) * Float(0.70710678118)); r = 0 }
                    else { l = Double(left[i]); r = Double(right[i]) }
                    let phase = -angle * Double(bin)
                    lr += l * w * cos(phase); li += l * w * sin(phase)
                    rr += r * w * cos(phase); ri += r * w * sin(phase)
                }
                let scale = (bin == 0 || bin == count / 2 ? 1.0 : 2.0) / (Double(count) * gain)
                let expected = transform == 3 ? sqrt((lr * lr + li * li + rr * rr + ri * ri) * 0.5) * scale : hypot(lr, li) * scale
                #expect(abs(Double(result[bin]) - expected) < 0.0001, "bin \(bin), transform \(transform)")
            }
        }
    }

}
