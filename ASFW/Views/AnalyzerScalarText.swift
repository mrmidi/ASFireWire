import Combine
import SwiftUI

/// Formatting is evaluated at scalar cadence. Only a different displayed
/// string invalidates its Text; sub-decimal DSP changes stay out of SwiftUI.
@MainActor
final class AnalyzerScalarReadout: ObservableObject {
    @Published private(set) var text: String
    private var subscription: AnyCancellable?

    init(state: AnalyzerPanelUIState,
         format: @escaping (AudioObserverMetrics, AudioObserverSnapshot) -> String) {
        text = format(state.metrics, state.snapshot)
        subscription = state.$metrics.sink { [weak self, weak state] metrics in
            guard let self, let state else { return }
            let next = format(metrics, state.snapshot)
            if next != self.text { self.text = next }
        }
    }
}

struct AnalyzerScalarText: View {
    @StateObject private var readout: AnalyzerScalarReadout

    init(state: AnalyzerPanelUIState,
         value: @escaping (AudioObserverMetrics, AudioObserverSnapshot) -> String) {
        _readout = StateObject(wrappedValue: AnalyzerScalarReadout(state: state, format: value))
    }
    var body: some View { Text(readout.text) }
}
