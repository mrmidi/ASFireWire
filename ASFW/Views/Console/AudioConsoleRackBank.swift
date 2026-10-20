import SwiftUI

/// A labelled group of aligned hardware strips within a horizontal rack.
struct AudioConsoleRackBank<Content: View, Accessory: View>: View {
    let title: String
    let tint: Color
    @ViewBuilder let accessory: () -> Accessory
    @ViewBuilder let content: () -> Content

    init(title: String, tint: Color, @ViewBuilder content: @escaping () -> Content) where Accessory == EmptyView {
        self.title = title
        self.tint = tint
        self.accessory = { EmptyView() }
        self.content = content
    }

    init(title: String, tint: Color,
         @ViewBuilder accessory: @escaping () -> Accessory,
         @ViewBuilder content: @escaping () -> Content) {
        self.title = title
        self.tint = tint
        self.accessory = accessory
        self.content = content
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Text(title)
                    .font(.system(size: 10).monospaced().bold())
                    .foregroundStyle(tint)
                accessory()
            }
            HStack(alignment: .top, spacing: 6, content: content)
        }
    }
}
