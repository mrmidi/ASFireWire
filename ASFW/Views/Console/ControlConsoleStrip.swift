import SwiftUI

/// One console strip: menus, buttons and rotaries on top, faders below, all
/// chosen by each parameter's domain and semantic.
struct ControlConsoleStrip: View {
    let strip: ControlConsoleLayout.Strip
    let snapshot: ControlSnapshot
    @ObservedObject var model: DeviceControlsViewModel
    let tint: Color
    let metrics: ControlConsoleMetrics

    var body: some View {
        let content = ControlConsoleStripContent(strip: strip, snapshot: snapshot)
        AudioConsoleStripShell(title: strip.title.uppercased(), tint: tint,
                               width: 142 + CGFloat(max(0, content.faders.count - 1)) * 58) {
            AudioConsoleStripSlots(controlHeight: metrics.controlHeight,
                                   faderHeight: ControlConsoleMetrics.faderBlockHeight) {
                controls(content)
            } faderBlock: {
                faders(content)
            }
            failure(content)
        }
    }

    @ViewBuilder private func controls(_ content: ControlConsoleStripContent) -> some View {
        VStack(spacing: 6) {
            ForEach(content.menus) { parameter in
                ControlConsoleMenu(parameter: parameter, state: model.writeState(parameter.id)) { value in
                    Task { await model.write(parameter.id, .int(value)) }
                }
            }
            ForEach(content.routers) { router in
                ControlConsoleRouteMenu(router: router, snapshot: snapshot, model: model)
            }
            let rows = stride(from: 0, to: content.toggles.count, by: 2).map {
                Array(content.toggles[$0..<min($0 + 2, content.toggles.count)])
            }
            ForEach(rows, id: \.first?.id) { row in
                HStack(spacing: 6) {
                    ForEach(row) { parameter in toggle(parameter) }
                }
            }
            if !content.knobs.isEmpty {
                HStack(spacing: 10) {
                    ForEach(content.knobs) { parameter in knob(parameter) }
                }
            }
            ForEach(content.actions) { parameter in
                AudioConsoleStripToggle(title: parameter.name.uppercased(), accessibilityLabel: parameter.name,
                                        isOn: false, isEnabled: parameter.isWritable && !model.writeState(parameter.id).isWriting,
                                        tint: tint) {
                    Task { await model.write(parameter.id, .action) }
                }
            }
        }
    }

    @ViewBuilder private func faders(_ content: ControlConsoleStripContent) -> some View {
        if content.faders.isEmpty {
            Color.clear
        } else {
            HStack(alignment: .bottom, spacing: 4) {
                ForEach(content.faders) { parameter in
                    ControlConsoleFader(parameter: parameter, state: model.writeState(parameter.id),
                                        label: content.faders.count > 1 ? Self.shortName(parameter) : nil,
                                        showsScale: parameter.id == content.faders.last?.id) { value in
                        Task { await model.write(parameter.id, .double(value)) }
                    }
                }
            }
        }
    }

    @ViewBuilder private func failure(_ content: ControlConsoleStripContent) -> some View {
        let ids = strip.parameters + strip.routers.map { $0 | 0x8000_0000 }
        if let text = ids.lazy.compactMap({ id -> String? in
            if case .failed(let text) = model.writeState(id) { text } else { nil }
        }).first {
            Text(text)
                .font(.system(size: 9))
                .foregroundStyle(.red)
                .lineLimit(4)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func toggle(_ parameter: ControlParameter) -> some View {
        let isOn = parameter.value?.boolValue ?? false
        return AudioConsoleStripToggle(
            title: Self.toggleTitle(parameter),
            accessibilityLabel: parameter.name,
            isOn: isOn,
            isEnabled: parameter.isWritable && parameter.value != nil && !model.writeState(parameter.id).isWriting,
            tint: Self.toggleTint(parameter.semantic, fallback: tint)
        ) {
            Task { await model.write(parameter.id, .bool(!isOn)) }
        }
        .help(parameter.name)
    }

    @ViewBuilder private func knob(_ parameter: ControlParameter) -> some View {
        if case .scalar(let domain) = parameter.effectiveDomain, let range = domain.range {
            let span = range.upperBound - range.lowerBound
            let position = ((parameter.value?.doubleValue ?? range.lowerBound) - range.lowerBound) / span
            AudioTopologyKnob(value: position, tint: tint, caption: Self.shortName(parameter).uppercased(),
                              isBipolar: range.lowerBound < 0 && range.upperBound > 0,
                              onCommitted: { position in
                                  Task { await model.write(parameter.id, .double(range.lowerBound + position * span)) }
                              }, onChanged: { _ in })
                .disabled(!parameter.isWritable)
        }
    }

    // MARK: Words

    static func toggleTitle(_ parameter: ControlParameter) -> String {
        switch parameter.semantic {
        case .phantomPower: "+48V"
        case .phaseInvert: "Ø"
        case .mute: "MUTE"
        case .solo: "SOLO"
        case .dim: "DIM"
        case .highPassFilter: "HPF"
        default: shortName(parameter).uppercased()
        }
    }

    static func toggleTint(_ semantic: ControlSemantic, fallback: Color) -> Color {
        switch semantic {
        case .mute: .red
        case .phantomPower: .orange
        case .solo: .yellow
        default: fallback
        }
    }

    /// A parameter's name without the channel number the strip already shows.
    static func shortName(_ parameter: ControlParameter) -> String {
        let words = parameter.name.split(separator: " ")
        guard words.count > 1, let last = words.last, last.allSatisfy(\.isNumber) else { return parameter.name }
        return words.dropLast().joined(separator: " ")
    }
}

/// A mixer input as a strip: one send fader per mixer output.
struct ControlConsoleMixerStrip: View {
    let source: ControlConsoleLayout.MixerSource
    let snapshot: ControlSnapshot
    @ObservedObject var model: DeviceControlsViewModel
    let metrics: ControlConsoleMetrics

    var body: some View {
        AudioConsoleStripShell(title: source.title.uppercased(), tint: .purple,
                               width: 56 + CGFloat(source.sends.count) * 50) {
            AudioConsoleStripSlots(controlHeight: metrics.controlHeight,
                                   faderHeight: ControlConsoleMetrics.faderBlockHeight) {
                Text("SENDS")
                    .font(.caption2.monospaced().bold())
                    .foregroundStyle(.secondary)
            } faderBlock: {
                HStack(alignment: .bottom, spacing: 4) {
                    ForEach(source.sends) { send in
                        if let parameter = snapshot.parameters.first(where: { $0.id == send.parameter }) {
                            ControlConsoleFader(parameter: parameter, state: model.writeState(parameter.id),
                                                label: send.label, showsScale: send.id == source.sends.last?.id,
                                                accessibilityName: "\(source.title) to \(send.destination)") { value in
                                Task { await model.write(parameter.id, .double(value)) }
                            }
                            .help("\(source.title) → \(send.destination)")
                        }
                    }
                }
            }
            let failure = source.sends.lazy.compactMap { send -> String? in
                if case .failed(let text) = model.writeState(send.parameter) { text } else { nil }
            }.first
            if let failure {
                Text(failure).font(.system(size: 9)).foregroundStyle(.red).lineLimit(4)
            }
        }
    }
}

extension DeviceControlsViewModel.WriteState {
    var isWriting: Bool { if case .writing = self { true } else { false } }
}
