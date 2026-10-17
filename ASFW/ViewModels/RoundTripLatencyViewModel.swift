//
//  RoundTripLatencyViewModel.swift
//  ASFW
//
//  State and orchestration for the Round-Trip Latency section. Everything that
//  decides what a number means is in the C core (tools/rtl); the IOProc is in
//  rtl_session.c. This type only: finds the ASFW devices, reads declarations,
//  checks microphone permission, pauses the app's own driver polling, runs a
//  measurement on a background task, and presents what came back.
//

import AVFoundation
import Combine
import CoreAudio
import Foundation

@MainActor
final class RoundTripLatencyViewModel: ObservableObject {

    // MARK: Types

    enum Mode: String, CaseIterable, Identifiable {
        case declared = "Declared"
        case measure = "Measure"
        case tone = "Tone Test"
        var id: String { rawValue }
    }

    enum RunState: Equatable {
        case idle
        /// Waiting for the microphone permission prompt.
        case requestingPermission
        case running(Mode)

        var isBusy: Bool { self != .idle }
    }

    enum SelfTestState: Equatable {
        case idle
        case running
        case finished(passed: Bool, failures: Int, log: String)
    }

    struct Banner: Equatable {
        enum Kind { case error, warning, info }
        var kind: Kind
        var text: String
    }

    /// An ASFW audio device as CoreAudio lists it.
    struct DeviceChoice: Identifiable, Hashable {
        let id: AudioObjectID
        let uid: String
        let name: String
        let inputChannels: UInt32
        let outputChannels: UInt32

        var canMeasure: Bool {
            RtlAvailability.canMeasure(inputChannels: inputChannels, outputChannels: outputChannels)
        }
    }

    // MARK: Published state

    @Published private(set) var devices: [DeviceChoice] = []
    @Published var selectedDeviceID: AudioObjectID? {
        didSet { if oldValue != selectedDeviceID { deviceSelectionChanged() } }
    }
    @Published var mode: Mode = .declared

    @Published private(set) var declared: RtlDeclared?
    @Published private(set) var bufferRange: ClosedRange<UInt32>?

    // Measurement options (they mirror the command line).
    @Published var requestBufferSize = false
    @Published var bufferFrames: Int = 64
    @Published var trials: Int = 20
    @Published var automaticWindow = true
    @Published var window: Int = 4096
    @Published var outChannel: Int = 0   // zero-based
    @Published var inChannel: Int = 0
    @Published var amplitude: Double = 0.9

    // Tone options.
    @Published var toneDuration: Double = 30
    @Published var toneFrequency: Double = 997
    @Published var toneAmplitude: Double = 0.5

    /// The user's confirmation that the cable is connected and nothing else plays.
    @Published var setupConfirmed = false

    @Published private(set) var runState: RunState = .idle
    @Published private(set) var progress: Double = 0
    @Published private(set) var measurement: RtlMeasurementResult?
    @Published private(set) var toneResult: RtlToneResult?
    @Published private(set) var selfTest: SelfTestState = .idle
    @Published private(set) var banner: Banner?
    @Published private(set) var microphone: MicrophoneAccess
    /// Another client is already running the selected device.
    @Published private(set) var deviceBusyElsewhere = false

    // MARK: Limits

    static let maximumTrials = Int(RTL_MAX_TRIALS)
    static let maximumWindow = Int(RTL_MAX_WINDOW)
    static let minimumWindow = 128
    static let toneDurationRange = 1.0...600.0
    static let edgeGuardFrames = UInt32(RTL_EDGE_GUARD_FRAMES)

    // MARK: Dependencies

    private let pauseDriverPolling: (Bool) -> Void
    private let readMicrophoneAccess: () -> MicrophoneAccess
    private let requestMicrophoneAccess: () async -> Bool
    private let appVersion: String

    private var runControl: RtlRunControl?
    /// Why the running measurement was cancelled when the user did not ask for
    /// it; shown instead of the plain "cancelled" notice.
    private var cancellationNote: String?
    private var progressTask: Task<Void, Never>?
    private var propertyObserver: AudioPropertyObserver?

    init(pauseDriverPolling: @escaping (Bool) -> Void,
         microphoneAccess: @escaping () -> MicrophoneAccess = RoundTripLatencyViewModel.systemMicrophoneAccess,
         requestMicrophoneAccess: @escaping () async -> Bool = RoundTripLatencyViewModel.requestSystemMicrophoneAccess,
         appVersion: String = RoundTripLatencyViewModel.bundleVersionString()) {
        self.pauseDriverPolling = pauseDriverPolling
        self.readMicrophoneAccess = microphoneAccess
        self.requestMicrophoneAccess = requestMicrophoneAccess
        self.appVersion = appVersion
        self.microphone = microphoneAccess()
    }

