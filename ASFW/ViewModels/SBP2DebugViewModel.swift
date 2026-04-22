import Foundation
import Combine

@MainActor
final class SBP2DebugViewModel: ObservableObject {
    struct SessionSnapshot: Equatable {
        let loginState: UInt8
        let loginID: UInt16
        let generation: UInt16
        let lastError: Int32
        let reconnectPending: Bool

        var loginStateDescription: String {
            switch loginState {
            case 0: return "Idle"
            case 1: return "LoggingIn"
            case 2: return "LoggedIn"
            case 3: return "Reconnecting"
            case 4: return "LoggingOut"
            case 5: return "Suspended"
            case 6: return "Failed"
            default: return "Unknown(\(loginState))"
            }
        }

        var isLoggedIn: Bool { loginState == 2 }
    }

    struct InquirySummary: Equatable {
        let vendor: String
        let product: String
        let revision: String
    }

    struct SenseSummary: Equatable {
        let senseKey: UInt8
        let asc: UInt8
        let ascq: UInt8
    }

    @Published var isConnected: Bool = false
    @Published var isLoadingDevices: Bool = false
    @Published var isBusy: Bool = false
    @Published var sbp2Devices: [ASFWDriverConnector.FWDeviceInfo] = []
    @Published var selectedDeviceID: UInt64? {
        didSet {
            guard selectedDeviceID != oldValue else { return }
            selectedUnitROMOffset = selectedDevice?.sbp2Units.first?.romOffset
            clearSessionState(releaseSession: true)
        }
    }
    @Published var selectedUnitROMOffset: UInt32? {
        didSet {
            guard selectedUnitROMOffset != oldValue else { return }
            clearSessionState(releaseSession: true)
        }
    }
    @Published var sessionHandle: UInt64?
    @Published var sessionState: SessionSnapshot?
    @Published var commandResult: SBP2CommandResult?
    @Published var lastCommandName: String?
    @Published var inquirySummary: InquirySummary?
    @Published var senseSummary: SenseSummary?
    @Published var rawCDBHex: String = "12 00 00 60 00 00"
    @Published var rawDirection: SBP2CommandDataDirection = .fromTarget
    @Published var rawTransferLength: String = "96"
    @Published var rawOutgoingHex: String = ""
    @Published var statusMessage: String?
    @Published var errorMessage: String?
    @Published var lastDeviceRefresh: Date?
    @Published var lastStateRefresh: Date?

    private let connector: ASFWDriverConnector
    private let workerQueue = DispatchQueue(label: "net.mrmidi.ASFW.sbp2.debug", qos: .userInitiated)
    private var cancellables = Set<AnyCancellable>()
    private var stateTimer: Timer?

    init(connector: ASFWDriverConnector) {
        self.connector = connector
        isConnected = connector.isConnected

        connector.$isConnected
            .receive(on: DispatchQueue.main)
            .sink { [weak self] connected in
                guard let self else { return }
                self.isConnected = connected
                if connected {
                    self.refreshDevices()
                } else {
                    self.handleDisconnect()
                }
            }
            .store(in: &cancellables)
    }

    var selectedDevice: ASFWDriverConnector.FWDeviceInfo? {
        guard let selectedDeviceID else { return nil }
        return sbp2Devices.first(where: { $0.guid == selectedDeviceID })
    }

    var selectedUnit: ASFWDriverConnector.FWUnitInfo? {
        guard let selectedUnitROMOffset else { return nil }
        return selectedDevice?.sbp2Units.first(where: { $0.romOffset == selectedUnitROMOffset })
    }

    var hasSelection: Bool {
        selectedDevice != nil && selectedUnit != nil
    }

    func refreshDevices(preferredDeviceID: UInt64? = nil) {
        guard connector.isConnected else {
            handleDisconnect()
            return
        }

        isLoadingDevices = true
        errorMessage = nil

        workerQueue.async { [weak self] in
            guard let self else { return }
            let devices = self.connector.getDiscoveredDevices()?.filter(\.hasSBP2Unit) ?? []

            DispatchQueue.main.async {
                self.isLoadingDevices = false
                self.sbp2Devices = devices
                self.lastDeviceRefresh = Date()

                let targetDeviceID = preferredDeviceID ?? self.selectedDeviceID
                if let targetDeviceID, devices.contains(where: { $0.guid == targetDeviceID }) {
                    self.selectedDeviceID = targetDeviceID
                } else {
                    self.selectedDeviceID = devices.first?.guid
                }

                if let selectedDevice = self.selectedDevice {
                    let units = selectedDevice.sbp2Units
                    if let romOffset = self.selectedUnitROMOffset,
                       units.contains(where: { $0.romOffset == romOffset }) {
                        self.selectedUnitROMOffset = romOffset
                    } else {
                        self.selectedUnitROMOffset = units.first?.romOffset
                    }
                } else {
                    self.selectedUnitROMOffset = nil
                }

                self.statusMessage = devices.isEmpty
                    ? "No SBP-2 devices discovered."
                    : "Found \(devices.count) SBP-2 device\(devices.count == 1 ? "" : "s")."
            }
        }
    }

