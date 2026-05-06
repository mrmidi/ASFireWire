import Foundation

extension ASFWDriverConnector {
    // MARK: - Device Discovery

    func getDiscoveredDevices() -> [FWDeviceInfo]? {
        guard isConnected else {
            log("getDiscoveredDevices: Not connected", level: .warning)
            return nil
        }

        // Use callStruct to get wire format data (4KB limit for IOConnectCallStructMethod)
        guard let wireData = callStruct(.getDiscoveredDevices, initialCap: 4096) else {
            log("getDiscoveredDevices: callStruct failed", level: .error)
            return nil
        }

        guard !wireData.isEmpty else {
            log("getDiscoveredDevices: no data returned", level: .warning)
            return []
        }

        print("[Connector] 📦 Received \(wireData.count) bytes of wire format data")

        // Parse wire format
        return Self.parseDeviceDiscoveryWire(wireData)
    }

    /// Parse wire format data from driver
    static func parseDeviceDiscoveryWire(_ data: Data) -> [FWDeviceInfo]? {
        @inline(__always)
        func readUInt8(at offset: Int) -> UInt8 {
            data[data.startIndex + offset]
        }

        @inline(__always)
        func readUInt32(at offset: Int) -> UInt32 {
            var value: UInt32 = 0
            for i in 0..<4 {
                value |= UInt32(data[data.startIndex + offset + i]) << (i * 8)
            }
            return value
        }

        @inline(__always)
        func readUInt64(at offset: Int) -> UInt64 {
            var value: UInt64 = 0
            for i in 0..<8 {
                value |= UInt64(data[data.startIndex + offset + i]) << (i * 8)
            }
            return value
        }

        func readCString(at offset: Int, length: Int) -> String {
            let start = data.startIndex + offset
            let end = start + length
            let bytes = data[start..<end]
            let trimmed = bytes.prefix { $0 != 0 }
            return String(decoding: trimmed, as: UTF8.self)
        }

        var offset = 0

        // Read header
        guard offset + 8 <= data.count else {
            print("[Connector] ❌ Data too small for header")
            return nil
        }

        let deviceCount = readUInt32(at: offset)
        offset += 8  // deviceCount + padding

        print("[Connector] 📋 Device count: \(deviceCount)")

        var devices: [FWDeviceInfo] = []

        // Read each device
        for _ in 0..<deviceCount {
            guard offset + 152 <= data.count else {  // sizeof(FWDeviceWire) = 152
                print("[Connector] ❌ Data truncated while reading device")
                return nil
            }

            let guid = readUInt64(at: offset)
            let vendorId = readUInt32(at: offset + 8)
            let modelId = readUInt32(at: offset + 12)
            let generation = readUInt32(at: offset + 16)
            let nodeId = readUInt8(at: offset + 20)
            let stateRaw = readUInt8(at: offset + 21)
            let unitCount = readUInt8(at: offset + 22)
            let deviceKind = readUInt8(at: offset + 23)
            let vendorName = readCString(at: offset + 24, length: 64)
            let modelName = readCString(at: offset + 88, length: 64)

            let state = FWDeviceState(rawValue: stateRaw) ?? .created

            offset += 152  // sizeof(FWDeviceWire)

            // Read units
            var units: [FWUnitInfo] = []
            for _ in 0..<unitCount {
                guard offset + 160 <= data.count else {  // sizeof(FWUnitWire) = 160
                    print("[Connector] ❌ Data truncated while reading unit")
                    return nil
                }

                let specId = readUInt32(at: offset)
                let swVersion = readUInt32(at: offset + 4)
                let romOffset = readUInt32(at: offset + 8)
                let unitStateRaw = readUInt8(at: offset + 12)
                let managementAgentOffset = readUInt32(at: offset + 16)
                let lun = readUInt32(at: offset + 20)
                let unitCharacteristics = readUInt32(at: offset + 24)
                let fastStart = readUInt32(at: offset + 28)
                let unitVendorName = readCString(at: offset + 32, length: 64)
                let unitProductName = readCString(at: offset + 96, length: 64)

                let unitState = FWUnitState(rawValue: unitStateRaw) ?? .created

                units.append(FWUnitInfo(
                    specId: specId,
                    swVersion: swVersion,
                    state: unitState,
                    romOffset: romOffset,
                    managementAgentOffset: managementAgentOffset == 0 ? nil : managementAgentOffset,
                    lun: lun == 0 ? nil : lun,
                    unitCharacteristics: unitCharacteristics == 0 ? nil : unitCharacteristics,
                    fastStart: fastStart == 0 ? nil : fastStart,
                    vendorName: unitVendorName.isEmpty ? nil : unitVendorName,
                    productName: unitProductName.isEmpty ? nil : unitProductName
                ))

                offset += 160  // sizeof(FWUnitWire)
            }

            devices.append(FWDeviceInfo(
                id: guid,
                guid: guid,
                vendorId: vendorId,
                modelId: modelId,
                vendorName: vendorName,
                modelName: modelName,
                nodeId: nodeId,
                generation: generation,
                state: state,
                units: units,
                deviceKind: deviceKind
            ))
        }

        print("[Connector] ✅ Parsed \(devices.count) devices")
        return devices
    }
}