    // MARK: Devices

    var selectedDevice: DeviceChoice? {
        devices.first { $0.id == selectedDeviceID }
    }

    /// Replaces the device list from the CoreAudio devices that are ASFW's.
    func updateDevices(from published: [AudioWrapperDevice]) {
        let choices = published.map { device in
            DeviceChoice(id: device.id, uid: device.uid, name: device.name,
                         inputChannels: RtlMeasurementRunner.channelCount(deviceID: device.id, input: true),
                         outputChannels: RtlMeasurementRunner.channelCount(deviceID: device.id, input: false))
        }
        apply(devices: choices)
    }

    /// Separate from updateDevices so the selection rules are testable without
    /// CoreAudio.
    func apply(devices choices: [DeviceChoice]) {
        if choices != devices { devices = choices }
        let resolved = Self.resolveSelection(current: selectedDeviceID, available: choices)
        if let running = selectedDeviceID, runState.isBusy, resolved != running {
            // The device under test is gone: nothing it would deliver is a measurement.
            cancellationNote = "The device disappeared while it was being measured. Nothing from this run is a measurement."
            cancel()
        }
        if resolved != selectedDeviceID { selectedDeviceID = resolved }
    }

    /// Keeps the current device while it is listed; otherwise prefers a duplex
    /// device (the only kind that can be measured), then the first listed.
    static func resolveSelection(current: AudioObjectID?, available: [DeviceChoice]) -> AudioObjectID? {
        if let current, available.contains(where: { $0.id == current }) { return current }
        return available.first(where: \.canMeasure)?.id ?? available.first?.id
    }

    private func deviceSelectionChanged() {
        measurement = nil
        toneResult = nil
        setupConfirmed = false
        banner = nil
        declared = nil
        bufferRange = nil
        propertyObserver = nil
        guard let id = selectedDeviceID else {
            deviceBusyElsewhere = false
            return
        }
        refreshDeclared()
        // Follow the declarations while the view is open: a rate or buffer size
        // changed in Audio MIDI Setup moves every number on the Declared tab.
        propertyObserver = AudioPropertyObserver(
            objectID: id,
            selectors: [kAudioDevicePropertyNominalSampleRate,
                        kAudioDevicePropertyBufferFrameSize,
                        kAudioDevicePropertyLatency,
                        kAudioDevicePropertySafetyOffset,
                        kAudioDevicePropertyStreams,
                        kAudioDevicePropertyStreamConfiguration,
                        kAudioStreamPropertyLatency]
        ) { [weak self] in self?.refreshDeclared() }
    }

    /// Re-reads the declarations (property reads only; no IO, no driver traffic).
    func refreshDeclared() {
        // A running measurement owns the device; its own result carries the
        // declarations it used.
        guard !runState.isBusy, let id = selectedDeviceID else { return }
        declared = RtlMeasurementRunner.readDeclared(deviceID: id)
        let range = AudioWrapperDevice(id: id).bufferFrameSizeRange
        bufferRange = range.max >= range.min && range.max > 0 ? range.min...range.max : nil
        deviceBusyElsewhere = RtlMeasurementRunner.isRunningElsewhere(deviceID: id)
        if let updated = devices.first(where: { $0.id == id }) {
            let refreshed = DeviceChoice(
                id: id, uid: updated.uid, name: updated.name,
                inputChannels: RtlMeasurementRunner.channelCount(deviceID: id, input: true),
                outputChannels: RtlMeasurementRunner.channelCount(deviceID: id, input: false))
            if refreshed != updated, let index = devices.firstIndex(of: updated) {
                devices[index] = refreshed
            }
        }
        clampChannels()
    }

    private func clampChannels() {
        guard let device = selectedDevice else { return }
        if device.outputChannels > 0, outChannel >= Int(device.outputChannels) { outChannel = 0 }
        if device.inputChannels > 0, inChannel >= Int(device.inputChannels) { inChannel = 0 }
    }

    // MARK: Derived

    var effectiveWindow: UInt32 {
        if automaticWindow, let declared { return min(declared.autoWindowFrames, UInt32(Self.maximumWindow)) }
        return UInt32(min(max(window, Self.minimumWindow), Self.maximumWindow))
    }

