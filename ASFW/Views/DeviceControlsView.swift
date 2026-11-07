import SwiftUI

/// How Device Controls draws a snapshot: as a console, or as one card per
/// node of the driver's model (every parameter, provenance included).
enum DeviceControlsPresentation: String, CaseIterable, Identifiable {
    case console = "Console"
    case parameters = "Parameters"
    var id: String { rawValue }
}

/// Device Controls: the selected device's control snapshot. Values shown are
/// the driver's; after a write, the one the hardware confirmed. Nothing here
/// polls while idle.
struct DeviceControlsView: View {
    @ObservedObject var model: DeviceControlsViewModel
    @AppStorage("deviceControls.presentation") private var presentation = DeviceControlsPresentation.console

    var body: some View {
        ScrollView {
            DeviceControlsContent(model: model, presentation: presentation)
                .frame(maxWidth: presentation == .console ? .infinity : 1120)
                .padding(24)
                .frame(maxWidth: .infinity)
        }
        .navigationTitle("Device Controls")
        .toolbar {
            ToolbarItemGroup {
                Picker("Presentation", selection: $presentation) {
                    ForEach(DeviceControlsPresentation.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .help("Console: the device as hardware strips. Parameters: every control of the driver's model, by block.")
                if let updated = model.lastUpdated {
                    Text("Updated \(updated.formatted(date: .omitted, time: .standard))")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Button { model.reload() } label: { Label("Refresh", systemImage: "arrow.clockwise") }
                    .help("Read the driver's current control state. Sends nothing to the device.")
                    .disabled(!model.isConnected)
            }
        }
        .task { model.reload() }
        .onChange(of: model.isConnected) { _, _ in model.reload() }
    }
}

/// The screen's content without the scroll view and toolbar (so it can be rendered on its own).
struct DeviceControlsContent: View {
    @ObservedObject var model: DeviceControlsViewModel
    var presentation: DeviceControlsPresentation = .parameters
    /// False only to render offscreen (see ControlConsoleView.scrolls).
    var consoleScrolls = true

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            header
            if let message = model.message { banner(message, systemImage: "arrow.triangle.2.circlepath", tint: .orange) }
            content
        }
    }

    // MARK: Header

    private var header: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Label("Device Controls", systemImage: "slider.vertical.3").font(.headline)
                Spacer()
                if model.devices.count > 0 {
                    Picker("Device", selection: Binding(get: { model.selectedGUID },
                                                        set: { model.select($0) })) {
                        ForEach(model.devices) { device in
                            Text(device.title).tag(Optional(device.guid))
                        }
                    }
                    .pickerStyle(.menu)
                    .fixedSize()
                }
            }
            if let device = model.selectedDevice {
                HStack(spacing: 8) {
                    ControlBadge(text: device.guidHex, monospaced: true)
                    if let snapshot = model.snapshot {
                        ControlBadge(text: snapshot.family.uppercased())
                        ControlBadge(text: "revision \(snapshot.revision)", monospaced: true)
                        ControlBadge(text: "\(snapshot.parameters.count) controls")
                    }
                }
            }
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
    }

    // MARK: Content

    @ViewBuilder private var content: some View {
        switch model.content {
        case .notConnected:
            emptyState("cable.connector.slash", "Not connected", "The app is not connected to the ASFW driver.")
        case .idle:
            if model.devices.isEmpty {
                emptyState("hifispeaker.2", "No devices",
                           "Connect a FireWire audio device. It appears here once the driver has discovered it.")
            } else {
                ProgressView().frame(maxWidth: .infinity).padding(.top, 60)
            }
        case .unsupported:
            emptyState("slider.horizontal.below.square.and.square.filled", "No controls yet",
                       "Controls for this device family are not available yet.")
        case .failed(let text):
            banner(text, systemImage: "exclamationmark.triangle.fill", tint: .red)
        case .loaded(let snapshot):
            switch presentation {
            case .console: ControlConsoleView(snapshot: snapshot, model: model, scrolls: consoleScrolls)
            case .parameters: nodes(snapshot)
            }
        }
    }

    @ViewBuilder private func nodes(_ snapshot: ControlSnapshot) -> some View {
        if snapshot.nodes.isEmpty && snapshot.parameters.isEmpty {
            emptyState("slider.horizontal.3", "Nothing to control",
                       "The driver found no controls on this device.")
        }
        ForEach(ControlNode.Kind.allCases, id: \.self) { kind in
            let group = snapshot.nodes.filter { $0.kind == kind }
            if !group.isEmpty {
                VStack(alignment: .leading, spacing: 10) {
                    Text(Self.groupTitle(kind)).font(.title3).fontWeight(.semibold)
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 340), spacing: 14, alignment: .top)],
                              alignment: .leading, spacing: 14) {
                        ForEach(group) { node in
                            switch node.kind {
                            case .router: RouterCard(node: node, snapshot: snapshot, model: model)
                            case .endpoint: ProcessorCard(node: node, snapshot: snapshot, model: model)
                            case .processor, .mixer: ProcessorCard(node: node, snapshot: snapshot, model: model)
                            }
                        }
                    }
                }
            }
        }
        let unowned = snapshot.unownedParameters
        if !unowned.isEmpty {
            ControlCard(title: "Other controls", systemImage: "square.grid.2x2") {
                ForEach(unowned) { parameter in
                    ParameterRow(parameter: parameter, state: model.writeState(parameter.id)) { value in
                        Task { await model.write(parameter.id, value) }
                    }
                }
            }
        }
    }

    static func groupTitle(_ kind: ControlNode.Kind) -> String {
        switch kind {
        case .processor: "Processors"
        case .mixer: "Mixers"
        case .router: "Routing"
        case .endpoint: "Endpoints"
        }
    }

    // MARK: Pieces

    private func banner(_ text: String, systemImage: String, tint: Color) -> some View {
        HStack(spacing: 10) {
            Image(systemName: systemImage).foregroundStyle(tint)
            Text(text).font(.callout)
            Spacer()
        }
        .padding(12)
        .background(tint.opacity(0.12), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
    }

    private func emptyState(_ systemImage: String, _ title: String, _ text: String) -> some View {
        VStack(spacing: 14) {
            Image(systemName: systemImage).font(.system(size: 48)).foregroundStyle(.tertiary)
            Text(title).font(.title3).fontWeight(.semibold)
            Text(text).font(.callout).foregroundStyle(.secondary).multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
        .padding(.top, 70)
    }
}

// MARK: - Cards

/// The screen's card: a material panel with a `Label` header.
private struct ControlCard<Content: View>: View {
    let title: String
    let systemImage: String
    var caption: String?
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Label(title, systemImage: systemImage).font(.headline)
                if let caption { Text(caption).font(.caption).foregroundStyle(.secondary) }
            }
            content
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
    }
}

