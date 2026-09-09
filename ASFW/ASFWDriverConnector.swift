import Foundation
import Combine
import IOKit
import SystemExtensions
import Darwin.Mach

final class ASFWDriverConnector: ObservableObject {
    // MARK: - Types

    enum Method: UInt32 {
        case getBusResetCount = 0
        case getBusResetHistory = 1
        case getControllerStatus = 2
        case getMetricsSnapshot = 3
        case clearHistory = 4
        case getSelfIDCapture = 5
        case getTopologySnapshot = 6
        case ping = 7
        case asyncRead = 8
        case asyncWrite = 9
        case registerStatusListener = 10
        case getTransactionResult = 12
        case exportConfigROM = 14
        case triggerROMRead = 15
        case getDiscoveredDevices = 16
        case asyncCompareSwap = 17
        case getDriverVersion = 18
        case setAsyncVerbosity = 19
        case setHexDumps = 20
        case getLogConfig = 21
        case getAVCUnits = 22
        case getSubunitCapabilities = 23
        case getSubunitDescriptor = 24
        case reScanAVCUnits = 25
        // IRM test methods (temporary for Phase 0.5)
        case testIRMAllocation = 26
        case testIRMRelease = 27
        // CMP test methods (temporary for Phase 0.5)
        case testCMPConnectOPCR = 28
        case testCMPDisconnectOPCR = 29
        case testCMPConnectIPCR = 30
        case testCMPDisconnectIPCR = 31
        // Isoch Stream Control & Metrics
        case startIsochReceive = 32
        case stopIsochReceive = 33
        case getIsochRxMetrics = 34
        // IT DMA Allocation (no CMP)
        case startIsochTransmit = 36
        case stopIsochTransmit = 37
        // AV/C raw FCP command (request/response)
        case sendRawFCPCommand = 38
        case getRawFCPCommandResult = 39
        /// The one AV/C exchange that is safe on firmware which hangs on
        /// ordinary discovery. The driver builds the whole frame; this side
        /// supplies only a plug direction and a plug id. Result is polled with
        /// `getRawFCPCommandResult`.
        case submitSignalFormatProbe = 64
        case setIsochVerbosity = 40
        case asyncBlockRead = 44
        case asyncBlockWrite = 45
        // DV capture (raw DIF stream via shared ring, memory type 1)
        case startDVCapture = 50
        case stopDVCapture = 51
        // Developer-only local software bus reset, generation-pinned.
        case requestUserBusReset = 61
        case startAudioStreaming = 62
        case stopAudioStreaming = 63
        // Read-only audio telemetry diagnostics.
        case getAudioTelemetry = 1013
        // Driver-owned LogRing category names and named filter presets.
        case getLogCatalog = 1014
        case getAudioConfiguration = 1015
        case requestAudioConfiguration = 1016
        case getAudioConfigurationEndpoints = 1017
        case getAudioControlSurface = 1018
        case requestAudioControlValue = 1019
        case getAudioMeterSnapshot = 1020
        case setAudioMeteringEnabled = 1021
        case submitAudioControlValue = 1022
        case getAudioConfigurationAsync = 1023
        case getAudioControlSurfaceAsync = 1024
        case getAudioMeterSnapshotAsync = 1025
        case setAudioMeteringEnabledAsync = 1026
        case requestAudioConfigurationAsync = 1027
        case getAudioSemanticTopology = 1028
        case getAudioSemanticTopologyEndpoints = 1029
        case getAudioSemanticConsoleLayout = 1030
        case getAudioSemanticMatrix = 1031
        case getAudioSemanticMatrixEndpoints = 1032
        case getAudioRuntimeTuning = 1033
        case requestAudioRuntimeTuning = 1034
        case getAudioRuntimeTuningEndpoints = 1035
        case startTxLatencySession = 1036
        case getTxLatencyResults = 1037
    }

    // MARK: - Re-exported Models

    typealias SharedStatusReason = DriverConnectorSharedStatusReason
    typealias SharedStatusFlags = DriverConnectorSharedStatusFlags
    typealias DriverStatus = DriverConnectorStatus
    typealias DriverVersionInfo = DriverConnectorVersionInfo
    typealias AVCSubunitInfo = DriverConnectorAVCSubunitInfo
    typealias AVCUnitInfo = DriverConnectorAVCUnitInfo
    typealias AVCMusicCapabilities = DriverConnectorAVCMusicCapabilities
    typealias FWDeviceState = DriverConnectorFWDeviceState
    typealias FWUnitState = DriverConnectorFWUnitState
    typealias FWDeviceInfo = DriverConnectorFWDeviceInfo
    typealias FWUnitInfo = DriverConnectorFWUnitInfo
    typealias LogMessage = DriverConnectorLogMessage

    // MARK: - Published Properties

