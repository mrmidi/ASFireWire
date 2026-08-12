import Foundation

extension ASFWDriverConnector {
    private static let deviceDiscoveryWireVersion: UInt16 = 2
    private static let discoveryHeaderSize = 16
    private static let discoveryDeviceSize = 174
    private static let discoveryUnitSize = 184

    func getDiscoveredDevices() -> [FWDeviceInfo]? {
        guard isConnected else {
            log("getDiscoveredDevices: Not connected", level: .warning)
            return nil
        }
        guard let wireData = callStruct(.getDiscoveredDevices,
                                        initialCap: DriverConnectorTransport.maxInlineStructOutputBytes) else {
            log("getDiscoveredDevices: callStruct failed", level: .error)
            return nil
        }
        guard !wireData.isEmpty else { return [] }
        guard let devices = Self.parseDeviceDiscoveryWire(wireData) else {
            // Failing closed here is right — a half-decoded device list is worse
            // than none — but it must not fail silently. Reporting nothing while
            // the driver logs a correct payload is indistinguishable from an
            // empty bus, and reads as a driver fault that it isn't.
            log("getDiscoveredDevices: could not parse \(wireData.count) bytes of "
                + "discovery v2; reporting no devices", level: .error)
            return nil
        }
        var routes: [DeviceInstanceID: UInt32] = [:]
        for device in devices {
            routes[device.id] = device.generation
        }
        reconcileDuetCachedStateRoutes(routes)
        return devices
    }

