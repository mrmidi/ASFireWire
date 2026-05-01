import SwiftUI

struct MidasStatusView: View {
    @ObservedObject private var connector: ASFWDriverConnector
    @ObservedObject var driverViewModel: DriverViewModel
    @StateObject private var viewModel: MidasStatusViewModel

    init(connector: ASFWDriverConnector, driverViewModel: DriverViewModel) {
        self.connector = connector
        self.driverViewModel = driverViewModel
        _viewModel = StateObject(wrappedValue: MidasStatusViewModel(connector: connector))
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                header
                profileSection
                coreAudioSection
                discoverySection
                diceSection
            }
            .padding()
        }
        .navigationTitle("Midas")
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
            Image(systemName: "slider.horizontal.3")
                .font(.system(size: 34))
                .foregroundStyle(viewModel.discoveredIdentity == nil ? Color.secondary : Color.green)

            VStack(alignment: .leading, spacing: 4) {
                Text("Midas Venice")
                    .font(.title2.bold())
                Text(viewModel.statusMessage)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }

            Spacer(minLength: 20)

            VStack(alignment: .trailing, spacing: 8) {
                if let lastUpdated = viewModel.lastUpdated {
                    Text("Updated \(lastUpdated.formatted(date: .omitted, time: .standard))")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                HStack(spacing: 8) {
                    Button {
                        viewModel.copyStatusSummary(lifecycle: driverViewModel.lifecycleStatus)
                    } label: {
                        Label("Copy Status", systemImage: "doc.on.doc")
                    }
                    .buttonStyle(.bordered)

                    Button {
                        viewModel.captureDiagnostics(using: driverViewModel)
                    } label: {
                        Label("Diagnostics", systemImage: "doc.text.magnifyingglass")
                    }
                    .buttonStyle(.bordered)
                    .disabled(driverViewModel.isBusy || !driverViewModel.canUseMaintenanceHelper)

                    Button {
                        driverViewModel.refreshLifecycleStatus()
                        viewModel.refresh()
                    } label: {
                        Label("Refresh", systemImage: "arrow.clockwise")
                    }
                    .buttonStyle(.bordered)
                    .disabled(viewModel.isRefreshing || driverViewModel.isBusy)
                }
            }
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    private var profileSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            sectionTitle("Device Library Profile", systemImage: "books.vertical")
            if let profile = viewModel.profile {
                statusGrid(rows: [
                    ("Name", profile.displayName),
                    ("IDs", profile.hexSummary),
                    ("Protocol", profile.protocolFamily.rawValue),
                    ("Mixer Hint", profile.mixerHint.isEmpty ? "None" : profile.mixerHint),
                    ("ASFW Status", profile.supportSummary),
                    ("Integration", profile.integrationMode.rawValue),
                    ("Source", profile.source)
                ])
            } else {
                unavailable("No Midas profile is available in the device library.", systemImage: "questionmark.folder")
            }
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    private var coreAudioSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            sectionTitle("CoreAudio", systemImage: "hifispeaker.fill")
            if let status = viewModel.coreAudioStatus {
                statusGrid(rows: [
                    ("Device", status.deviceName),
                    ("Channels", status.channelSummary),
                    ("Sample Rate", status.sampleRateSummary),
                    ("Buffer", "\(status.bufferFrameSize) frames"),
                    ("Input Latency", "\(status.inputLatency) frames"),
                    ("Output Latency", "\(status.outputLatency) frames"),
                    ("Running", status.isRunning ? "Yes" : "No"),
                    ("Default", defaultStatus(status))
                ])
            } else {
                unavailable("CoreAudio does not currently publish a Midas Venice device.", systemImage: "waveform.slash")
            }
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    private var discoverySection: some View {
        VStack(alignment: .leading, spacing: 12) {
            sectionTitle("Config ROM / Discovery", systemImage: "memorychip")
            if let identity = viewModel.discoveredIdentity {
                statusGrid(rows: [
                    ("Name", identity.displayName),
                    ("GUID", identity.guidHex),
                    ("Node", "\(identity.nodeID)"),
                    ("Generation", "\(identity.generation)"),
                    ("Vendor ID", identity.vendorHex),
                    ("Model ID", identity.modelHex),
                    ("Unit Count", "\(identity.units.count)")
                ])

                if !identity.units.isEmpty {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Unit Directories")
                            .font(.subheadline.weight(.semibold))
                        ForEach(identity.units) { unit in
                            Text("\(unit.specIdHex) / \(unit.swVersionHex) - \(unit.productName ?? unit.vendorName ?? unit.stateString)")
                                .font(.caption.monospaced())
                                .textSelection(.enabled)
                        }
                    }
                }
            } else {
                unavailable(viewModel.userClientAvailable
                            ? "The debug user-client is connected, but no Midas device is currently discovered."
                            : "Config ROM discovery needs the debug user-client connection.",
                            systemImage: "cable.connector.slash")
            }
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    private var diceSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            sectionTitle("DICE Runtime", systemImage: "waveform.path.ecg")

            if let published = viewModel.publishedDiceStatus?.inner {
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
                unavailable("ASFW has not published Midas DICE runtime state into IORegistry.", systemImage: "info.circle")
            }

            if let diagnostic = viewModel.diceProbeDiagnostic {
                Divider()
                Label("Last ASFW Driver Probe", systemImage: diagnostic.probeState == "failed" ? "exclamationmark.triangle" : "checkmark.seal")
                    .font(.subheadline.weight(.semibold))
                statusGrid(rows: [
                    ("State", diagnostic.humanState),
                    ("Fail Reason", diagnostic.humanFailReason),
                    ("Caps Source", diagnostic.capsSource),
                    ("Profile Source", diagnostic.profileSource),
                    ("Channels", diagnostic.channelSummary),
                    ("Streams", diagnostic.streamSummary),
                    ("AM824 Slots", diagnostic.slotSummary),
                    ("Sample Rate", diagnostic.sampleRateHz > 0 ? "\(diagnostic.sampleRateHz) Hz" : "Unknown"),
                    ("GUID", diagnostic.guidHex)
                ])
            } else {
                unavailable("No Midas-specific ASFW driver probe result is published yet.", systemImage: "doc.text.magnifyingglass")
            }

            if let snapshot = viewModel.diceSnapshot {
                Divider()
                MidasDiceSnapshotSummary(snapshot: snapshot)
            } else {
                unavailable("Read-only DICE register snapshot is unavailable.", systemImage: "list.bullet.rectangle")
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

    private func defaultStatus(_ status: MidasCoreAudioStatus) -> String {
        switch (status.isDefaultInput, status.isDefaultOutput) {
        case (true, true): return "Input and output"
        case (true, false): return "Input"
        case (false, true): return "Output"
        case (false, false): return "No"
        }
    }
}

private struct MidasDiceSnapshotSummary: View {
    let snapshot: AlesisDiceSnapshot

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label("Read-only DICE Registers", systemImage: "list.bullet.rectangle")
                .font(.subheadline.weight(.semibold))

            Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 8) {
                row("Global", snapshot.sections.global)
                row("TX Stream", snapshot.sections.txStreamFormat)
                row("RX Stream", snapshot.sections.rxStreamFormat)
                row("External Sync", snapshot.sections.extSync)
            }

            if let global = snapshot.global {
                Text("Clock \(global.clockSourceName), \(global.sourceLocked ? "locked" : "unlocked"), rate \(global.nominalRateHz > 0 ? "\(global.nominalRateHz) Hz" : "unknown")")
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }

            HStack(alignment: .top, spacing: 24) {
                streamSummary("TX", snapshot.txStreams)
                streamSummary("RX", snapshot.rxStreams)
            }
        }
    }

    private func row(_ label: String, _ section: AlesisDiceSection) -> some View {
        GridRow {
            Text(label).foregroundStyle(.secondary)
            Text(String(format: "offset 0x%08X, %u bytes", section.offset, section.size))
                .font(.system(.body, design: .monospaced))
                .textSelection(.enabled)
        }
    }

    private func streamSummary(_ title: String, _ config: AlesisDiceStreamConfig?) -> some View {
        VStack(alignment: .leading, spacing: 6) {
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
