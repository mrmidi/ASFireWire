import Testing
@testable import ASFW

struct AnalyzerDisplaySmootherTests {
    @Test func indicatorsEaseTowardTheTargetAndResetWhenInvalid() {
        var smoother = AnalyzerDisplaySmoother()
        _ = smoother.update(value: 0, peak: 0, mode: 1, now: 0, active: true)
        let next = smoother.update(value: 1, peak: 0, mode: 1, now: 0.02, active: true)
        #expect(next.value > 0 && next.value < 0.2)
        _ = smoother.update(value: 0, peak: 0, mode: 1, now: 0.03, active: false)
        let restarted = smoother.update(value: -1, peak: 0, mode: 1, now: 0.04, active: true)
        #expect(restarted.value == -1)
    }

    @Test func meterAttackIsFasterThanReleaseAndPeaksRiseImmediately() {
        var rising = AnalyzerDisplaySmoother()
        _ = rising.update(value: 0.01, peak: 0.01, mode: 0, now: 0, active: true)
        let attack = rising.update(value: 1, peak: 1, mode: 0, now: 0.02, active: true)
        var falling = AnalyzerDisplaySmoother()
        _ = falling.update(value: 1, peak: 1, mode: 0, now: 0, active: true)
        let release = falling.update(value: 0.01, peak: 0.01, mode: 0, now: 0.02, active: true)
        #expect(attack.peak == 1)
        #expect(release.value > 0.7)
        #expect(release.peak > release.value)
    }

    @Test func smoothingUsesElapsedTimeRatherThanFrameCount() {
        var fast = AnalyzerDisplaySmoother()
        var slow = AnalyzerDisplaySmoother()
        _ = fast.update(value: 0, peak: 0, mode: 1, now: 0, active: true)
        _ = slow.update(value: 0, peak: 0, mode: 1, now: 0, active: true)
        var a: Float = 0
        var b: Float = 0
        for frame in 1...20 { a = fast.update(value: 1, peak: 0, mode: 1, now: Double(frame) * 0.01, active: true).value }
        for frame in 1...10 { b = slow.update(value: 1, peak: 0, mode: 1, now: Double(frame) * 0.02, active: true).value }
        #expect(abs(a - b) < 0.00001)
    }
}
