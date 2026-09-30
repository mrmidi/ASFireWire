import Foundation
import Combine

@MainActor
@Observable
final class DuetControlViewModel {
    var isConnected: Bool = false
    var isLoading: Bool = false
    var isApplying: Bool = false
    var errorMessage: String?
    var infoMessage: String?

    var duetGUID: UInt64?

    var outputParams: DuetOutputParams = DuetOutputParams()
    var inputParams: DuetInputParams = DuetInputParams()
    var mixerParams: DuetMixerParams = DuetMixerParams()
    var displayParams: DuetDisplayParams = DuetDisplayParams()

    var firmwareID: UInt32?
    var hardwareID: UInt32?
    var selectedOutputBank: DuetOutputBank = .output1

    var lastRefreshTime: Date?

    @ObservationIgnored private let connectorObservable: ASFWDriverConnector.Observable
    @ObservationIgnored private var cancellables = Set<AnyCancellable>()
    @ObservationIgnored private var pendingMixerWrite: Task<Void, Never>?
    @ObservationIgnored private var pendingInputGainWrite: Task<Void, Never>?
    @ObservationIgnored private var pendingMixerDestination: Int = 0
    @ObservationIgnored private var pendingMixerSource: Int = 0
    @ObservationIgnored private var pendingMixerValue: UInt16 = DuetMixerParams.gainMin
    @ObservationIgnored private var pendingInputGainChannel: Int = 0
    @ObservationIgnored private var pendingInputGainValue: UInt8 = DuetInputParams.gainMin

    init(connectorObservable: ASFWDriverConnector.Observable) {
        self.connectorObservable = connectorObservable

        connectorObservable.$isConnected
            .receive(on: DispatchQueue.main)
            .sink { [weak self] connected in
                guard let self else { return }
                self.isConnected = connected
                if connected {
                    self.refresh()
                } else {
                    self.duetGUID = nil
                    self.errorMessage = "Driver not connected"
                }
            }
            .store(in: &cancellables)

        isConnected = connectorObservable.isConnected
    }

    deinit {
        // Note: deinit in @MainActor classes is nonisolated in Swift 6.
        // DispatchWorkItem is not Sendable, so we cannot cancel it in deinit.
        // The work items will be automatically discarded when the class is deallocated.
        // We rely on the fact that cancel() is idempotent and safe to not call.
    }

    var selectedDestinationIndex: Int {
        selectedOutputBank.rawValue
    }

    func refresh() {
        guard connectorObservable.isConnected else {
            errorMessage = "Driver not connected"
            return
        }

        isLoading = true
        errorMessage = nil
        infoMessage = nil

        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak self, weak connector] in
            guard let self, let connector else { return }

            guard let guid = await connector.getFirstDuetUnitGUID() else {
                Task { @MainActor in
                    self.isLoading = false
                    self.duetGUID = nil
                    self.errorMessage = "No Apogee Duet AV/C unit found"
                }
                return
            }

            let snapshot = await connector.refreshDuetState(guid: guid)
            let cached = await connector.getDuetCachedState(guid: guid)
            let state = snapshot ?? cached

