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

        appendLE(UInt32(0x00609E), to: &wire)
        appendLE(UInt32(0x010483), to: &wire)
        appendLE(UInt32(0x44), to: &wire)
        wire.append(1) // unitState = Ready
        wire.append(contentsOf: [UInt8](repeating: 0, count: 3))
        appendLE(UInt32(0), to: &wire) // management agent offset
        appendLE(UInt32(0), to: &wire) // lun
        appendLE(UInt32(0), to: &wire) // unit characteristics
        appendLE(UInt32(0), to: &wire) // fast start
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
        #expect(device.units[0].specId == 0x00609E)
        #expect(device.units[0].isSBP2Storage)
    }

    @Test func parsesSBP2UnitMetadataEvenWhenDeviceKindIsNotStorage() {
        var wire = Data()

        appendLE(UInt32(1), to: &wire)
        appendLE(UInt32(0), to: &wire)

        let guid: UInt64 = 0x0003_DB00_01AA_AA22
        appendLE(guid, to: &wire)
        appendLE(UInt32(0x0003DB), to: &wire)
        appendLE(UInt32(0x01AAAA), to: &wire)
        appendLE(UInt32(9), to: &wire)
        wire.append(0x21) // nodeId
        wire.append(1)    // state = Ready
        wire.append(1)    // unitCount
        wire.append(0)    // deviceKind = Unknown
        appendCString("ScannerCo", byteCount: 64, to: &wire)
        appendCString("FilmScanner", byteCount: 64, to: &wire)

        appendLE(UInt32(0x00609E), to: &wire)
        appendLE(UInt32(0x010483), to: &wire)
        appendLE(UInt32(0x88), to: &wire)
        wire.append(1) // unitState = Ready
        wire.append(contentsOf: [UInt8](repeating: 0, count: 3))
        appendLE(UInt32(0x00000080), to: &wire) // management agent offset
        appendLE(UInt32(0x00000002), to: &wire) // lun
        appendLE(UInt32(0x00080400), to: &wire) // unit characteristics
        appendLE(UInt32(0x00000011), to: &wire) // fast start
        appendCString("ScannerCo", byteCount: 64, to: &wire)
        appendCString("Scanner Unit", byteCount: 64, to: &wire)

        let devices = ASFWDriverConnector.parseDeviceDiscoveryWire(wire)
        #expect(devices?.count == 1)

        guard let device = devices?.first else { return }
        #expect(!device.isStorage)
        #expect(device.hasSBP2Unit)
        #expect(device.sbp2Units.count == 1)
        #expect(device.sbp2Units[0].managementAgentOffset == 0x80)
        #expect(device.sbp2Units[0].lun == 0x02)
        #expect(device.sbp2Units[0].unitCharacteristics == 0x00080400)
        #expect(device.sbp2Units[0].fastStart == 0x11)
    }
}
