import Combine
import Foundation
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

struct AnalyzerScalarPublicationTests {
    @MainActor
    @Test func subDecimalChangesDoNotInvalidateTheDisplayedNumber() {
        let state = AnalyzerPanelUIState(section: .monitor)
        let readout = AnalyzerScalarReadout(state: state) { metrics, _ in
            String(format: "%.1f", metrics.analysis.stereo.correlation.value ?? 0)
        }
        var invalidations = 0
        let subscription = readout.objectWillChange.sink { invalidations += 1 }
        var metrics = AudioObserverMetrics()
        metrics.analysis.stereo.correlation = .valid(0.811)
        state.publish(metrics, snapshot: AudioObserverSnapshot())
        #expect(readout.text == "0.8")
        #expect(invalidations == 1)
        metrics.analysis.stereo.correlation = .valid(0.819)
        state.publish(metrics, snapshot: AudioObserverSnapshot())
        #expect(invalidations == 1)
        metrics.analysis.stereo.correlation = .valid(0.91)
        state.publish(metrics, snapshot: AudioObserverSnapshot())
        #expect(readout.text == "0.9")
        #expect(invalidations == 2)
        withExtendedLifetime(subscription) {}
    }
}