            Task { @MainActor in
                self.isLoading = false
                self.duetGUID = guid

                guard let state else {
                    self.errorMessage = "Failed to read Duet state"
                    return
                }

                if let output = state.outputParams {
                    self.outputParams = output
                }
                if let input = state.inputParams {
                    self.inputParams = input
                }
                if let mixer = state.mixerParams {
                    self.mixerParams = mixer
                }
                if let display = state.displayParams {
                    self.displayParams = display
                }

                self.firmwareID = state.firmwareID
                self.hardwareID = state.hardwareID
                self.lastRefreshTime = Date()
                self.errorMessage = nil
            }
        }
    }

    func mixerGain(source: Int) -> Double {
        return Double(mixerParams.gain(destination: selectedDestinationIndex, source: source))
    }

    func setMixerGain(source: Int, gain: Double) {
        guard source >= 0 && source < 4 else { return }
        let clamped = max(Double(DuetMixerParams.gainMin), min(Double(DuetMixerParams.gainMax), gain))
        let clampedValue = UInt16(clamped)
        mixerParams.setGain(destination: selectedDestinationIndex,
                            source: source,
                            value: clampedValue)
        pendingMixerDestination = selectedDestinationIndex
        pendingMixerSource = source
        pendingMixerValue = clampedValue
        scheduleMixerWrite()
    }

    func setInputGain(channel: Int, gain: Double) {
        guard channel >= 0 && channel < inputParams.gains.count else { return }
        let clamped = max(Double(DuetInputParams.gainMin), min(Double(DuetInputParams.gainMax), gain))
        let clampedValue = UInt8(clamped)
        inputParams.gains[channel] = clampedValue
        pendingInputGainChannel = channel
        pendingInputGainValue = clampedValue
        scheduleInputGainWrite()
    }

    func setInputSource(channel: Int, source: DuetInputSource) {
        guard channel >= 0 && channel < inputParams.sources.count else { return }
        inputParams.sources[channel] = source
        let connector = connectorObservable.connector
        performInputWrite(failureMessage: "Failed to apply input source") { [connector] guid in
            connector.setDuetInputSource(guid: guid, channel: channel, source: source)
        }
    }

    func setInputXlrNominalLevel(channel: Int, level: DuetInputXlrNominalLevel) {
        guard channel >= 0 && channel < inputParams.xlrNominalLevels.count else { return }
        inputParams.xlrNominalLevels[channel] = level
        let connector = connectorObservable.connector
        performInputWrite(failureMessage: "Failed to apply XLR nominal level") { [connector] guid in
            connector.setDuetInputXlrNominalLevel(guid: guid, channel: channel, level: level)
        }
    }

    func setInputPhantom(channel: Int, enabled: Bool) {
        guard channel >= 0 && channel < inputParams.phantomPowerings.count else { return }
        inputParams.phantomPowerings[channel] = enabled
        let connector = connectorObservable.connector
        performInputWrite(failureMessage: "Failed to apply phantom power") { [connector] guid in
            connector.setDuetInputPhantom(guid: guid, channel: channel, enabled: enabled)
        }
    }

    func setInputPolarity(channel: Int, inverted: Bool) {
        guard channel >= 0 && channel < inputParams.polarities.count else { return }
        inputParams.polarities[channel] = inverted
        let connector = connectorObservable.connector
        performInputWrite(failureMessage: "Failed to apply input polarity") { [connector] guid in
            connector.setDuetInputPolarity(guid: guid, channel: channel, inverted: inverted)
        }
    }

    func setClickless(_ enabled: Bool) {
        inputParams.clickless = enabled
        let connector = connectorObservable.connector
        performInputWrite(failureMessage: "Failed to apply clickless mode") { [connector] guid in
            connector.setDuetInputClickless(guid: guid, enabled: enabled)
        }
    }

    private func scheduleMixerWrite() {
        pendingMixerWrite?.cancel()

        guard let guid = duetGUID else { return }
        let destination = pendingMixerDestination
        let source = pendingMixerSource
        let value = pendingMixerValue

        let connector = connectorObservable.connector
        pendingMixerWrite = Task.detached(priority: .utility) { [weak connector, weak self] in
            try? await Task.sleep(until: .now.advanced(by: .milliseconds(120)))
            guard let connector, !Task.isCancelled else { return }
            let ok = await connector.setDuetMixerGain(guid: guid,
                                                     destination: destination,
                                                     source: source,
                                                     gain: value)
            guard !Task.isCancelled else { return }
            Task { @MainActor in
                guard let self else { return }
                if !ok {
                    self.errorMessage = "Failed to apply mixer value"
                } else {
                    self.errorMessage = nil
                    self.lastRefreshTime = Date()
                }
            }
        }
    }

    private func scheduleInputGainWrite() {
        pendingInputGainWrite?.cancel()

        guard let guid = duetGUID else { return }
        let channel = pendingInputGainChannel
        let value = pendingInputGainValue

        let connector = connectorObservable.connector
        pendingInputGainWrite = Task.detached(priority: .utility) { [weak connector, weak self] in
            try? await Task.sleep(until: .now.advanced(by: .milliseconds(120)))
            guard let connector, !Task.isCancelled else { return }
            let ok = await connector.setDuetInputGain(guid: guid, channel: channel, gain: value)
            guard !Task.isCancelled else { return }
            Task { @MainActor in
                guard let self else { return }
                if !ok {
                    self.errorMessage = "Failed to apply input gain"
                } else {
                    self.errorMessage = nil
                    self.lastRefreshTime = Date()
                }
            }
        }
    }

    private func performInputWrite(failureMessage: String, _ operation: @escaping @ASFWDriverConnectorQueue @Sendable (_ guid: UInt64) -> Bool) {
        pendingInputGainWrite?.cancel()
        pendingInputGainWrite = nil

        guard let guid = duetGUID else { return }

        isApplying = true
        Task { @ASFWDriverConnectorQueue [weak self] in
            guard let self else { return }
            let ok = operation(guid)
            Task { @MainActor in
                self.isApplying = false
                if !ok {
                    self.errorMessage = failureMessage
                } else {
                    self.errorMessage = nil
                    self.lastRefreshTime = Date()
                }
            }
        }
    }
}
