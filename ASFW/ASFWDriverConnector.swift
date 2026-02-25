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
        case setIsochVerbosity = 40
        case setIsochTxVerifier = 41
        case asyncBlockRead = 44
        case asyncBlockWrite = 45
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
    let serviceName = "ASFWDriver"

    var notificationPort: IONotificationPortRef?
    var matchedIterator: io_iterator_t = 0
    var terminatedIterator: io_iterator_t = 0
    var currentService: io_object_t = 0
    var monitoringActive = false

    var asyncPort: mach_port_t = mach_port_t(MACH_PORT_NULL)
    var asyncSource: DispatchSourceMachReceive?

    var sharedMemoryAddress: mach_vm_address_t = 0
    var sharedMemoryLength: mach_vm_size_t = 0
    var sharedMemoryPointer: UnsafeMutableRawPointer?
    var lastDeliveredSequence: UInt64 = 0
    private let logStore = DriverConnectorLogStore(maxEntries: 100)

    lazy var transport = DriverConnectorTransport(
        connectionProvider: { [weak self] in self?.connection ?? 0 },
        interpretIOReturn: { [weak self] kr in
            self?.interpretIOReturn(kr) ?? String(format: "Unknown error 0x%x (%d)", UInt32(bitPattern: kr), kr)
        },
        errorHandler: { [weak self] message in self?.lastError = message }
    )

    // MARK: - Initialisation

    init() {
        startMonitoring()
    }

    deinit {
        connectionQueue.sync {
            self.closeConnectionLocked(reason: "Connector deinit")
            self.stopMonitoringLocked()
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

    func callStruct(_ selector: Method, input: Data? = nil, initialCap: Int = 64 * 1024) -> Data? {
        transport.callStruct(selector: selector.rawValue, input: input, initialCap: initialCap)
    }

    func callStructWithScalar(_ selector: Method, input: Data? = nil, initialCap: Int = 64 * 1024, scalarOutput: inout UInt64) -> Data? {
        transport.callStructWithScalar(selector: selector.rawValue, input: input, initialCap: initialCap, scalarOutput: &scalarOutput)
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
    