    static func parseDeviceDiscoveryWire(_ data: Data) -> [FWDeviceInfo]? {
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

        func cString(_ offset: Int, length: Int, limit: Int) -> String? {
            guard contains(offset, length, limit: limit) else { return nil }
            let bytes = data[(data.startIndex + offset)..<(data.startIndex + offset + length)]
            return String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
        }

        guard contains(0, discoveryHeaderSize),
              u16(0) == deviceDiscoveryWireVersion,
              let headerSize = u16(2).map(Int.init),
              headerSize >= discoveryHeaderSize,
              let byteSize = u32(4).map(Int.init),
              byteSize >= headerSize,
              byteSize <= data.count,
              let deviceCount = u32(8),
              u32(12) == 0 else {
            return nil
        }

        var cursor = headerSize
        var devices: [FWDeviceInfo] = []
        devices.reserveCapacity(Int(deviceCount))

        for _ in 0..<deviceCount {
            guard contains(cursor, discoveryDeviceSize, limit: byteSize),
                  u16(cursor) == deviceDiscoveryWireVersion,
                  let recordSize = u16(cursor + 2).map(Int.init),
                  recordSize >= discoveryDeviceSize,
                  recordSize <= byteSize - cursor,
                  let rawInstance = u64(cursor + 4),
                  rawInstance != 0,
                  let observedGuid = u64(cursor + 12),
                  let generation = u32(cursor + 20),
                  let nodeId = u16(cursor + 24),
                  let stateRaw = u8(cursor + 26),
                  let quarantineRaw = u8(cursor + 27),
                  let deviceKind = u8(cursor + 28),
                  let evidenceFlags = u8(cursor + 29),
                  let unitCount = u16(cursor + 30),
                  let busInfoWordCount = u16(cursor + 32),
                  let nodeVendorOui = u32(cursor + 34),
                  let rootVendorRaw = u32(cursor + 38),
                  let rootModelRaw = u32(cursor + 42),
                  let deviceState = FWDeviceState(rawValue: stateRaw) else {
                return nil
            }

            let recordEnd = cursor + recordSize
            guard let vendorName = cString(cursor + 46, length: 64, limit: recordEnd),
                  let modelName = cString(cursor + 110, length: 64, limit: recordEnd) else {
                return nil
            }

            let deviceId = DeviceInstanceID(rawValue: rawInstance)
            var sectionCursor = cursor + discoveryDeviceSize
            let busInfoBytes = Int(busInfoWordCount) * MemoryLayout<UInt32>.size
            guard contains(sectionCursor, busInfoBytes, limit: recordEnd) else { return nil }
            var rawBusInfo: [UInt32] = []
            rawBusInfo.reserveCapacity(Int(busInfoWordCount))
            for index in 0..<Int(busInfoWordCount) {
                guard let word = u32(sectionCursor + index * 4) else { return nil }
                rawBusInfo.append(word)
            }
            sectionCursor += busInfoBytes

            var units: [FWUnitInfo] = []
            units.reserveCapacity(Int(unitCount))
            for _ in 0..<unitCount {
                guard contains(sectionCursor, discoveryUnitSize, limit: recordEnd),
                      u16(sectionCursor) == deviceDiscoveryWireVersion,
                      let unitByteSize = u16(sectionCursor + 2).map(Int.init),
                      unitByteSize == discoveryUnitSize,
                      let unitDeviceRaw = u64(sectionCursor + 4),
                      unitDeviceRaw == rawInstance,
                      let romOffset = u32(sectionCursor + 12),
                      let specId = u32(sectionCursor + 16),
                      let swVersion = u32(sectionCursor + 20),
                      let unitFlags = u32(sectionCursor + 24),
                      let unitVendorRaw = u32(sectionCursor + 28),
                      let unitModelRaw = u32(sectionCursor + 32),
                      let lunRaw = u32(sectionCursor + 36),
                      let managementRaw = u32(sectionCursor + 40),
                      let characteristicsRaw = u32(sectionCursor + 44),
                      let fastStartRaw = u32(sectionCursor + 48),
                      let unitStateRaw = u8(sectionCursor + 52),
                      let unitState = FWUnitState(rawValue: unitStateRaw),
                      let unitVendorName = cString(sectionCursor + 56, length: 64, limit: recordEnd),
                      let unitProductName = cString(sectionCursor + 120, length: 64, limit: recordEnd) else {
                    return nil
                }

                let hasUnitVendor = (unitFlags & (1 << 0)) != 0
                let hasUnitModel = (unitFlags & (1 << 1)) != 0
                let hasLun = (unitFlags & (1 << 2)) != 0
                let hasManagement = (unitFlags & (1 << 3)) != 0
                let hasCharacteristics = (unitFlags & (1 << 4)) != 0
                let hasFastStart = (unitFlags & (1 << 5)) != 0
                units.append(FWUnitInfo(
                    id: UnitInstanceID(device: deviceId, unitDirectoryOffset: romOffset),
                    specId: specId,
                    swVersion: swVersion,
                    vendorId: hasUnitVendor ? unitVendorRaw : nil,
                    modelId: hasUnitModel ? unitModelRaw : nil,
                    state: unitState,
                    romOffset: romOffset,
                    managementAgentOffset: hasManagement ? managementRaw : nil,
                    lun: hasLun ? lunRaw : nil,
                    unitCharacteristics: hasCharacteristics ? characteristicsRaw : nil,
                    fastStart: hasFastStart ? fastStartRaw : nil,
                    vendorName: unitVendorName.isEmpty ? nil : unitVendorName,
                    productName: unitProductName.isEmpty ? nil : unitProductName
                ))
                sectionCursor += unitByteSize
            }

            guard sectionCursor == recordEnd else { return nil }
            devices.append(FWDeviceInfo(
                id: deviceId,
                observedGuid: observedGuid,
                nodeVendorOui: nodeVendorOui,
                rootVendorId: (evidenceFlags & (1 << 0)) != 0 ? rootVendorRaw : nil,
                rootModelId: (evidenceFlags & (1 << 1)) != 0 ? rootModelRaw : nil,
                vendorName: vendorName,
                modelName: modelName,
                nodeId: nodeId,
                generation: generation,
                state: deviceState,
                quarantineReason: DriverConnectorDeviceQuarantineReason(wireValue: quarantineRaw),
                rawBusInfoQuadlets: rawBusInfo,
                units: units,
                deviceKind: deviceKind
            ))
            cursor = recordEnd
        }

        guard cursor == byteSize, byteSize == data.count else { return nil }
        return devices
    }
}
