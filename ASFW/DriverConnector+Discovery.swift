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
        return parseDeviceDiscoveryWire(wireData)
    }

    /// Parse wire format data from driver
    private func parseDeviceDiscoveryWire(_ data: Data) -> [FWDeviceInfo]? {
        var offset = 0

        // Read header
        guard offset + 8 <= data.count else {
            print("[Connector] ❌ Data too small for header")
            return nil
        }

        let deviceCount = data.withUnsafeBytes { $0.load(fromByteOffset: offset, as: UInt32.self) }
        offset += 8  // deviceCount + padding

        print("[Connector] 📋 Device count: \(deviceCount)")

        var devices: [FWDeviceInfo] = []

        // Read each device
        for _ in 0..<deviceCount {
            guard offset + 152 <= data.count else {  // sizeof(FWDeviceWire) = 152
                print("[Connector] ❌ Data truncated while reading device")
                return nil
            }

            let guid = data.withUnsafeBytes { $0.load(fromByteOffset: offset, as: UInt64.self) }
            let vendorId = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 8, as: UInt32.self) }
            let modelId = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 12, as: UInt32.self) }
            let generation = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 16, as: UInt32.self) }
            let nodeId = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 20, as: UInt8.self) }
            let stateRaw = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 21, as: UInt8.self) }
            let unitCount = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 22, as: UInt8.self) }

            // Read vendor name (64 bytes at offset 24)
            let vendorNameData = data.subdata(in: (offset + 24)..<(offset + 88))
            let vendorName = String(cString: vendorNameData.withUnsafeBytes { $0.bindMemory(to: CChar.self).baseAddress! })

            // Read model name (64 bytes at offset 88)
            let modelNameData = data.subdata(in: (offset + 88)..<(offset + 152))
            let modelName = String(cString: modelNameData.withUnsafeBytes { $0.bindMemory(to: CChar.self).baseAddress! })

            let state = FWDeviceState(rawValue: stateRaw) ?? .created

            offset += 152  // sizeof(FWDeviceWire)

            // Read units
            var units: [FWUnitInfo] = []
            for _ in 0..<unitCount {
                guard offset + 144 <= data.count else {  // sizeof(FWUnitWire) = 144
                    print("[Connector] ❌ Data truncated while reading unit")
                    return nil
                }

                let specId = data.withUnsafeBytes { $0.load(fromByteOffset: offset, as: UInt32.self) }
                let swVersion = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 4, as: UInt32.self) }
                let romOffset = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 8, as: UInt32.self) }
                let unitStateRaw = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 12, as: UInt8.self) }

                // Read unit vendor name (64 bytes at offset 16)
                let unitVendorData = data.subdata(in: (offset + 16)..<(offset + 80))
                let unitVendorName = String(cString: unitVendorData.withUnsafeBytes { $0.bindMemory(to: CChar.self).baseAddress! })

                // Read unit product name (64 bytes at offset 80)
                let unitProductData = data.subdata(in: (offset + 80)..<(offset + 144))
                let unitProductName = String(cString: unitProductData.withUnsafeBytes { $0.bindMemory(to: CChar.self).baseAddress! })

                let unitState = FWUnitState(rawValue: unitStateRaw) ?? .created

                units.append(FWUnitInfo(
                    specId: specId,
                    swVersion: swVersion,
                    state: unitState,
                    romOffset: romOffset,
                    vendorName: unitVendorName.isEmpty ? nil : unitVendorName,
                    productName: unitProductName.isEmpty ? nil : unitProductName
                ))

                offset += 144  // sizeof(FWUnitWire)
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
                units: units
            ))
        }

        print("[Connector] ✅ Parsed \(devices.count) devices")
        return devices
    }
}
