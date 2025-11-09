import SwiftUI

/// Apply a sensible bounded frame for placeholder or empty-state views.
/// Use `.modifier(BoundedPlaceholder(minHeight: 120, maxHeight: 400))` to be consistent across the app.
struct BoundedPlaceholder: ViewModifier {
    var minHeight: CGFloat = 120
    var maxHeight: CGFloat? = nil

    func body(content: Content) -> some View {
        if let maxH = maxHeight {
            content
                .frame(maxWidth: .infinity, minHeight: minHeight, maxHeight: maxH)
        } else {
            content
                .frame(maxWidth: .infinity, minHeight: minHeight)
        }
    }
}

extension View {
    func boundedPlaceholder(minHeight: CGFloat = 120, maxHeight: CGFloat? = nil) -> some View {
        modifier(BoundedPlaceholder(minHeight: minHeight, maxHeight: maxHeight))
    }
}
