//
//  RoundTripLatencyView.swift
//  ASFW
//
//  Round-Trip Latency: the declared geometry of an ASFW audio device, the
//  uncompensated electrical round trip through an output-to-input cable, a
//  continuous-tone glitch check, and the instrument's own self-test.
//
//  Vocabulary (RTL_raw, RTL_ts, scheduling distance, residual) is defined in
//  tools/rtl/README.md and is shown here under the same names.
//

import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct RoundTripLatencyView: View {
    @ObservedObject var viewModel: RoundTripLatencyViewModel
    /// ASFW devices CoreAudio lists right now.
    let publishedDevices: [AudioWrapperDevice]
    /// The MCP server answers external clients; its driver reads are not paused.
    var mcpServerEnabled = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                header
                if let banner = viewModel.banner {
                    RtlBannerView(banner: banner)
                }
                if viewModel.selectedDevice != nil {
                    switch viewModel.mode {
                    case .declared:
                        RtlDeclaredCard(declared: viewModel.declared,
                                        refresh: viewModel.refreshDeclared)
                    case .measure:
                        measureSection
                    case .tone:
                        toneSection
                    }
                } else {
                    ContentUnavailableView(
                        "No ASFW Audio Device",
                        systemImage: "waveform",
                        description: Text("Connect an audio device and wait for it to become available in Core Audio."))
                }
                selfTestCard
            }
            .padding()
        }
        .navigationTitle("Round-Trip Latency")
        .onAppear {
            viewModel.updateDevices(from: publishedDevices)
            viewModel.refreshMicrophoneAccess()
        }
        .onChange(of: publishedDevices) { _, devices in
            viewModel.updateDevices(from: devices)
        }
        .onDisappear { viewModel.viewDidDisappear() }
    }

    // MARK: Header

    private var header: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                Image(systemName: "arrow.left.arrow.right.circle.fill")
                    .font(.system(size: 36))
                    .foregroundStyle(.blue.gradient)
                VStack(alignment: .leading, spacing: 4) {
                    Text("Round-Trip Latency").font(.title.bold())
                    Text("Uncompensated electrical round trip through an output-to-input cable, no DAW in the path.")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
            }

            if viewModel.devices.count > 1 {
                Picker("Device", selection: $viewModel.selectedDeviceID) {
                    ForEach(viewModel.devices) { device in
                        Text(device.name).tag(Optional(device.id))
                    }
                }
                .disabled(viewModel.runState.isBusy)
            } else if let device = viewModel.selectedDevice {
                InfoRow(label: "Device", value: device.name)
            }

            Picker("Mode", selection: $viewModel.mode) {
                ForEach(RoundTripLatencyViewModel.Mode.allCases) { mode in
                    Text(mode.rawValue).tag(mode)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .disabled(viewModel.runState.isBusy)
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
    }

    // MARK: Measure

    @ViewBuilder
    private var measureSection: some View {
        RtlSetupCard(confirmed: $viewModel.setupConfirmed,
                     disabled: viewModel.runState.isBusy,
                     busyElsewhere: viewModel.deviceBusyElsewhere)
        RtlMicrophoneNotice(access: viewModel.microphone)

        card("Measurement", systemImage: "slider.horizontal.3") {
            VStack(alignment: .leading, spacing: 12) {
            RtlBufferOptions(viewModel: viewModel)
            Stepper(value: $viewModel.trials, in: 1...RoundTripLatencyViewModel.maximumTrials) {
                InfoRow(label: "Trials", value: "\(viewModel.trials)")
            }
            Toggle("Automatic capture window", isOn: $viewModel.automaticWindow)
            if viewModel.automaticWindow {
                InfoRow(label: "Window",
                        value: viewModel.declared == nil
                            ? "unavailable"
                            : "\(viewModel.effectiveWindow) fr (4x declared round trip, at least 1024)")
            } else {
                Stepper(value: $viewModel.window,
                        in: RoundTripLatencyViewModel.minimumWindow...RoundTripLatencyViewModel.maximumWindow,
                        step: 256) {
                    InfoRow(label: "Window", value: "\(viewModel.effectiveWindow) fr")
                }
            }
            windowHint
            RtlChannelOptions(viewModel: viewModel)
            HStack {
                Text("Impulse amplitude").foregroundStyle(.secondary)
                Slider(value: $viewModel.amplitude, in: 0.05...1.0)
                Text(String(format: "%.2f", viewModel.amplitude))
                    .font(.system(.body, design: .monospaced))
                    .frame(width: 44, alignment: .trailing)
            }
            }
            // Options lock during a run; Cancel must stay live.
            .disabled(viewModel.runState.isBusy)
            runBar(mode: .measure)
        }

        if let result = viewModel.measurement {
            RtlMeasurementResultView(result: result,
                                     canExport: viewModel.canExport,
                                     export: exportEvidence)
        }
    }

    @ViewBuilder
    private var windowHint: some View {
        if let rate = viewModel.declared?.sampleRate {
            let longest = RtlFormat.maximumMeasurableFrames(
                window: viewModel.effectiveWindow, edgeGuard: RoundTripLatencyViewModel.edgeGuardFrames)
            Text("The window must exceed the real round trip, which is what is being measured. This window holds up to \(longest) fr (\(RtlFormat.millisecondsText(frames: Double(longest), sampleRate: rate, decimals: 1)))."
                 + (viewModel.estimatedMeasurementSeconds.map { " The run takes \(RtlFormat.durationText(seconds: $0))." } ?? ""))
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    // MARK: Tone

    @ViewBuilder
    private var toneSection: some View {
        RtlSetupCard(confirmed: $viewModel.setupConfirmed,
                     disabled: viewModel.runState.isBusy,
                     busyElsewhere: viewModel.deviceBusyElsewhere)
        RtlMicrophoneNotice(access: viewModel.microphone)

        card("Tone glitch check", systemImage: "waveform") {
            VStack(alignment: .leading, spacing: 12) {
            Text("Plays a sine on one output, captures the looped-back input and fits the sine block by block. Whatever the sine does not explain is a glitch: a dropout, a slip or a click.")
                .font(.callout)
                .foregroundStyle(.secondary)
            RtlBufferOptions(viewModel: viewModel)
            Stepper(value: $viewModel.toneDuration,
                    in: RoundTripLatencyViewModel.toneDurationRange, step: 5) {
                InfoRow(label: "Capture length", value: String(format: "%.0f s", viewModel.toneDuration))
            }
            HStack {
                Text("Frequency").foregroundStyle(.secondary)
                Spacer()
                TextField("Hz", value: $viewModel.toneFrequency, format: .number.precision(.fractionLength(0...1)))
                    .textFieldStyle(.roundedBorder)
                    .multilineTextAlignment(.trailing)
                    .frame(width: 100)
                Text("Hz")
            }
            Text("997 Hz divides no buffer or packet size, so a glitch cannot hide in phase with one.")
                .font(.caption)
                .foregroundStyle(.secondary)
            RtlChannelOptions(viewModel: viewModel)
            HStack {
                Text("Tone amplitude").foregroundStyle(.secondary)
                Slider(value: $viewModel.toneAmplitude, in: 0.05...1.0)
                Text(String(format: "%.2f", viewModel.toneAmplitude))
                    .font(.system(.body, design: .monospaced))
                    .frame(width: 44, alignment: .trailing)
            }
            }
            .disabled(viewModel.runState.isBusy)
            runBar(mode: .tone)
        }

        if let result = viewModel.toneResult {
            RtlToneResultView(result: result)
        }
    }

    // MARK: Run bar

    @ViewBuilder
    private func runBar(mode: RoundTripLatencyViewModel.Mode) -> some View {
        Divider()
        HStack(spacing: 12) {
            Button {
                if mode == .tone { viewModel.startTone() } else { viewModel.startMeasurement() }
            } label: {
                Label(mode == .tone ? "Start Tone Test" : "Start Measurement", systemImage: "play.fill")
            }
            .buttonStyle(.borderedProminent)
            .disabled(viewModel.startBlocker(for: mode) != nil)
            .help(viewModel.startBlocker(for: mode) ?? "Start real IO on the device.")

            if viewModel.runState.isBusy {
                ProgressView(value: viewModel.progress)
                    .frame(maxWidth: 240)
                Text(viewModel.runState == .requestingPermission
                     ? "Waiting for microphone permission"
                     : "\(Int((viewModel.progress * 100).rounded()))%")
                    .font(.system(.callout, design: .monospaced))
                    .foregroundStyle(.secondary)
                Button("Cancel", role: .cancel) { viewModel.cancel() }
                    .disabled(viewModel.runState == .requestingPermission)
            }
            Spacer()
        }
        if let blocker = viewModel.startBlocker(for: mode), !viewModel.runState.isBusy {
            Text(blocker)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        if viewModel.runState.isBusy {
            Text("The app's own driver polling (discovery, snapshots, topology, diagnostics) is paused while this runs. Leaving this page cancels the run. Do not play audio to the device.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        if mcpServerEnabled {
            Label("The MCP server is enabled. Requests from an MCP client still read the driver during a run and can disturb the timing; stop the server or keep the client idle.",
                  systemImage: "exclamationmark.triangle.fill")
                .font(.caption)
                .foregroundStyle(.orange)
        }
    }

    // MARK: Self-test

    private var selfTestCard: some View {
        card("Instrument Self-Test", systemImage: "checkmark.seal") {
            Text("Runs the detector, timeline audit, aggregation and adversarial simulator at 44.1, 48 and 96 kHz. No hardware needed. A measurement from an unverified detector is not evidence.")
                .font(.callout)
                .foregroundStyle(.secondary)
            HStack(spacing: 12) {
                Button {
                    viewModel.runSelfTest()
                } label: {
                    Label("Run Self-Test", systemImage: "play.circle")
                }
                .disabled(viewModel.selfTest == .running || viewModel.runState.isBusy)
                switch viewModel.selfTest {
                case .idle:
                    EmptyView()
                case .running:
                    ProgressView().controlSize(.small)
                case let .finished(passed, failures, _):
                    if passed {
                        Label("Passed at 44.1 / 48 / 96 kHz", systemImage: "checkmark.circle.fill")
                            .foregroundStyle(.green)
                    } else {
                        Label("\(failures) failure\(failures == 1 ? "" : "s")", systemImage: "xmark.octagon.fill")
                            .foregroundStyle(.red)
                    }
                }
            }
            if case let .finished(_, _, log) = viewModel.selfTest {
                DisclosureGroup("Log") {
                    ScrollView {
                        Text(log)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(maxHeight: 260)
                }
            }
        }
    }

    // MARK: Export

    private func exportEvidence() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = viewModel.suggestedExportName
        panel.title = "Save Round-Trip Latency Evidence"
        panel.prompt = "Save"
        panel.begin { response in
            guard response == .OK, let url = panel.url else { return }
            Task { @MainActor in viewModel.exportEvidence(to: url) }
        }
    }

    // MARK: Card helper

    private func card<Content: View>(_ title: String, systemImage: String,
                                     @ViewBuilder content: () -> Content) -> some View {
        RtlCard(title: title, systemImage: systemImage, content: content)
    }
}

// MARK: - Shared pieces

struct RtlCard<Content: View>: View {
    let title: String
    let systemImage: String
    @ViewBuilder let content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label(title, systemImage: systemImage)
                .font(.headline)
            content
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
    }
}

struct RtlBannerView: View {
    let banner: RoundTripLatencyViewModel.Banner

    var body: some View {
        Label(banner.text, systemImage: symbol)
            .foregroundStyle(tint)
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(tint.opacity(0.12), in: RoundedRectangle(cornerRadius: 10))
    }

    private var symbol: String {
        switch banner.kind {
        case .error: "xmark.octagon.fill"
        case .warning: "exclamationmark.triangle.fill"
        case .info: "info.circle.fill"
        }
    }

    private var tint: Color {
        switch banner.kind {
        case .error: .red
        case .warning: .orange
        case .info: .blue
        }
    }
}

/// The physical setup from tools/rtl/README.md, confirmed before IO starts.
struct RtlSetupCard: View {
    @Binding var confirmed: Bool
    let disabled: Bool
    let busyElsewhere: Bool

    var body: some View {
        RtlCard(title: "Physical Setup", systemImage: "cable.connector") {
            VStack(alignment: .leading, spacing: 6) {
                step("One balanced or unbalanced cable, output 1 to input 1. Electrical, not acoustic. The result includes both converters (DAC then ADC) and cannot separate them.")
                step("Output at unity, no DSP or monitor mix engaged on the device.")
                step("Input gain set so the returned impulse peaks around -6 dBFS. The result warns below 30x signal-to-noise.")
                step("Nothing else playing to the device. Quit anything holding it open.")
            }
            Label("Nothing else may play to this device while a run is in progress. Any other client changes the IO cycle and invalidates trials.",
                  systemImage: "speaker.slash.fill")
                .font(.callout)
                .foregroundStyle(.orange)
            if busyElsewhere {
                Label("Another client is already running this device. Its IO buffer, not ours, may set the device's IO cycle.",
                      systemImage: "exclamationmark.triangle.fill")
                    .font(.callout)
                    .foregroundStyle(.orange)
            }
            Toggle("The cable is connected and nothing else is using the device", isOn: $confirmed)
                .disabled(disabled)
        }
    }

    private func step(_ text: String) -> some View {
        Label {
            Text(text).fixedSize(horizontal: false, vertical: true)
        } icon: {
            Image(systemName: "checkmark.circle").foregroundStyle(.secondary)
        }
        .font(.callout)
    }
}

/// Explains a denied microphone before the user wonders why nothing measures.
struct RtlMicrophoneNotice: View {
    let access: MicrophoneAccess

    var body: some View {
        if let reason = access.blockingExplanation {
            Label(reason, systemImage: "mic.slash.fill")
                .foregroundStyle(.red)
                .padding()
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.red.opacity(0.12), in: RoundedRectangle(cornerRadius: 10))
        } else if access == .notDetermined {
            Label("The first run asks for microphone access. The loopback is recorded through the device's input; audio is analysed locally and never stored or sent.",
                  systemImage: "mic.fill")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
    }
}

struct RtlBufferOptions: View {
    @ObservedObject var viewModel: RoundTripLatencyViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Toggle("Request a client buffer size", isOn: $viewModel.requestBufferSize)
            if viewModel.requestBufferSize {
                Stepper(value: $viewModel.bufferFrames, in: range, step: 16) {
                    InfoRow(label: "Buffer frames", value: "\(viewModel.bufferFrames)")
                }
                Text("A request the device refuses or coerces is reported with the result. The buffer size this app had before is restored when the run ends. A different buffer size is a different measurement configuration: larger buffers move the scheduling distance.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else if let declared = viewModel.declared {
                InfoRow(label: "Buffer frames", value: "\(declared.bufferFrames) (device)")
            }
        }
    }

    private var range: ClosedRange<Int> {
        if let range = viewModel.bufferRange { return Int(range.lowerBound)...Int(range.upperBound) }
        return 16...4096
    }
}

struct RtlChannelOptions: View {
    @ObservedObject var viewModel: RoundTripLatencyViewModel

    var body: some View {
        if let device = viewModel.selectedDevice {
            HStack(spacing: 24) {
                Picker("Output channel", selection: $viewModel.outChannel) {
                    ForEach(0..<Int(device.outputChannels), id: \.self) { Text("\($0 + 1)").tag($0) }
                }
                Picker("Input channel", selection: $viewModel.inChannel) {
                    ForEach(0..<Int(device.inputChannels), id: \.self) { Text("\($0 + 1)").tag($0) }
                }
            }
            .pickerStyle(.menu)
            Text("\(device.outputChannels) output and \(device.inputChannels) input channels. The impulse leaves on the output channel and is captured on the input channel.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }
}

// MARK: - Declared

struct RtlDeclaredCard: View {
    let declared: RtlDeclared?
    let refresh: () -> Void

    var body: some View {
        RtlCard(title: "Declared Geometry", systemImage: "list.bullet.rectangle") {
            if let declared {
                content(declared)
            } else {
                Text("No declarations could be read.").foregroundStyle(.secondary)
            }
            HStack {
                Button {
                    refresh()
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                Text("Updates itself when the sample rate or buffer size changes. No IO is started.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder
    private func content(_ d: RtlDeclared) -> some View {
        let rate = d.sampleRate
        InfoRow(label: "Sample rate", value: rate.map { String(format: "%.1f Hz", $0) } ?? "unavailable")
        InfoRow(label: "Buffer frame size", value: "\(d.bufferFrames) fr")
        InfoRow(label: "Safety offset", value: "\(d.safetyIn) in / \(d.safetyOut) out")
        InfoRow(label: "Device latency", value: "\(d.deviceLatencyIn) in / \(d.deviceLatencyOut) out")
        InfoRow(label: "Stream latency", value: "\(d.streamLatencyIn) in / \(d.streamLatencyOut) out")
        Divider()
        // Apple's composition (coreaudio-api 2004/Oct/msg00266): a scheduling
        // half and a hardware half; only the second is in RTL_ts.
        InfoRow(label: "Declared scheduling (2 x io + safety)",
                value: RtlFormat.framesAndMilliseconds(Double(d.schedulingFrames), sampleRate: rate, decimals: 0))
        InfoRow(label: "Declared hw latency (device + stream)",
                value: RtlFormat.framesAndMilliseconds(Double(d.hardwareFrames), sampleRate: rate, decimals: 0))
        if let roundTrip = d.roundTripFrames {
            InfoRow(label: "Declared round trip",
                    value: RtlFormat.framesAndMilliseconds(Double(roundTrip), sampleRate: rate, decimals: 0))
        } else {
            Label("Declared round trip unavailable: a declaration could not be read.",
                  systemImage: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
        }
        ForEach(d.missing, id: \.self) { name in
            Label("Not read: \(name)", systemImage: "questionmark.circle")
                .font(.caption)
                .foregroundStyle(.orange)
        }
        Text("These are what the device claims, not a measurement. Use Measure with an output-to-input cable to read the physical round trip.")
            .font(.caption)
            .foregroundStyle(.secondary)
    }
}
