import Metal
import Testing
@testable import ASFW

struct AnalyzerRenderSubmissionTests {
    @MainActor
    @Test func plotsShareOneSubmissionPerVisualFrame() throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let submission = AnalyzerRenderSubmission()
        var commands: [MTLCommandBuffer] = []
        submission.withFrame {
            for _ in 0..<8 {
                if let command = submission.commandBuffer(for: device) {
                    commands.append(command)
                    submission.commit(command)
                }
            }
        }
        #expect(commands.count == 8)
        let first = try #require(commands.first)
        #expect(commands.allSatisfy { ObjectIdentifier($0) == ObjectIdentifier(first) })
        first.waitUntilCompleted()
        #expect(first.status == .completed)
        #expect(submission.submittedCommandBuffers == 1)
        submission.withFrame {}
        #expect(submission.submittedCommandBuffers == 1)
    }
}

struct AnalyzerDrawCadenceTests {
    @MainActor
    @Test func differentCompletionAndDrawRatesDoNotQuantizeSpectrumToHalfRate() {
        var cadence = AnalyzerDrawCadence()
        let draws = (0..<100).filter { cadence.shouldDraw(now: Double($0) / 50, hz: 30) }
        #expect(draws.count >= 59 && draws.count <= 61)
        let duplicate = cadence.shouldDraw(now: 1.98, hz: 30)
        #expect(!duplicate)
        // A long stall produces one fresh frame, never a backlog of redraws.
        let afterStall = cadence.shouldDraw(now: 10, hz: 30)
        let catchUp = cadence.shouldDraw(now: 10, hz: 30)
        #expect(afterStall)
        #expect(!catchUp)
    }
}
