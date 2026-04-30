import SwiftUI

struct AlesisStatusView: View {
    @ObservedObject private var connector: ASFWDriverConnector
    @ObservedObject var driverViewModel: DriverViewModel
    @StateObject private var viewModel: AlesisStatusViewModel

    init(connector: ASFWDriverConnector, driverViewModel: DriverViewModel) {
        self.connector = connector
        self.driverViewModel = driverViewModel
        _viewModel = StateObject(wrappedValue: AlesisStatusViewModel(connector: connector))
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                header
                audioStatusSection
                asfwStatusSection
                diceStatusSection
            }
            .padding()
        }
        .navigationTitle("Alesis")
        .onAppear {
            driverViewModel.refreshLifecycleStatus()
            viewModel.refresh()
        }
        .onChange(of: connector.isConnected) { _, _ in
            driverViewModel.setUserClientConnected(connector.isConnected)
            viewModel.refresh()
        }
    }

    private var header: some View {
        HStack(alignment: .center, spacing: 12) {
            Image(systemName: "waveform.badge.mic")
                .font(.system(size: 34))
                .foregroundStyle(viewModel.coreAudioStatus == nil ? Color.secondary : Color.green)

            VStack(alignment: .leading, spacing: 4) {
                Text("Alesis MultiMix Firewire")
                    .font(.title2.bold())
                Text(viewModel.statusMessage)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }

            Spacer(minLength: 20)

            if let lastUpdated = viewModel.lastUpdated {
                Text("Updated \(lastUpdated.formatted(date: .omitted, time: .standard))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Button {
                driverViewModel.refreshLifecycleStatus()
                viewModel.refresh()
            } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
            }
            .buttonStyle(.bordered)
            .disabled(viewModel.isRefreshing || driverViewModel.isBusy)

            if viewModel.isRefreshing {
                ProgressView()
                    .controlSize(.small)
            }
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    @ViewBuilder
    private var audioStatusSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            sectionTitle("CoreAudio", systemImage: "hifispeaker.fill")

            if let status = viewModel.coreAudioStatus {
                statusGrid(rows: [
                    ("Device", status.deviceName),
                    ("Channels", status.channelSummary),
                    ("Sample Rate", status.sampleRateSummary),
                    ("Buffer", status.bufferSummary),
                    ("Input Latency", "\(status.inputLatency) frames"),
                    ("Output Latency", "\(status.outputLatency) frames"),
                    ("Input Safety", "\(status.inputSafetyOffset) frames"),
                    ("Output Safety", "\(status.outputSafetyOffset) frames"),
                    ("Running", status.isRunning ? "Yes" : "No"),
                    ("Default", defaultStatus(status))
                ])
            } else {
                unavailable("CoreAudio does not currently publish Alesis.",
                            systemImage: "waveform.slash")
            }
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    private var asfwStatusSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            sectionTitle("ASFW State", systemImage: "lock.shield")

            HStack(alignment: .top, spacing: 12) {
                Image(systemName: lifecycleIcon)
                    .font(.title3)
                    .foregroundStyle(lifecycleColor)
                    .frame(width: 28)

                VStack(alignment: .leading, spacing: 6) {
                    Text(driverViewModel.lifecycleStatus.summary)
                        .font(.headline)
                    Text(driverViewModel.lifecycleStatus.detail)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }

                Spacer(minLength: 16)

                VStack(alignment: .trailing, spacing: 8) {
                    Button {
                        driverViewModel.repairDriver()
                    } label: {
                        Label("Repair Driver", systemImage: "wrench.and.screwdriver.fill")
                    }
                    .buttonStyle(.bordered)
                    .disabled(driverViewModel.isBusy || !driverViewModel.canRepairDriver)

                    Button {
                        viewModel.captureDiagnostics(using: driverViewModel)
                    } label: {
                        Label("Capture Diagnostics", systemImage: "doc.text.magnifyingglass")
                    }
                    .buttonStyle(.bordered)
                    .disabled(driverViewModel.isBusy || !driverViewModel.canUseMaintenanceHelper)
                }
            }

            statusGrid(rows: [
                ("Active Driver", yesNo(driverViewModel.lifecycleStatus.activeDriver)),
                ("ASFW Audio Nub", yesNo(driverViewModel.lifecycleStatus.audioNubVisible)),
                ("CoreAudio Alesis", yesNo(driverViewModel.lifecycleStatus.coreAudioDeviceVisible)),
                ("Debug User-Client", driverViewModel.userClientConnected ? "Connected" : "Unavailable"),
                ("Helper", driverViewModel.helperStatus.displayName),
                ("Staged Driver", yesNo(driverViewModel.lifecycleStatus.stagedDriverPresent)),
                ("Active CDHash", shortHash(driverViewModel.lifecycleStatus.activeCDHash)),
                ("Expected CDHash", shortHash(driverViewModel.lifecycleStatus.expectedCDHash)),
                ("Recommended Action", driverViewModel.lifecycleStatus.recommendedAction.displayName),
                ("Last Snapshot", driverViewModel.lastMaintenanceSnapshotPath ?? "None")
            ])
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    @ViewBuilder
    private var diceStatusSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            sectionTitle("DICE / Discovery", systemImage: "memorychip")

            if let published = viewModel.publishedDiceStatus {
                Label("Published by ASFWAudioNub", systemImage: "checkmark.seal.fill")
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.green)
                statusGrid(rows: [
                    ("Protocol", published.protocolName),
                    ("Caps Source", published.capsSource),
                    ("PCM Channels", published.channelSummary),
                    ("AM824 Slots", published.slotSummary),
                    ("Sample Rate", "\(published.sampleRateHz) Hz"),
                    ("Iso Channels", published.isoSummary)
                ])
                Divider()
            } else {
                unavailable("This active driver has not published DICE IORegistry state yet.",
                            systemImage: "info.circle")
            }

            if let identity = viewModel.discoveredIdentity {
                statusGrid(rows: [
                    ("Name", identity.displayName),
                    ("GUID", identity.guidHex),
                    ("Node", "\(identity.nodeID)"),
                    ("Generation", "\(identity.generation)"),
                    ("Vendor ID", identity.vendorHex),
                    ("Model ID", identity.modelHex),
                    ("Unit Directories", "\(identity.unitCount)")
                ])
            } else {
                unavailable(viewModel.userClientAvailable
                            ? "No Alesis discovery record is available from the debug user-client."
                            : "DICE discovery needs the debug user-client; CoreAudio can still work without it.",
                            systemImage: "cable.connector.slash")
            }

            if let snapshot = viewModel.diceSnapshot {
                DiceSnapshotView(snapshot: snapshot)
            }
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    private func sectionTitle(_ title: String, systemImage: String) -> some View {
        Label(title, systemImage: systemImage)
            .font(.headline)
    }

    private func statusGrid(rows: [(String, String)]) -> some View {
        Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 8) {
            ForEach(rows, id: \.0) { label, value in
                GridRow {
                    Text(label)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text(value)
                        .font(.system(.body, design: .monospaced))
                        .lineLimit(2)
                        .truncationMode(.middle)
                        .textSelection(.enabled)
                }
            }
        }
    }

    private func unavailable(_ message: String, systemImage: String) -> some View {
        Label(message, systemImage: systemImage)
            .foregroundStyle(.secondary)
            .padding(.vertical, 8)
    }

    private func defaultStatus(_ status: AlesisCoreAudioStatus) -> String {
        switch (status.isDefaultInput, status.isDefaultOutput) {
        case (true, true): return "Input and output"
        case (true, false): return "Input"
        case (false, true): return "Output"
        case (false, false): return "No"
        }
    }

    private func yesNo(_ value: Bool) -> String {
        value ? "Yes" : "No"
    }

    private func shortHash(_ hash: String?) -> String {
        guard let hash, !hash.isEmpty else { return "Unknown" }
        if hash.count <= 12 { return hash }
        return "\(hash.prefix(12))..."
    }

    private var lifecycleIcon: String {
        switch driverViewModel.lifecycleStatus.health {
        case .clean: return "checkmark.circle.fill"
        case .repairNeeded: return "wrench.and.screwdriver.fill"
        case .rebootRequired: return "restart.circle.fill"
        case .uninstalled: return "arrow.down.circle.fill"
        case .unknown: return "questionmark.circle.fill"
        }
    }

    private var lifecycleColor: Color {
        switch driverViewModel.lifecycleStatus.health {
        case .clean: return .green
        case .repairNeeded: return .orange
        case .rebootRequired: return .red
        case .uninstalled, .unknown: return .secondary
        }
    }
}

