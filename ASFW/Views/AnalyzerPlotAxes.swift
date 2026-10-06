import SwiftUI

struct AnalyzerPlotAxes: View {
    enum Kind { case goniometer; case waveform; case spectrum(sampleRate: UInt32); case spectrogram(sampleRate: UInt32, seconds: Double); case waterfall(sampleRate: UInt32, seconds: Double) }
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
                let padding: CGFloat = 16
                let side = max(1, min(size.width, size.height) - 2 * padding)
                let plot = CGRect(x: (size.width - side) / 2, y: (size.height - side) / 2, width: side, height: side)

                // Outer bounding frame
                var box = Path()
                box.addRect(plot)
                context.stroke(box, with: .color(.white.opacity(0.20)), lineWidth: 0.7)

                // Center crosshair (Mid vertical, Side horizontal)
                line(CGPoint(x: plot.midX, y: plot.minY), CGPoint(x: plot.midX, y: plot.maxY))
                line(CGPoint(x: plot.minX, y: plot.midY), CGPoint(x: plot.maxX, y: plot.midY))

                // 45° Diagonals (L top-left, R top-right)
                line(CGPoint(x: plot.minX, y: plot.minY), CGPoint(x: plot.maxX, y: plot.maxY))
                line(CGPoint(x: plot.minX, y: plot.maxY), CGPoint(x: plot.maxX, y: plot.minY))

                // Axis and channel labels matching standard studio goniometer
                func goniometerLabel(_ text: String, at point: CGPoint, anchor: UnitPoint) {
                    context.draw(Text(text).font(.system(size: 11, weight: .regular))
                        .foregroundStyle(Color.white.opacity(0.65)), at: point, anchor: anchor)
                }
                goniometerLabel("L", at: CGPoint(x: plot.minX + 8, y: plot.minY + 6), anchor: .topLeading)
                goniometerLabel("R", at: CGPoint(x: plot.maxX - 8, y: plot.minY + 6), anchor: .topTrailing)
                goniometerLabel("Mid", at: CGPoint(x: plot.midX, y: plot.minY + 6), anchor: .top)
                goniometerLabel("Side", at: CGPoint(x: plot.maxX - 6, y: plot.midY + 4), anchor: .topTrailing)
            case .waveform:
                for lane in 0..<2 {
                    let middle = size.height * (CGFloat(lane) + 0.5) / 2
                    line(CGPoint(x: 0, y: middle), CGPoint(x: size.width, y: middle))
                    label(lane == 0 ? "L" : "R", at: CGPoint(x: 10, y: middle - 12))
                }
            case .waterfall(let rate, let seconds):
                let plot = CGRect(x: 38, y: 12, width: max(1, size.width - 50), height: max(1, size.height - 42))
                func projected(_ frequency: Double, _ age: Double, _ level: Double = 0) -> CGPoint {
                    let p = WaterfallProjection.point(frequency: frequency, age: age, level: level)
                    return CGPoint(x: plot.minX + (p.x + 1) / 2 * plot.width,
                                   y: plot.minY + (1 - p.y) / 2 * plot.height)
                }
                line(projected(0, 0), projected(1, 0))
                line(projected(0, 0), projected(0, 1))
                line(projected(1, 0), projected(1, 1))
                line(projected(0, 1), projected(1, 1))
                line(projected(0, 0), projected(0, 0, 1))
                let maximum = min(20000.0, Double(rate == 0 ? 48000 : rate) / 2)
                for hz in [20.0, 100, 1000, 10000, 20000] where hz <= maximum {
                    let frequency = log(hz / 20) / log(maximum / 20)
                    let p = projected(frequency, 0)
                    line(p, projected(frequency, 1))
                    line(p, CGPoint(x: p.x, y: p.y + 4))
                    label(hz >= 1000 ? "\(Int(hz / 1000))k" : "\(Int(hz))",
                          at: CGPoint(x: p.x, y: p.y + 10))
                }
                for db in [-100, -50, 0] {
                    let p = projected(0, 0, Double(db + 100) / 100)
                    line(p, CGPoint(x: p.x - 4, y: p.y))
                    label("\(db)", at: CGPoint(x: p.x - 5, y: p.y), anchor: .trailing)
                }
                for tick in 0...3 {
                    let age = Double(tick) / 3
                    let p = projected(1, age)
                    line(projected(0, age), p)
                    let text = tick == 0 ? "now" : String(format: "−%.1fs", seconds * age)
                    label(text, at: CGPoint(x: p.x + 8, y: p.y), anchor: .leading)
                }
                let front = projected(0.5, 0)
                label("Frequency · Hz →", at: CGPoint(x: front.x, y: front.y + 26))
                let level = projected(0, 0, 1)
                label("Level · dBFS", at: CGPoint(x: level.x, y: level.y - 14), anchor: .leading)
                let time = projected(1, 1)
                label("History · s", at: CGPoint(x: time.x, y: time.y - 16), anchor: .trailing)
            case .spectrogram(let rate, let seconds):
                let plot = CGRect(x: 38, y: 12, width: max(1, size.width - 50), height: max(1, size.height - 42))
                let maximum = min(20000.0, Double(rate == 0 ? 48000 : rate) / 2)
                for hz in [20.0, 100, 1000, 10000, 20000] where hz <= maximum {
                    let y = plot.maxY - CGFloat(log(hz / 20) / log(maximum / 20)) * plot.height
                    label(hz >= 1000 ? "\(Int(hz / 1000))k" : "\(Int(hz))",
                          at: CGPoint(x: plot.minX - 5, y: y), anchor: .trailing)
                }
                for tick in 0...4 {
                    let x = plot.minX + CGFloat(tick) / 4 * plot.width
                    let text = tick == 4 ? "now" : String(format: "−%.1fs", seconds * Double(4 - tick) / 4)
                    label(text, at: CGPoint(x: x, y: plot.maxY + 10),
                          anchor: tick == 4 ? .trailing : tick == 0 ? .leading : .center)
                }
                label("Hz", at: CGPoint(x: 18, y: size.height - 10))
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
