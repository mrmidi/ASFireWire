import Foundation
import IOKit

extension ASFWDriverConnector {
    // MARK: - AVC Queries

    func getAVCUnits() -> [AVCUnitInfo]? {
        guard isConnected else {
            log("getAVCUnits: Not connected", level: .warning)
            return nil
        }

        // Initial capacity 4KB
        guard let data = callStruct(.getAVCUnits, initialCap: 4096) else {
            log("getAVCUnits: callStruct failed", level: .error)
            return nil
        }

        guard data.count >= 4 else {
            log("getAVCUnits: data too short", level: .error)
            return nil
        }

        return Self.parseAVCUnitsWire(data)
    }

    static func parseAVCUnitsWire(_ data: Data) -> [AVCUnitInfo] {
        guard data.count >= 4 else { return [] }

        // Helper to read UInt64 from unaligned offset
        func readUInt64(_ data: Data, at offset: Int) -> UInt64 {
            var value: UInt64 = 0
            for i in 0..<8 {
                value |= UInt64(data[offset + i]) << (i * 8)
            }
            return value
        }

        // Helper to read UInt32 from unaligned offset
        func readUInt32(_ data: Data, at offset: Int) -> UInt32 {
            var value: UInt32 = 0
            for i in 0..<4 {
                value |= UInt32(data[offset + i]) << (i * 8)
            }
            return value
        }

        // Helper to read UInt16 from unaligned offset
        func readUInt16(_ data: Data, at offset: Int) -> UInt16 {
            return UInt16(data[offset]) | (UInt16(data[offset + 1]) << 8)
        }

        let unitCount = readUInt32(data, at: 0)
        var offset = 4
        var units: [AVCUnitInfo] = []

        for _ in 0..<unitCount {
            // Check if we have enough data for AVCUnitInfoWire (24 bytes)
            if offset + 24 > data.count { break }

            let guid = readUInt64(data, at: offset)
            let nodeID = readUInt16(data, at: offset + 8)
            let vendorID = readUInt32(data, at: offset + 10)
            let modelID = readUInt32(data, at: offset + 14)
            let subunitCount = data[offset + 18]
            
            // Unit-level plug counts (from AVCUnitPlugInfoCommand)
            let isoInputPlugs = data[offset + 19]
            let isoOutputPlugs = data[offset + 20]
            let extInputPlugs = data[offset + 21]
            let extOutputPlugs = data[offset + 22]
            let diagnosticStatus = data[offset + 23]

            offset += 24

            var subunits: [AVCSubunitInfo] = []
            for _ in 0..<subunitCount {
                if offset + 4 > data.count { break }

                let type = data[offset]
                let subID = data[offset + 1]
                let numSrc = data[offset + 2]
                let numDest = data[offset + 3]

                subunits.append(AVCSubunitInfo(type: type, subunitID: subID, numSrcPlugs: numSrc, numDestPlugs: numDest))
                offset += 4
            }

            units.append(AVCUnitInfo(
                guid: guid, 
                nodeID: nodeID, 
                vendorID: vendorID, 
                modelID: modelID, 
                subunits: subunits,
                isoInputPlugs: isoInputPlugs,
                isoOutputPlugs: isoOutputPlugs,
                extInputPlugs: extInputPlugs,
                extOutputPlugs: extOutputPlugs,
                diagnosticStatus: diagnosticStatus
            ))
        }

        return units
    }

    func getDriverVersion() -> DriverVersionInfo? {
        guard isConnected else {
            log("getDriverVersion: Not connected", level: .warning)
            return nil
        }

        // DriverVersionInfo is 280 bytes
        guard let data = callStruct(.getDriverVersion, initialCap: 280) else {
            log("getDriverVersion: callStruct failed", level: .error)
            return nil
        }

        guard let info = DriverVersionInfo(data: data) else {
            log("getDriverVersion: failed to decode data", level: .error)
            return nil
        }

        return info
    }

    func getSubunitCapabilities(guid: UInt64, type: UInt8, id: UInt8) -> AVCMusicCapabilities? {
        guard let data = getSubunitCapabilitiesData(guid: guid, type: type, id: id) else { return nil }
        return AVCMusicCapabilities(data: data)
    }

