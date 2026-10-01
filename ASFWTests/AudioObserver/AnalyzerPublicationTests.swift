import Combine
import Foundation
import Testing
@testable import ASFW

struct AnalyzerPublicationTests {
    @MainActor
    @Test func scalarPublicationOnlyInvalidatesTheLoudnessControlsOnAPhaseChange() {
        let model = AudioObserverPanelModel(guid: 0)
        var root = 0
        var controls = 0
        let subscriptions = [
            model.objectWillChange.sink { root += 1 },
            model.loudnessControlsUI.objectWillChange.sink { controls += 1 },
        ]
        var metrics = AudioObserverMetrics()
        metrics.gpuMilliseconds = 1.25
        metrics.analysis.levels.left.rms = .valid(0.25)
        model.publishScalarPanels(metrics, snapshot: AudioObserverSnapshot())
        #expect(root == 0)
        #expect(controls == 0)
        withExtendedLifetime(subscriptions) {}
    }
}
