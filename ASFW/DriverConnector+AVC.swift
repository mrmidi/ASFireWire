import Foundation
import IOKit

extension ASFWDriverConnector {
    private static let avcUnitsWireVersion: UInt16 = 2
    private static let avcUnitsHeaderSize = 16
    private static let avcUnitRecordSize = 60
    private static let avcSubunitRecordSize = 8

    func getAVCUnits() -> [AVCUnitInfo]? {
        guard isConnected else {
            log("getAVCUnits: Not connected", level: .warning)
            return nil
        }
        guard let data = callStruct(.getAVCUnits,
                                    initialCap: DriverConnectorTransport.maxInlineStructOutputBytes) else {
            log("getAVCUnits: callStruct failed", level: .error)
            return nil
        }
        guard let units = Self.parseAVCUnitsWire(data) else {
            log("getAVCUnits: could not parse \(data.count) bytes of AV/C unit v2; "
                + "reporting no units", level: .error)
            return nil
        }
        return units
    }

    static func parseAVCUnitsWire(_ data: Data) -> [AVCUnitInfo]? {
        func contains(_ offset: Int, _ length: Int, limit: Int? = nil) -> Bool {
            guard offset >= 0, length >= 0 else { return false }
            let upper = limit ?? data.count
            return offset <= upper && length <= upper - offset && upper <= data.count
        }

        func u8(_ offset: Int) -> UInt8? {
            guard contains(offset, 1) else { return nil }
            return data[data.startIndex + offset]
        }

        func u16(_ offset: Int) -> UInt16? {
            guard contains(offset, 2) else { return nil }
            return UInt16(data[data.startIndex + offset]) |
                (UInt16(data[data.startIndex + offset + 1]) << 8)
        }

        func u32(_ offset: Int) -> UInt32? {
            guard contains(offset, 4) else { return nil }
            var value: UInt32 = 0
            for index in 0..<4 {
                value |= UInt32(data[data.startIndex + offset + index]) << (index * 8)
            }
            return value
        }

        func u64(_ offset: Int) -> UInt64? {
            guard contains(offset, 8) else { return nil }
            var value: UInt64 = 0
            for index in 0..<8 {
                value |= UInt64(data[data.startIndex + offset + index]) << (index * 8)
            }
            return value
        }

        guard contains(0, avcUnitsHeaderSize),
              u16(0) == avcUnitsWireVersion,
              let headerSize = u16(2).map(Int.init),
              headerSize >= avcUnitsHeaderSize,
              let byteSize = u32(4).map(Int.init),
              byteSize >= headerSize,
              byteSize <= data.count,
              let unitCount = u32(8),
              u32(12) == 0 else {
            return nil
        }

        var cursor = headerSize
        var units: [AVCUnitInfo] = []
        units.reserveCapacity(Int(unitCount))

        for _ in 0..<unitCount {
            guard contains(cursor, avcUnitRecordSize, limit: byteSize),
                  u16(cursor) == avcUnitsWireVersion,
                  let recordSize = u16(cursor + 2).map(Int.init),
                  recordSize >= avcUnitRecordSize,
                  recordSize <= byteSize - cursor,
                  let rawDeviceId = u64(cursor + 4),
                  rawDeviceId != 0,
                  let unitOffset = u32(cursor + 12),
                  let generation = u32(cursor + 16),
                  let observedGuid = u64(cursor + 20),
                  let nodeID = u16(cursor + 28),
                  let initialized = u8(cursor + 30),
                  let subunitCount = u8(cursor + 31),
                  let rootVendorID = u32(cursor + 32),
                  let rootModelID = u32(cursor + 36),
                  let unitVendorID = u32(cursor + 40),
                  let unitModelID = u32(cursor + 44),
                  let specifierID = u32(cursor + 48),
                  let unitVersion = u32(cursor + 52),
                  let isoInputPlugs = u8(cursor + 56),
                  let isoOutputPlugs = u8(cursor + 57),
                  let extInputPlugs = u8(cursor + 58),
                  let extOutputPlugs = u8(cursor + 59) else {
                return nil
            }

            let recordEnd = cursor + recordSize
            var subunitCursor = cursor + avcUnitRecordSize
            var subunits: [AVCSubunitInfo] = []
            subunits.reserveCapacity(Int(subunitCount))
            for _ in 0..<subunitCount {
                guard contains(subunitCursor, avcSubunitRecordSize, limit: recordEnd),
                      u16(subunitCursor) == avcUnitsWireVersion,
                      u16(subunitCursor + 2) == UInt16(avcSubunitRecordSize),
                      let type = u8(subunitCursor + 4),
                      let subunitID = u8(subunitCursor + 5),
                      let numSrcPlugs = u8(subunitCursor + 6),
                      let numDestPlugs = u8(subunitCursor + 7) else {
                    return nil
                }
                subunits.append(AVCSubunitInfo(
                    type: type,
                    subunitID: subunitID,
                    numSrcPlugs: numSrcPlugs,
                    numDestPlugs: numDestPlugs
                ))
                subunitCursor += avcSubunitRecordSize
            }
            guard subunitCursor == recordEnd else { return nil }

            let deviceId = DeviceInstanceID(rawValue: rawDeviceId)
            units.append(AVCUnitInfo(
                id: UnitInstanceID(device: deviceId, unitDirectoryOffset: unitOffset),
                observedGuid: observedGuid,
                generation: generation,
                nodeID: nodeID,
                isInitialized: initialized != 0,
                rootVendorID: rootVendorID,
                rootModelID: rootModelID,
                unitVendorID: unitVendorID,
                unitModelID: unitModelID,
                specifierID: specifierID,
                unitVersion: unitVersion,
                subunits: subunits,
                isoInputPlugs: isoInputPlugs,
                isoOutputPlugs: isoOutputPlugs,
                extInputPlugs: extInputPlugs,
                extOutputPlugs: extOutputPlugs
            ))
            cursor = recordEnd
        }

        guard cursor == byteSize else { return nil }
        return units
    }

