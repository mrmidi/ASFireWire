import SwiftUI

struct AnalyzerCanvasAnchor {
    let mode: UInt32
    let index: UInt32
    let otherChannel: UInt32
    let bounds: Anchor<CGRect>
}

struct AnalyzerCanvasAnchors: PreferenceKey {
    static var defaultValue: [Int: AnalyzerCanvasAnchor] { [:] }
    static func reduce(value: inout [Int: AnalyzerCanvasAnchor], nextValue: () -> [Int: AnalyzerCanvasAnchor]) {
        value.merge(nextValue(), uniquingKeysWith: { _, next in next })
    }
}

/// A layout slot, not a Metal surface. Its parent panel draws all slots in one
/// transparent drawable while SwiftUI owns labels, backgrounds and controls.
struct AnalyzerCanvasSlot: View {
    let mode: UInt32
    let index: UInt32
    var otherChannel: UInt32 = 0
    var body: some View {
        Color.clear.anchorPreference(key: AnalyzerCanvasAnchors.self, value: .bounds) {
            [Int(mode) * 256 + Int(index): AnalyzerCanvasAnchor(mode: mode, index: index,
                otherChannel: otherChannel, bounds: $0)]
        }
    }
}

extension View {
    func analyzerCanvas(client: ASFWAudioObserverClient) -> some View {
        overlayPreferenceValue(AnalyzerCanvasAnchors.self) { anchors in
            GeometryReader { geometry in
                if !anchors.isEmpty {
                    let regions = anchors.keys.sorted().compactMap { key -> AnalyzerPlotRegion? in
                        guard let plot = anchors[key] else { return nil }
                        return AnalyzerPlotRegion(mode: plot.mode, index: plot.index,
                            rect: geometry[plot.bounds], otherChannel: plot.otherChannel)
                    }
                    MetalAnalyzerPlotView(client: client, mode: 0, index: 0, regions: regions,
                        historyState: client.plotHistory)
                        .allowsHitTesting(false).accessibilityHidden(true)
                }
            }
        }
    }
}
