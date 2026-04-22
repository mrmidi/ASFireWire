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

    @Published var isConnected: Bool = false
    @Published var isLoadingDevices: Bool = false
    @Published var isBusy: Bool = false
    @Published var storageDevices: [ASFWDriverConnector.FWDeviceInfo] = []
    @Published var selectedDeviceID: UInt64? {
        didSet {
            guard selectedDeviceID != oldValue else { return }
            selectedUnitROMOffset = selectedDevice?.storageUnits.first?.romOffset
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
    @Published var inquiryData: Data?
    @Published var inquirySummary: InquirySummary?
    @Published var statusMessage: String?
    @Published var errorMessage: String?
    @Published var lastDeviceRefresh: Date?
    @Published var lastStateRefresh: Date?

    nonisolated(unsafe) private let connector: ASFWDriverConnector
    private let workerQueue = DispatchQueue(label: "net.mrmidi.ASFW.sbp2.debug", qos: .userInitiated)
    private var cancellables = Set<AnyCancellable>()
    private var stateTimer: Timer?

    init(connector: ASFWDriverConnector) {
        self.connector = connector
        self.isConnected = connector.isConnected

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
        return storageDevices.first(where: { $0.guid == selectedDeviceID })
    }

    var selectedUnit: ASFWDriverConnector.FWUnitInfo? {
        guard let selectedUnitROMOffset else { return nil }
        return selectedDevice?.storageUnits.first(where: { $0.romOffset == selectedUnitROMOffset })
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
            let devices = self.connector.getDiscoveredDevices()?.filter(\.isStorage) ?? []

            DispatchQueue.main.async {
                self.isLoadingDevices = false
                self.storageDevices = devices
                self.lastDeviceRefresh = Date()

                let targetDeviceID = preferredDeviceID ?? self.selectedDeviceID
                if let targetDeviceID,
                   devices.contains(where: { $0.guid == targetDeviceID }) {
                    self.selectedDeviceID = targetDeviceID
                } else {
                    self.selectedDeviceID = devices.first?.guid
                }

                if let selectedDevice = self.selectedDevice {
                    let units = selectedDevice.storageUnits
                    if let romOffset = self.selectedUnitROMOffset,
                       units.contains(where: { $0.romOffset == romOffset }) {
                        self.selectedUnitROMOffset = romOffset
                    } else {
                        self.selectedUnitROMOffset = units.first?.romOffset
                    }
                } else {
                    self.selectedUnitROMOffset = nil
                }

                if devices.isEmpty {
                    self.statusMessage = "No SBP-2 storage devices discovered."
                } else {
                    self.statusMessage = "Found \(devices.count) SBP-2 storage device\(devices.count == 1 ? "" : "s")."
                }
            }
        }
    }

    func openDevice(_ device: ASFWDriverConnector.FWDeviceInfo) {
        refreshDevices(preferredDeviceID: device.guid)
    }

    func createSession() {
        guard let device = selectedDevice, let unit = selectedUnit else {
            errorMessage = "Select an SBP-2 storage device first."
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
                    reconnectPending: state.reconnectPending
                )
                self.lastStateRefresh = Date()
            }
        }
    }

    func runInquiry() {
        guard let handle = sessionHandle else {
            errorMessage = "Create a session before running INQUIRY."
            return
        }
        guard sessionState?.isLoggedIn == true else {
            errorMessage = "Log in to the SBP-2 session before running INQUIRY."
            return
        }

        isBusy = true
        errorMessage = nil
        statusMessage = "Submitting SCSI INQUIRY..."
        inquiryData = nil
        inquirySummary = nil

        workerQueue.async { [weak self] in
            guard let self else { return }
            let ok = self.connector.submitSBP2Inquiry(handle: handle)

            DispatchQueue.main.async {
                self.isBusy = false
                if ok {
                    self.statusMessage = "INQUIRY submitted. Waiting for result..."
                    self.pollInquiryResult(handle: handle, remainingAttempts: 15)
                } else {
                    self.errorMessage = self.connector.lastError ?? "Failed to submit INQUIRY."
                }
            }
        }
    }

    func releaseSession() {
        clearSessionState(releaseSession: true)
    }

    private func pollInquiryResult(handle: UInt64, remainingAttempts: Int) {
        guard remainingAttempts > 0 else {
            errorMessage = "Timed out waiting for INQUIRY result."
            return
        }

        workerQueue.asyncAfter(deadline: .now() + 0.4) { [weak self] in
            guard let self else { return }
            let data = self.connector.getSBP2InquiryResult(handle: handle)

            DispatchQueue.main.async {
                if let data {
                    self.inquiryData = data
                    self.inquirySummary = Self.parseInquirySummary(data)
                    self.statusMessage = "INQUIRY completed."
                    return
                }

                self.pollInquiryResult(handle: handle, remainingAttempts: remainingAttempts - 1)
            }
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
        inquiryData = nil
        inquirySummary = nil
        lastStateRefresh = nil
    }

    private func handleDisconnect() {
        stopStatePolling()
        storageDevices = []
        selectedDeviceID = nil
        selectedUnitROMOffset = nil
        sessionHandle = nil
        sessionState = nil
        inquiryData = nil
        inquirySummary = nil
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
            revision: readASCII(32..<36)
        )
    }
}