    func getDriverVersion() -> DriverVersionInfo? {
        guard isConnected else {
            log("getDriverVersion: Not connected", level: .warning)
            return nil
        }
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

    func getSubunitCapabilities(
        unitID: UnitInstanceID,
        type: UInt8,
        id: UInt8
    ) -> AVCMusicCapabilities? {
        guard isConnected, connection != 0 else { return nil }
        let scalarInputs: [UInt64] = [
            unitID.device.rawValue,
            UInt64(unitID.unitDirectoryOffset),
            UInt64(type),
            UInt64(id)
        ]

        var outSize = 1024
        var out = Data(count: outSize)
        let kr = out.withUnsafeMutableBytes { outPtr in
            scalarInputs.withUnsafeBufferPointer { scalarPtr in
                IOConnectCallMethod(
                    connection,
                    Method.getSubunitCapabilities.rawValue,
                    scalarPtr.baseAddress, UInt32(scalarInputs.count),
                    nil, 0,
                    nil, nil,
                    outPtr.baseAddress?.assumingMemoryBound(to: UInt8.self), &outSize
                )
            }
        }
        guard kr == KERN_SUCCESS else {
            log("getSubunitCapabilities failed: \(interpretIOReturn(kr))", level: .error)
            return nil
        }
        out.count = outSize
        return AVCMusicCapabilities(data: out)
    }

    func getSubunitDescriptor(
        unitID: UnitInstanceID,
        type: UInt8,
        id: UInt8
    ) -> Data? {
        guard isConnected, connection != 0 else { return nil }
        let scalarInputs: [UInt64] = [
            unitID.device.rawValue,
            UInt64(unitID.unitDirectoryOffset),
            UInt64(type),
            UInt64(id)
        ]

        var outSize = 4 * 1024
        var out = Data(count: outSize)
        let kr = out.withUnsafeMutableBytes { outPtr in
            scalarInputs.withUnsafeBufferPointer { scalarPtr in
                IOConnectCallMethod(
                    connection,
                    Method.getSubunitDescriptor.rawValue,
                    scalarPtr.baseAddress, UInt32(scalarInputs.count),
                    nil, 0,
                    nil, nil,
                    outPtr.baseAddress?.assumingMemoryBound(to: UInt8.self), &outSize
                )
            }
        }
        guard kr == KERN_SUCCESS else {
            log("getSubunitDescriptor failed: \(interpretIOReturn(kr))", level: .error)
            return nil
        }
        out.count = outSize
        return out
    }

    func sendRawFCPCommand(
        unitID: UnitInstanceID,
        frame: Data,
        timeoutMs: UInt32 = 15_000
    ) -> Data? {
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
            unitID.device.rawValue,
            UInt64(unitID.unitDirectoryOffset)
        ]
        var requestID: UInt64 = 0
        var scalarOutputCount: UInt32 = 1
        let submitKR = frame.withUnsafeBytes { framePtr in
            scalarInputs.withUnsafeBufferPointer { scalarPtr in
                IOConnectCallMethod(
                    connection,
                    Method.sendRawFCPCommand.rawValue,
                    scalarPtr.baseAddress, UInt32(scalarInputs.count),
                    framePtr.baseAddress, frame.count,
                    &requestID, &scalarOutputCount,
                    nil, nil
                )
            }
        }

        guard submitKR == KERN_SUCCESS, scalarOutputCount >= 1 else {
            let error = "sendRawFCPCommand submit failed: \(interpretIOReturn(submitKR))"
            log(error, level: .error)
            lastError = error
            return nil
        }

        let start = DispatchTime.now().uptimeNanoseconds
        let timeoutNanos = UInt64(timeoutMs) * 1_000_000
        while DispatchTime.now().uptimeNanoseconds - start <= timeoutNanos {
            let pollInput: [UInt64] = [requestID]
            var outSize = 1024
            var out = Data(count: outSize)
            let pollKR = out.withUnsafeMutableBytes { outPtr in
                pollInput.withUnsafeBufferPointer { inputPtr in
                    IOConnectCallMethod(
                        connection,
                        Method.getRawFCPCommandResult.rawValue,
                        inputPtr.baseAddress, UInt32(pollInput.count),
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

        let error = "sendRawFCPCommand timed out waiting for response (\(timeoutMs) ms)"
        log(error, level: .error)
        lastError = error
        return nil
    }

    func reScanAVCUnits() -> Bool {
        guard isConnected, connection != 0 else { return false }
        let kr = IOConnectCallScalarMethod(
            connection,
            Method.reScanAVCUnits.rawValue,
            nil, 0,
            nil, nil
        )
        guard kr == KERN_SUCCESS else {
            log("reScanAVCUnits failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }
        return true
    }
}
