import Foundation
import Testing
@testable import ASFW

struct DeviceDiscoveryWireParsingTests {
    @Test func parsesSBP2MetadataForScannerUnit() {
        var wire = Data()
        wire.appendUInt32LE(1)
        wire.appendUInt32LE(0)
        wire.appendUInt64LE(0x0003_DB00_01AA_AA22)
        wire.appendUInt32LE(0x0003DB)
        wire.appendUInt32LE(0x01AAAA)
        wire.appendUInt32LE(9)
        wire.append(contentsOf: [0x21, 1, 1, 0])
        wire.appendCString("EPSON", byteCount: 64)
        wire.appendCString("GT-X970", byteCount: 64)
        wire.appendUInt32LE(0x00609E)
        wire.appendUInt32LE(0x010483)
        wire.appendUInt32LE(0x88)
        wire.append(contentsOf: [1, 0, 0, 0])
        wire.appendUInt32LE(0x00000080)
        wire.appendUInt32LE(0x00000002)
        wire.appendUInt32LE(0x00080400)
        wire.appendUInt32LE(0x00000011)
        wire.appendCString("EPSON", byteCount: 64)
        wire.appendCString("Scanner Unit", byteCount: 64)

        let devices = ASFWDriverConnector.parseDeviceDiscoveryWire(wire)

        #expect(devices?.count == 1)
        guard let device = devices?.first, let unit = device.units.first else {
            return
        }
        #expect(!device.isStorage)
        #expect(device.hasSBP2Unit)
        #expect(unit.managementAgentOffset == 0x80)
        #expect(unit.lun == 0x02)
        #expect(unit.unitCharacteristics == 0x00080400)
        #expect(unit.fastStart == 0x11)
    }
}

private extension Data {
    mutating func appendUInt64LE(_ value: UInt64) {
        var raw = value.littleEndian
        Swift.withUnsafeBytes(of: &raw) { append(contentsOf: $0) }
    }

    mutating func appendCString(_ value: String, byteCount: Int) {
        var bytes = Array(value.utf8.prefix(byteCount - 1))
        bytes.append(0)
        bytes.append(contentsOf: repeatElement(0, count: byteCount - bytes.count))
        append(contentsOf: bytes)
    }
}