    func openDevice(_ device: ASFWDriverConnector.FWDeviceInfo) {
        refreshDevices(preferredDeviceID: device.guid)
    }

    func createSession() {
        guard let device = selectedDevice, let unit = selectedUnit else {
            errorMessage = "Select an SBP-2 device first."
            return
        }

        clearSessionState(releaseSession: true)
        isBusy = true
        errorMessage = nil
        statusMessage = "Creating SBP-2 session..."

        let guidHi = UInt32((device.guid >> 32) & 0xFFFF_FFFF)
        let guidLo = UInt32(device.guid & 0xFFFF_FFFF)
        let romOffset = unit.romOffset

        workerQueue.async { [weak self] in
            guard let self else { return }
            let handle = self.connector.createSBP2Session(guidHi: guidHi, guidLo: guidLo, romOffset: romOffset)

            DispatchQueue.main.async {
                self.isBusy = false
                guard let handle else {
                    self.errorMessage = self.connector.lastError ?? "Failed to create SBP-2 session."
                    return
                }

                self.sessionHandle = handle
                self.statusMessage = String(format: "Session created (handle=0x%llX).", handle)
                self.refreshSessionState()
                self.startStatePolling()
            }
        }
    }

    func startLogin() {
        guard let handle = sessionHandle else {
            errorMessage = "Create a session before starting login."
            return
        }

        isBusy = true
        errorMessage = nil
        statusMessage = "Starting SBP-2 login..."

        workerQueue.async { [weak self] in
            guard let self else { return }
            let ok = self.connector.startSBP2Login(handle: handle)

            DispatchQueue.main.async {
                self.isBusy = false
                if ok {
                    self.statusMessage = "Login started. Polling session state..."
                    self.refreshSessionState()
                    self.startStatePolling()
                } else {
                    self.errorMessage = self.connector.lastError ?? "Failed to start SBP-2 login."
                }
            }
        }
    }

    func refreshSessionState() {
        guard let handle = sessionHandle else { return }

        workerQueue.async { [weak self] in
            guard let self else { return }
            let state = self.connector.getSBP2SessionState(handle: handle)

            DispatchQueue.main.async {
                guard let state else {
                    self.errorMessage = self.connector.lastError ?? "Failed to read session state."
                    return
                }

                self.sessionState = SessionSnapshot(
                    loginState: state.loginState,
                    loginID: state.loginID,
                    generation: state.generation,
                    lastError: state.lastError,
                    reconnectPending: state.reconnectPending)
                self.lastStateRefresh = Date()
            }
        }
    }

    func runInquiry() {
        submitCommand(.inquiry(), label: "INQUIRY")
    }

    func runTestUnitReady() {
        submitCommand(.testUnitReady(), label: "TEST UNIT READY")
    }

    func runRequestSense() {
        submitCommand(.requestSense(), label: "REQUEST SENSE")
    }

    func runRawCommand() {
        guard let cdb = Self.parseHexBytes(rawCDBHex), !cdb.isEmpty else {
            errorMessage = "Raw CDB must contain at least one byte."
            return
        }

        let transferLength = UInt32(rawTransferLength.trimmingCharacters(in: .whitespacesAndNewlines)) ?? 0
        let outgoingData: Data
        if rawDirection == .toTarget {
            guard let bytes = Self.parseHexBytes(rawOutgoingHex) else {
                errorMessage = "Outgoing payload is not valid hex."
                return
            }
            outgoingData = Data(bytes)
        } else {
            outgoingData = Data()
        }

        let request = SBP2CommandRequest(
            cdb: cdb,
            direction: rawDirection,
            transferLength: transferLength,
            outgoingData: outgoingData)
        submitCommand(request, label: "RAW CDB")
    }

    func releaseSession() {
        clearSessionState(releaseSession: true)
    }

