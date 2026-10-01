import Combine
import Testing
@testable import ASFW

struct AnalyzerPublicationTests {
    @MainActor
    @Test func scalarPublicationDoesNotInvalidateTheDashboardOrUnrelatedPanels() {
        let model = AudioObserverPanelModel(guid: 0)
        var root = 0
        var monitor = 0
        var loudness = 0
        var diagnostics = 0
        let subscriptions = [
            model.objectWillChange.sink { root += 1 },
            model.monitorUI.objectWillChange.sink { monitor += 1 },
            model.loudnessUI.objectWillChange.sink { loudness += 1 },
            model.diagnosticsUI.objectWillChange.sink { diagnostics += 1 }
        ]
        var metrics = AudioObserverMetrics()
        metrics.gpuMilliseconds = 1.25
        model.publishScalarPanels(metrics, snapshot: AudioObserverSnapshot())
        #expect(root == 0)
        #expect(monitor == 0)
        #expect(loudness == 0)
        #expect(diagnostics == 1)
        metrics.analysis.levels.left.rms = .valid(0.25)
        model.publishScalarPanels(metrics, snapshot: AudioObserverSnapshot())
        #expect(root == 0)
        #expect(monitor == 1)
        #expect(loudness == 0)
        #expect(diagnostics == 2)
        model.publishScalarPanels(metrics, snapshot: AudioObserverSnapshot())
        #expect(monitor == 1)
        #expect(diagnostics == 2)
        withExtendedLifetime(subscriptions) {}
    }
}
