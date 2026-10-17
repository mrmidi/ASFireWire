import SwiftUI

/// The shared hardware-strip chrome. Derived from the M-Audio console so every
/// family gets the same aligned header, border, and selected-state treatment.
struct AudioConsoleStripShell<Content: View>: View {
    let title: String
    let tint: Color
    let width: CGFloat
    var borderTint: Color? = nil
    var isDimmed = false
    @ViewBuilder let content: () -> Content

    var body: some View {
        VStack(spacing: 6) {
            Text(title)
                .font(.system(size: 10, weight: .bold).monospaced())
                .lineLimit(1)
                .minimumScaleFactor(0.65)
                .foregroundStyle(tint)
                .frame(maxWidth: .infinity, minHeight: 24)
                .background(tint.opacity(0.2))
                .clipShape(RoundedRectangle(cornerRadius: 4))

            content()
        }
        .padding(8)
        .frame(width: width)
        .frame(maxHeight: .infinity, alignment: .top)
        .background(
            RoundedRectangle(cornerRadius: 9)
                .fill(Color(white: 0.13))
                .overlay {
                    RoundedRectangle(cornerRadius: 9)
                        .strokeBorder(borderTint?.opacity(0.8) ?? tint.opacity(0.28),
                                      lineWidth: borderTint == nil ? 1 : 2)
                }
        )
        .opacity(isDimmed ? 0.5 : 1)
    }
}