    @Published var isConnected: Bool = false
    @Published var lastError: String?
    @Published var logMessages: [LogMessage] = []
    @Published var latestStatus: DriverStatus?

    // MARK: - Public Publishers

    let statusSubject = PassthroughSubject<DriverStatus, Never>()
    var statusPublisher: AnyPublisher<DriverStatus, Never> {
        statusSubject.eraseToAnyPublisher()
    }

    // MARK: - Connection State

    var connection: io_connect_t = 0
    let connectionQueue = DispatchQueue(label: "net.mrmidi.ASFWDriverConnector.connection")

    /// Marks `connectionQueue` so `deinit` can tell whether it is already running on
    /// it. The async-notification event handler takes a temporary strong `self` for the
    /// duration of the callback (`[weak self]` cannot prevent that), so when the last
    /// other reference is dropped while a notification is in flight, `deinit` runs on
    /// `connectionQueue` itself. A `sync` there is a self-deadlock and traps with
    /// "dispatch_sync called on queue already owned by current thread".
    private static let connectionQueueKey = DispatchSpecificKey<UInt8>()
    let serviceName = "ASFWDriver"

    var notificationPort: IONotificationPortRef?
    var matchedIterator: io_iterator_t = 0
    var terminatedIterator: io_iterator_t = 0
    var currentService: io_object_t = 0
    var monitoringActive = false

    var asyncPort: mach_port_t = mach_port_t(MACH_PORT_NULL)
    var asyncSource: DispatchSourceMachReceive?
    static let statusAsyncReference: UInt64 = 0x4153_4657_5354_4154 // "ASFWSTAT"
    static let audioControlAsyncReference: UInt64 = 0x4153_4657_4354_524c // "ASFWCTRL"
    static let audioConfigurationSnapshotAsyncReference: UInt64 = 0x4153_4657_4346_4753 // "ASFWCFGS"
    static let audioControlSnapshotAsyncReference: UInt64 = 0x4153_4657_4353_4e50 // "ASFWCSNP"
    static let audioMeterSnapshotAsyncReference: UInt64 = 0x4153_4657_4d53_4e50 // "ASFWMSNP"
    static let audioMeteringAsyncReference: UInt64 = 0x4153_4657_4d45_5452 // "ASFW M ETR"
    static let audioConfigurationAsyncReference: UInt64 = 0x4153_4657_4346_4752 // "ASFWCFGR"
    var nextAudioControlRequestID: UInt64 = 1
    var audioControlCompletions: [UInt64: (kern_return_t) -> Void] = [:]
    var audioConfigurationSnapshotCompletions: [UInt64: (kern_return_t, [UInt64]) -> Void] = [:]
    var audioControlSnapshotCompletions: [UInt64: (kern_return_t, [UInt64]) -> Void] = [:]
    var audioMeterSnapshotCompletions: [UInt64: (kern_return_t, [UInt64]) -> Void] = [:]
    var audioMeteringCompletions: [UInt64: (kern_return_t) -> Void] = [:]
    var audioConfigurationCompletions: [UInt64: (kern_return_t) -> Void] = [:]

    var sharedMemoryAddress: mach_vm_address_t = 0
    var sharedMemoryLength: mach_vm_size_t = 0
    var sharedMemoryPointer: UnsafeMutableRawPointer?
    var lastDeliveredSequence: UInt64 = 0
    var pendingStatusDelivery: DriverStatus?
    var statusDeliveryScheduled = false
    var statusDeliveryGeneration: UInt64 = 0
    private let logStore = DriverConnectorLogStore(maxEntries: 100)
    let duetStateCacheStore = DriverConnectorDuetStateCacheStore()

    lazy var transport = DriverConnectorTransport(
        connectionProvider: { [weak self] in self?.connection ?? 0 },
        interpretIOReturn: { [weak self] kr in
            self?.interpretIOReturn(kr) ?? String(format: "Unknown error 0x%x (%d)", UInt32(bitPattern: kr), kr)
        },
        errorHandler: { [weak self] message in
            DispatchQueue.main.async { [weak self] in
                self?.lastError = message
            }
        }
    )

    // MARK: - Initialisation

    init() {
        connectionQueue.setSpecific(key: Self.connectionQueueKey, value: 1)
        startMonitoring()
    }

    deinit {
        // By deinit no other reference exists, so running the teardown inline when we
        // are already on `connectionQueue` cannot race with anything — and it is the
        // only safe option, since `sync` onto the current queue traps.
        if DispatchQueue.getSpecific(key: Self.connectionQueueKey) != nil {
            closeConnectionLocked(reason: "Connector deinit (on connection queue)")
            stopMonitoringLocked()
        } else {
            connectionQueue.sync {
                self.closeConnectionLocked(reason: "Connector deinit")
                self.stopMonitoringLocked()
            }
        }
    }