    /// Preserve the driver capability wire blob for investigation reports.
    func getSubunitCapabilitiesData(guid: UInt64, type: UInt8, id: UInt8) -> Data? {
        try? subunitCapabilitiesBlob(guid: guid, type: type, id: id).get()
    }

    func getSubunitDescriptor(guid: UInt64, type: UInt8, id: UInt8) -> Data? {
        try? subunitDescriptorBlob(guid: guid, type: type, id: id).get()
    }

    func subunitCapabilitiesBlob(guid: UInt64, type: UInt8, id: UInt8) -> Result<Data, AvcBlobUnavailable> {
        fetchSubunitBlob(.getSubunitCapabilities, guid: guid, type: type, id: id,
                         notFound: "the driver has no such subunit")
    }

    func subunitDescriptorBlob(guid: UInt64, type: UInt8, id: UInt8) -> Result<Data, AvcBlobUnavailable> {
        fetchSubunitBlob(.getSubunitDescriptor, guid: guid, type: type, id: id,
                         notFound: "the driver has not read this descriptor from the device")
    }

    /// One subunit blob, or the driver's reason for not returning it. Both
    /// methods take the GUID halves, type and id as four scalars and answer
    /// with at most 4096 bytes (the driver's structure-output limit).
    private func fetchSubunitBlob(_ method: Method, guid: UInt64, type: UInt8, id: UInt8,
                                  notFound: String) -> Result<Data, AvcBlobUnavailable> {
        guard isConnected, connection != 0 else {
            return .failure(.init(reason: "no driver connection"))
        }
        let scalarInputs: [UInt64] = [guid >> 32, guid & 0xFFFF_FFFF, UInt64(type), UInt64(id)]
        var outSize = 4 * 1024
        var out = Data(count: outSize)
        let kr = out.withUnsafeMutableBytes { outPtr in
            scalarInputs.withUnsafeBufferPointer { scalarPtr in
                IOConnectCallMethod(connection, method.rawValue,
                                    scalarPtr.baseAddress, UInt32(scalarInputs.count),
                                    nil, 0, nil, nil,
                                    outPtr.baseAddress?.assumingMemoryBound(to: UInt8.self), &outSize)
            }
        }
        switch kr {
        case KERN_SUCCESS:
            out.count = outSize
            return .success(out)
        case kIOReturnNotFound:
            return .failure(.init(reason: notFound))
        case kIOReturnUnsupported:
            return .failure(.init(reason: "the driver does not export this for this subunit type"))
        case kIOReturnMessageTooLarge:
            return .failure(.init(reason: "larger than the 4096-byte export limit"))
        default:
            return .failure(.init(reason: "driver call failed: \(interpretIOReturn(kr))"))
        }
    }

    /// Every FCP exchange the driver had with this unit since attach or the
    /// last refresh, read page by page.
    func getFCPExchangeLog(guid: UInt64) -> AvcReportSnapshot.ExchangeLog? {
        guard isConnected, connection != 0 else { return nil }
        var log: AvcReportSnapshot.ExchangeLog?
        var next: UInt32 = 0
        // The log holds at most 1024 records, so this is bounded; the cap also
        // stops a page that keeps arriving empty-but-incomplete.
        for _ in 0..<1100 {
            let scalarInputs: [UInt64] = [guid >> 32, guid & 0xFFFF_FFFF, UInt64(next)]
            var outSize = 4 * 1024
            var out = Data(count: outSize)
            let kr = out.withUnsafeMutableBytes { outPtr in
                scalarInputs.withUnsafeBufferPointer { scalarPtr in
                    IOConnectCallMethod(connection, Method.getFCPExchangeLog.rawValue,
                                        scalarPtr.baseAddress, 3, nil, 0, nil, nil,
                                        outPtr.baseAddress?.assumingMemoryBound(to: UInt8.self), &outSize)
                }
            }
            guard kr == KERN_SUCCESS else {
                print("[Connector] ❌ getFCPExchangeLog failed: \(interpretIOReturn(kr))")
                return nil
            }
            out.count = outSize
            guard let page = AvcReportSnapshot.ExchangeLog.parsePage(out) else { return nil }
            if log == nil {
                log = .init(session: page.session, dropped: page.dropped, records: [])
            }
            // A refresh started mid-read: the pages no longer belong together.
            guard page.session == log?.session else { return nil }
            log?.dropped = page.dropped
            log?.records += page.records
            next += UInt32(page.records.count)
            if page.records.isEmpty || next >= page.totalRecords { return log }
        }
        return log
    }

