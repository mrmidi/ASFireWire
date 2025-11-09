import Foundation
import Combine
import IOKit
import SystemExtensions
import Darwin.Mach

enum SharedStatusReason: UInt32 {
    case boot = 1
    case interrupt = 2
    case busReset = 3
    case asyncActivity = 4
    case watchdog = 5
    case manual = 6
    case disconnect = 7
    case unknown = 0
}

struct SharedStatusFlags {
    static let isIRM: UInt32 = 1 << 0
    static let isCycleMaster: UInt32 = 1 << 1
    static let linkActive: UInt32 = 1 << 2
}

extension SharedStatusReason {
    var displayName: String {
        switch self {
        case .boot: return "Boot"
        case .interrupt: return "Interrupt"
        case .busReset: return "Bus Reset"
        case .asyncActivity: return "Async Activity"
        case .watchdog: return "Watchdog"
        case .manual: return "Manual"
        case .disconnect: return "Disconnect"
        case .unknown: return "Unknown"
        }
    }
}

struct DriverStatus {
    let sequence: UInt64
    let timestampMach: UInt64
    let reason: SharedStatusReason
    let detailMask: UInt32
    let controllerState: UInt32
    let controllerStateName: String
    let flags: UInt32

    let busGeneration: UInt32
    let nodeCount: UInt32
    let localNodeID: UInt32?
    let rootNodeID: UInt32?
    let irmNodeID: UInt32?

    let busResetCount: UInt64
    let lastBusResetStart: UInt64
    let lastBusResetCompletion: UInt64

    let asyncLastCompletion: UInt64
    let asyncTimeouts: UInt32
    let watchdogTickCount: UInt64
    let watchdogLastTickUsec: UInt64

    init?(rawPointer: UnsafeRawPointer, length: Int) {
        guard length >= 256 else { return nil }

        func loadUInt32(_ offset: Int) -> UInt32 {
            return rawPointer.load(fromByteOffset: offset, as: UInt32.self).littleEndian
        }

        func loadUInt64(_ offset: Int) -> UInt64 {
            return rawPointer.load(fromByteOffset: offset, as: UInt64.self).littleEndian
        }

        let version = loadUInt32(0)
        guard version == 1 else { return nil }

        let payloadLength = loadUInt32(4)
        guard payloadLength <= length else { return nil }

        self.sequence = loadUInt64(8)
        self.timestampMach = loadUInt64(16)
        self.reason = SharedStatusReason(rawValue: loadUInt32(24)) ?? .unknown
        self.detailMask = loadUInt32(28)

        var nameBuffer = [CChar](repeating: 0, count: 32)
        nameBuffer.withUnsafeMutableBytes { dest in
            dest.copyBytes(from: UnsafeRawBufferPointer(start: rawPointer.advanced(by: 32), count: 32))
        }
        self.controllerStateName = String(cString: nameBuffer)

        self.controllerState = loadUInt32(64)
        self.flags = loadUInt32(68)

        self.busGeneration = loadUInt32(72)
        self.nodeCount = loadUInt32(76)

        func decodeNodeID(_ raw: UInt32) -> UInt32? {
            return raw == 0xFFFF_FFFF ? nil : raw
        }

        self.localNodeID = decodeNodeID(loadUInt32(80))
        self.rootNodeID = decodeNodeID(loadUInt32(84))
        self.irmNodeID = decodeNodeID(loadUInt32(88))

        self.busResetCount = loadUInt64(96)
        self.lastBusResetStart = loadUInt64(104)
        self.lastBusResetCompletion = loadUInt64(112)
        self.asyncLastCompletion = loadUInt64(120)
        _ = loadUInt32(128) // asyncPending (reserved)
        self.asyncTimeouts = loadUInt32(132)
        self.watchdogTickCount = loadUInt64(136)
        self.watchdogLastTickUsec = loadUInt64(144)
    }

    var isIRM: Bool { (flags & SharedStatusFlags.isIRM) != 0 }
    var isCycleMaster: Bool { (flags & SharedStatusFlags.isCycleMaster) != 0 }
    var linkActive: Bool { (flags & SharedStatusFlags.linkActive) != 0 }
}

final class ASFWDriverConnector: ObservableObject {
    // MARK: - Types

    struct LogMessage: Identifiable, Equatable {
        let id = UUID()
        let timestamp: Date
        let level: Level
        let message: String

