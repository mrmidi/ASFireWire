import Foundation
import Metal
import Testing
@testable import ASFW

struct WaterfallTests {
    @Test func fixedCameraKeepsAxesInFrame() {
        for frequency in [0.0, 0.5, 1] {
            for age in [0.0, 1] {
                for level in [0.0, 1] {
                    let p = WaterfallProjection.point(frequency: frequency, age: age, level: level)
                    #expect(abs(p.x) < 1 && abs(p.y) < 1)
                }
            }
        }
        let front = WaterfallProjection.point(frequency: 0.5, age: 0)
        let back = WaterfallProjection.point(frequency: 0.5, age: 1)
        #expect(front.y < back.y)
        #expect(SpectrumVisualization.waterfall.usesHistory)
        #expect(!SpectrumVisualization.spectrum.usesHistory)
    }

    @Test func ridgesProjectFadeAndSkipMissingHistory() throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let library = try #require(device.makeDefaultLibrary())
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = try #require(library.makeFunction(name: "asfwWaterfallVertex"))
        descriptor.fragmentFunction = try #require(library.makeFunction(name: "asfwWaterfallFragment"))
        let attachment = descriptor.colorAttachments[0]!
        attachment.pixelFormat = .rgba8Unorm
        attachment.isBlendingEnabled = false
        attachment.sourceRGBBlendFactor = .sourceAlpha
        attachment.destinationRGBBlendFactor = .oneMinusSourceAlpha
        attachment.sourceAlphaBlendFactor = .one
        attachment.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        descriptor.depthAttachmentPixelFormat = .depth32Float
        let pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
        descriptor.vertexFunction = try #require(library.makeFunction(name: "asfwWaterfallCurtainVertex"))
        let curtainPipeline = try device.makeRenderPipelineState(descriptor: descriptor)
        let depthDescriptor = MTLDepthStencilDescriptor()
        depthDescriptor.depthCompareFunction = .lessEqual
        depthDescriptor.isDepthWriteEnabled = true
        let depthWrite = try #require(device.makeDepthStencilState(descriptor: depthDescriptor))
        depthDescriptor.isDepthWriteEnabled = false
        let depthRead = try #require(device.makeDepthStencilState(descriptor: depthDescriptor))
        let queue = try #require(device.makeCommandQueue())
        let historyDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float, width: 1024, height: 513, mipmapped: false)
        historyDescriptor.storageMode = .shared; historyDescriptor.usage = .shaderRead
        let history = try #require(device.makeTexture(descriptor: historyDescriptor))
        var bins = [Float](repeating: -100, count: 1024 * 513)
        // A bin-centred 1.5 kHz tone, on the newest and oldest ridge only.
        bins[32 * 1024] = 0
        bins[32 * 1024 + 1] = 0
        bins.withUnsafeBytes {
            history.replace(region: MTLRegionMake2D(0, 0, 1024, 513), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 1024 * 4)
        }
        let stamps = try #require(device.makeBuffer(length: 1024 * 8, options: .storageModeShared))
        let imageDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 256, height: 256, mipmapped: false)
        imageDescriptor.storageMode = .shared; imageDescriptor.usage = .renderTarget
        let image = try #require(device.makeTexture(descriptor: imageDescriptor))
        let depthTextureDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float, width: 256, height: 256, mipmapped: false)
        depthTextureDescriptor.storageMode = .private; depthTextureDescriptor.usage = .renderTarget
        let depthTexture = try #require(device.makeTexture(descriptor: depthTextureDescriptor))
        func render(column: Int?, stamp: UInt64 = 0, secondColumn: Int? = nil, secondStamp: UInt64 = 0, curtains: Bool = true) throws -> [UInt8] {
            memset(stamps.contents(), 0, stamps.length)
            if let column { stamps.contents().assumingMemoryBound(to: UInt64.self)[column] = stamp }
            if let secondColumn { stamps.contents().assumingMemoryBound(to: UInt64.self)[secondColumn] = secondStamp }
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = image
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].loadAction = .clear; pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depthTexture
            pass.depthAttachment.loadAction = .clear; pass.depthAttachment.clearDepth = 1
            let command = try #require(queue.makeCommandBuffer())
            let encoder = try #require(command.makeRenderCommandEncoder(descriptor: pass))
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexTexture(history, index: 0)
            encoder.setVertexBuffer(stamps, offset: 0, index: 0)
            let params: [UInt32] = [2048, 0, 1024, 1024, 48000, 256]
            params.withUnsafeBytes { encoder.setVertexBytes($0.baseAddress!, length: $0.count, index: 1) }
            var camera = WaterfallProjection.uniforms
            encoder.setVertexBytes(&camera, length: MemoryLayout<WaterfallCameraUniforms>.stride, index: 2)
            if curtains {
                encoder.setDepthStencilState(depthWrite)
                encoder.setRenderPipelineState(curtainPipeline)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 512, instanceCount: WaterfallProjection.ridgeCount)
            }
            encoder.setRenderPipelineState(pipeline)
            encoder.setDepthStencilState(depthRead)
            encoder.setDepthBias(-0.00001, slopeScale: 0, clamp: 0)
            encoder.drawPrimitives(type: .lineStrip, vertexStart: 0, vertexCount: 256, instanceCount: WaterfallProjection.ridgeCount)
            encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
            #expect(command.status == .completed)
            var pixels = [UInt8](repeating: 0, count: 256 * 256 * 4)
            pixels.withUnsafeMutableBytes {
                image.getBytes($0.baseAddress!, bytesPerRow: 256 * 4, from: MTLRegionMake2D(0, 0, 256, 256), mipmapLevel: 0)
            }
            return pixels
        }
        let missing = try render(column: nil)
        #expect(missing.allSatisfy { $0 == 0 })
        let front = try render(column: 0, stamp: 2049)
        let back = try render(column: 1, stamp: 1026)
        let frontRed = stride(from: 0, to: front.count, by: 4).map { front[$0] }.max() ?? 0
        let backRed = stride(from: 0, to: back.count, by: 4).map { back[$0] }.max() ?? 0
        #expect(frontRed > 150)
        #expect(backRed > 20 && Int(backRed) * 4 < Int(frontRed) * 3)
        // A stale ring slot must never become a ridge for the current time range.
        let stale = try render(column: 0, stamp: 1025)
        #expect(stale.allSatisfy { $0 == 0 })

        // A nearby, quieter cyan ridge is hidden by the taller red foreground curtain.
        let olderAge = 1023 / (WaterfallProjection.ridgeCount - 1)
        let olderSlice = 2048 - olderAge
        let olderColumn = olderSlice % 1024
        for bin in 0..<513 {
            bins[bin * 1024] = 0
            bins[bin * 1024 + olderColumn] = -40
        }
        bins.withUnsafeBytes {
            history.replace(region: MTLRegionMake2D(0, 0, 1024, 513), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 1024 * 4)
        }
        let transparent = try render(column: 0, stamp: 2049, secondColumn: olderColumn,
                                     secondStamp: UInt64(olderSlice + 1), curtains: false)
        let occluded = try render(column: 0, stamp: 2049, secondColumn: olderColumn,
                                  secondStamp: UInt64(olderSlice + 1))
        let visibleBefore = stride(from: 1, to: transparent.count, by: 4).filter { transparent[$0] > 100 }.count
        let visibleAfter = stride(from: 1, to: occluded.count, by: 4).filter { occluded[$0] > 100 }.count
        #expect(visibleBefore > 20)
        #expect(visibleAfter < visibleBefore / 4)

    }
}