private struct DiceSnapshotView: View {
    let snapshot: AlesisDiceSnapshot

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Divider()
            Label("Read-only DICE Registers", systemImage: "list.bullet.rectangle")
                .font(.subheadline.weight(.semibold))

            Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 8) {
                sectionRow("Global", snapshot.sections.global)
                sectionRow("TX Stream", snapshot.sections.txStreamFormat)
                sectionRow("RX Stream", snapshot.sections.rxStreamFormat)
                sectionRow("External Sync", snapshot.sections.extSync)
            }

            if let global = snapshot.global {
                Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 8) {
                    GridRow {
                        Text("Nickname").foregroundStyle(.secondary)
                        Text(global.nickname.isEmpty ? "Unknown" : global.nickname)
                            .font(.system(.body, design: .monospaced))
                            .textSelection(.enabled)
                    }
                    GridRow {
                        Text("Clock").foregroundStyle(.secondary)
                        Text("\(global.clockSourceName), \(global.sourceLocked ? "locked" : "unlocked")")
                            .font(.system(.body, design: .monospaced))
                    }
                    GridRow {
                        Text("DICE Rate").foregroundStyle(.secondary)
                        Text("\(global.nominalRateHz > 0 ? "\(global.nominalRateHz) Hz" : "Unknown")")
                            .font(.system(.body, design: .monospaced))
                    }
                }
            }

            if snapshot.txStreams != nil || snapshot.rxStreams != nil {
                HStack(alignment: .top, spacing: 24) {
                    streamSummary(title: "TX", config: snapshot.txStreams)
                    streamSummary(title: "RX", config: snapshot.rxStreams)
                }
            }
        }
    }

    private func sectionRow(_ label: String, _ section: AlesisDiceSection) -> some View {
        GridRow {
            Text(label).foregroundStyle(.secondary)
            Text(String(format: "offset 0x%08X, %u bytes", section.offset, section.size))
                .font(.system(.body, design: .monospaced))
                .textSelection(.enabled)
        }
    }

    @ViewBuilder
    private func streamSummary(title: String, config: AlesisDiceStreamConfig?) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.subheadline.weight(.semibold))
            if let config {
                Text("\(config.activePcmChannels) active PCM / \(config.totalPcmChannels) total PCM")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                ForEach(config.streams) { stream in
                    Text("Stream \(stream.id): iso \(stream.isoChannel), pcm \(stream.pcmChannels), midi \(stream.midiPorts)")
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                }
            } else {
                Text("Unavailable")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}