        enum Level {
            case info, warning, error, success

            var emoji: String {
                switch self {
                case .info: return "ℹ️"
                case .warning: return "⚠️"
                case .error: return "❌"
                case .success: return "✅"
                }
            }

            var color: String {
                switch self {
                case .info: return "blue"
                case .warning: return "orange"
                case .error: return "red"
                case .success: return "green"
                }
            }
        }
    }

    private enum Method: UInt32 {
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
        case exportConfigROM = 14
        case triggerROMRead = 15
    }

    // MARK: - Published Properties

    @Published var isConnected: Bool = false
    @Published var lastError: String?
    @Published var logMessages: [LogMessage] = []
    @Published var latestStatus: DriverStatus?

    // MARK: - Public Publishers

    private let statusSubject = PassthroughSubject<DriverStatus, Never>()
    var statusPublisher: AnyPublisher<DriverStatus, Never> {
        statusSubject.eraseToAnyPublisher()
    }

    // MARK: - Connection State

    private var connection: io_connect_t = 0
    private let connectionQueue = DispatchQueue(label: "net.mrmidi.ASFWDriverConnector.connection")
    private let serviceName = "ASFWDriver"

    private var notificationPort: IONotificationPortRef?
    private var matchedIterator: io_iterator_t = 0
    private var terminatedIterator: io_iterator_t = 0
    private var currentService: io_object_t = 0
    private var monitoringActive = false

    private var asyncPort: mach_port_t = mach_port_t(MACH_PORT_NULL)
    private var asyncSource: DispatchSourceMachReceive?

    private var sharedMemoryAddress: mach_vm_address_t = 0
    private var sharedMemoryLength: mach_vm_size_t = 0
    private var sharedMemoryPointer: UnsafeMutableRawPointer?
    private var lastDeliveredSequence: UInt64 = 0

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

    // Legacy blocking calls retained for compatibility ---------------------------------

