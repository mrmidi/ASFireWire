import Metal
import Testing
@testable import ASFW

nonisolated private struct FixedAnalysisStateReader: AudioAnalysisStateReading {
    let state: AudioObserverWireState
    func read() throws -> AudioObserverWireState { state }
}

struct AudioAnalysisEngineTests {
    @Test func stopDrainsGPUCompletionAndPreventsNewSubmissions() async throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let ring = try #require(device.makeBuffer(length: 12_288 * 2 * MemoryLayout<Float>.stride,
                                                  options: .storageModeShared))
        ring.contents().initializeMemory(as: UInt8.self, repeating: 0, count: ring.length)
        let metrics = AudioObserverMetricsState()
        let wire = AudioObserverWireState(writeEndFrame: 1_480, oldestValidFrame: 0,
            sessionEpoch: 1, discontinuityEpoch: 1, memoryGeneration: 1, mappedGeneration: 1,
            activeRingFrames: 12_288, channels: 2, sampleRateHz: 48_000)
        let engine = try AudioAnalysisEngine(device: device, ringBuffer: ring,
            stateReader: FixedAnalysisStateReader(state: wire), metrics: metrics,
            plotHistory: AnalyzerPlotHistoryState())
        var snapshot = AudioObserverSnapshot()
        snapshot.ioRunning = true
        snapshot.activeRingFrames = 12_288
        snapshot.mappedFrames = 12_288
        snapshot.channels = 2
        snapshot.sampleRateHz = 48_000
        snapshot.memoryGeneration = 1
        snapshot.sessionEpoch = 1
        snapshot.discontinuityEpoch = 1
        snapshot.writeEndFrame = 1_000
        await engine.consume(snapshot) // Establish cursor.
        snapshot.writeEndFrame = 1_480
        await engine.consume(snapshot) // Submit actual Metal work.
        let submitted = metrics.read(includeHistory: false)
        #expect(UInt64(submitted.inFlight) + submitted.windowsRendered == 1)
        await withTaskGroup(of: Void.self) { group in
            group.addTask { await engine.stop() }
            group.addTask { await engine.stop() }
        }
        let stopped = metrics.read(includeHistory: false)
        #expect(stopped.inFlight == 0)
        #expect(stopped.windowsRendered + stopped.unsafeWindows <= 1)
        snapshot.writeEndFrame = 1_960
        await engine.consume(snapshot, pair: AudioChannelPair(leftIndex: 1, rightIndex: 0, generation: 1))
        #expect(metrics.read(includeHistory: false) == stopped)
    }
}


/// Protected control data only; no simulated PCM mutation races the GPU.
nonisolated private final class AdvancingAnalysisReader: AudioAnalysisStateReading, @unchecked Sendable {
    private let lock = NSLock()
    private var reads = 0
    let progressed = DispatchSemaphore(value: 0)
    func read() throws -> AudioObserverWireState {
        lock.lock()
        defer { lock.unlock() }
        reads += 1
        if reads == 8 { progressed.signal() }
        let end = UInt64(reads) * 480
        return AudioObserverWireState(writeEndFrame: end, oldestValidFrame: end > 12_288 ? end - 12_288 : 0,
            sessionEpoch: 1, discontinuityEpoch: 1, memoryGeneration: 1, mappedGeneration: 1,
            activeRingFrames: 12_288, channels: 2, sampleRateHz: 48_000)
    }
}

extension AudioAnalysisEngineTests {
    // Deliberate main-executor blocking proves acquisition doesn't need it.
    @MainActor
    private func waitForAcquisition(_ reader: AdvancingAnalysisReader) -> Bool {
        reader.progressed.wait(timeout: .now() + 3) == .success
    }

    @MainActor
    @Test func acquisitionContinuesWhileTheMainExecutorIsBlocked() async throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let ring = try #require(device.makeBuffer(length: 12_288 * 2 * 4, options: .storageModeShared))
        ring.contents().initializeMemory(as: UInt8.self, repeating: 0, count: ring.length)
        let reader = AdvancingAnalysisReader()
        let engine = try await AudioAnalysisEngine.make(device: device, ringBuffer: ring, stateReader: reader,
            metrics: AudioObserverMetricsState(), plotHistory: AnalyzerPlotHistoryState())
        let renderState = AudioObserverRenderState()
        await withTaskGroup(of: Void.self) { group in
            group.addTask {
                do { try await engine.run(renderState: renderState) { _ in } }
                catch is CancellationError { }
                catch { Issue.record("Acquisition failed: \(error)") }
            }
            #expect(waitForAcquisition(reader))
            group.cancelAll()
            await engine.stop()
        }
        #expect(renderState.read().writeEndFrame >= 480 * 4)
    }
}