    func sendRawFCPCommand(guid: UInt64, frame: Data, timeoutMs: UInt32 = 15_000) -> Data? {
        guard isConnected else {
            log("sendRawFCPCommand: Not connected", level: .warning)
            return nil
        }
        guard connection != 0 else {
            log("sendRawFCPCommand: Invalid connection", level: .warning)
            return nil
        }
        guard frame.count >= 3 && frame.count <= 512 else {
            let message = "sendRawFCPCommand: Invalid frame length \(frame.count) (must be 3-512)"
            log(message, level: .error)
            lastError = message
            return nil
        }

        let scalarInputs: [UInt64] = [
            guid >> 32,
            guid & 0xFFFFFFFF
        ]

        var requestID: UInt64 = 0
        var scalarOutputCount: UInt32 = 1
        let scalarInputCount: UInt32 = UInt32(scalarInputs.count)

        let submitKR = frame.withUnsafeBytes { framePtr in
            scalarInputs.withUnsafeBufferPointer { scalarPtr in
                IOConnectCallMethod(
                    connection,
                    Method.sendRawFCPCommand.rawValue,
                    scalarPtr.baseAddress, scalarInputCount,
                    framePtr.baseAddress, frame.count,
                    &requestID, &scalarOutputCount,
                    nil, nil
                )
            }
        }

        guard submitKR == KERN_SUCCESS else {
            let error = "sendRawFCPCommand submit failed: \(interpretIOReturn(submitKR))"
            log(error, level: .error)
            lastError = error
            return nil
        }
        guard scalarOutputCount >= 1 else {
            let error = "sendRawFCPCommand submit failed: missing request ID"
            log(error, level: .error)
            lastError = error
            return nil
        }

        let start = DispatchTime.now().uptimeNanoseconds
        let timeoutNanos = UInt64(timeoutMs) * 1_000_000

        while DispatchTime.now().uptimeNanoseconds - start <= timeoutNanos {
            var pollInput: [UInt64] = [requestID]
            var outSize = 1024
            var out = Data(count: outSize)
            let pollInputCount: UInt32 = 1

            let pollKR = out.withUnsafeMutableBytes { outPtr in
                pollInput.withUnsafeMutableBufferPointer { inputPtr in
                    IOConnectCallMethod(
                        connection,
                        Method.getRawFCPCommandResult.rawValue,
                        inputPtr.baseAddress, pollInputCount,
                        nil, 0,
                        nil, nil,
                        outPtr.baseAddress?.assumingMemoryBound(to: UInt8.self), &outSize
                    )
                }
            }

            if pollKR == KERN_SUCCESS {
                out.count = outSize
                return out
            }

            if pollKR == kIOReturnNotReady {
                Thread.sleep(forTimeInterval: 0.005)
                continue
            }

            let error = "sendRawFCPCommand poll failed: \(interpretIOReturn(pollKR))"
            log(error, level: .error)
            lastError = error
            return nil
        }

        let timeoutError = "sendRawFCPCommand timed out waiting for response (\(timeoutMs) ms)"
        log(timeoutError, level: .error)
        lastError = timeoutError
        return nil
    }

    func reScanAVCUnits() -> Bool {
        guard isConnected else { return false }
        guard connection != 0 else { return false }

        let kr = IOConnectCallScalarMethod(
            connection,
            Method.reScanAVCUnits.rawValue,
            nil, 0,  // No inputs
            nil, nil // No outputs
        )

        if kr != KERN_SUCCESS {
            print("[Connector] ❌ reScanAVCUnits failed: \(interpretIOReturn(kr))")
            return false
        }

        print("[Connector] ✅ reScanAVCUnits triggered successfully")
        return true
    }
}
