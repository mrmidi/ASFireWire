import SwiftUI

struct DeviceLibraryView: View {
    @ObservedObject private var connector: ASFWDriverConnector
    @StateObject private var viewModel: DeviceLibraryViewModel

    init(connector: ASFWDriverConnector) {
        self.connector = connector
        _viewModel = StateObject(wrappedValue: DeviceLibraryViewModel(connector: connector))
    }

    var body: some View {
        VStack(spacing: 0) {
            filterBar
            Divider()
            HSplitView {
                profileList
                    .frame(minWidth: 520)
                connectedDevices
                    .frame(minWidth: 320)
            }
        }
        .navigationTitle("Device Library")
        .onAppear {
            viewModel.refreshConnectedDevices()
        }
        .onChange(of: connector.isConnected) { _, _ in
            viewModel.refreshConnectedDevices()
        }
    }

    private var filterBar: some View {
        HStack(spacing: 12) {
            TextField("Search vendor, model, protocol, source, or hex ID", text: $viewModel.searchText)
                .textFieldStyle(.roundedBorder)
                .frame(minWidth: 320)

            Picker("Family", selection: familyBinding) {
                Text("All families").tag("")
                ForEach(FireWireProtocolFamily.allCases) { family in
                    Text(family.rawValue).tag(family.rawValue)
                }
            }
            .frame(width: 170)

            Picker("Status", selection: statusBinding) {
                Text("All statuses").tag("")
                ForEach(FireWireProfileSupportStatus.allCases) { status in
                    Text(status.rawValue).tag(status.rawValue)
                }
            }
            .frame(width: 190)

            Spacer()

            Text("\(viewModel.filteredProfiles.count) / \(FireWireDeviceProfiles.all.count)")
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)

            Button {
                viewModel.refreshConnectedDevices()
            } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
            }
            .buttonStyle(.borderedProminent)
        }
        .padding()
    }

    private var profileList: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 10) {
                Text("Recognized FireWire Audio Metadata")
                    .font(.headline)
                    .frame(maxWidth: .infinity, alignment: .leading)

                Text("Recognition is not support. Metadata-only profiles are for identification, logging, and diagnostics.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)

                ForEach(viewModel.filteredProfiles) { profile in
                    DeviceLibraryProfileCard(profile: profile,
                                             isConnectedMatch: viewModel.matchedProfileIDs.contains(profile.id))
                }
            }
            .padding()
        }
    }

    private var connectedDevices: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                HStack {
                    Label("Connected FireWire Devices", systemImage: "cable.connector")
                        .font(.headline)
                    Spacer()
                    if let lastUpdated = viewModel.lastUpdated {
                        Text(lastUpdated.formatted(date: .omitted, time: .standard))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                if !viewModel.userClientAvailable {
                    Label("Debug user-client is unavailable, so live matching is limited to the static library.", systemImage: "cable.connector.slash")
                        .foregroundStyle(.secondary)
                } else if viewModel.discoveredDevices.isEmpty {
                    Label("No FireWire devices are currently reported by the debug user-client.", systemImage: "questionmark.circle")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(viewModel.discoveredDevices) { device in
                        ConnectedDeviceProfileCard(device: device,
                                                   profile: FireWireDeviceProfiles.bestMatch(for: device))
                    }
                }
            }
            .padding()
        }
    }

    private var familyBinding: Binding<String> {
        Binding(
            get: { viewModel.selectedFamily?.rawValue ?? "" },
            set: { raw in
                viewModel.selectedFamily = raw.isEmpty ? nil : FireWireProtocolFamily(rawValue: raw)
            }
        )
    }

    private var statusBinding: Binding<String> {
        Binding(
            get: { viewModel.selectedStatus?.rawValue ?? "" },
            set: { raw in
                viewModel.selectedStatus = raw.isEmpty ? nil : FireWireProfileSupportStatus(rawValue: raw)
            }
        )
    }
}

private struct DeviceLibraryProfileCard: View {
    let profile: FireWireAudioDeviceProfile
    let isConnectedMatch: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline) {
                Text(profile.displayName.isEmpty ? "Unnamed FireWire Device" : profile.displayName)
                    .font(.headline)
                if isConnectedMatch {
                    Label("Connected", systemImage: "checkmark.circle.fill")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.green)
                }
                Spacer()
                Text(profile.supportSummary)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(profile.isBoundByASFW ? .green : .secondary)
            }

            HStack(spacing: 16) {
                Label(profile.protocolFamily.rawValue, systemImage: "memorychip")
                Label(profile.source, systemImage: "link")
                if !profile.mixerHint.isEmpty {
                    Label(profile.mixerHint, systemImage: "slider.horizontal.3")
                }
            }
            .font(.caption)
            .foregroundStyle(.secondary)

            Text(profile.hexSummary)
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }
}

private struct ConnectedDeviceProfileCard: View {
    let device: ASFWDriverConnector.FWDeviceInfo
    let profile: FireWireAudioDeviceProfile?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(displayName)
                .font(.headline)
            Text(String(format: "GUID 0x%016llX, vendor 0x%06X, model 0x%06X", device.guid, device.vendorId, device.modelId))
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
            if let profile {
                Text(profile.supportSummary)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(profile.isBoundByASFW ? .green : .secondary)
                Text("Matched \(profile.displayName) from \(profile.source)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                Text("No metadata profile matched.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    private var displayName: String {
        let name = "\(device.vendorName) \(device.modelName)".trimmingCharacters(in: .whitespacesAndNewlines)
        return name.isEmpty ? "FireWire Device" : name
    }
}
