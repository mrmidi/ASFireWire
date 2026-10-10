import Foundation
import Combine

final class DuetControlViewModel: ObservableObject {
    @Published var isConnected: Bool = false
    @Published var isLoading: Bool = false
    @Published var isApplying: Bool = false
    @Published var errorMessage: String?
    @Published var infoMessage: String?

    @Published private(set) var hasInputState = false
    @Published private(set) var hasMixerState = false
    @Published var duetGUID: UInt64?

    @Published var outputParams: DuetOutputParams = DuetOutputParams()
    @Published var inputParams: DuetInputParams = DuetInputParams()
    @Published var mixerParams: DuetMixerParams = DuetMixerParams()
    @Published var displayParams: DuetDisplayParams = DuetDisplayParams()

    @Published var firmwareID: UInt32?
    @Published var hardwareID: UInt32?
    @Published var selectedOutputBank: DuetOutputBank = .output1

    @Published var lastRefreshTime: Date?

    private let connector: ASFWDriverConnector
    private var cancellables = Set<AnyCancellable>()
    private var refreshRequestID = UUID()
    private var pendingMixerWrite: DispatchWorkItem?
    private var pendingInputGainWrite: DispatchWorkItem?
    private let inputWriteQueue = DispatchQueue(label: "net.asfw.app.Duet.input-write", qos: .userInitiated)
    private var pendingMixerDestination: Int = 0
    private var pendingMixerSource: Int = 0
    private var pendingMixerValue: UInt16 = DuetMixerParams.gainMin
    private var pendingInputGainChannel: Int = 0
    private var pendingInputGainValue: UInt8 = DuetInputParams.gainMin

    init(connector: ASFWDriverConnector) {
        self.connector = connector

        connector.$isConnected
            .receive(on: DispatchQueue.main)
            .sink { [weak self] connected in
                guard let self else { return }
                self.isConnected = connected
                if connected {
                    self.refresh()
                } else {
                    self.refreshRequestID = UUID()
                    self.isLoading = false
                    self.hasInputState = false
                    self.hasMixerState = false
                    self.pendingMixerWrite?.cancel()
                    self.pendingInputGainWrite?.cancel()
                    self.duetGUID = nil
                    self.errorMessage = "Driver not connected"
                }
            }
            .store(in: &cancellables)

        isConnected = connector.isConnected
    }

    deinit {
        pendingMixerWrite?.cancel()
        pendingInputGainWrite?.cancel()
    }

    var selectedDestinationIndex: Int {
        selectedOutputBank.rawValue
    }

    func refresh() {
        guard connector.isConnected else {
            errorMessage = "Driver not connected"
            return
        }

        guard !isLoading, !isApplying else { return }
        let requestID = UUID()
        refreshRequestID = requestID
        isLoading = true
        errorMessage = nil
        infoMessage = nil

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }

            guard let guid = self.connector.getFirstDuetUnitGUID() else {
                DispatchQueue.main.async {
                    guard self.refreshRequestID == requestID, self.isConnected else { return }
                    self.isLoading = false
                    self.hasInputState = false
                    self.hasMixerState = false
                    self.duetGUID = nil
                    self.errorMessage = "No Apogee Duet AV/C unit found"
                }
                return
            }

            let snapshot = self.connector.refreshDuetState(guid: guid)
            DispatchQueue.main.async {
                guard self.refreshRequestID == requestID, self.isConnected else { return }
                self.isLoading = false
                self.applyRefreshedState(guid: guid, snapshot: snapshot)
            }
        }
    }

    func applyRefreshedState(guid: UInt64, snapshot: DuetStateSnapshot?) {
        duetGUID = guid
        hasInputState = snapshot?.inputParams != nil
        hasMixerState = snapshot?.mixerParams != nil
        firmwareID = snapshot?.firmwareID
        hardwareID = snapshot?.hardwareID
        lastRefreshTime = snapshot?.updatedAt
        if let input = snapshot?.inputParams { inputParams = input }
        if let mixer = snapshot?.mixerParams { mixerParams = mixer }
        if let output = snapshot?.outputParams { outputParams = output }
        if let display = snapshot?.displayParams { displayParams = display }
        errorMessage = hasInputState && hasMixerState ? nil
            : "Duet was detected, but some controls could not be read. Refresh to try again."
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
        performInputWrite(failureMessage: "Failed to apply input source") { [connector] guid in
            connector.setDuetInputSource(guid: guid, channel: channel, source: source)
        }
    }

    func setInputXlrNominalLevel(channel: Int, level: DuetInputXlrNominalLevel) {
        guard channel >= 0 && channel < inputParams.xlrNominalLevels.count else { return }
        inputParams.xlrNominalLevels[channel] = level
        performInputWrite(failureMessage: "Failed to apply XLR nominal level") { [connector] guid in
            connector.setDuetInputXlrNominalLevel(guid: guid, channel: channel, level: level)
        }
    }

    func setInputPhantom(channel: Int, enabled: Bool) {
        guard channel >= 0 && channel < inputParams.phantomPowerings.count else { return }
        inputParams.phantomPowerings[channel] = enabled
        performInputWrite(failureMessage: "Failed to apply phantom power") { [connector] guid in
            connector.setDuetInputPhantom(guid: guid, channel: channel, enabled: enabled)
        }
    }

    func setInputPolarity(channel: Int, inverted: Bool) {
        guard channel >= 0 && channel < inputParams.polarities.count else { return }
        inputParams.polarities[channel] = inverted
        performInputWrite(failureMessage: "Failed to apply input polarity") { [connector] guid in
            connector.setDuetInputPolarity(guid: guid, channel: channel, inverted: inverted)
        }
    }

    func setClickless(_ enabled: Bool) {
        inputParams.clickless = enabled
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

        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            let ok = self.connector.setDuetMixerGain(guid: guid,
                                                     destination: destination,
                                                     source: source,
                                                     gain: value)
            DispatchQueue.main.async {
                if !ok {
                    self.errorMessage = "Failed to apply mixer value"
                } else {
                    self.errorMessage = nil
                    self.lastRefreshTime = Date()
                }
            }
        }

        pendingMixerWrite = work
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 0.12, execute: work)
    }

    private func scheduleInputGainWrite() {
        pendingInputGainWrite?.cancel()

        guard let guid = duetGUID else { return }
        let channel = pendingInputGainChannel
        let value = pendingInputGainValue

        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            let ok = self.connector.setDuetInputGain(guid: guid, channel: channel, gain: value)
            DispatchQueue.main.async {
                if !ok {
                    self.errorMessage = "Failed to apply input gain"
                } else {
                    self.errorMessage = nil
                    self.lastRefreshTime = Date()
                }
            }
        }

        pendingInputGainWrite = work
        inputWriteQueue.asyncAfter(deadline: .now() + 0.12, execute: work)
    }

    private func performInputWrite(failureMessage: String, _ operation: @escaping (_ guid: UInt64) -> Bool) {
        pendingInputGainWrite?.cancel()
        pendingInputGainWrite = nil

        guard let guid = duetGUID else { return }

        isApplying = true
        inputWriteQueue.async { [weak self] in
            guard let self else { return }
            let ok = operation(guid)
            DispatchQueue.main.async {
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