    var channelsAreValid: Bool {
        guard let device = selectedDevice else { return false }
        return RtlAvailability.channelsAreValid(
            out: outChannel, in: inChannel,
            outputChannels: device.outputChannels, inputChannels: device.inputChannels)
    }

    var estimatedMeasurementSeconds: Double? {
        guard let rate = declared?.sampleRate else { return nil }
        return rtl_impulse_expected_seconds(UInt32(clamping: trials), effectiveWindow, rate)
    }

    /// Why Start is unavailable; nil when it can run.
    func startBlocker(for target: Mode) -> String? {
        guard let device = selectedDevice else { return "No ASFW audio device." }
        guard device.canMeasure else { return "This device needs both input and output channels." }
        if runState.isBusy { return "A run is in progress." }
        if selfTest == .running { return "The self-test is running; it would disturb the timing." }
        // Before anything else about the run: without permission the input
        // delivers silence, and the result would read "no signal" for the wrong reason.
        if let reason = microphone.blockingExplanation { return reason }
        if declared?.sampleRate == nil { return "CoreAudio does not report a sample rate for this device." }
        if !channelsAreValid { return "The selected channels do not exist on this device." }
        if !setupConfirmed { return "Confirm the physical setup first." }
        if target == .tone {
            if !Self.toneDurationRange.contains(toneDuration) { return "Duration must be 1 to 600 seconds." }
            if let rate = declared?.sampleRate, !(toneFrequency > 0 && toneFrequency < rate / 2) {
                return String(format: "Frequency must be between 0 and %.0f Hz.", rate / 2)
            }
        }
        return nil
    }

    // MARK: Permission

    func refreshMicrophoneAccess() {
        microphone = readMicrophoneAccess()
    }

