import Combine
import SwiftUI

/// Only this child subscribes to its panel's scalar publication.
@MainActor
final class AnalyzerPanelUIState: ObservableObject {
    enum Section { case monitor, stereo, loudness, loudnessControls, diagnostics }
    let section: Section
    init(section: Section = .diagnostics) { self.section = section }

    @Published private(set) var metrics = AudioObserverMetrics()
    var snapshot = AudioObserverSnapshot()

    func publish(_ metrics: AudioObserverMetrics, snapshot: AudioObserverSnapshot) {
        let changed: Bool
        switch section {
        case .monitor:
            changed = self.metrics.analysis.levels != metrics.analysis.levels ||
                self.metrics.analysis.stereo != metrics.analysis.stereo
        case .stereo: changed = self.metrics.analysis.stereo != metrics.analysis.stereo
        case .loudness: changed = self.metrics.analysis.loudness != metrics.analysis.loudness
        case .loudnessControls: changed = self.metrics.analysis.loudness.sessionPhase != metrics.analysis.loudness.sessionPhase
        case .diagnostics: changed = self.metrics != metrics || self.snapshot != snapshot
        }
        guard changed || self.snapshot.ioRunning != snapshot.ioRunning else { return }
        self.snapshot = snapshot
        self.metrics = metrics
    }
}

struct AnalyzerLivePanel<Content: View>: View {
    @ObservedObject var state: AnalyzerPanelUIState
    @ViewBuilder let content: (AudioObserverMetrics, AudioObserverSnapshot) -> Content

    var body: some View { content(state.metrics, state.snapshot) }
}