    private func submitCommand(_ request: SBP2CommandRequest, label: String) {
        guard let handle = sessionHandle else {
            errorMessage = "Create a session before submitting commands."
            return
        }
        guard sessionState?.isLoggedIn == true else {
            errorMessage = "Log in to the SBP-2 session before submitting commands."
            return
        }

        isBusy = true
        errorMessage = nil
        statusMessage = "Submitting \(label)..."
        commandResult = nil
        lastCommandName = label

        workerQueue.async { [weak self] in
            guard let self else { return }
            let ok = self.connector.submitSBP2Command(handle: handle, request: request)

            DispatchQueue.main.async {
                self.isBusy = false
                if ok {
                    self.statusMessage = "\(label) submitted. Waiting for result..."
                    self.pollCommandResult(handle: handle, label: label, remainingAttempts: 15)
                } else {
                    self.errorMessage = self.connector.lastError ?? "Failed to submit \(label)."
                }
            }
        }
    }

    private func pollCommandResult(handle: UInt64, label: String, remainingAttempts: Int) {
        guard remainingAttempts > 0 else {
            errorMessage = "Timed out waiting for \(label) result."
            return
        }

        workerQueue.asyncAfter(deadline: .now() + 0.4) { [weak self] in
            guard let self else { return }
            let result = self.connector.getSBP2CommandResult(handle: handle)

            DispatchQueue.main.async {
                if let result {
                    self.applyCommandResult(result, label: label)
                    return
                }

                self.pollCommandResult(handle: handle, label: label, remainingAttempts: remainingAttempts - 1)
            }
        }
    }

    private func applyCommandResult(_ result: SBP2CommandResult, label: String) {
        commandResult = result
        lastCommandName = label

        if label == "INQUIRY", result.isSuccess {
            inquirySummary = Self.parseInquirySummary(result.payload)
        }
        if label == "REQUEST SENSE", result.isSuccess {
            senseSummary = Self.parseSenseSummary(result.senseData.isEmpty ? result.payload : result.senseData)
        }

        if result.isSuccess {
            statusMessage = "\(label) completed."
        } else {
            errorMessage = "\(label) failed (transport=\(result.transportStatus), sbp=\(result.sbpStatus))."
        }
    }

    private func startStatePolling() {
        stopStatePolling()

        stateTimer = Timer.scheduledTimer(withTimeInterval: 0.8, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self else { return }
                guard self.sessionHandle != nil else {
                    self.stopStatePolling()
                    return
                }
                self.refreshSessionState()
            }
        }
    }

    private func stopStatePolling() {
        stateTimer?.invalidate()
        stateTimer = nil
    }

    private func clearSessionState(releaseSession: Bool) {
        let handle = sessionHandle
        stopStatePolling()

        if releaseSession, let handle {
            workerQueue.async { [weak self] in
                guard let self else { return }
                _ = self.connector.releaseSBP2Session(handle: handle)
            }
        }

        sessionHandle = nil
        sessionState = nil
        commandResult = nil
        lastCommandName = nil
        inquirySummary = nil
        senseSummary = nil
        lastStateRefresh = nil
    }

    private func handleDisconnect() {
        stopStatePolling()
        sbp2Devices = []
        selectedDeviceID = nil
        selectedUnitROMOffset = nil
        sessionHandle = nil
        sessionState = nil
        commandResult = nil
        lastCommandName = nil
        inquirySummary = nil
        senseSummary = nil
        errorMessage = nil
        statusMessage = "Driver not connected."
    }

    private static func parseInquirySummary(_ data: Data) -> InquirySummary? {
        guard data.count >= 36 else { return nil }

        func readASCII(_ range: Range<Int>) -> String {
            let slice = data[range]
            return String(decoding: slice, as: UTF8.self)
                .trimmingCharacters(in: .whitespacesAndNewlines.union(.controlCharacters))
        }

        return InquirySummary(
            vendor: readASCII(8..<16),
            product: readASCII(16..<32),
            revision: readASCII(32..<36))
    }

    private static func parseSenseSummary(_ data: Data) -> SenseSummary? {
        guard data.count >= 14 else { return nil }
        return SenseSummary(
            senseKey: data[data.startIndex + 2] & 0x0F,
            asc: data[data.startIndex + 12],
            ascq: data[data.startIndex + 13])
    }

    private static func parseHexBytes(_ text: String) -> [UInt8]? {
        let sanitized = text
            .replacingOccurrences(of: ",", with: " ")
            .replacingOccurrences(of: "\n", with: " ")
            .split(separator: " ")
            .map(String.init)

        if sanitized.isEmpty {
            return []
        }

        var bytes: [UInt8] = []
        bytes.reserveCapacity(sanitized.count)
        for token in sanitized {
            let normalized = token.hasPrefix("0x") || token.hasPrefix("0X")
                ? String(token.dropFirst(2))
                : token
            guard let value = UInt8(normalized, radix: 16) else {
                return nil
            }
            bytes.append(value)
        }
        return bytes
    }
}
