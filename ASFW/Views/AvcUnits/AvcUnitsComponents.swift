import SwiftUI

/// Colours the AV/C Units screen uses for the same thing everywhere: a stream's direction, MIDI, sync.
enum AvcPalette {
    /// Host → device (playback).
    static let playback = Color.blue
    /// Device → host (capture).
    static let capture = Color.green
    static let midi = Color.orange
    static let sync = Color.teal
    static let control = Color.purple
    static let neutral = Color.secondary
}

/// A rounded panel on the window background: the screen's basic container.
struct AvcCard<Content: View>: View {
    var padding: CGFloat = 16
    @ViewBuilder var content: Content

    var body: some View {
        content
            .padding(padding)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color(nsColor: .controlBackgroundColor), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(Color.primary.opacity(0.07), lineWidth: 1))
    }
}

/// A card title: an icon in a tinted square, the title, an optional caption and trailing content.
struct AvcSectionHeader<Trailing: View>: View {
    let title: String
    var caption: String?
    var systemImage: String?
    var tint: Color = .accentColor
    @ViewBuilder var trailing: Trailing

    var body: some View {
        HStack(spacing: 10) {
            if let systemImage {
                Image(systemName: systemImage)
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(tint)
                    .frame(width: 28, height: 28)
                    .background(tint.opacity(0.14), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
            }
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.headline)
                if let caption { Text(caption).font(.caption).foregroundStyle(.secondary) }
            }
            Spacer(minLength: 8)
            trailing
        }
    }
}

extension AvcSectionHeader where Trailing == EmptyView {
    init(title: String, caption: String? = nil, systemImage: String? = nil, tint: Color = .accentColor) {
        self.init(title: title, caption: caption, systemImage: systemImage, tint: tint) { EmptyView() }
    }
}

/// A small pill of text.
struct AvcChip: View {
    let text: String
    var tint: Color = .secondary
    var filled = false
    var monospaced = false

    var body: some View {
        Text(text)
            .font(monospaced ? .system(.caption, design: .monospaced) : .caption)
            .fontWeight(filled ? .semibold : .regular)
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .foregroundStyle(filled ? Color.white : tint)
            .background(filled ? tint : tint.opacity(0.14), in: Capsule())
    }
}

/// A headline number with a label, for the strip under the device header.
struct AvcStatTile<Footer: View>: View {
    let title: String
    let value: String
    var unit: String?
    var systemImage: String
    var tint: Color
    @ViewBuilder var footer: Footer

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(title, systemImage: systemImage)
                .font(.caption)
                .fontWeight(.medium)
                .foregroundStyle(tint)
                .labelStyle(.titleAndIcon)
            HStack(alignment: .firstTextBaseline, spacing: 4) {
                // A number is large; a phrase ("bus + external") is smaller so it stays on one line.
                Text(value)
                    .font(.system(size: value.count > 6 ? 19 : 26, weight: .semibold, design: .rounded))
                    .monospacedDigit()
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                if let unit { Text(unit).font(.callout).foregroundStyle(.secondary) }
            }
            footer
        }
        .padding(14)
        .frame(maxWidth: .infinity, minHeight: 104, maxHeight: .infinity, alignment: .topLeading)
        .background(tint.opacity(0.08), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).strokeBorder(tint.opacity(0.18), lineWidth: 1))
    }
}

extension AvcStatTile where Footer == EmptyView {
    init(title: String, value: String, unit: String? = nil, systemImage: String, tint: Color) {
        self.init(title: title, value: value, unit: unit, systemImage: systemImage, tint: tint) { EmptyView() }
    }
}

/// A status pill with a coloured dot.
struct AvcStatusPill: View {
    let text: String
    let tint: Color

    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(tint).frame(width: 7, height: 7)
            Text(text).font(.caption).fontWeight(.medium)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(tint.opacity(0.14), in: Capsule())
        .foregroundStyle(tint)
    }
}

/// A line of wrapped chips.
struct AvcFlow<Content: View>: View {
    var spacing: CGFloat = 6
    @ViewBuilder var content: Content

    var body: some View {
        FlowLayout(spacing: spacing) { content }
    }
}

/// Lays children out left to right and wraps them.
struct FlowLayout: Layout {
    var spacing: CGFloat = 6

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width = proposal.width ?? .infinity
        var x: CGFloat = 0, y: CGFloat = 0, rowHeight: CGFloat = 0, maxX: CGFloat = 0
        for view in subviews {
            let size = view.sizeThatFits(.unspecified)
            if x > 0, x + size.width > width { x = 0; y += rowHeight + spacing; rowHeight = 0 }
            x += size.width + spacing
            rowHeight = max(rowHeight, size.height)
            maxX = max(maxX, x - spacing)
        }
        return CGSize(width: maxX, height: y + rowHeight)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x = bounds.minX, y = bounds.minY, rowHeight: CGFloat = 0
        for view in subviews {
            let size = view.sizeThatFits(.unspecified)
            if x > bounds.minX, x + size.width > bounds.maxX { x = bounds.minX; y += rowHeight + spacing; rowHeight = 0 }
            view.place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(size))
            x += size.width + spacing
            rowHeight = max(rowHeight, size.height)
        }
    }
}

/// "Key  value" rows of a fact list.
struct AvcFact: View {
    let label: String
    let value: String
    var monospaced = false

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label).foregroundStyle(.secondary)
            Spacer(minLength: 12)
            Text(value)
                .font(monospaced ? .system(.callout, design: .monospaced) : .callout)
                .multilineTextAlignment(.trailing)
                .textSelection(.enabled)
        }
        .font(.callout)
    }
}

/// The row of tabs under the device header. Built from plain views, so it looks the same everywhere and renders offscreen.
struct AvcTabBar: View {
    @Binding var selection: AvcUnitTab

    var body: some View {
        HStack(spacing: 4) {
            ForEach(AvcUnitTab.allCases) { tab in
                let selected = tab == selection
                Button { selection = tab } label: {
                    Label(tab.rawValue, systemImage: tab.systemImage)
                        .font(.callout)
                        .fontWeight(selected ? .semibold : .regular)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 7)
                        .foregroundStyle(selected ? Color.primary : Color.secondary)
                        .background(selected ? Color.primary.opacity(0.12) : Color.clear, in: Capsule())
                        .contentShape(Capsule())
                }
                .buttonStyle(.plain)
            }
            Spacer(minLength: 0)
        }
        .padding(4)
        .background(Color.primary.opacity(0.05), in: Capsule())
        .fixedSize(horizontal: true, vertical: false)
    }
}