    func getBusResetCount() -> (count: UInt64, generation: UInt8, timestamp: UInt64)? {
        guard isConnected else {
            log("getBusResetCount: Not connected", level: .warning)
            return nil
        }

        var output = [UInt64](repeating: 0, count: 3)
        var outputCount: UInt32 = 3

        let kr = IOConnectCallScalarMethod(
            connection,
            Method.getBusResetCount.rawValue,
            nil,
            0,
            &output,
            &outputCount
        )

        guard kr == KERN_SUCCESS else {
            let errorMsg = "getBusResetCount failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        return (output[0], UInt8(output[1]), output[2])
    }

    func getControllerStatus() -> ControllerStatus? {
        guard isConnected else { return nil }

        var outputStruct = Data(count: MemoryLayout<ControllerStatusWire>.size)
        var outputSize = outputStruct.count

        let kr = outputStruct.withUnsafeMutableBytes { bufferPtr in
            IOConnectCallStructMethod(
                connection,
                Method.getControllerStatus.rawValue,
                nil,
                0,
                bufferPtr.baseAddress,
                &outputSize
            )
        }

        guard kr == KERN_SUCCESS else {
            lastError = "getControllerStatus failed: \(interpretIOReturn(kr))"
            return nil
        }

        return outputStruct.withUnsafeBytes { ptr in
            let wire = ptr.load(as: ControllerStatusWire.self)
            return ControllerStatus(wire: wire)
        }
    }

    // MARK: - Struct Method Helper (handles variable-size returns and kIOReturnNoSpace retry)
    
    private func callStruct(_ selector: Method, input: Data? = nil, initialCap: Int = 64 * 1024) -> Data? {
        guard connection != 0 else {
            print("[Connector] ❌ callStruct: connection=0 (not connected)")
            lastError = "Not connected to driver"
            return nil
        }
        
        print("[Connector] 📞 callStruct: selector=\(selector) connection=0x\(String(connection, radix: 16)) initialCap=\(initialCap)")
        
        var outSize = initialCap
        var out = Data(count: outSize)

        func doCall() -> kern_return_t {
            out.withUnsafeMutableBytes { outPtr in
                if let input = input {
                    return input.withUnsafeBytes { inPtr in
                        IOConnectCallStructMethod(connection, selector.rawValue,
                                                  inPtr.baseAddress, input.count,
                                                  outPtr.baseAddress, &outSize)
                    }
                } else {
                    return IOConnectCallStructMethod(connection, selector.rawValue,
                                                     nil, 0,
                                                     outPtr.baseAddress, &outSize)
                }
            }
        }

        var kr = doCall()
        if kr == kIOReturnNoSpace {
            print("[Connector] callStruct: got kIOReturnNoSpace, retrying with size=\(outSize)")
            out = Data(count: outSize)
            kr = doCall()
        }
        
        guard kr == KERN_SUCCESS else {
            let errMsg = "\(selector) failed: \(interpretIOReturn(kr))"
            print("[Connector] ❌ callStruct error: \(errMsg)")
            lastError = errMsg
            return nil
        }
        
        print("[Connector] ✅ callStruct success: selector=\(selector) outSize=\(outSize)")
        out.count = outSize
        return out
    }

    func getBusResetHistory(startIndex: UInt64 = 0, count: UInt64 = 10) -> [BusResetPacketSnapshot]? {
        guard connection != 0 else { return nil }
        
        var input = Data()
        withUnsafeBytes(of: startIndex.littleEndian) { input.append(contentsOf: $0) }
        withUnsafeBytes(of: count.littleEndian) { input.append(contentsOf: $0) }
        
        guard let bytes = callStruct(.getBusResetHistory, input: input, initialCap: 4096) else { return nil }
        
        let packetSize = MemoryLayout<BusResetPacketWire>.size
        guard !bytes.isEmpty, bytes.count % packetSize == 0 else { return [] }
        
        return bytes.withUnsafeBytes { ptr in
            let n = bytes.count / packetSize
            return (0..<n).map { i in
                BusResetPacketSnapshot(wire: ptr.load(fromByteOffset: i * packetSize, as: BusResetPacketWire.self))
            }
        }
    }

    func clearHistory() -> Bool {
        guard connection != 0 else { return false }

        let kr = IOConnectCallScalarMethod(
            connection,
            Method.clearHistory.rawValue,
            nil,
            0,
            nil,
            nil
        )

        guard kr == KERN_SUCCESS else {
            lastError = "clearHistory failed: \(interpretIOReturn(kr))"
            return false
        }

        return true
    }

    func getSelfIDCapture(generation: UInt32? = nil) -> SelfIDCapture? {
        guard connection != 0 else { 
            print("[Connector] ❌ getSelfIDCapture: not connected")
            return nil 
        }

        print("[Connector] 📞 Calling getSelfIDCapture via IOConnectCallStructMethod")
        guard let bytes = callStruct(.getSelfIDCapture, initialCap: 1024) else {
            print("[Connector] ❌ getSelfIDCapture: callStruct failed")
            return nil
        }

        print("[Connector] 📦 getSelfIDCapture: received \(bytes.count) bytes")
        guard !bytes.isEmpty else {
            print("[Connector] ⚠️  getSelfIDCapture: 0 bytes returned (no data from driver)")
            return nil
        }

        print("[Connector] 🔍 Decoding \(bytes.count) bytes...")
        let result = SelfIDCapture.decode(from: bytes)
        print("[Connector] Decode result: \(result != nil ? "✅ SUCCESS" : "❌ FAILED")")
        return result
    }

    func getTopologySnapshot(generation: UInt32? = nil) -> TopologySnapshot? {
        guard connection != 0 else {
            log("❌ getTopologySnapshot: not connected", level: .error)
            return nil
        }

        guard let bytes = callStruct(.getTopologySnapshot, initialCap: 4096) else {
            log("❌ getTopologySnapshot: callStruct failed", level: .error)
            return nil
        }

        guard !bytes.isEmpty else {
            log("⚠️  getTopologySnapshot: driver returned 0 bytes (no topology)", level: .warning)
            return nil
        }

        log("✅ getTopologySnapshot: received \(bytes.count) bytes from driver", level: .success)
        let result = TopologySnapshot.decode(from: bytes)
        if result == nil {
            log("❌ getTopologySnapshot: decode returned nil!", level: .error)
        }
        return result
    }

    func ping() -> String? {
        guard isConnected else {
            log("ping: Not connected", level: .warning)
            return nil
        }

        var output = Data(count: 128)
        var outputSize = output.count

        let kr = output.withUnsafeMutableBytes { bufferPtr in
            IOConnectCallStructMethod(
                connection,
                Method.ping.rawValue,
                nil,
                0,
                bufferPtr.baseAddress,
                &outputSize
            )
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "ping failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        guard outputSize > 0 else {
            log("ping: driver returned empty payload", level: .warning)
            return nil
        }

        let messageData = output.prefix(outputSize)
        let message: String? = messageData.withUnsafeBytes { buffer in
            let ptr = buffer.bindMemory(to: CChar.self).baseAddress
            if let cString = ptr {
                return String(cString: cString)
            }
            return nil
        }

        if let message = message {
            log("ping response: \(message)", level: .success)
        }
        return message
    }

    // MARK: - Monitoring & Notifications

    private func startMonitoring() {
        connectionQueue.sync {
            if monitoringActive { return }
            monitoringActive = true
            startMonitoringLocked()
        }
    }

    private func startMonitoringLocked() {
        guard notificationPort == nil else { return }

        guard let port = IONotificationPortCreate(kIOMainPortDefault) else {
            log("Failed to create IONotificationPort", level: .error)
            return
        }
        notificationPort = port
        IONotificationPortSetDispatchQueue(port, connectionQueue)

        var matched: io_iterator_t = 0
        let matchDict = IOServiceNameMatching(serviceName)
        let matchResult = IOServiceAddMatchingNotification(
            port,
            kIOFirstMatchNotification,
            matchDict,
            { refCon, iterator in
                guard let refCon = refCon else { return }
                let connector = Unmanaged<ASFWDriverConnector>.fromOpaque(refCon).takeUnretainedValue()
                connector.handleMatched(iterator: iterator)
            },
            UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque()),
            &matched
        )

        if matchResult == KERN_SUCCESS {
            matchedIterator = matched
            handleMatched(iterator: matched)
        } else {
            log("IOServiceAddMatchingNotification (first match) failed: \(interpretIOReturn(matchResult))", level: .error)
        }

        var terminated: io_iterator_t = 0
        let termDict = IOServiceNameMatching(serviceName)
        let termResult = IOServiceAddMatchingNotification(
            port,
            kIOTerminatedNotification,
            termDict,
            { refCon, iterator in
                guard let refCon = refCon else { return }
                let connector = Unmanaged<ASFWDriverConnector>.fromOpaque(refCon).takeUnretainedValue()
                connector.handleTerminated(iterator: iterator)
            },
            UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque()),
            &terminated
        )

