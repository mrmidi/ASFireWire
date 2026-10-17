import SwiftUI

/// Device Controls as a hardware console, laid out by `ControlConsoleLayout`
/// from whatever the device publishes. Values are the driver's; a moved
/// control shows its sent value until the device confirms or refuses it.
struct ControlConsoleView: View {
    let snapshot: ControlSnapshot
    @ObservedObject var model: DeviceControlsViewModel
    /// False only to render the rack offscreen (ImageRenderer draws no scroll view content).
    var scrolls = true

    var body: some View {
        let layout = ControlConsoleLayout(snapshot)
        let metrics = ControlConsoleMetrics(layout: layout, snapshot: snapshot)
        VStack(alignment: .leading, spacing: 16) {
            if !layout.isEmpty {
                Group {
                    if scrolls {
                        ScrollView(.horizontal, showsIndicators: true) { rack(layout, metrics: metrics) }
                    } else {
                        rack(layout, metrics: metrics)
                    }
                }
                .background(Color(white: 0.07), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(.white.opacity(0.08)))
                .environment(\.colorScheme, .dark)
            }
            let settings = layout.settings.compactMap { id in snapshot.parameters.first { $0.id == id } }
            if !settings.isEmpty {
                ControlConsoleSettings(parameters: settings, model: model)
            }
        }
    }

    private func rack(_ layout: ControlConsoleLayout, metrics: ControlConsoleMetrics) -> some View {
        HStack(alignment: .top, spacing: 14) {
            ForEach(Array(Self.sections(layout).enumerated()), id: \.element.id) { index, section in
                if index > 0 { AudioConsoleRackDivider() }
                sectionView(section, metrics: metrics)
            }
        }
        .padding(14)
    }

    // MARK: Sections

    enum Section: Identifiable {
        case strips(title: String, tint: Color, strips: [ControlConsoleLayout.Strip])
        case mixer(ControlConsoleLayout.Mixer)

        var id: String {
            switch self {
            case .strips(let title, _, _): "strips:\(title)"
            case .mixer(let mixer): "mixer:\(mixer.id)"
            }
        }
    }

    static func sections(_ layout: ControlConsoleLayout) -> [Section] {
        var sections: [Section] = []
        if !layout.inputs.isEmpty { sections.append(.strips(title: "INPUTS", tint: .cyan, strips: layout.inputs)) }
        sections += layout.mixers.map(Section.mixer)
        if !layout.outputs.isEmpty { sections.append(.strips(title: "OUTPUTS", tint: .orange, strips: layout.outputs)) }
        if !layout.other.isEmpty { sections.append(.strips(title: "OTHER", tint: .gray, strips: layout.other)) }
        return sections
    }

    @ViewBuilder private func sectionView(_ section: Section, metrics: ControlConsoleMetrics) -> some View {
        switch section {
        case .strips(let title, let tint, let strips):
            AudioConsoleRackBank(title: title, tint: tint) {
                ForEach(strips) { strip in
                    ControlConsoleStrip(strip: strip, snapshot: snapshot, model: model, tint: tint, metrics: metrics)
                }
            }
        case .mixer(let mixer):
            AudioConsoleRackBank(title: mixer.title.uppercased(), tint: .purple) {
                ForEach(mixer.sources) { source in
                    ControlConsoleMixerStrip(source: source, snapshot: snapshot, model: model, metrics: metrics)
                }
            }
        }
    }
}

/// Shared strip geometry, so every strip's fader block starts at the same height.
struct ControlConsoleMetrics {
    static let menuRow: CGFloat = 40
    static let toggleRow: CGFloat = 26
    static let knobRow: CGFloat = 46
    static let faderHeight: CGFloat = 168
    static let faderBlockHeight: CGFloat = 210

    let controlHeight: CGFloat

    init(layout: ControlConsoleLayout, snapshot: ControlSnapshot) {
        let heights = (layout.inputs + layout.outputs + layout.other).map { strip in
            ControlConsoleStripContent(strip: strip, snapshot: snapshot).controlHeight
        }
        controlHeight = max(Self.toggleRow, heights.max() ?? 0)
    }
}

/// A strip's parameters sorted into the console's control kinds.
struct ControlConsoleStripContent {
    let menus: [ControlParameter]
    let toggles: [ControlParameter]
    let knobs: [ControlParameter]
    let faders: [ControlParameter]
    let actions: [ControlParameter]
    let routers: [ControlNode]

    init(strip: ControlConsoleLayout.Strip, snapshot: ControlSnapshot) {
        let parameters = strip.parameters.compactMap { id in snapshot.parameters.first { $0.id == id } }
        var menus: [ControlParameter] = [], toggles: [ControlParameter] = [], knobs: [ControlParameter] = []
        var faders: [ControlParameter] = [], actions: [ControlParameter] = []
        for parameter in parameters {
            switch parameter.effectiveDomain {
            case .enumeration: menus.append(parameter)
            case .bool: toggles.append(parameter)
            case .action: actions.append(parameter)
            case .scalar:
                if parameter.semantic == .pan || parameter.semantic == .balance { knobs.append(parameter) }
                else { faders.append(parameter) }
            }
        }
        // Mute sits last, nearest the fader, as on a hardware strip.
        toggles.sort { ($0.semantic == .mute ? 1 : 0) < ($1.semantic == .mute ? 1 : 0) }
        self.menus = menus
        self.toggles = toggles
        self.knobs = knobs
        self.faders = faders
        self.actions = actions
        routers = strip.routers.compactMap(snapshot.node)
    }

    var controlHeight: CGFloat {
        CGFloat(menus.count + routers.count) * ControlConsoleMetrics.menuRow
            + CGFloat((toggles.count + 1) / 2 + actions.count) * ControlConsoleMetrics.toggleRow
            + (knobs.isEmpty ? 0 : ControlConsoleMetrics.knobRow)
    }
}

// MARK: - Device settings

/// Device-wide settings (front panel, display, clickless switching): the
/// parameters no strip owns, as plain rows.
private struct ControlConsoleSettings: View {
    let parameters: [ControlParameter]
    @ObservedObject var model: DeviceControlsViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label("Device Settings", systemImage: "gearshape").font(.headline)
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 260), spacing: 18, alignment: .top)],
                      alignment: .leading, spacing: 12) {
                ForEach(parameters) { parameter in
                    ParameterRow(parameter: parameter, state: model.writeState(parameter.id)) { value in
                        Task { await model.write(parameter.id, value) }
                    }
                }
            }
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
    }
}