    nonisolated static func systemMicrophoneAccess() -> MicrophoneAccess {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: return .authorized
        case .notDetermined: return .notDetermined
        case .denied: return .denied
        case .restricted: return .restricted
        @unknown default: return .denied
        }
    }

    nonisolated static func requestSystemMicrophoneAccess() async -> Bool {
        await AVCaptureDevice.requestAccess(for: .audio)
    }

    nonisolated static func bundleVersionString() -> String {
        let info = Bundle.main.infoDictionary
        let version = info?["CFBundleShortVersionString"] as? String ?? "unknown"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        return "ASFW \(version) (\(build))"
    }

    // MARK: Running

    func startMeasurement() {
        guard startBlocker(for: .measure) == nil else { return }
        Task { await performMeasurement() }
    }

    func startTone() {
        guard startBlocker(for: .tone) == nil else { return }
        Task { await performTone() }
    }

    func cancel() {
        runControl?.cancel()
    }

    /// The view left the screen: whatever it started must not keep the device.
    func viewDidDisappear() {
        cancel()
    }

    private func performMeasurement() async {
        guard let device = selectedDevice, let control = await beginRun(.measure) else { return }
        defer { endRun() }

        let request = RtlMeasureRequest(
            deviceID: device.id,
            requestedBufferFrames: requestBufferSize ? UInt32(clamping: bufferFrames) : nil,
            trials: UInt32(clamping: trials),
            window: automaticWindow ? nil : effectiveWindow,
            outChannel: UInt32(clamping: outChannel),
            inChannel: UInt32(clamping: inChannel),
            amplitude: Float(amplitude),
            appVersion: appVersion,
            optionsDescription: optionsDescription)
        let outcome = await Task.detached(priority: .userInitiated) {
            RtlMeasurementRunner.measure(request, control: control)
        }.value

        switch outcome {
        case .success(_) where cancellationNote != nil:
            banner = cancellationBanner(default: "Measurement cancelled.")
        case .success(let result):
            measurement = result
            if result.outcome == .timedOut {
                banner = Banner(kind: .warning,
                                text: "The measurement timed out with \(result.trialsRun) of \(result.trialsRequested) trials: IO may have stalled.")
            } else if result.bufferWasCoerced, let requested = result.bufferRequested {
                banner = Banner(kind: .warning,
                                text: "Buffer size: requested \(requested) frames, the device is at \(result.bufferActual). This run is at \(result.bufferActual), not \(requested).")
            }
        case .failure(.cancelled):
            banner = cancellationBanner(default: "Measurement cancelled.")
        case .failure(let error):
            banner = Banner(kind: .error, text: error.localizedDescription)
        }
    }

    private func performTone() async {
        guard let device = selectedDevice, let control = await beginRun(.tone) else { return }
        defer { endRun() }

        let request = RtlToneRequest(
            deviceID: device.id,
            requestedBufferFrames: requestBufferSize ? UInt32(clamping: bufferFrames) : nil,
            durationSeconds: toneDuration,
            frequencyHz: toneFrequency,
            amplitude: Float(toneAmplitude),
            outChannel: UInt32(clamping: outChannel),
            inChannel: UInt32(clamping: inChannel),
            maxListedEvents: 50)
        let outcome = await Task.detached(priority: .userInitiated) {
            RtlMeasurementRunner.tone(request, control: control)
        }.value

        switch outcome {
        case .success(_) where cancellationNote != nil:
            banner = cancellationBanner(default: "Tone test cancelled.")
        case .success(let result):
            toneResult = result
            if result.outcome == .timedOut {
                banner = Banner(kind: .warning, text: "The tone capture timed out: IO may have stalled.")
            } else if result.bufferWasCoerced, let requested = result.bufferRequested {
                banner = Banner(kind: .warning,
                                text: "Buffer size: requested \(requested) frames, the device is at \(result.bufferActual). This run is at \(result.bufferActual), not \(requested).")
            }
        case .failure(.cancelled):
            banner = cancellationBanner(default: "Tone test cancelled.")
        case .failure(let error):
            banner = Banner(kind: .error, text: error.localizedDescription)
        }
    }

    /// Checks permission, takes the device and silences the app's own driver
    /// polling for the run. Returns nil when the run must not start.
    private func beginRun(_ target: Mode) async -> RtlRunControl? {
        guard !runState.isBusy else { return nil }
        banner = nil
        cancellationNote = nil
        runState = .requestingPermission
        var access = readMicrophoneAccess()
        if access == .notDetermined {
            _ = await requestMicrophoneAccess()
            access = readMicrophoneAccess()
        }
        microphone = access
        if let reason = access.blockingExplanation {
            banner = Banner(kind: .error, text: reason)
            runState = .idle
            return nil
        }
        guard access.allowsCapture else {
            // Still undetermined after asking: do not start IO that would capture silence.
            banner = Banner(kind: .error, text: "Microphone access was not granted.")
            runState = .idle
            return nil
        }

        measurementCleared(for: target)
        runState = .running(target)
        progress = 0
        pauseDriverPolling(true)
        let control = RtlRunControl()
        runControl = control
        progressTask = Task { [weak self] in
            while !Task.isCancelled {
                self?.progress = control.fraction()
                try? await Task.sleep(for: .milliseconds(100))
            }
        }
        return control
    }

    private func cancellationBanner(default text: String) -> Banner {
        guard let note = cancellationNote else { return Banner(kind: .info, text: text) }
        return Banner(kind: .warning, text: note)
    }

    private func measurementCleared(for target: Mode) {
        switch target {
        case .measure: measurement = nil
        case .tone: toneResult = nil
        case .declared: break
        }
    }

    private func endRun() {
        progressTask?.cancel()
        progressTask = nil
        runControl = nil
        progress = 0
        runState = .idle
        pauseDriverPolling(false)
        // The setup stays confirmed for repeat runs on the same device, but the
        // declarations may have moved while the device was busy.
        refreshDeclared()
    }

    /// The options as they were set, for the evidence record.
    var optionsDescription: String {
        let windowText = automaticWindow ? "auto" : "\(effectiveWindow)"
        let frames = requestBufferSize ? "\(bufferFrames)" : "device"
        return "frames=\(frames) trials=\(trials) window=\(windowText) "
            + "out-ch=\(outChannel) in-ch=\(inChannel) amp=\(String(format: "%.2f", amplitude))"
    }

    // MARK: Self-test

    func runSelfTest() {
        guard selfTest != .running, !runState.isBusy else { return }
        selfTest = .running
        Task {
            let outcome = await Task.detached(priority: .userInitiated) {
                RtlMeasurementRunner.selfTest()
            }.value
            selfTest = .finished(passed: outcome.failures == 0, failures: outcome.failures,
                                 log: outcome.log)
        }
    }

    // MARK: Export

    var canExport: Bool { measurement?.json != nil }

    var suggestedExportName: String {
        guard let result = measurement else { return "rtl.json" }
        return RtlFormat.exportFileName(deviceName: result.deviceName, sampleRate: result.sampleRate,
                                        date: Date())
    }

    func exportEvidence(to url: URL) {
        guard let data = measurement?.json else { return }
        do {
            try data.write(to: url, options: .atomic)
            banner = Banner(kind: .info, text: "Saved \(url.lastPathComponent).")
        } catch {
            banner = Banner(kind: .error, text: "Could not save the evidence file: \(error.localizedDescription)")
        }
    }
}
