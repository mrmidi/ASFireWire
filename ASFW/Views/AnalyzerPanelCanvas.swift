import SwiftUI

struct AnalyzerCanvasAnchor {
    let mode: UInt32
    let index: UInt32
    let otherChannel: UInt32
    let bounds: Anchor<CGRect>
    var text: AnalyzerTextSpec? = nil
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
                            rect: geometry[plot.bounds], otherChannel: plot.otherChannel, text: plot.text)
                    }
                    MetalAnalyzerPlotView(client: client, mode: 0, index: 0, regions: regions,
                        historyState: client.plotHistory)
                        .allowsHitTesting(false).accessibilityHidden(true)
                }
            }
        }
    }
}

/// A numeric readout drawn by the enclosing panel canvas. SwiftUI lays out a
/// hidden `template` once, so a changing value never re-lays out the panel;
/// the canvas formats and draws the value in that slot.
struct AnalyzerMetalText: View {
    let spec: AnalyzerTextSpec
    let template: String

    init(_ id: String, style: AnalyzerTextStyle, template: String,
         tone: AnalyzerTextSpec.Tone = .primary, alignment: AnalyzerTextAlignment,
         interval: Double = AnalyzerTextSpec.metering,
         format: @escaping (AudioObserverMetrics, AudioObserverSnapshot) -> String) {
        spec = AnalyzerTextSpec(id: id, style: style, tone: tone, alignment: alignment,
                                interval: interval, format: format)
        self.template = template
    }

    var body: some View {
        Text(template).font(spec.style.swiftUIFont).lineLimit(1).fixedSize().hidden()
            .overlay {
                Color.clear.anchorPreference(key: AnalyzerCanvasAnchors.self, value: .bounds) { [spec] in
                    [spec.key: AnalyzerCanvasAnchor(mode: AnalyzerPlotRegion.textMode, index: 0,
                        otherChannel: 0, bounds: $0, text: spec)]
                }
            }
            .accessibilityHidden(true)
    }
}
