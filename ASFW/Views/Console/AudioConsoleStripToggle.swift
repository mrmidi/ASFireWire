import SwiftUI

/// The compact hardware-button treatment shared by console strips.
struct AudioConsoleStripToggle: View {
    let title: String
    let accessibilityLabel: String
    let isOn: Bool
    let isEnabled: Bool
    let tint: Color
    let action: () -> Void

    var body: some View {
        Button(title, action: action)
            .buttonStyle(.plain)
            .font(.system(size: 10, weight: .bold))
            .frame(maxWidth: .infinity, minHeight: 20)
            .background(isOn ? tint : Color.white.opacity(0.08))
            .foregroundStyle(isOn ? Color.black : Color.secondary)
            .clipShape(RoundedRectangle(cornerRadius: 4))
            .disabled(!isEnabled)
            .accessibilityLabel(accessibilityLabel)
            .accessibilityValue(isOn ? "On" : "Off")
            .accessibilityAddTraits(isOn ? .isSelected : [])
    }
}
