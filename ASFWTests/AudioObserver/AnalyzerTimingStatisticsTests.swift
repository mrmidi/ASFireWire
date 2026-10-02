import Testing
import Metal
@testable import ASFW

struct AnalyzerTimingStatisticsTests {
    @Test func onlyFinishedSecondIsPublishedAndExtremaAreRetained() {
        var stats = AnalyzerTimingStatistics()
        stats.append(milliseconds: 2, at: 10.1)
        stats.append(milliseconds: 4, at: 10.8)
        #expect(stats.mean == nil)
        stats.append(milliseconds: 1, at: 11.1)
        #expect(stats.mean == 3 && stats.fastest == 2 && stats.peak == 4 && stats.rate == 2)
        stats.append(milliseconds: 5, at: 11.8)
        #expect(stats.mean == 3) // no changes in the displayed bucket during this second
        stats.append(milliseconds: 3, at: 12.1)
        #expect(stats.mean == 3 && stats.fastest == 1 && stats.peak == 5)
        #expect(stats.runFastest == 1 && stats.runPeak == 5 && stats.runMean == 3)
        #expect(stats.count == 5)
    }

    @Test func derivedLoadAndBudgetUseCompletedSecond() {
        var stats = AnalyzerTimingStatistics()
        #expect(stats.spanPercent == nil && stats.frameBudgetMilliseconds == nil)
        for index in 0..<23 { stats.append(milliseconds: 3, at: 10 + Double(index) / 23) }
        stats.append(milliseconds: 9, at: 11.1)
        #expect(stats.millisecondsPerSecond == 69)
        #expect(stats.spanPercent == 6.9)
        #expect(stats.frameBudgetMilliseconds == 1_000.0 / 23)
    }

    @Test func invalidSamplesAreNotZeroCostMeasurements() {
        var stats = AnalyzerTimingStatistics()
        stats.append(milliseconds: nil, at: 10)
        stats.append(milliseconds: .nan, at: 10)
        stats.append(milliseconds: -1, at: 10)
        #expect(stats.count == 0 && stats.mean == nil && stats.runPeak == nil)
        stats.append(milliseconds: 2, at: 10.1)
        stats.append(milliseconds: 4, at: 14.1)
        #expect(stats.rate == nil) // a suspended stream is not a continuous 1 Hz measurement
        stats = AnalyzerTimingStatistics()
        #expect(stats.count == 0 && stats.runMean == nil && stats.fastest == nil)
    }

    @Test func visualResultsAndResetUseTheSharedMetricStore() {
        let metrics = AudioObserverMetricsState()
        metrics.recordVisual(gpu: 2, cpu: 0.5, queue: 0.3, chain: 6, at: 10.1)
        metrics.recordVisual(gpu: 4, cpu: 0.7, queue: 0.1, chain: 8, at: 10.8)
        metrics.recordVisual(gpu: 3, cpu: 0.1, queue: 0.2, chain: 7, at: 11.1)
        let result = metrics.read(includeHistory: false)
        #expect(result.visualGPU.mean == 3 && result.analysisToVisual.mean == 7)
        #expect(result.visualCPU.mean == 0.6 && result.visualQueue.mean == 0.2)
        #expect(result.analysisGPU.count == 0)
        metrics.resetGPUTiming()
        let reset = metrics.read(includeHistory: false)
        #expect(reset.visualGPU.count == 0 && reset.analysisToVisual.mean == nil)
    }
    @Test func drawableTimeIsSeparatedFromVisualPreparation() {
        let metrics = AudioObserverMetricsState()
        metrics.recordVisual(gpu: 1, cpu: 4, queue: 0, chain: nil, drawable: 3, at: 10.1)
        metrics.recordVisual(gpu: 1, cpu: 4, queue: 0, chain: nil, drawable: 3, at: 11.1)
        let result = metrics.read(includeHistory: false)
        #expect(result.visualCPU.mean == 4)
        #expect(result.visualDrawable.mean == 3)
        #expect(result.visualEncode.mean == 1)
        metrics.resetGPUTiming()
        #expect(metrics.read(includeHistory: false).visualEncode.count == 0)
    }

    @MainActor @Test func visualCommandCompletionRecordsActualGPUTimestamps() throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let buffer = try #require(device.makeBuffer(length: 4096, options: .storageModePrivate))
        let metrics = AudioObserverMetricsState()
        let submission = AnalyzerRenderSubmission(metrics: metrics)
        var captured: MTLCommandBuffer?
        submission.withFrame {
            captured = submission.commandBuffer(for: device)
            if let blit = captured?.makeBlitCommandEncoder() {
                blit.fill(buffer: buffer, range: 0..<buffer.length, value: 0)
                blit.endEncoding()
            }
        }
        let command = try #require(captured)
        command.waitUntilCompleted()
        #expect(command.status == .completed)
        let value = metrics.read(includeHistory: false)
        #expect(value.visualGPU.count == 1 && value.visualGPU.runMean != nil)
        #expect(value.visualCPU.count == 1 && value.visualQueue.count == 1)
        #expect(value.analysisToVisual.count == 0) // no analysis origin in this isolated GPU test
    }

    @Test func hardwareCountersMeasureAnExistingComputePass() throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        guard device.supportsFamily(.apple1), device.supportsCounterSampling(.atStageBoundary) else { return }
        let timing = try #require(AnalyzerKernelTiming(device: device))
        let library = try #require(device.makeDefaultLibrary())
        let pipeline = try device.makeComputePipelineState(function: #require(library.makeFunction(name: "asfwConsumeOutputRange")))
        let source = try #require(device.makeBuffer(length: 256 * 2 * 4, options: .storageModeShared))
        source.contents().initializeMemory(as: UInt8.self, repeating: 0, count: source.length)
        let output = try #require(device.makeBuffer(length: AudioAnalysisLayout.outputWords * 4, options: .storageModeShared))
        let queue = try #require(device.makeCommandQueue())
        let command = try #require(queue.makeCommandBuffer())
        let encoder = try #require(timing.encoder(command, stage: 0))
        struct Params {
            var startFrame: UInt64; var frameCount: UInt32; var ringFrames: UInt32
            var channels: UInt32; var leftChannel: UInt32; var rightChannel: UInt32
        }
        var params = Params(startFrame: 0, frameCount: 256, ringFrames: 256,
                                        channels: 2, leftChannel: 0, rightChannel: 1)
        encoder.setComputePipelineState(pipeline)
        encoder.setBuffer(source, offset: 0, index: 0)
        encoder.setBuffer(output, offset: 0, index: 1)
        encoder.setBytes(&params, length: MemoryLayout<Params>.stride, index: 2)
        encoder.dispatchThreads(MTLSize(width: 256, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
        encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
        try #require(command.status == .completed)
        let duration = try #require(timing.milliseconds(stage: 0))
        let whole = (command.gpuEndTime - command.gpuStartTime) * 1_000
        #expect(duration >= 0 && duration <= whole + 0.1, "counter \(duration) ms, command \(whole) ms")
    }

}
