import SwiftUI

/// The interaction and drawing contract shared by hardware-console faders.
/// Device wrappers map their register units onto this 0-based scalar control
/// and decide whether writes are continuous or committed at the end of a drag.
struct AudioConsoleVerticalFader: View {
    let title: String
    let value: Double
    let range: ClosedRange<Double>
    let step: Double
    let tint: Color
    let trackWidth: CGFloat
    let thumbSize: CGSize
    let fillsTrack: Bool
    let isEnabled: Bool
    let valueDescription: (Double) -> String
    let onValueChanged: (Double) -> Void
    let onValueCommitted: (Double) -> Void

    @State private var shownValue: Double
    @State private var dragStartValue: Double?

    init(
        title: String,
        value: Double,
        range: ClosedRange<Double>,
        step: Double,
        tint: Color,
        trackWidth: CGFloat,
        thumbSize: CGSize,
        fillsTrack: Bool,
        isEnabled: Bool = true,
        valueDescription: @escaping (Double) -> String,
        onValueChanged: @escaping (Double) -> Void = { _ in },
        onValueCommitted: @escaping (Double) -> Void = { _ in }
    ) {
        self.title = title
        self.value = value
        self.range = range
        self.step = step
        self.tint = tint
        self.trackWidth = trackWidth
        self.thumbSize = thumbSize
        self.fillsTrack = fillsTrack
        self.isEnabled = isEnabled
        self.valueDescription = valueDescription
        self.onValueChanged = onValueChanged
        self.onValueCommitted = onValueCommitted
        _shownValue = State(initialValue: value)
    }

    var body: some View {
        GeometryReader { geometry in
            let travel = max(12, geometry.size.height - thumbSize.height)
            let thumbOffset = (1 - normalizedValue) * travel

            ZStack(alignment: .top) {
                track(height: travel)
                    .offset(y: thumbSize.height / 2)

                thumb
                    .offset(y: thumbOffset)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .contentShape(Rectangle())
            .gesture(dragGesture(travel: travel))
        }
        .frame(width: max(trackWidth, thumbSize.width))
        .opacity(isEnabled ? 1 : 0.45)
        .onChange(of: value) { _, updatedValue in
            guard dragStartValue == nil else { return }
            shownValue = clamped(updatedValue)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(title)
        .accessibilityValue(valueDescription(shownValue))
        .accessibilityAdjustableAction(adjustAccessibilityValue)
    }

    private var normalizedValue: CGFloat {
        let span = range.upperBound - range.lowerBound
        guard span > 0 else { return 0 }
        return CGFloat(min(1, max(0, (shownValue - range.lowerBound) / span)))
    }

    private var thumb: some View {
        RoundedRectangle(cornerRadius: min(4, thumbSize.height / 4), style: .continuous)
            .fill(LinearGradient(
                colors: [Color(white: 0.96), Color(white: 0.72), Color(white: 0.9)],
                startPoint: .top,
                endPoint: .bottom
            ))
            .frame(width: thumbSize.width, height: thumbSize.height)
            .overlay {
                Rectangle()
                    .fill(tint.opacity(isEnabled ? 0.9 : 0.3))
                    .frame(width: max(2, thumbSize.width - 12), height: 2)
            }
            .shadow(color: .black.opacity(0.3), radius: 2, y: 1)
    }

    private func track(height: CGFloat) -> some View {
        ZStack(alignment: .bottom) {
            Capsule()
                .fill(.black.opacity(0.32))
                .overlay(Capsule().strokeBorder(.white.opacity(0.15)))

            if fillsTrack {
                Capsule()
                    .fill(tint.opacity(isEnabled ? 0.55 : 0.2))
                    .frame(height: max(0, height * normalizedValue))
            }
        }
        .frame(width: trackWidth, height: height)
    }

    private func dragGesture(travel: CGFloat) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { gesture in
                guard isEnabled else { return }
                let start = dragStartValue ?? shownValue
                dragStartValue = start
                let span = range.upperBound - range.lowerBound
                let next = start - Double(gesture.translation.height / travel) * span
                shownValue = clamped(next)
                onValueChanged(shownValue)
            }
            .onEnded { _ in
                guard isEnabled else {
                    dragStartValue = nil
                    return
                }
                shownValue = quantized(shownValue)
                dragStartValue = nil
                onValueCommitted(shownValue)
            }
    }

    private func adjustAccessibilityValue(_ direction: AccessibilityAdjustmentDirection) {
        guard isEnabled else { return }
        let delta: Double
        switch direction {
        case .increment:
            delta = step
        case .decrement:
            delta = -step
        @unknown default:
            return
        }
        shownValue = quantized(shownValue + delta)
        onValueChanged(shownValue)
        onValueCommitted(shownValue)
    }

    private func clamped(_ candidate: Double) -> Double {
        min(range.upperBound, max(range.lowerBound, candidate))
    }

    private func quantized(_ candidate: Double) -> Double {
        guard step > 0 else { return clamped(candidate) }
        let offset = ((candidate - range.lowerBound) / step).rounded() * step
        return clamped(range.lowerBound + offset)
    }
}
