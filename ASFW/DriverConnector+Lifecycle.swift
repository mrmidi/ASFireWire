import Foundation
import Combine
import IOKit
import Darwin.Mach

extension ASFWDriverConnector {
    // MARK: - Monitoring & Notifications

    func startMonitoring() {
        connectionQueue.sync {
            if monitoringActive { return }
            monitoringActive = true
            startMonitoringLocked()
        }
    }

    func stopMonitoringLocked() {
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

    func handleMatched(iterator: io_iterator_t) {
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

    func handleTerminated(iterator: io_iterator_t) {
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

    func manualConnectLocked(forceAttempt: Bool) {
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

    func openConnectionLocked(to service: io_service_t, reason: String) {
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

        DispatchQueue.main.async { [weak self] in
            self?.isConnected = true
        }
        log("Connection established", level: .success)
    }

    func closeConnectionLocked(reason: String) {
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
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.isConnected = false
            self.latestStatus = nil
        }
        log("Connection closed: \(reason)", level: .warning)
    }

    func mapSharedStatusMemoryLocked() -> Bool {
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

    func registerStatusNotificationsLocked() -> Bool {
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

    func handleAsyncMessages() {
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

    func handleStatusNotification(sequence: UInt64, reason: UInt32) {
        guard let pointer = sharedMemoryPointer else { return }
        guard let status = DriverStatus(rawPointer: UnsafeRawPointer(pointer), length: Int(sharedMemoryLength)) else { return }
        guard status.sequence != 0 else { return }
        guard status.sequence != lastDeliveredSequence else { return }
        lastDeliveredSequence = status.sequence

        DispatchQueue.main.async { [weak self] in
            self?.latestStatus = status
        }
        statusSubject.send(status)
    }

    func emitCurrentStatus() {
        guard let pointer = sharedMemoryPointer else { return }
        guard let status = DriverStatus(rawPointer: UnsafeRawPointer(pointer), length: Int(sharedMemoryLength)) else { return }
        guard status.sequence != 0 else { return }
        lastDeliveredSequence = status.sequence
        DispatchQueue.main.async { [weak self] in
            self?.latestStatus = status
        }
        statusSubject.send(status)
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
}
