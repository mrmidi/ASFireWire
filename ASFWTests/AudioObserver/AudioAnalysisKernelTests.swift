import Foundation
import Metal
import MetalKit
import SwiftUI
import Testing
@testable import ASFW

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
        let output = try #require(device.makeBuffer(length: AudioAnalysisLayout.outputWords * MemoryLayout<UInt32>.stride,
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

    @Test func delayedAnalysisIsSplitWithoutCorruptingHeadersOrLosingChunks() throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let library = try #require(device.makeDefaultLibrary())
        let reduction = try device.makeComputePipelineState(function: #require(library.makeFunction(name: "asfwConsumeOutputRange")))
        let weighting = try device.makeComputePipelineState(function: #require(library.makeFunction(name: "asfwKWeightRange")))
        let frames = 9_600 // 200 ms: previously overflowed the production result buffer.
        var samples = [Float](repeating: 0.25, count: frames * 2)
        samples[0] = .nan
        let source = try #require(device.makeBuffer(bytes: samples, length: samples.count * 4, options: .storageModeShared))
        let state = try #require(device.makeBuffer(length: 49 * 4, options: .storageModeShared))
        state.contents().initializeMemory(as: UInt8.self, repeating: 0, count: state.length)
        let output = try #require(device.makeBuffer(length: (AudioAnalysisLayout.outputWords + 8) * 4, options: .storageModeShared))
        let queue = try #require(device.makeCommandQueue())
        let words = output.contents().assumingMemoryBound(to: UInt32.self)
        var start: UInt64 = 0
        var totalChunks = 0
        while start < UInt64(frames) {
            let end = AudioAnalysisLayout.batchEnd(start: start, availableEnd: UInt64(frames))
            try #require(end > start)
            #expect(end - start <= AudioAnalysisLayout.maximumBatchFrames)
            for i in 0..<(AudioAnalysisLayout.outputWords + 8) { words[i] = 0xDEADBEEF }
            var params = makeLoudnessTestParams(startFrame: start, frameCount: UInt32(end - start),
                                                ringFrames: UInt32(frames), channels: 2)
            let command = try #require(queue.makeCommandBuffer())
            let first = try #require(command.makeComputeCommandEncoder())
            first.setComputePipelineState(reduction)
            first.setBuffer(source, offset: 0, index: 0)
            first.setBuffer(output, offset: 0, index: 1)
            first.setBytes(&params, length: MemoryLayout<ConsumeRangeTestParams>.stride, index: 2)
            first.dispatchThreads(MTLSize(width: 256, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
            first.endEncoding()
            let second = try #require(command.makeComputeCommandEncoder())
            second.setComputePipelineState(weighting)
            second.setBuffer(source, offset: 0, index: 0)
            second.setBuffer(state, offset: 0, index: 1)
            second.setBuffer(output, offset: 0, index: 2)
            second.setBytes(&params, length: MemoryLayout<ConsumeRangeTestParams>.stride, index: 3)
            second.dispatchThreads(MTLSize(width: 1, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
            second.endEncoding()
            command.commit()
            command.waitUntilCompleted()
            try #require(command.status == .completed)
            #expect(words[90] == (start == 0 ? 1 : 0))
            #expect(words[91] == 0)
            #expect(words[94] == 1)
            #expect(Float(bitPattern: words[92]).isFinite)
            try #require(words[16] == UInt32(AudioAnalysisLayout.chunkCapacity))
            for chunk in 0..<Int(words[16]) {
                let offset = AudioAnalysisLayout.chunkOffset + chunk * AudioAnalysisLayout.chunkWords
                let chunkEnd = UInt64(words[offset]) | UInt64(words[offset + 1]) << 32
                #expect(chunkEnd == start + UInt64((chunk + 1) * 480))
                #expect(Float(bitPattern: words[offset + 2]).isFinite)
                #expect(Float(bitPattern: words[offset + 4]) > 0)
                #expect(Float(bitPattern: words[offset + 5]) == 0.25)
                #expect(Float(bitPattern: words[offset + 6]).isFinite)
            }
            for i in AudioAnalysisLayout.outputWords..<(AudioAnalysisLayout.outputWords + 8) {
                #expect(words[i] == 0xDEADBEEF)
            }
            totalChunks += Int(words[16])
            start = end
        }
        #expect(totalChunks == 20)
        #expect(state.contents().assumingMemoryBound(to: UInt32.self)[18] == 0)
    }

    @MainActor
    @Test func gpuCompletionAttachesRendererWithoutASwiftUIUpdate() throws {
        let client = ASFWAudioObserverClient(guid: 0)
        let plot = MetalAnalyzerPlotView(client: client, mode: 0, index: 0)
        let coordinator = plot.makeCoordinator()
        let view = MTKView(frame: NSRect(x: 0, y: 0, width: 38, height: 200), device: nil)
        var availableDevice: MTLDevice?
        plot.configure(view, coordinator: coordinator, device: nil)
        plot.observeCompletions(view, coordinator: coordinator) { availableDevice }
        #expect(view.delegate == nil)
        availableDevice = try #require(MTLCreateSystemDefaultDevice())
        // No updateNSView call: the view's client/mode/index inputs are unchanged.
        NotificationCenter.default.post(name: .asfwAnalysisCompleted, object: client.renderState)
        try #require(view.delegate)
        #expect(view.device?.registryID == availableDevice?.registryID)
    }

    @MainActor
    @Test func monitorMetalViewsReceiveUsableSizesFromSwiftUILayout() throws {
        let client = ASFWAudioObserverClient(guid: 0)
        let host = NSHostingView(rootView: StereoMetersView(client: client,
            state: AnalyzerPanelUIState(section: .monitor), active: true))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 420, height: 300),
                              styleMask: .borderless, backing: .buffered, defer: false)
        window.contentView = host
        window.layoutIfNeeded()
        host.layoutSubtreeIfNeeded()
        func metalViews(_ view: NSView) -> [MTKView] {
            if let metal = view as? MTKView { return [metal] }
            return view.subviews.flatMap(metalViews)
        }
        let plots = metalViews(host)
        try #require(plots.count == 1)
        let device = try #require(MTLCreateSystemDefaultDevice())
        for plot in plots {
            #expect(plot.bounds.width > 0)
            #expect(plot.bounds.height > 0)
            let representable = MetalAnalyzerPlotView(client: client, mode: 0, index: 0)
            let coordinator = representable.makeCoordinator()
            representable.configure(plot, coordinator: coordinator, device: device)
            #expect(plot.drawableSize.width == plot.bounds.width * window.backingScaleFactor)
            #expect(plot.drawableSize.height == plot.bounds.height * window.backingScaleFactor)
            try #require(plot.currentRenderPassDescriptor)
        }
    }

    @Test func plotParameterLayoutMatchesTheMetalShader() {
        #expect(MemoryLayout<AnalyzerPlotParams>.stride == 48)
        #expect(MemoryLayout<AnalyzerPlotParams>.offset(of: \.mode) == 0)
        #expect(MemoryLayout<AnalyzerPlotParams>.offset(of: \.index) == 4)
        #expect(MemoryLayout<AnalyzerPlotParams>.offset(of: \.active) == 8)
        #expect(MemoryLayout<AnalyzerPlotParams>.offset(of: \.latestFrame) == 16)
        #expect(MemoryLayout<AnalyzerPlotParams>.offset(of: \.value) == 32)
        #expect(MemoryLayout<AnalyzerPlotParams>.offset(of: \.height) == 44)
        #expect(MemoryLayout<AnalyzerHistoryVertex>.stride == 24)
    }

    @MainActor
    @Test(arguments: [UInt32(0), 1, 2])
    func plotViewRecoversWhenMetalBecomesAvailableAfterCreation(mode: UInt32) throws {
        let client = ASFWAudioObserverClient(guid: 0)
        let plot = MetalAnalyzerPlotView(client: client, mode: mode, index: 0)
        let coordinator = plot.makeCoordinator()
        let view = MTKView(frame: .zero, device: nil)
        plot.configure(view, coordinator: coordinator, device: nil)
        #expect(view.delegate == nil)
        let device = try #require(MTLCreateSystemDefaultDevice())
        plot.configure(view, coordinator: coordinator, device: device)
        let firstRenderer = try #require(view.delegate)
        #expect(view.device?.registryID == device.registryID)
        // Ordinary updates must preserve the renderer and its history.
        plot.configure(view, coordinator: coordinator, device: device)
        #expect(view.delegate === firstRenderer)
        plot.configure(view, coordinator: coordinator, device: nil)
        #expect(view.delegate == nil)
        plot.configure(view, coordinator: coordinator, device: device)
        let recoveredRenderer = try #require(view.delegate)
        #expect(recoveredRenderer !== firstRenderer)
    }

    @MainActor
    @Test(arguments: [UInt32(0), 1, 2, 3, 4])
    func analyzerPlotsRenderOnTheGPU(mode: UInt32) throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let pipeline = try #require(MetalAnalyzerPlotView.pipeline(device))
        let textureDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: 64, height: 64, mipmapped: false)
        textureDescriptor.storageMode = .shared
        textureDescriptor.usage = .renderTarget
        let texture = try #require(device.makeTexture(descriptor: textureDescriptor))
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0)
        // Packed layout matches AnalyzerPlotParams and AnalyzerHistoryVertex.
        var params = AnalyzerPlotParams(mode: mode, index: 0, active: 1, count: 601,
            latestFrame: 2_880_000, sampleRate: 48_000, value: 0.5, peak: 0.75, width: 64, height: 64)
        let points: [UInt32] = (0...600).flatMap { i in
            [UInt32(i * 4_800), 0, Float(mode == 3 ? -18 : 0.5).bitPattern, Float(mode == 3 ? -20 : 0.1).bitPattern, 0, Float(-22).bitPattern]
        }
        let pointBuffer = try #require(device.makeBuffer(bytes: points, length: points.count * 4, options: .storageModeShared))
        let queue = try #require(device.makeCommandQueue())
        let command = try #require(queue.makeCommandBuffer())
        let encoder = try #require(command.makeRenderCommandEncoder(descriptor: pass))
        encoder.setRenderPipelineState(pipeline)
        encoder.setVertexBytes(&params, length: MemoryLayout<AnalyzerPlotParams>.stride, index: 0)
        var emptyPoint = AnalyzerHistoryVertex(frame: 0, correlation: 0, sideEnergy: 0, breakBefore: 0)
        if mode == 2 || mode == 3 { encoder.setVertexBuffer(pointBuffer, offset: 0, index: 1) }
        else { encoder.setVertexBytes(&emptyPoint, length: MemoryLayout<AnalyzerHistoryVertex>.stride, index: 1) }
        encoder.drawPrimitives(type: (mode == 2 || mode == 3) ? .line : .triangle,
                               vertexStart: 0, vertexCount: (mode == 2 || mode == 3) ? 1_200 : 12)
        encoder.endEncoding()
        command.commit()
        command.waitUntilCompleted()
        try #require(command.status == .completed)
        var pixels = [UInt8](repeating: 0, count: 64 * 64 * 4)
        pixels.withUnsafeMutableBytes {
            texture.getBytes($0.baseAddress!, bytesPerRow: 64 * 4,
                             from: MTLRegionMake2D(0, 0, 64, 64), mipmapLevel: 0)
        }
        #expect(stride(from: 3, to: pixels.count, by: 4).contains { pixels[$0] > 128 },
                "Plots must leave visible alpha on the transparent canvas; hidden rectangles must not erase earlier geometry")
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
        let state = try #require(device.makeBuffer(length: 49 * MemoryLayout<UInt32>.stride,
                                                   options: .storageModeShared))
        state.contents().initializeMemory(as: UInt8.self, repeating: 0, count: state.length)
        let output = try #require(device.makeBuffer(length: AudioAnalysisLayout.outputWords * MemoryLayout<UInt32>.stride,
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
        #expect(values[AudioAnalysisLayout.chunkOffset] == UInt32(sampleCount))
        let energyLeft = Float(bitPattern: values[AudioAnalysisLayout.chunkOffset + 2])
        let energyRight = Float(bitPattern: values[AudioAnalysisLayout.chunkOffset + 3])
        let lufs = -0.691 + 10 * log10((energyLeft + energyRight) / Float(sampleCount))
        #expect(abs(lufs + 23) < 0.7)
        #expect(values[94] == 1)
        let gpuTruePeakLeft = Float(bitPattern: values[92])
        let cpuTruePeakLeft = scalarTruePeak(samples, channel: 0)
        #expect(abs(gpuTruePeakLeft - cpuTruePeakLeft) < 1e-5,
                "GPU \(gpuTruePeakLeft), CPU \(cpuTruePeakLeft)")
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
        let state = try #require(device.makeBuffer(length: 49 * MemoryLayout<UInt32>.stride,
                                                   options: .storageModeShared))
        state.contents().initializeMemory(as: UInt8.self, repeating: 0, count: state.length)
        let output = try #require(device.makeBuffer(length: AudioAnalysisLayout.outputWords * MemoryLayout<UInt32>.stride,
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
