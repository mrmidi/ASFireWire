import Foundation
import Testing
@testable import ASFW

struct DeviceDiscoveryWireParsingTests {
    private func appendLE<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var raw = value.littleEndian
        withUnsafeBytes(of: &raw) { bytes in
            data.append(contentsOf: bytes)
        }
    }

    private func appendCString(_ value: String, byteCount: Int, to data: inout Data) {
        precondition(byteCount > 0)

        var bytes = Array(value.utf8.prefix(byteCount - 1))
        bytes.append(0)
        if bytes.count < byteCount {
            bytes.append(contentsOf: repeatElement(0, count: byteCount - bytes.count))
        }
        data.append(contentsOf: bytes)
    }

    @Test func parsesStorageDeviceKindAndUnitROMOffset() {
        var wire = Data()

        appendLE(UInt32(1), to: &wire)
        appendLE(UInt32(0), to: &wire)

        let guid: UInt64 = 0x0003_DB00_01DD_DD11
        appendLE(guid, to: &wire)
        appendLE(UInt32(0x0003DB), to: &wire)
        appendLE(UInt32(0x01DDDD), to: &wire)
        appendLE(UInt32(7), to: &wire)
        wire.append(0x1C) // nodeId
        wire.append(1)    // state = Ready
        wire.append(1)    // unitCount
        wire.append(4)    // deviceKind = Storage
        appendCString("Oxford", byteCount: 64, to: &wire)
        appendCString("911 Bridge", byteCount: 64, to: &wire)

        appendLE(UInt32(0x010483), to: &wire)
        appendLE(UInt32(0x060000), to: &wire)
        appendLE(UInt32(0x44), to: &wire)
        wire.append(1) // unitState = Ready
        wire.append(contentsOf: [UInt8](repeating: 0, count: 3))
        appendCString("Oxford", byteCount: 64, to: &wire)
        appendCString("SBP-2 Unit", byteCount: 64, to: &wire)

        let devices = ASFWDriverConnector.parseDeviceDiscoveryWire(wire)
        #expect(devices?.count == 1)

        guard let device = devices?.first else { return }
        #expect(device.guid == guid)
        #expect(device.deviceKind == 4)
        #expect(device.isStorage)
        #expect(device.vendorName == "Oxford")
        #expect(device.modelName == "911 Bridge")
        #expect(device.units.count == 1)
        #expect(device.units[0].romOffset == 0x44)
        #expect(device.units[0].specId == 0x010483)
        #expect(device.units[0].isSBP2Storage)
    }
}