/// A processor or mixer: its parameters, grouped by what they act on (the
/// whole node first, then each channel's port).
private struct ProcessorCard: View {
    let node: ControlNode
    let snapshot: ControlSnapshot
    @ObservedObject var model: DeviceControlsViewModel

    var body: some View {
        let parameters = snapshot.parameters(of: node.id)
        let groups = Self.groups(parameters)
        ControlCard(title: node.name, systemImage: node.kind == .mixer ? "slider.horizontal.3" : "slider.vertical.3",
                    caption: node.kind == .endpoint ? (node.host == true ? "Host" : "Connectors") : "\(node.outputs.count) channel\(node.outputs.count == 1 ? "" : "s")") {
            if parameters.isEmpty {
                Text("No controls published for this block.").font(.callout).foregroundStyle(.secondary)
            }
            ForEach(groups, id: \.target) { group in
                VStack(alignment: .leading, spacing: 6) {
                    Text(title(of: group.target))
                        .font(.caption).fontWeight(.semibold).foregroundStyle(.secondary).textCase(.uppercase)
                    ForEach(group.parameters) { parameter in
                        ParameterRow(parameter: parameter, state: model.writeState(parameter.id)) { value in
                            Task { await model.write(parameter.id, value) }
                        }
                    }
                }
                if group.target != groups.last?.target { Divider() }
            }
        }
    }

    private func title(of target: ControlTarget) -> String {
        switch target {
        case .node: "Master"
        case .port(let id): snapshot.port(id)?.name ?? "Port \(id)"
        case .crosspoint(let id): "Crosspoint \(id)"
        }
    }

    struct Group { let target: ControlTarget; let parameters: [ControlParameter] }

    /// Consecutive parameters with the same target, in document order.
    static func groups(_ parameters: [ControlParameter]) -> [Group] {
        var groups: [Group] = []
        for parameter in parameters {
            if let last = groups.last, last.target == parameter.target {
                groups[groups.count - 1] = Group(target: last.target, parameters: last.parameters + [parameter])
            } else {
                groups.append(Group(target: parameter.target, parameters: [parameter]))
            }
        }
        return groups
    }
}

