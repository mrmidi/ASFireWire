import Foundation

extension ASFWDriverConnector {
    func getAlesisMultiMixDevice() -> FWDeviceInfo? {
        getDiscoveredDevices()?.first { device in
            let deviceMatch = device.vendorId == AlesisConstants.vendorID
                && device.modelId == AlesisConstants.multiMixModelID
            let unitMatch = device.units.contains { unit in
                unit.specId == AlesisConstants.fireWireUnitSpecID
                && unit.swVersion == AlesisConstants.fireWireUnitSoftwareVersion
            }
            let nameMatch = "\(device.vendorName) \(device.modelName)"
                .localizedCaseInsensitiveContains("Alesis")
            return deviceMatch || unitMatch || nameMatch
        }
    }

    func refreshAlesisDiceSnapshot(device: FWDeviceInfo? = nil) -> AlesisDiceSnapshot? {
        guard isConnected else { return nil }
        guard let target = device ?? getAlesisMultiMixDevice() else { return nil }
        let destinationID = UInt16(target.nodeId)

        guard let sectionData = readAlesisDiceBlock(destinationID: destinationID, offset: 0, length: 40),
              let sections = AlesisDiceSections.parse(sectionData) else {
            return nil
        }

        let globalLength = boundedReadLength(sections.global.size, minimum: 0x68, maximum: 512)
        let txLength = boundedReadLength(sections.txStreamFormat.size, minimum: 8, maximum: 2048)
        let rxLength = boundedReadLength(sections.rxStreamFormat.size, minimum: 8, maximum: 2048)

        let global = readAlesisDiceBlock(destinationID: destinationID,
                                         offset: sections.global.offset,
                                         length: globalLength)
            .flatMap(AlesisDiceGlobalState.parse)
        let tx = readAlesisDiceBlock(destinationID: destinationID,
                                     offset: sections.txStreamFormat.offset,
                                     length: txLength)
            .flatMap { AlesisDiceStreamConfig.parse($0, isRxLayout: false) }
        let rx = readAlesisDiceBlock(destinationID: destinationID,
                                     offset: sections.rxStreamFormat.offset,
                                     length: rxLength)
            .flatMap { AlesisDiceStreamConfig.parse($0, isRxLayout: true) }

        return AlesisDiceSnapshot(sections: sections, global: global, txStreams: tx, rxStreams: rx)
    }

    func readAlesisDiceBlock(destinationID: UInt16, offset: UInt32, length: Int) -> Data? {
        guard length > 0 else { return nil }
        let absolute = AlesisConstants.diceBaseAddress + UInt64(offset)
        let addressHigh = UInt16((absolute >> 32) & 0xFFFF)
        let addressLow = UInt32(absolute & 0xFFFF_FFFF)

        guard let handle = asyncBlockRead(destinationID: destinationID,
                                          addressHigh: addressHigh,
                                          addressLow: addressLow,
                                          length: UInt32(length)) else {
            return nil
        }

        let deadline = Date().addingTimeInterval(2.0)
        while Date() < deadline {
            if let result = getTransactionResult(handle: handle, initialPayloadCapacity: length + 128) {
                guard result.status == 0 && result.responseCode == 0 else {
                    log(String(format: "Alesis DICE read failed status=0x%08X rCode=0x%02X", result.status, result.responseCode),
                        level: .warning)
                    return nil
                }
                return result.payload
            }
            Thread.sleep(forTimeInterval: 0.05)
        }

        log(String(format: "Alesis DICE read timed out waiting for result (handle=0x%04X)", handle), level: .warning)
        return nil
    }

    private func boundedReadLength(_ sectionSize: UInt32, minimum: Int, maximum: Int) -> Int {
        let raw = sectionSize > 0 ? Int(sectionSize) : minimum
        return max(minimum, min(maximum, raw))
    }
}
