import Foundation
import Metal
import Testing
@testable import ASFW

struct SpectrogramTests {
    @Test func timeScaleAndCatchUp() {
        #expect(SpectrogramTimeline.hop(sampleRate: 48000) == 512)
        #expect(SpectrogramTimeline.hop(sampleRate: 192000) == 2048)
        #expect(abs(SpectrogramTimeline.duration(sampleRate: 48000) - SpectrogramTimeline.duration(sampleRate: 192000)) < 0.0001)
        var clock = SpectrogramTimeline()
        #expect(clock.append(writeEnd: 10240, oldest: 0, fftSize: 2048, sampleRate: 48000) == 20...20)
        #expect(clock.append(writeEnd: 11776, oldest: 0, fftSize: 2048, sampleRate: 48000) == 21...23)
        #expect(clock.append(writeEnd: 11776, oldest: 0, fftSize: 2048, sampleRate: 48000) == nil)
        let catchUp = clock.append(writeEnd: 51200, oldest: 43000, fftSize: 2048, sampleRate: 48000)
        #expect(catchUp == 92...100)
        #expect(clock.append(writeEnd: 52000, oldest: 51000, fftSize: 4096, sampleRate: 48000) == nil)
    }

    @Test(arguments: [UInt32(1024), 8192])
    func batchedSTFTWrapsColumnsAndPreservesStereoPower(size: UInt32) throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let library = try #require(device.makeDefaultLibrary())
        let function = try #require(library.makeFunction(name: "asfwSpectrogramSTFT"))
        let pipeline = try device.makeComputePipelineState(function: function)
        let queue = try #require(device.makeCommandQueue())
        let samples: [Float] = (0..<12288).flatMap { frame in
            let sample = Float(0.5 * sin(2 * Double.pi * 32 * Double(frame) / Double(size)))
            return [sample, -sample]
        }
        let ring = try #require(samples.withUnsafeBytes {
            device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
        })
        let scratch = try #require(device.makeBuffer(length: 3 * SpectrumFFTLayout.binCount(size) * 4, options: .storageModePrivate))
        let stamps = try #require(device.makeBuffer(length: 8 * 8, options: .storageModeShared))
        memset(stamps.contents(), 0, stamps.length)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float, width: 8, height: SpectrumFFTLayout.binCount(size), mipmapped: false)
        descriptor.storageMode = .shared
        descriptor.usage = [.shaderWrite, .shaderRead]
        let texture = try #require(device.makeTexture(descriptor: descriptor))
        // SpectrumParams (48 bytes: 10 fields + calibrationOffsetDB + padding), firstSlice (8), hop and columns (8).
        let params: [UInt32] = [0, 0, 12288, 2, 0, 48000, 1, 3, size, 0, 0, 0, 32, 0, 512, 8]
        let command = try #require(queue.makeCommandBuffer())
        let encoder = try #require(command.makeComputeCommandEncoder())
        encoder.setComputePipelineState(pipeline)
        encoder.setThreadgroupMemoryLength(SpectrumFFTLayout.scratchBytes(size), index: 0)
        encoder.setBuffer(scratch, offset: 0, index: 3)
        encoder.setBuffer(ring, offset: 0, index: 0)
        encoder.setBuffer(stamps, offset: 0, index: 1)
        encoder.setTexture(texture, index: 0)
        params.withUnsafeBytes { encoder.setBytes($0.baseAddress!, length: $0.count, index: 2) }
        encoder.dispatchThreadgroups(MTLSize(width: 3, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
        encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
        #expect(command.status == .completed)
        let clock = stamps.contents().assumingMemoryBound(to: UInt64.self)
        #expect(clock[0] == 33 && clock[1] == 34 && clock[2] == 35 && clock[3] == 0)
        var bins = [Float](repeating: 0, count: 8)
        bins.withUnsafeMutableBytes {
            texture.getBytes($0.baseAddress!, bytesPerRow: 8 * 4, from: MTLRegionMake2D(0, 32, 8, 1), mipmapLevel: 0)
        }
        for column in 0..<3 { #expect(abs(bins[column] + 6.0206) < 0.001) }

        let renderDescriptor = MTLRenderPipelineDescriptor()
        renderDescriptor.vertexFunction = try #require(library.makeFunction(name: "asfwSpectrogramVertex"))
        renderDescriptor.fragmentFunction = try #require(library.makeFunction(name: "asfwSpectrogramFragment"))
        renderDescriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        let renderPipeline = try device.makeRenderPipelineState(descriptor: renderDescriptor)
        let imageDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 64, height: 64, mipmapped: false)
        imageDescriptor.storageMode = .shared; imageDescriptor.usage = .renderTarget
        let image = try #require(device.makeTexture(descriptor: imageDescriptor))
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = image
        pass.colorAttachments[0].loadAction = .clear; pass.colorAttachments[0].storeAction = .store
        let renderCommand = try #require(queue.makeCommandBuffer())
        let drawing = try #require(renderCommand.makeRenderCommandEncoder(descriptor: pass))
        drawing.setRenderPipelineState(renderPipeline)
        drawing.setFragmentTexture(texture, index: 0)
        drawing.setFragmentBuffer(stamps, offset: 0, index: 0)
        let display: [UInt32] = [34, 0, 8, size, 48000, 64]
        display.withUnsafeBytes { drawing.setFragmentBytes($0.baseAddress!, length: $0.count, index: 1) }
        drawing.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        drawing.endEncoding(); renderCommand.commit(); renderCommand.waitUntilCompleted()
        #expect(renderCommand.status == .completed)
        var pixels = [UInt8](repeating: 0, count: 64 * 64 * 4)
        pixels.withUnsafeMutableBytes {
            image.getBytes($0.baseAddress!, bytesPerRow: 64 * 4, from: MTLRegionMake2D(0, 0, 64, 64), mipmapLevel: 0)
        }
        let hz = 32.0 * 48000 / Double(size)
        let row = Int((1 - log(hz / 20) / log(20000.0 / 20)) * 64)
        #expect(pixels[(row * 64) * 4 + 1] < 15) // unfilled past
        #expect((max(0, row - 1)...min(63, row + 1)).map { pixels[($0 * 64 + 63) * 4] }.max()! > 200) // latest tone on the right
        #expect(pixels[(63 * 64 + 63) * 4 + 1] < 15) // low frequencies remain dark

    }
}
