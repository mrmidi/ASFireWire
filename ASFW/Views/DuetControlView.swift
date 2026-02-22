import SwiftUI

struct DuetControlView: View {
    @StateObject private var viewModel: DuetControlViewModel

    private let faderLabels = ["In 1", "In 2", "Str 1", "Str 2"]

    init(connector: ASFWDriverConnector) {
        _viewModel = StateObject(wrappedValue: DuetControlViewModel(connector: connector))
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                headerSection

                if !viewModel.isConnected {
                    stateCard(title: "Driver Not Connected",
                              message: "Connect to ASFWDriver to control Duet settings.")
                } else if viewModel.duetGUID == nil {
                    stateCard(title: "No Apogee Duet Found",
                              message: "Discover a Duet AV/C unit, then refresh this page.")
                } else {
                    mixerSection
                    inputSection
                    clicklessSection
                }

                if let error = viewModel.errorMessage {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                        .font(.callout)
                }
            }
            .padding()
        }
        .navigationTitle("Duet")
        .onAppear {
            viewModel.refresh()
        }
    }

    private var headerSection: some View {
        GroupBox {
            HStack(alignment: .center) {
                VStack(alignment: .leading, spacing: 6) {
                    if let guid = viewModel.duetGUID {
                        Text(String(format: "GUID 0x%016llX", guid))
                            .font(.system(.subheadline, design: .monospaced))
                    } else {
                        Text("No Duet selected")
                            .foregroundStyle(.secondary)
                    }

                    HStack(spacing: 16) {
                        if let firmware = viewModel.firmwareID {
                            Text(String(format: "FW 0x%08X", firmware))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        if let hardware = viewModel.hardwareID {
                            Text(String(format: "HW 0x%08X", hardware))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }

                    if let refreshed = viewModel.lastRefreshTime {
                        Text("Updated \(refreshed.formatted(date: .omitted, time: .standard))")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                }

                Spacer()

                if viewModel.isLoading {
                    ProgressView()
                        .controlSize(.small)
                }

                Button {
                    viewModel.refresh()
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .disabled(viewModel.isLoading)
            }
        } label: {
            Label("Duet Status", systemImage: "slider.horizontal.below.square.filled.and.square")
                .font(.headline)
        }
    }

    private var mixerSection: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 16) {
                Picker("Output Bank", selection: $viewModel.selectedOutputBank) {
                    ForEach(DuetOutputBank.allCases) { bank in
                        Text(bank.displayName).tag(bank)
                    }
                }
                .pickerStyle(.segmented)

                HStack(alignment: .top, spacing: 20) {
                    ForEach(Array(faderLabels.enumerated()), id: \.offset) { pair in
                        VerticalFader(label: pair.element,
                                      value: Binding(
                                        get: { viewModel.mixerGain(source: pair.offset) },
                                        set: { viewModel.setMixerGain(source: pair.offset, gain: $0) }
                                      ),
                                      range: Double(DuetMixerParams.gainMin)...Double(DuetMixerParams.gainMax))
                    }
                }
                .frame(maxWidth: .infinity)
            }
        } label: {
            Label("Mixer", systemImage: "slider.vertical.3")
                .font(.headline)
        }
    }

    private var inputSection: some View {
        GroupBox {
            VStack(spacing: 14) {
                ForEach(0..<2, id: \.self) { channel in
                    VStack(alignment: .leading, spacing: 10) {
                        Text("Input \(channel + 1)")
                            .font(.subheadline.bold())

                        HStack(spacing: 12) {
                            Picker("Source", selection: Binding(
                                get: { viewModel.inputParams.sources[channel] },
                                set: { viewModel.setInputSource(channel: channel, source: $0) }
                            )) {
                                ForEach(DuetInputSource.allCases) { source in
                                    Text(source.displayName).tag(source)
                                }
                            }
                            .frame(maxWidth: 150)

                            Picker("XLR", selection: Binding(
                                get: { viewModel.inputParams.xlrNominalLevels[channel] },
                                set: { viewModel.setInputXlrNominalLevel(channel: channel, level: $0) }
                            )) {
                                ForEach(DuetInputXlrNominalLevel.allCases) { level in
                                    Text(level.displayName).tag(level)
                                }
                            }
                            .frame(maxWidth: 150)

                            Toggle("48V", isOn: Binding(
                                get: { viewModel.inputParams.phantomPowerings[channel] },
                                set: { viewModel.setInputPhantom(channel: channel, enabled: $0) }
                            ))
                            .toggleStyle(.switch)
                            .frame(maxWidth: 90)

                            Toggle("Polarity", isOn: Binding(
                                get: { viewModel.inputParams.polarities[channel] },
                                set: { viewModel.setInputPolarity(channel: channel, inverted: $0) }
                            ))
                            .toggleStyle(.switch)
                        }

                        HStack(spacing: 10) {
                            Text("Gain")
                                .font(.caption)
                                .foregroundStyle(.secondary)

                            Slider(value: Binding(
                                get: { Double(viewModel.inputParams.gains[channel]) },
                                set: { viewModel.setInputGain(channel: channel, gain: $0) }
                            ),
                            in: Double(DuetInputParams.gainMin)...Double(DuetInputParams.gainMax),
                            step: 1)

                            Text("\(viewModel.inputParams.gains[channel])")
                                .font(.system(.caption, design: .monospaced))
                                .frame(width: 36, alignment: .trailing)
                        }
                    }
                    .padding(10)
                    .background(Color(NSColor.controlBackgroundColor).opacity(0.45))
                    .cornerRadius(8)
                }
            }
        } label: {
            Label("Input Controls", systemImage: "mic")
                .font(.headline)
        }
    }

    private var clicklessSection: some View {
        GroupBox {
            Toggle("Clickless Switching", isOn: Binding(
                get: { viewModel.inputParams.clickless },
                set: { viewModel.setClickless($0) }
            ))
            .toggleStyle(.switch)
        } label: {
            Label("Global", systemImage: "switch.2")
                .font(.headline)
        }
    }

    private func stateCard(title: String, message: String) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                Text(title)
                    .font(.headline)
                Text(message)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

private struct VerticalFader: View {
    let label: String
    @Binding var value: Double
    let range: ClosedRange<Double>

    var body: some View {
        VStack(spacing: 8) {
            Text(String(format: "%.0f", value))
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)

            Slider(value: $value, in: range, step: 1)
                .frame(height: 140)
                .rotationEffect(.degrees(-90))
                .frame(width: 36, height: 140)

            Text(label)
                .font(.caption)
        }
        .frame(width: 64)
    }
}
