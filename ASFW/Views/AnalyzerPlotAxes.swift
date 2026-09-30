import SwiftUI

struct AnalyzerPlotAxes: View {
    enum Kind { case goniometer; case spectrum(sampleRate: UInt32) }
    let kind: Kind

    var body: some View {
        Canvas { context, size in
            func label(_ text: String, at point: CGPoint, anchor: UnitPoint = .center) {
                context.draw(Text(text).font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(.gray), at: point, anchor: anchor)
            }
            func line(_ start: CGPoint, _ end: CGPoint) {
                var path = Path()
                path.move(to: start); path.addLine(to: end)
                context.stroke(path, with: .color(.white.opacity(0.20)), lineWidth: 0.7)
            }
            switch kind {
            case .goniometer:
                let plot = CGRect(x: 30, y: 30, width: size.width - 60, height: size.height - 60)
                line(CGPoint(x: plot.midX, y: plot.minY), CGPoint(x: plot.midX, y: plot.maxY))
                line(CGPoint(x: plot.minX, y: plot.midY), CGPoint(x: plot.maxX, y: plot.midY))
                label("+1", at: CGPoint(x: plot.minX - 5, y: plot.minY), anchor: .trailing)
                label("−1", at: CGPoint(x: plot.minX - 5, y: plot.maxY), anchor: .trailing)
                label("−1", at: CGPoint(x: plot.minX, y: plot.midY + 12))
                label("+1", at: CGPoint(x: plot.maxX, y: plot.midY + 12))
                label("0", at: CGPoint(x: plot.midX - 8, y: plot.midY + 12))
                label("Y: (L+R)/√2 · in phase", at: CGPoint(x: size.width / 2, y: 12))
                label("X: (L−R)/√2 · opposite phase", at: CGPoint(x: size.width / 2, y: size.height - 12))
            case .spectrum(let rate):
                let plot = CGRect(x: 38, y: 12, width: max(1, size.width - 50), height: max(1, size.height - 42))
                for db in [6, 0, -30, -60, -90, -120] {
                    let y = plot.minY + CGFloat(6 - db) / 126 * plot.height
                    line(CGPoint(x: plot.minX, y: y), CGPoint(x: plot.maxX, y: y))
                    label("\(db)", at: CGPoint(x: plot.minX - 5, y: y), anchor: .trailing)
                }
                let maximum = min(20000.0, Double(rate == 0 ? 48000 : rate) / 2)
                for hz in [20.0, 100, 1000, 10000, 20000] where hz <= maximum {
                    let x = plot.minX + CGFloat(log(hz / 20) / log(maximum / 20)) * plot.width
                    line(CGPoint(x: x, y: plot.minY), CGPoint(x: x, y: plot.maxY))
                    label(hz >= 1000 ? "\(Int(hz / 1000))k" : "\(Int(hz))",
                          at: CGPoint(x: x, y: plot.maxY + 10))
                }
                label("dBFS", at: CGPoint(x: 18, y: size.height - 10))
                label("Hz", at: CGPoint(x: size.width - 12, y: size.height - 10), anchor: .trailing)
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}
