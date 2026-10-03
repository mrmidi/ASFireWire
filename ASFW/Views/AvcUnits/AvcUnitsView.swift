import SwiftUI

/// The AV/C Units screen: each unit the driver knows, as a dashboard built from its discovery document.
struct AvcUnitsView: View {
    @ObservedObject var store: AvcUnitsStore
    let connector: ASFWDriverConnector
    /// Show the bus-test buttons (IRM, CMP, DMA). They act on the bus; they are for development.
    var developerTools = false

    @State private var tab: AvcUnitTab = .signal
    @State private var confirmingReScan = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                if let message = store.message { messageBanner(message) }
                if store.units.count > 1 { unitPicker }
                if let unit = store.selected {
                    AvcUnitDashboardView(unit: unit, tab: $tab, developerTools: developerTools ? AnyView(AvcDeveloperToolsCard(connector: connector)) : nil)
                } else if store.isLoading {
                    ProgressView("Reading units…").frame(maxWidth: .infinity).padding(.top, 80)
                } else {
                    emptyState
                }
            }
            .frame(maxWidth: 1120)
            .padding(24)
            .frame(maxWidth: .infinity)
        }
        .background(Color(nsColor: .windowBackgroundColor))
        .navigationTitle("AV/C Units")
        .toolbar {
            ToolbarItemGroup {
                if store.isLoading { ProgressView().controlSize(.small) }
                if let updated = store.lastUpdated {
                    Text("Updated \(updated.formatted(date: .omitted, time: .shortened))").font(.caption).foregroundStyle(.secondary)
                }
                Button { Task { await store.reload() } } label: { Label("Refresh", systemImage: "arrow.clockwise") }
                    .help("Read the driver's current discovery data. Sends nothing to the device.")
                    .disabled(!store.isConnected || store.isLoading)
                Button { confirmingReScan = true } label: { Label("Re-scan", systemImage: "arrow.triangle.2.circlepath") }
                    .help("Run AV/C discovery again on the device")
                    .disabled(!store.isConnected || store.isLoading)
            }
        }
        .avcProbeConfirmation(isPresented: $confirmingReScan) { Task { await store.reScan() } }
        .task { await store.reload() }
        .onChange(of: store.isConnected) { _, connected in
            if connected { Task { await store.reload() } }
        }
    }

    // MARK: Pieces

    private var unitPicker: some View {
        HStack(spacing: 8) {
            ForEach(store.units) { unit in
                let selected = unit.guid == store.selected?.guid
                Button { store.selectedGUID = unit.guid } label: {
                    Text(unit.title).font(.callout).fontWeight(selected ? .semibold : .regular)
                        .padding(.horizontal, 12).padding(.vertical, 6)
                        .foregroundStyle(selected ? Color.white : Color.primary)
                        .background(selected ? Color.accentColor : Color.primary.opacity(0.08), in: Capsule())
                }
                .buttonStyle(.plain)
            }
            Spacer()
        }
    }

    private func messageBanner(_ message: String) -> some View {
        HStack(spacing: 10) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            Text(message).font(.callout)
            Spacer()
        }
        .padding(12)
        .background(Color.orange.opacity(0.12), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
    }

    private var emptyState: some View {
        VStack(spacing: 14) {
            Image(systemName: "hifispeaker.2").font(.system(size: 52)).foregroundStyle(.tertiary)
            Text("No AV/C units").font(.title3).fontWeight(.semibold)
            Text(store.isConnected ? "Connect a FireWire audio device. It appears here once the driver has discovered it." : "The app is not connected to the ASFW driver.")
                .font(.callout).foregroundStyle(.secondary).multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
        .padding(.top, 90)
    }
}

/// Bus tests that act on the bus (IRM allocation, CMP, DMA). Development only: shown in Debug mode.
struct AvcDeveloperToolsCard: View {
    let connector: ASFWDriverConnector

    var body: some View {
        AvcCard {
            VStack(alignment: .leading, spacing: 12) {
                AvcSectionHeader(title: "Bus tests", caption: "Development tools. These act on the bus, not on this screen's data.", systemImage: "wrench.and.screwdriver", tint: .red)
                AvcFlow(spacing: 8) {
                    tool("Allocate IRM", "plus.circle", "Allocate IRM (channel 0, 84 BW units); see Console") { _ = connector.testIRMAllocation() }
                    tool("Free IRM", "minus.circle", "Release IRM (channel 0, 84 BW units)") { _ = connector.testIRMRelease() }
                    tool("Connect oPCR", "arrow.right.circle", "CMP connect oPCR[0]: starts device → host stream") { _ = connector.testCMPConnectOPCR() }
                    tool("Disconnect oPCR", "arrow.left.circle", "CMP disconnect oPCR[0]: stops device → host stream") { _ = connector.testCMPDisconnectOPCR() }
                    tool("Allocate IT DMA", "square.and.arrow.up", "Allocate IT DMA (about 2 MB), DMA only, no CMP iPCR") { _ = connector.allocateITDMA(channel: 1) }
                    tool("Free IT DMA", "trash", "Deallocate IT DMA") { _ = connector.deallocateITDMA() }
                }
            }
        }
    }

    private func tool(_ title: String, _ image: String, _ help: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) { Label(title, systemImage: image) }
            .buttonStyle(.bordered)
            .help(help)
            .disabled(!connector.isConnected)
    }
}

enum AvcUnitTab: String, CaseIterable, Identifiable {
    case signal = "Signal", controls = "Controls", plugs = "Plugs", capabilities = "Capabilities", diagnostics = "Diagnostics"
    var id: String { rawValue }
    var systemImage: String {
        switch self {
        case .signal: "waveform"
        case .controls: "slider.horizontal.3"
        case .plugs: "cable.connector"
        case .capabilities: "list.bullet.rectangle"
        case .diagnostics: "stethoscope"
        }
    }
}

/// One unit's dashboard: the device header, the tab bar and the selected tab.
struct AvcUnitDashboardView: View {
    let unit: AvcUnitDashboard
    @Binding var tab: AvcUnitTab
    var developerTools: AnyView?

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            AvcDeviceHeader(unit: unit)
            if unit.needsDocumentUpdate {
                AvcNoticeCard(title: "Driver update needed", message: unit.documentUpdateMessage, systemImage: "arrow.down.circle")
            }
            AvcTabBar(selection: $tab)
            switch tab {
            case .signal: AvcSignalTab(unit: unit)
            case .controls: AvcControlsTab(unit: unit)
            case .plugs: AvcPlugsTab(unit: unit)
            case .capabilities: AvcCapabilitiesTab(unit: unit)
            case .diagnostics: AvcDiagnosticsTab(unit: unit, developerTools: developerTools)
            }
        }
    }
}
