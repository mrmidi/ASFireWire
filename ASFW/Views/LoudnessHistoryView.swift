import SwiftUI

struct LoudnessHistoryView: View {
    @ObservedObject private var themeState = AnalyzerThemeState.shared
    let client: ASFWAudioObserverClient

    var body: some View {
        let mode = themeState.mode
        let isLight = mode.isLight
        let plotBackground = mode.plotBackground
        let plotBorder = mode.plotBorder
        let plotGridLine = mode.plotGridLine
        let plotLabelColor = mode.plotLabelColor

        GeometryReader { geometry in
            let leftMargin: CGFloat = 44
            let rightMargin: CGFloat = 10
            let topMargin: CGFloat = 18
            let bottomMargin: CGFloat = 22
            let rect = CGRect(x: leftMargin, y: topMargin,
                              width: max(1, geometry.size.width - (leftMargin + rightMargin)),
                              height: max(1, geometry.size.height - (topMargin + bottomMargin)))
            ZStack {
                plotBackground

                ForEach(0..<3) { index in
                    AnalyzerCanvasSlot(mode: 3, index: UInt32(index))
                        .padding(.leading, leftMargin).padding(.trailing, rightMargin)
                        .padding(.top, topMargin).padding(.bottom, bottomMargin)
                }

                Canvas { context, size in
                    func label(_ text: String, _ point: CGPoint, _ anchor: UnitPoint = .center,
                               color: Color? = nil) {
                        let finalColor = color ?? plotLabelColor
                        context.draw(
                            Text(text)
                                .font(.system(size: 9.5, weight: isLight ? .semibold : .medium, design: .monospaced))
                                .foregroundStyle(finalColor),
                            at: point, anchor: anchor
                        )
                    }

                    // LUFS unit indicator with breathing room above -6, comfortably inside the viewport
                    label("LUFS", CGPoint(x: rect.minX - 6, y: 9), .trailing,
                          color: isLight ? Color(red: 0.38, green: 0.44, blue: 0.52) : Color(red: 0.55, green: 0.62, blue: 0.70))

                    // Standard dB grid lines (-6 down to -36)
                    for db in [-6, -12, -18, -24, -30, -36] {
                        let y = rect.minY + CGFloat(-6 - db) / 30 * rect.height
                        var path = Path()
                        path.move(to: CGPoint(x: rect.minX, y: y))
                        path.addLine(to: CGPoint(x: rect.maxX, y: y))
                        context.stroke(path, with: .color(plotGridLine), lineWidth: 0.5)
                        label("\(db)", CGPoint(x: rect.minX - 6, y: y), .trailing)
                    }

                    // Target line: -14 LUFS (Streaming target: Spotify, YouTube, Apple Music)
                    let yStream = rect.minY + CGFloat(-6 - (-14)) / 30 * rect.height
                    var streamPath = Path()
                    streamPath.move(to: CGPoint(x: rect.minX, y: yStream))
                    streamPath.addLine(to: CGPoint(x: rect.maxX, y: yStream))
                    let streamColor = isLight ? Color(red: 0.02, green: 0.45, blue: 0.75) : Color.cyan
                    context.stroke(streamPath, with: .color(streamColor.opacity(isLight ? 0.75 : 0.60)),
                                   style: StrokeStyle(lineWidth: 1.0, dash: [4, 4]))
                    label("−14", CGPoint(x: rect.minX + 4, y: yStream - 7), .leading,
                          color: streamColor.opacity(isLight ? 0.95 : 0.85))

                    // Target line: -23 LUFS (EBU R128 broadcast standard)
                    let yEBU = rect.minY + CGFloat(-6 - (-23)) / 30 * rect.height
                    var ebuPath = Path()
                    ebuPath.move(to: CGPoint(x: rect.minX, y: yEBU))
                    ebuPath.addLine(to: CGPoint(x: rect.maxX, y: yEBU))
                    let ebuColor = isLight ? Color(red: 0.80, green: 0.38, blue: 0.02) : Color.orange
                    context.stroke(ebuPath, with: .color(ebuColor.opacity(isLight ? 0.75 : 0.65)),
                                   style: StrokeStyle(lineWidth: 1.0, dash: [4, 4]))
                    label("−23", CGPoint(x: rect.minX + 4, y: yEBU - 7), .leading,
                          color: ebuColor.opacity(isLight ? 0.95 : 0.85))

                    // Time grid lines (60s window)
                    for second in stride(from: 0, through: 60, by: 10) {
                        let x = rect.minX + CGFloat(second) / 60 * rect.width
                        var path = Path()
                        path.move(to: CGPoint(x: x, y: rect.minY))
                        path.addLine(to: CGPoint(x: x, y: rect.maxY))
                        let timeGridColor = isLight ? Color.black.opacity(0.04) : Color.white.opacity(0.06)
                        context.stroke(path, with: .color(timeGridColor), lineWidth: 0.5)
                        let timeLabelColor = isLight ? Color(red: 0.38, green: 0.44, blue: 0.52) : Color(red: 0.60, green: 0.68, blue: 0.76)
                        label(second == 60 ? "now" : "−\(60 - second)s",
                              CGPoint(x: x, y: size.height - 9),
                              second == 0 ? .leading : second == 60 ? .trailing : .center,
                              color: timeLabelColor)
                    }
                }.allowsHitTesting(false)

                AnalyzerMetalText("loudness.history.note", style: .caption,
                                  template: "History appears during playback",
                                  tone: .secondary, alignment: .center) { metrics, _ in
                    metrics.analysis.token == nil ? "History appears during playback" : ""
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(plotBorder, lineWidth: 1))
        }
        .accessibilityLabel("60-second loudness history")
    }
}
