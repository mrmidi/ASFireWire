import Foundation
import Metal
import Testing
@testable import ASFW

struct WaterfallTests {
    @Test func frontSpectrumAxesAndHistoryAreUnambiguous() {
        for frequency in [0.0, 0.5, 1] {
            for age in [0.0, 1] {
                for level in [0.0, 1] {
                    let p = WaterfallProjection.point(frequency: frequency, age: age, level: level)
                    #expect(abs(p.x) < 1 && abs(p.y) < 1)
                }
            }
        }
        let left = WaterfallProjection.point(frequency: 0, age: 0)
        let right = WaterfallProjection.point(frequency: 1, age: 0)
        let top = WaterfallProjection.point(frequency: 0, age: 0, level: 1)
        let past = WaterfallProjection.point(frequency: 0, age: 1)
        #expect(left.y == right.y && left.x < right.x)
        #expect(abs(left.x - top.x) < 0.0001 && top.y - left.y > 0.8)
        #expect(past.x > left.x && past.y > left.y)
    }

    @Test func surfacePreservesPaletteAndDoesNotBridgeMissingSTFT() throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let library = try #require(device.makeDefaultLibrary())
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "asfwWaterfallSurfaceVertex")
        descriptor.fragmentFunction = library.makeFunction(name: "asfwWaterfallFragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.depthAttachmentPixelFormat = .depth32Float
        let pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
        let continuityFunction = try #require(library.makeFunction(name: "asfwWaterfallContinuity"))
        let continuityPipeline = try device.makeComputePipelineState(function: continuityFunction)
        let depthDescriptor = MTLDepthStencilDescriptor()
        depthDescriptor.depthCompareFunction = .lessEqual
        depthDescriptor.isDepthWriteEnabled = true
        let depth = try #require(device.makeDepthStencilState(descriptor: depthDescriptor))
        let queue = try #require(device.makeCommandQueue())
        let historyDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float, width: 1024, height: 513, mipmapped: false)
        historyDescriptor.storageMode = .shared; historyDescriptor.usage = .shaderRead
        let history = try #require(device.makeTexture(descriptor: historyDescriptor))
        let bins = [Float](repeating: -20, count: 1024 * 513)
        bins.withUnsafeBytes {
            history.replace(region: MTLRegionMake2D(0, 0, 1024, 513), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 1024 * 4)
        }
        let stamps = try #require(device.makeBuffer(length: 1024 * 8, options: .storageModeShared))
        let validity = try #require(device.makeBuffer(length: (WaterfallProjection.historyRows - 1) * 4, options: .storageModeShared))
        let imageDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 256, height: 256, mipmapped: false)
        imageDescriptor.storageMode = .shared; imageDescriptor.usage = .renderTarget
        let image = try #require(device.makeTexture(descriptor: imageDescriptor))
        let depthTextureDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float, width: 256, height: 256, mipmapped: false)
        depthTextureDescriptor.storageMode = .private; depthTextureDescriptor.usage = .renderTarget
        let depthTexture = try #require(device.makeTexture(descriptor: depthTextureDescriptor))
        func render(complete: Bool, missingAge: Int? = nil) throws -> [UInt8] {
            memset(stamps.contents(), 0, stamps.length)
            if complete {
                for age in 0..<1024 where age != missingAge {
                    let slice = 2048 - age
                    stamps.contents().assumingMemoryBound(to: UInt64.self)[slice % 1024] = UInt64(slice + 1)
                }
            }
            let command = try #require(queue.makeCommandBuffer())
            let compute = try #require(command.makeComputeCommandEncoder())
            let params: [UInt32] = [2048, 0, 1024, 1024, 48000, 256]
            var camera = WaterfallProjection.uniforms
            compute.setComputePipelineState(continuityPipeline)
            compute.setBuffer(stamps, offset: 0, index: 0)
            compute.setBuffer(validity, offset: 0, index: 1)
            params.withUnsafeBytes { compute.setBytes($0.baseAddress!, length: $0.count, index: 2) }
            compute.setBytes(&camera, length: MemoryLayout<WaterfallCameraUniforms>.stride, index: 3)
            compute.dispatchThreads(MTLSize(width: WaterfallProjection.historyRows - 1, height: 1, depth: 1),
                                    threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
            compute.endEncoding()
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = image
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].loadAction = .clear; pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depthTexture
            pass.depthAttachment.loadAction = .clear; pass.depthAttachment.clearDepth = 1
            let encoder = try #require(command.makeRenderCommandEncoder(descriptor: pass))
            encoder.setRenderPipelineState(pipeline)
            encoder.setDepthStencilState(depth)
            encoder.setVertexTexture(history, index: 0)
            encoder.setVertexBuffer(stamps, offset: 0, index: 0)
            params.withUnsafeBytes { encoder.setVertexBytes($0.baseAddress!, length: $0.count, index: 1) }
            encoder.setVertexBytes(&camera, length: MemoryLayout<WaterfallCameraUniforms>.stride, index: 2)
            encoder.setVertexBuffer(validity, offset: 0, index: 3)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 512, instanceCount: WaterfallProjection.historyRows - 1)
            encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
            #expect(command.status == .completed)
            var pixels = [UInt8](repeating: 0, count: 256 * 256 * 4)
            pixels.withUnsafeMutableBytes {
                image.getBytes($0.baseAddress!, bytesPerRow: 256 * 4, from: MTLRegionMake2D(0, 0, 256, 256), mipmapLevel: 0)
            }
            return pixels
        }
        let empty = try render(complete: false)
        #expect(empty.allSatisfy { $0 == 0 })
        let flat = try render(complete: true)
        let flags = validity.contents().assumingMemoryBound(to: UInt32.self)
        #expect((0..<64).allSatisfy { flags[$0] == 1 })
        func pixel(age: Double) -> [UInt8] {
            let p = WaterfallProjection.point(frequency: 0.5, age: age, level: 0.8)
            let x = Int((p.x + 1) * 128), y = Int((1 - p.y) * 128)
            return Array(flat[(y * 256 + x) * 4..<(y * 256 + x) * 4 + 4])
        }
        let near = pixel(age: 0.05), far = pixel(age: 0.90)
        #expect(near[3] == 255 && far[3] == 255)
        for component in 0..<3 { #expect(abs(Int(near[component]) - Int(far[component])) <= 1) }
        #expect(near[0] > 150 && near[1] > 150) // existing −20 dBFS yellow-green palette
        _ = try render(complete: true, missingAge: 7)
        #expect(flags[0] == 0 && flags[1] == 1) // endpoints survive; interior loss still breaks mesh
    }
}