        if termResult == KERN_SUCCESS {
            terminatedIterator = terminated
            handleTerminated(iterator: terminated)
        } else {
            log("IOServiceAddMatchingNotification (terminated) failed: \(interpretIOReturn(termResult))", level: .error)
        }
    }

    private func stopMonitoringLocked() {
        if matchedIterator != 0 {
            IOObjectRelease(matchedIterator)
            matchedIterator = 0
        }
        if terminatedIterator != 0 {
            IOObjectRelease(terminatedIterator)
            terminatedIterator = 0
        }
        if let port = notificationPort {
            IONotificationPortDestroy(port)
            notificationPort = nil
        }
        monitoringActive = false
    }

    private func handleMatched(iterator: io_iterator_t) {
        while true {
            let service = IOIteratorNext(iterator)
            if service == 0 { break }
            log("Matched service 0x\(String(service, radix: 16))", level: .info)

            IOObjectRetain(service)
            if currentService != 0 {
                IOObjectRelease(currentService)
            }
            currentService = service
            openConnectionLocked(to: service, reason: "match notification")
            IOObjectRelease(service)
        }
    }

    private func handleTerminated(iterator: io_iterator_t) {
        var terminationObserved = false
        while true {
            let service = IOIteratorNext(iterator)
            if service == 0 { break }
            terminationObserved = true
            log("Service terminated 0x\(String(service, radix: 16))", level: .warning)
            IOObjectRelease(service)
        }

        if terminationObserved {
            closeConnectionLocked(reason: "Service terminated")
        }
    }

    // MARK: - Connection management

    private func manualConnectLocked(forceAttempt: Bool) {
        if connection != 0 && !forceAttempt {
            return
        }

        let matchingDict = IOServiceNameMatching(serviceName)
        let service = IOServiceGetMatchingService(kIOMainPortDefault, matchingDict)
        guard service != 0 else {
            log("ASFWDriver service not found in IORegistry", level: .error)
            lastError = "ASFWDriver service not found"
            return
        }

        openConnectionLocked(to: service, reason: "manual connect")
        IOObjectRelease(service)
    }

    private func openConnectionLocked(to service: io_service_t, reason: String) {
        if connection != 0 {
            return
        }

        log("Opening connection (\(reason))...", level: .info)
        var newConnection: io_connect_t = 0
        let kr = IOServiceOpen(service, mach_task_self_, 0, &newConnection)
        guard kr == KERN_SUCCESS else {
            let errorMsg = "Failed to open service: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return
        }

        connection = newConnection
        lastError = nil

        if !mapSharedStatusMemoryLocked() {
            closeConnectionLocked(reason: "Failed to map shared status memory")
            return
        }

        if !registerStatusNotificationsLocked() {
            closeConnectionLocked(reason: "Failed to register status notifications")
            return
        }

        DispatchQueue.main.async {
            self.isConnected = true
        }
        log("Connection established", level: .success)
    }

    private func closeConnectionLocked(reason: String) {
        if asyncSource != nil {
            asyncSource?.cancel()
            asyncSource = nil
        }

        if asyncPort != mach_port_t(MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self_, asyncPort)
            asyncPort = mach_port_t(MACH_PORT_NULL)
        }

        if sharedMemoryPointer != nil {
            IOConnectUnmapMemory64(connection,
                                   0,
                                   mach_task_self_,
                                   sharedMemoryAddress)
            sharedMemoryPointer = nil
            sharedMemoryAddress = 0
            sharedMemoryLength = 0
        }

        if connection != 0 {
            IOServiceClose(connection)
            connection = 0
        }

        if currentService != 0 {
            IOObjectRelease(currentService)
            currentService = 0
        }

        lastDeliveredSequence = 0
        DispatchQueue.main.async {
            self.isConnected = false
            self.latestStatus = nil
        }
        log("Connection closed: \(reason)", level: .warning)
    }

    private func mapSharedStatusMemoryLocked() -> Bool {
        var address: mach_vm_address_t = 0
        var length: mach_vm_size_t = 0
        let options: UInt32 = UInt32(kIOMapAnywhere | kIOMapDefaultCache)
        let kr = IOConnectMapMemory64(connection,
                                      0,
                                      mach_task_self_,
                                      &address,
                                      &length,
                                      options)
        guard kr == KERN_SUCCESS, let pointer = UnsafeMutableRawPointer(bitPattern: UInt(address)) else {
            log("IOConnectMapMemory64 failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }

        sharedMemoryAddress = address
        sharedMemoryLength = length
        sharedMemoryPointer = pointer
        emitCurrentStatus()
        return true
    }

    private func registerStatusNotificationsLocked() -> Bool {
        guard asyncPort == mach_port_t(MACH_PORT_NULL) else { return true }

        var port: mach_port_t = mach_port_t(MACH_PORT_NULL)
        var kr = mach_port_allocate(mach_task_self_, MACH_PORT_RIGHT_RECEIVE, &port)
        guard kr == KERN_SUCCESS else {
            log("mach_port_allocate failed: \(kernResultString(kr))", level: .error)
            return false
        }

        kr = mach_port_insert_right(mach_task_self_, port, port, mach_msg_type_name_t(MACH_MSG_TYPE_MAKE_SEND))
        guard kr == KERN_SUCCESS else {
            mach_port_deallocate(mach_task_self_, port)
            log("mach_port_insert_right failed: \(kernResultString(kr))", level: .error)
            return false
        }

        let token = UInt64(UInt(bitPattern: Unmanaged.passUnretained(self).toOpaque()))
        var asyncRef: [UInt64] = [token]
        kr = IOConnectCallAsyncScalarMethod(connection,
                                            Method.registerStatusListener.rawValue,
                                            port,
                                            &asyncRef,
                                            UInt32(asyncRef.count),
                                            nil,
                                            0,
                                            nil,
                                            nil)
        guard kr == KERN_SUCCESS else {
            mach_port_deallocate(mach_task_self_, port)
            log("IOConnectCallAsyncScalarMethod failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }

        asyncPort = port
        let source = DispatchSource.makeMachReceiveSource(port: port, queue: connectionQueue)
        source.setEventHandler { [weak self] in
            self?.handleAsyncMessages()
        }
        source.setCancelHandler { [port] in
            mach_port_deallocate(mach_task_self_, port)
        }
        asyncSource = source
        source.resume()
        emitCurrentStatus()
        return true
    }

    private func handleAsyncMessages() {
        guard asyncPort != mach_port_t(MACH_PORT_NULL) else { return }
        var buffer = [UInt8](repeating: 0, count: 512)
        let messageSize = mach_msg_size_t(buffer.count)

        while true {
            let result = buffer.withUnsafeMutableBytes { rawPtr -> kern_return_t in
                let headerPtr = rawPtr.bindMemory(to: mach_msg_header_t.self).baseAddress!
                return mach_msg(headerPtr,
                                mach_msg_option_t(MACH_RCV_MSG | MACH_RCV_TIMEOUT),
                                0,
                                messageSize,
                                asyncPort,
                                0,
                                mach_port_name_t(MACH_PORT_NULL))
            }

            if result == MACH_RCV_TIMED_OUT {
                break
            } else if result != KERN_SUCCESS {
                log("mach_msg receive failed: \(kernResultString(result))", level: .error)
                break
            }

            buffer.withUnsafeBytes { rawPtr in
                let base = rawPtr.baseAddress!
                let scalarCountOffset = MemoryLayout<mach_msg_header_t>.size + MemoryLayout<mach_msg_body_t>.size + MemoryLayout<mach_msg_port_descriptor_t>.size
                let count = base.load(fromByteOffset: scalarCountOffset, as: UInt32.self).littleEndian
                guard count >= 2 else { return }
                let scalarsOffset = scalarCountOffset + MemoryLayout<UInt32>.size
                let scalarsPtr = base.advanced(by: scalarsOffset).assumingMemoryBound(to: UInt64.self)
                let sequence = scalarsPtr.pointee
                let reasonRaw = scalarsPtr.advanced(by: 1).pointee
                handleStatusNotification(sequence: sequence, reason: UInt32(truncatingIfNeeded: reasonRaw))
            }
        }
    }

    private func handleStatusNotification(sequence: UInt64, reason: UInt32) {
        guard let pointer = sharedMemoryPointer else { return }
        guard let status = DriverStatus(rawPointer: UnsafeRawPointer(pointer), length: Int(sharedMemoryLength)) else { return }
        guard status.sequence != 0 else { return }
        guard status.sequence != lastDeliveredSequence else { return }
        lastDeliveredSequence = status.sequence

        DispatchQueue.main.async {
            self.latestStatus = status
        }
        statusSubject.send(status)
    }

    // MARK: - Legacy command helpers

    func asyncRead(destinationID: UInt16,
                   addressHigh: UInt16,
                   addressLow: UInt32,
                   length: UInt32) -> UInt16? {
        guard isConnected else {
            log("asyncRead: Not connected", level: .warning)
            return nil
        }

        var inputs: [UInt64] = [
            UInt64(destinationID),
            UInt64(addressHigh),
            UInt64(addressLow),
            UInt64(length)
        ]

        var output: UInt64 = 0
        var outputCount: UInt32 = 1

        let kr = inputs.withUnsafeMutableBufferPointer { buffer -> kern_return_t in
            IOConnectCallScalarMethod(
                connection,
                Method.asyncRead.rawValue,
                buffer.baseAddress,
                UInt32(buffer.count),
                &output,
                &outputCount)
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "asyncRead failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        let handle = UInt16(truncatingIfNeeded: output)
        log(String(format: "AsyncRead issued (handle=0x%04X)", handle), level: .success)
        return handle
    }

    func asyncWrite(destinationID: UInt16,
                    addressHigh: UInt16,
                    addressLow: UInt32,
                    payload: Data) -> UInt16? {
        guard isConnected else {
            log("asyncWrite: Not connected", level: .warning)
            return nil
        }

        var scalars: [UInt64] = [
            UInt64(destinationID),
            UInt64(addressHigh),
            UInt64(addressLow),
            UInt64(payload.count)
        ]

        var output: UInt64 = 0
        var outputCount: UInt32 = 1

        let kr = payload.withUnsafeBytes { payloadPtr in
            scalars.withUnsafeMutableBufferPointer { scalarPtr -> kern_return_t in
                IOConnectCallMethod(
                    connection,
                    Method.asyncWrite.rawValue,
                    scalarPtr.baseAddress,
                    UInt32(scalarPtr.count),
                    payloadPtr.baseAddress,
                    payload.count,
                    &output,
                    &outputCount,
                    nil,
                    nil)
            }
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "asyncWrite failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        let handle = UInt16(truncatingIfNeeded: output)
        log(String(format: "AsyncWrite issued (handle=0x%04X, bytes=%u)", handle, payload.count), level: .success)
        return handle
    }

    // MARK: - Config ROM Operations

    func getConfigROM(nodeId: UInt8, generation: UInt16) -> Data? {
        guard isConnected else {
            log("getConfigROM: Not connected", level: .warning)
            return nil
        }

        let input: [UInt64] = [UInt64(nodeId), UInt64(generation)]
        let maxSize = 1024 * 4  // Max 1024 quadlets
        var outputStruct = Data(count: maxSize)
        var outputSize = outputStruct.count

        let kr = outputStruct.withUnsafeMutableBytes { bufferPtr in
            IOConnectCallMethod(
                connection,
                Method.exportConfigROM.rawValue,
                input,
                UInt32(input.count),
                nil,
                0,
                nil,
                nil,
                bufferPtr.baseAddress,
                &outputSize
            )
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "getConfigROM failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        guard outputSize > 0 else {
            log("getConfigROM: ROM not cached for node=\(nodeId) gen=\(generation)", level: .info)
            return nil  // ROM not cached
        }

        let romData = outputStruct.prefix(outputSize)
        log("getConfigROM: received \(outputSize) bytes for node=\(nodeId) gen=\(generation)", level: .success)
        return romData
    }

    enum ROMReadStatus: UInt32 {
        case initiated = 0
        case alreadyInProgress = 1
        case failed = 2
    }

    func triggerROMRead(nodeId: UInt8) -> ROMReadStatus {
        guard isConnected else {
            log("triggerROMRead: Not connected", level: .warning)
            print("[Connector] ❌ triggerROMRead: Not connected")
            return .failed
        }

        var input: [UInt64] = [UInt64(nodeId)]
        var output = [UInt64](repeating: 0, count: 1)  // Use array like getBusResetCount
        var outputCount: UInt32 = 1

        print("[Connector] 📞 triggerROMRead: nodeId=\(nodeId) (0x\(String(nodeId, radix: 16))) connection=0x\(String(connection, radix: 16))")
        print("[Connector]    input=\(input) inputCount=\(input.count) selector=\(Method.triggerROMRead.rawValue)")

        let kr = input.withUnsafeMutableBufferPointer { buffer -> kern_return_t in
            IOConnectCallScalarMethod(
                connection,
                Method.triggerROMRead.rawValue,
                buffer.baseAddress,
                UInt32(buffer.count),
                &output,
                &outputCount
            )
        }

        print("[Connector]    IOKit result: kr=\(kr) (0x\(String(UInt32(bitPattern: kr), radix: 16))) output=\(output[0]) outputCount=\(outputCount)")

        guard kr == KERN_SUCCESS else {
            let errorMsg = "triggerROMRead failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            print("[Connector] ❌ triggerROMRead failed: \(errorMsg)")
            return .failed
        }

        let status = ROMReadStatus(rawValue: UInt32(output[0])) ?? .failed
        let statusText = status == .initiated ? "initiated" :
                        status == .alreadyInProgress ? "already in progress" : "failed"
        log("triggerROMRead: node=\(nodeId) \(statusText)", level: status == .failed ? .error : .success)
        print("[Connector] ✅ triggerROMRead: node=\(nodeId) status=\(statusText) (rawStatus=\(output[0]))")
        return status
    }

    private func emitCurrentStatus() {
        guard let pointer = sharedMemoryPointer else { return }
        guard let status = DriverStatus(rawPointer: UnsafeRawPointer(pointer), length: Int(sharedMemoryLength)) else { return }
        guard status.sequence != 0 else { return }
        lastDeliveredSequence = status.sequence
        DispatchQueue.main.async {
            self.latestStatus = status
        }
        statusSubject.send(status)
    }

    // MARK: - Logging helpers

    private func log(_ message: String, level: LogMessage.Level = .info) {
        let logEntry = LogMessage(timestamp: Date(), level: level, message: message)
        DispatchQueue.main.async {
            self.logMessages.append(logEntry)
            if self.logMessages.count > 100 {
                self.logMessages.removeFirst(self.logMessages.count - 100)
            }
        }
    }

    private func interpretIOReturn(_ kr: kern_return_t) -> String {
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

    private func kernResultString(_ kr: kern_return_t) -> String {
        if let cString = mach_error_string(kr) {
            return String(cString: cString)
        }
        return "kern_result = \(kr)"
    }
}
