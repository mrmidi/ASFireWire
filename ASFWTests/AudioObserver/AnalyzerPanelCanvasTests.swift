import AppKit
import MetalKit
import SwiftUI
import Testing
@testable import ASFW

struct AnalyzerPanelCanvasTests {
    @MainActor
    @Test func multiplePlotSlotsShareOneMetalSurfaceAndSurviveRelayout() throws {
        let client = ASFWAudioObserverClient(guid: 0)
        let content = VStack {
            HStack {
                AnalyzerCanvasSlot(mode: 4, index: 0).frame(height: 5)
                AnalyzerCanvasSlot(mode: 4, index: 1).frame(height: 5)
                AnalyzerCanvasSlot(mode: 4, index: 2).frame(height: 5)
            }
            ZStack {
                ForEach(0..<3) { AnalyzerCanvasSlot(mode: 3, index: UInt32($0)) }
            }.frame(height: 120)
        }.analyzerCanvas(client: client)
        let host = NSHostingView(rootView: content)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 400, height: 200),
            styleMask: .borderless, backing: .buffered, defer: false)
        window.contentView = host
        window.layoutIfNeeded()
        host.layoutSubtreeIfNeeded()
        func metalViews(_ view: NSView) -> [MTKView] {
            if let metal = view as? MTKView { return [metal] }
            return view.subviews.flatMap(metalViews)
        }
        let plots = metalViews(host)
        #expect(plots.count == 1)
        let plot = try #require(plots.first)
        #expect(plot.bounds.width > 0 && plot.bounds.height > 0)
        window.setContentSize(NSSize(width: 600, height: 300))
        window.layoutIfNeeded()
        host.layoutSubtreeIfNeeded()
        #expect(metalViews(host).count == 1)
        #expect(metalViews(host).first === plot)
    }
}