/// A router (an AV/C selector): its bundles as a list of inputs, the active one marked. Read-only for now.
private struct RouterCard: View {
    let node: ControlNode
    let snapshot: ControlSnapshot
    @ObservedObject var model: DeviceControlsViewModel

    var body: some View {
        ControlCard(title: node.name, systemImage: "arrow.triangle.branch",
                    caption: node.writable ? "Select an input; the device confirms the active route." : "Read-only routing.") {
            ForEach(snapshot.parameters(of: node.id)) { parameter in
                ParameterRow(parameter: parameter, state: model.writeState(parameter.id)) { value in
                    Task { await model.write(parameter.id, value) }
                }
            }
            if node.bundles.isEmpty {
                Text("No inputs published.").font(.callout).foregroundStyle(.secondary)
            }
            VStack(alignment: .leading, spacing: 6) {
                ForEach(node.bundles) { bundle in
                    let active = node.activeBundles.contains(bundle.id)
                    HStack(spacing: 8) {
                        Image(systemName: active ? "largecircle.fill.circle" : "circle")
                            .foregroundStyle(active ? Color.accentColor : Color.secondary)
                        Text(label(bundle)).fontWeight(active ? .semibold : .regular)
                        Spacer()
                        if active { ControlBadge(text: "active", tint: .accentColor) }
                        if node.writable,
                           !snapshot.parameters(of: node.id).contains(where: { $0.semantic == .nominalLevel && $0.origin == .derived }),
                           let route = bundle.routes.first {
                            Button("Select") {
                                Task { await model.writeRoute(router: node.id, output: route.output, input: route.input) }
                            }
                            .disabled(active || { if case .writing = model.writeState(node.id | 0x8000_0000) { true } else { false } }())
                        }
                    }
                    .font(.callout)
                }
            }
            if case .failed(let message) = model.writeState(node.id | 0x8000_0000) {
                Text(message).font(.caption).foregroundStyle(.red)
            }
            if !node.bundles.isEmpty && node.activeBundles.isEmpty {
                Text("The device did not report the active input.").font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private func label(_ bundle: ControlBundle) -> String {
        let names = bundle.routes.map { route in
            let input = snapshot.port(route.input)?.name ?? "Port \(route.input)"
            // A selector has one output: name it only when there are several.
            guard node.outputs.count > 1 else { return input }
            return "\(input) → \(snapshot.port(route.output)?.name ?? "Port \(route.output)")"
        }
        return names.isEmpty ? "Bundle \(bundle.id)" : names.joined(separator: ", ")
    }
}

private struct EndpointCard: View {
    let node: ControlNode

    var body: some View {
        ControlCard(title: node.name, systemImage: node.host == true ? "desktopcomputer" : "cable.connector") {
            Text(node.host == true ? "Host side" : "Device side").font(.callout).foregroundStyle(.secondary)
        }
    }
}

// MARK: - Parameters

/// One parameter: a slider, toggle or menu by its domain, with its write state and provenance.
struct ParameterRow: View {
    let parameter: ControlParameter
    let state: DeviceControlsViewModel.WriteState
    let write: (ControlValue) -> Void

    /// The slider position while the user drags it; nil otherwise (the slider shows the confirmed value).
    @State private var dragValue: Double?

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            control.disabled(!parameter.isWritable)
            if !parameter.applicable { Text("Not applicable").font(.caption).foregroundStyle(.secondary) }
            else if parameter.access == .readOnly { Text("Read-only").font(.caption).foregroundStyle(.secondary) }
            status
        }
    }

    @ViewBuilder private var control: some View {
        if !parameter.isWritable && parameter.domain != .action {
            HStack {
                Text(Self.shortName(parameter))
                provenance
                Spacer()
                Text(parameter.value.map { Self.describe($0, parameter.effectiveDomain) } ?? "Unknown")
                    .monospacedDigit()
            }
        } else {
            switch parameter.effectiveDomain {
            case .action:
                Button(parameter.name) { write(.action) }
                    .disabled({ if case .writing = state { true } else { false } }())
            case .scalar(let domain):
                scalar(domain)
            case .bool:
                Toggle(isOn: Binding(get: { parameter.value?.boolValue ?? false },
                                     set: { write(.bool($0)) })) {
                    HStack(spacing: 6) {
                        Text(Self.shortName(parameter))
                        provenance
                    }
                }
                .toggleStyle(.switch)
                .controlSize(.small)
                .disabled(parameter.value == nil)
            case .enumeration(let items):
                Picker(selection: Binding(get: { parameter.value?.intValue },
                                          set: { if let value = $0 { write(.int(value)) } })) {
                    if parameter.value == nil { Text("Unknown").tag(Optional<Int64>.none) }
                    ForEach(items, id: \.value) { item in Text(item.name).tag(Optional(item.value)) }
                } label: {
                    HStack(spacing: 6) {
                        Text(Self.shortName(parameter))
                        provenance
                    }
                }
                .pickerStyle(.menu)
            }
        }
    }

    @ViewBuilder private func scalar(_ domain: ControlScalarDomain) -> some View {
        let shown = dragValue.map(domain.snap) ?? parameter.value?.doubleValue
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                Text(Self.shortName(parameter))
                provenance
                Spacer()
                Text(shown.map { Self.format($0, domain) } ?? "—")
                    .monospacedDigit()
                    .foregroundStyle(dragValue == nil ? .primary : .secondary)
            }
            if let range = domain.range {
                Slider(value: Binding(get: { dragValue ?? parameter.value?.doubleValue ?? range.lowerBound },
                                      set: { dragValue = $0 }),
                       in: range) { editing in
                    if editing {
                        dragValue = dragValue ?? parameter.value?.doubleValue
                    } else if let value = dragValue {
                        // Send once, on release; the view model snaps it onto the step grid.
                        dragValue = nil
                        write(.double(value))
                    }
                }
                .controlSize(.small)
                HStack {
                    Text(Self.format(range.lowerBound, domain))
                    Spacer()
                    if let step = domain.step { Text("step \(Self.format(step, domain))") }
                    Spacer()
                    Text(Self.format(range.upperBound, domain))
                }
                .font(.caption2).monospacedDigit().foregroundStyle(.tertiary)
            } else {
                Text("No finite range published; shown read-only.").font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder private var provenance: some View {
        if parameter.domain == .action {
            EmptyView()
        } else if parameter.value == nil {
            ControlBadge(text: "no value", tint: .secondary)
        } else if let origin = parameter.origin, origin != .observed {
            ControlBadge(text: origin.rawValue, tint: .secondary)
                .help(origin == .cached ? "Remembered by the driver, not read back from the device."
                                        : "Computed by the driver, not read from the device.")
        }
    }

    @ViewBuilder private var status: some View {
        switch state {
        case .idle:
            EmptyView()
        case .writing(let value):
            HStack(spacing: 6) {
                ProgressView().controlSize(.mini)
                Text("Setting \(Self.describe(value, parameter.domain))…")
            }
            .font(.caption).foregroundStyle(.secondary)
        case .failed(let text):
            Label(text, systemImage: "exclamationmark.triangle.fill")
                .font(.caption).foregroundStyle(.red)
        }
    }

    /// "Ch 1 Volume" under a "Ch 1 Out" heading reads as "Volume".
    static func shortName(_ parameter: ControlParameter) -> String {
        switch parameter.semantic {
        case .level: "Level"
        case .mute: "Mute"
        default: parameter.name
        }
    }

    static func format(_ value: Double, _ domain: ControlScalarDomain) -> String {
        let decimals = (domain.step ?? 0.01) >= 1 ? 0 : ((domain.step ?? 0.01) >= 0.1 ? 1 : 2)
        let number = String(format: "%.\(decimals)f", value)
        switch domain.unit {
        case "generic", "normalized": return number
        default: return "\(number) \(domain.unit)"
        }
    }

    static func describe(_ value: ControlValue, _ domain: ControlDomain) -> String {
        switch (value, domain) {
        case (.action, _): "action"
        case (.bool(let on), _): on ? "on" : "off"
        case (.double(let number), .scalar(let scalar)): format(number, scalar)
        case (.int(let number), .enumeration(let items)): items.first { $0.value == number }?.name ?? "\(number)"
        case (.double(let number), _): String(format: "%.2f", number)
        case (.int(let number), _): "\(number)"
        }
    }
}

/// A small capsule of text.
struct ControlBadge: View {
    let text: String
    var tint: Color = .secondary
    var monospaced = false

    var body: some View {
        Text(text)
            .font(monospaced ? .system(.caption2, design: .monospaced) : .caption2)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .foregroundStyle(tint)
            .background(tint.opacity(0.14), in: Capsule())
    }
}