    // MARK: - Public API

    func connect(forceAttempt: Bool = false) -> Bool {
        log("Manual connect requested", level: .info)
        connectionQueue.async {
            self.manualConnectLocked(forceAttempt: forceAttempt)
        }
        return true
    }

    func disconnect() {
        connectionQueue.async {
            self.closeConnectionLocked(reason: "Manual disconnect")
        }
    }

    // MARK: - Struct Method Helper (handles variable-size returns and kIOReturnNoSpace retry)

    func callStruct(_ selector: Method, input: Data? = nil,
                    initialCap: Int = DriverConnectorTransport.maxInlineStructOutputBytes) -> Data? {
        transport.callStruct(selector: selector.rawValue, input: input, initialCap: initialCap)
    }

    func callStructWithScalar(_ selector: Method, input: Data? = nil,
                              initialCap: Int = DriverConnectorTransport.maxInlineStructOutputBytes,
                              scalarOutput: inout UInt64) -> Data? {
        transport.callStructWithScalar(selector: selector.rawValue, input: input, initialCap: initialCap, scalarOutput: &scalarOutput)
    }

    /// Submits a value for a parameter declared in an audio semantic topology.
    /// The driver remains the protocol owner; the app never sends vendor FCP
    /// operands or reconstructs a multi-field device command itself.
    func submitAudioControlValue(
        endpointID: AudioEndpointID,
        controlID: UInt32,
        value: Int32,
        completion: @escaping (kern_return_t) -> Void
    ) {
        connectionQueue.async { [weak self] in
            guard let self, self.connection != 0,
                  self.asyncPort != mach_port_t(MACH_PORT_NULL),
                  endpointID.rawValue != 0 else {
                DispatchQueue.main.async { completion(kIOReturnNotReady) }
                return
            }

            let requestID = self.nextAudioControlRequestID
            self.nextAudioControlRequestID &+= 1
            self.audioControlCompletions[requestID] = completion
            var reference = DriverKitAsyncCompletionDecoder.reference(
                marker: Self.audioControlAsyncReference
            )
            var inputs = [endpointID.rawValue, UInt64(controlID),
                          UInt64(UInt32(bitPattern: value)), requestID]
            let kr = IOConnectCallAsyncScalarMethod(
                self.connection, Method.submitAudioControlValue.rawValue,
                self.asyncPort, &reference, UInt32(reference.count),
                &inputs, UInt32(inputs.count), nil, nil)
            if kr != KERN_SUCCESS {
                let callback = self.audioControlCompletions.removeValue(forKey: requestID)
                DispatchQueue.main.async { callback?(kr) }
            }
        }
    }

    // MARK: - Logging helpers

    func log(_ message: String, level: LogMessage.Level = .info) {
        let logEntry = LogMessage(timestamp: Date(), level: level, message: message)
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.logMessages = self.logStore.append(logEntry)
        }
    }


    func interpretIOReturn(_ kr: kern_return_t) -> String {
        let KERN_SUCCESS: kern_return_t = 0
        let KERN_PROTECTION_FAILURE: kern_return_t = -308
        let kIOReturnNotPrivileged: kern_return_t = -536870207
        let kIOReturnNoDevice: kern_return_t = -536870208
        let kIOReturnBadArgument: kern_return_t = -536870206
        let kIOReturnUnsupported: kern_return_t = -536870201
        let kIOReturnNotOpen: kern_return_t = -536870195
        let kIOReturnBusy: kern_return_t = -536870187
        let kIOReturnTimeout: kern_return_t = -536870186
        let kIOReturnNotFound: kern_return_t = -536870160

        switch kr {
        case KERN_SUCCESS:
            return "Success"
        case KERN_PROTECTION_FAILURE:
            return "KERN_PROTECTION_FAILURE (open denied; check Info.plist UserClient and entitlements)"
        case kIOReturnNotPrivileged:
            return "Not Privileged (app sandbox must be disabled)"
        case kIOReturnNoDevice:
            return "No Device (driver not loaded)"
        case kIOReturnBadArgument:
            return "Bad Argument"
        case kIOReturnUnsupported:
            return "Unsupported Operation"
        case kIOReturnNotOpen:
            return "Not Open"
        case kIOReturnBusy:
            return "Device Busy"
        case kIOReturnTimeout:
            return "Timeout"
        case kIOReturnNotFound:
            return "Not Found"
        default:
            return String(format: "Unknown error 0x%x (%d)", UInt32(bitPattern: kr), kr)
        }
    }

    func kernResultString(_ kr: kern_return_t) -> String {
        if let cString = mach_error_string(kr) {
            return String(cString: cString)
        }
        return "kern_result = \(kr)"
    }

}
