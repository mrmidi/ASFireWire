import SwiftUI

/// A scalar parameter as a console fader with its scale and readout. The
/// value is sent once, when the drag ends; until the device answers, the
/// fader holds the sent value, then follows the confirmed one (a refused
/// write springs back).
struct ControlConsoleFader: View {
    let parameter: ControlParameter
    let state: DeviceControlsViewModel.WriteState
    var label: String?
    var showsScale = true
    var accessibilityName: String?
    let write: (Double) -> Void

    @State private var pending: Double?

    private static let thumb = CGSize(width: 34, height: 22)

    var body: some View {
        VStack(spacing: 5) {
            if let label {
                Text(label).font(.caption2.weight(.bold)).foregroundStyle(.secondary).lineLimit(1)
            }
            HStack(alignment: .bottom, spacing: 4) {
                if case .scalar(let domain) = parameter.effectiveDomain, let range = domain.range {
                    AudioConsoleVerticalFader(
                        title: accessibilityName ?? parameter.name,
                        value: pending ?? parameter.value?.doubleValue ?? range.lowerBound,
                        range: range,
                        step: domain.step ?? (range.upperBound - range.lowerBound) / 100,
                        tint: .white,
                        trackWidth: 7,
                        thumbSize: Self.thumb,
                        fillsTrack: false,
                        isEnabled: parameter.isWritable && parameter.value != nil,
                        valueDescription: { Self.format($0, domain) },
                        onValueCommitted: { value in
                            let snapped = domain.snap(value)
                            pending = snapped
                            write(snapped)
                        })
                    .frame(width: 46, height: ControlConsoleMetrics.faderHeight)
                    if showsScale { ControlConsoleFaderScale(domain: domain, range: range, thumbHeight: Self.thumb.height) }
                } else {
                    Text("No range")
                        .font(.caption2).foregroundStyle(.secondary)
                        .frame(width: 46, height: ControlConsoleMetrics.faderHeight)
                }
            }
            Text(readout)
                .font(.caption.monospacedDigit().weight(.semibold))
                .foregroundStyle(pending == nil ? Color.primary : Color.secondary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
                .padding(.top, 6)
                .accessibilityHidden(true)
        }
        .onChange(of: state) { _, state in
            if !state.isWriting { pending = nil }
        }
        .help(parameter.applicable ? parameter.name : "\(parameter.name): not applicable in the current mode")
    }

    private var readout: String {
        guard parameter.applicable else { return "n/a" }
        guard case .scalar(let domain) = parameter.effectiveDomain,
              let value = pending ?? parameter.value?.doubleValue else { return "—" }
        return Self.format(value, domain)
    }

    /// dB and other units as numbers; unitless levels as a percentage of their range.
    static func format(_ value: Double, _ domain: ControlScalarDomain) -> String {
        switch domain.unit {
        case "generic", "normalized":
            guard let range = domain.range else { return String(format: "%.2f", value) }
            let position = (value - range.lowerBound) / (range.upperBound - range.lowerBound)
            return "\(Int((position * 100).rounded()))%"
        default:
            let decimals = (domain.step ?? 1) >= 1 ? 0 : 1
            // No "-0 dB": a value that rounds to zero prints as zero.
            let shown = abs(value) < (decimals == 0 ? 0.5 : 0.05) ? 0 : value
            return String(format: "%.\(decimals)f %@", shown, domain.unit)
        }
    }
}

/// Marks beside a fader, placed where the thumb's centre sits for each value.
struct ControlConsoleFaderScale: View {
    let domain: ControlScalarDomain
    let range: ClosedRange<Double>
    let thumbHeight: CGFloat

    var body: some View {
        let height = ControlConsoleMetrics.faderHeight
        ZStack(alignment: .topTrailing) {
            ForEach(marks, id: \.position) { mark in
                Text(mark.label)
                    .font(.system(size: 8).monospacedDigit())
                    .foregroundStyle(.secondary)
                    .offset(y: thumbHeight / 2 + (1 - mark.position) * (height - thumbHeight) - 5)
            }
        }
        .frame(width: 24, height: height, alignment: .topTrailing)
        .accessibilityHidden(true)
    }

    private var marks: [(label: String, position: Double)] {
        switch domain.unit {
        case "generic", "normalized":
            return [("100", 1), ("50", 0.5), ("0", 0)]
        default:
            let span = range.upperBound - range.lowerBound
            return (0...4).map { index in
                let position = 1 - Double(index) / 4
                return (String(format: "%.0f", range.lowerBound + span * position), position)
            }
        }
    }
}

/// An enumerated parameter as a captioned menu.
struct ControlConsoleMenu: View {
    let parameter: ControlParameter
    let state: DeviceControlsViewModel.WriteState
    let write: (Int64) -> Void

    var body: some View {
        if case .enumeration(let items) = parameter.effectiveDomain {
            let current = items.first { $0.value == parameter.value?.intValue }
            ControlConsoleMenuButton(caption: ControlConsoleStrip.shortName(parameter),
                                     value: current?.name ?? "—",
                                     isEnabled: parameter.isWritable && !state.isWriting) {
                ForEach(items, id: \.value) { item in
                    Button(item.name) { write(item.value) }
                        .disabled(item.value == current?.value)
                }
            }
        }
    }
}

/// A writable router no parameter speaks for: its legal bundles as a menu.
struct ControlConsoleRouteMenu: View {
    let router: ControlNode
    let snapshot: ControlSnapshot
    @ObservedObject var model: DeviceControlsViewModel

    var body: some View {
        let active = router.bundles.first { router.activeBundles.contains($0.id) }
        ControlConsoleMenuButton(caption: router.name, value: active.map(label) ?? "—",
                                 isEnabled: !model.writeState(router.id | 0x8000_0000).isWriting) {
            ForEach(router.bundles) { bundle in
                if let route = bundle.routes.first {
                    Button(label(bundle)) {
                        Task { await model.writeRoute(router: router.id, output: route.output, input: route.input) }
                    }
                    .disabled(bundle.id == active?.id)
                }
            }
        }
    }

    private func label(_ bundle: ControlBundle) -> String {
        let names = bundle.routes.compactMap { snapshot.port($0.input)?.name }
        return names.isEmpty ? "Input \(bundle.id)" : names.joined(separator: ", ")
    }
}

/// The console's menu chrome: a small caption over a value button.
struct ControlConsoleMenuButton<Items: View>: View {
    let caption: String
    let value: String
    let isEnabled: Bool
    @ViewBuilder let items: () -> Items

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(caption.uppercased())
                .font(.system(size: 8, weight: .semibold).monospaced())
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            Menu {
                items()
            } label: {
                HStack(spacing: 4) {
                    Text(value).lineLimit(1).minimumScaleFactor(0.75)
                    Spacer(minLength: 0)
                    Image(systemName: "chevron.down").imageScale(.small)
                }
                .font(.caption.weight(.semibold))
                .padding(.horizontal, 7)
                .frame(maxWidth: .infinity, minHeight: 24)
                .background(Color.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 5, style: .continuous))
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .disabled(!isEnabled)
            .accessibilityLabel(caption)
            .accessibilityValue(value)
        }
        .frame(height: ControlConsoleMetrics.menuRow - 6)
    }
}
