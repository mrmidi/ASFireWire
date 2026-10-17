import SwiftUI

/// A rotary. Bipolar controls fill outward from the top centre so a centred pan
/// is visibly centred; unipolar ones fill from the minimum.
struct AudioTopologyKnob: View {
    let value: Double
    let tint: Color
    let caption: String
    var isBipolar = false
    /// Called once when the gesture ends. Devices whose write is a multi-step
    /// bus transaction commit here instead of on every drag sample; the default
    /// no-op keeps continuous-write call sites unchanged. Declared ahead of
    /// `onChanged` so an unlabelled trailing closure still binds to `onChanged`.
    var onCommitted: (Double) -> Void = { _ in }
    let onChanged: (Double) -> Void

    @State private var dragStart: Double?
    @State private var live: Double?

    private var shown: Double { live ?? value }

    var body: some View {
        VStack(spacing: 1) {
            ZStack {
                Circle().stroke(Color(white: 0.28), lineWidth: 2.5)
                Circle()
                    .trim(from: isBipolar ? min(0.375, shown * 0.75) : 0,
                          to: isBipolar ? max(0.375, shown * 0.75) : max(0.004, shown * 0.75))
                    .stroke(tint, style: StrokeStyle(lineWidth: 2.5, lineCap: .butt))
                    .rotationEffect(.degrees(135))
                Rectangle()
                    .fill(Color.white)
                    .frame(width: 2, height: 9)
                    .offset(y: -5)
                    .rotationEffect(.degrees(-135 + shown * 270))
            }
            .frame(width: 26, height: 26)
            Text(caption)
                .font(.system(size: 8).monospaced())
                .foregroundStyle(.secondary)
                .lineLimit(1).minimumScaleFactor(0.6)
        }
        .contentShape(Rectangle())
        .gesture(DragGesture(minimumDistance: 0)
            .onChanged { gesture in
                let start = dragStart ?? shown
                dragStart = start
                let next = max(0, min(1, start - Double(gesture.translation.height) / 140))
                live = next
                onChanged(next)
            }
            .onEnded { _ in
                onCommitted(shown)
                dragStart = nil
                live = nil
            })
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(caption)
        .accessibilityValue("\(Int((shown * 100).rounded())) percent")
        .accessibilityAdjustableAction { direction in
            switch direction {
            case .increment:
                onChanged(min(1, value + 0.05))
            case .decrement:
                onChanged(max(0, value - 0.05))
            @unknown default:
                break
            }
        }
    }
}
