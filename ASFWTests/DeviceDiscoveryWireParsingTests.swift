import Foundation
import Testing
@testable import ASFW

struct DeviceDiscoveryWireParsingTests {
    private func appendLE<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var raw = value.littleEndian
        withUnsafeBytes(of: &raw) { data.append(contentsOf: $0) }
    }

    private func appendFixedString(_ value: String, count: Int, to data: inout Data) {
        let bytes = Array(value.utf8.prefix(count - 1))
        data.append(contentsOf: bytes)
        data.append(contentsOf: repeatElement(0, count: count - bytes.count))
    }

    private func appendUnit(
        deviceID: UInt64,
        offset: UInt32,
        specifier: UInt32,
        version: UInt32,
        vendor: UInt32,
        model: UInt32,
        lun: UInt32,
        vendorName: String,
        productName: String,
        to data: inout Data
    ) {
        appendLE(UInt16(2), to: &data)
        appendLE(UInt16(184), to: &data)
        appendLE(deviceID, to: &data)
        appendLE(offset, to: &data)
        appendLE(specifier, to: &data)
        appendLE(version, to: &data)
        appendLE(UInt32(0x3F), to: &data)
        appendLE(vendor, to: &data)
        appendLE(model, to: &data)
        appendLE(lun, to: &data)
        appendLE(UInt32(0x0001_2340), to: &data)
        appendLE(UInt32(0x0000_0100), to: &data)
        appendLE(UInt32(1), to: &data)
        data.append(1)
        data.append(contentsOf: [0, 0, 0])
        appendFixedString(vendorName, count: 64, to: &data)
        appendFixedString(productName, count: 64, to: &data)
    }

    private func fixture(version: UInt16 = 2) -> Data {
        let deviceID: UInt64 = 41
        let busInfo: [UInt32] = [
            0x0504_1234, 0x3133_3934, 0xE0FF_1232,
            0x000D_6C04, 0x0400_0002,
        ]
        let deviceBytes = 174 + busInfo.count * 4 + 2 * 184
        var wire = Data()

        appendLE(version, to: &wire)
        appendLE(UInt16(16), to: &wire)
        appendLE(UInt32(16 + deviceBytes), to: &wire)
        appendLE(UInt32(1), to: &wire)
        appendLE(UInt32(0), to: &wire)

        appendLE(version, to: &wire)
        appendLE(UInt16(deviceBytes), to: &wire)
        appendLE(deviceID, to: &wire)
        appendLE(UInt64(0x000D_6C04_0400_0002), to: &wire)
        appendLE(UInt32(17), to: &wire)
        appendLE(UInt16(0xFFC3), to: &wire)
        wire.append(2) // quarantined
        wire.append(2) // duplicate observed GUID
        wire.append(3) // audio
        wire.append(0x03) // independent root vendor/model present
        appendLE(UInt16(2), to: &wire)
        appendLE(UInt16(busInfo.count), to: &wire)
        appendLE(UInt32(0x000D6C), to: &wire)
        appendLE(UInt32(0x00130E), to: &wire)
        appendLE(UInt32(0x000100), to: &wire)
        appendFixedString("Root Vendor", count: 64, to: &wire)
        appendFixedString("Root Model", count: 64, to: &wire)
        for word in busInfo { appendLE(word, to: &wire) }

        appendUnit(
            deviceID: deviceID, offset: 0x20, specifier: 0x00A02D,
            version: 0x000101, vendor: 0x000A92, model: 0x01001800,
            lun: 0, vendorName: "Unit Vendor A", productName: "Unit A", to: &wire
        )
        appendUnit(
            deviceID: deviceID, offset: 0x48, specifier: 0x000A35,
            version: 0x000103, vendor: 0x00130E, model: 0x000200,
            lun: 7, vendorName: "Unit Vendor B", productName: "Unit B", to: &wire
        )
        return wire
    }

    @Test func preservesIndependentRootBusAndMultipleUnitEvidence() throws {
        let devices = try #require(ASFWDriverConnector.parseDeviceDiscoveryWire(fixture()))
        let device = try #require(devices.first)

        #expect(devices.count == 1)
        #expect(device.id == DeviceInstanceID(41))
        #expect(device.observedGuid == 0x000D_6C04_0400_0002)
        #expect(device.state == .quarantined)
        #expect(device.quarantineReason == .duplicateObservedGuid)
        #expect(device.quarantineReason.description == "Duplicate observed GUID")
        #expect(device.nodeVendorOui == 0x000D6C)
        #expect(device.rootVendorId == 0x00130E)
        #expect(device.rootModelId == 0x000100)
        #expect(device.rawBusInfoQuadlets.count == 5)
        #expect(device.rawBusInfoQuadlets[1] == 0x3133_3934)
        #expect(device.units.count == 2)
        #expect(device.units[0].id == UnitInstanceID(device: DeviceInstanceID(41), unitDirectoryOffset: 0x20))
        #expect(device.units[0].vendorId == 0x000A92)
        #expect(device.units[0].modelId == 0x01001800)
        #expect(device.units[1].id.unitDirectoryOffset == 0x48)
        #expect(device.units[1].specId == 0x000A35)
        #expect(device.units[1].swVersion == 0x000103)
        #expect(device.units[1].lun == 7)
        #expect(device.units[1].productName == "Unit B")
    }

    @Test func rejectsUnknownVersionsTruncationAndTrailingBytes() {
        #expect(ASFWDriverConnector.parseDeviceDiscoveryWire(fixture(version: 3)) == nil)
        #expect(ASFWDriverConnector.parseDeviceDiscoveryWire(Data(fixture().dropLast())) == nil)
        var trailing = fixture()
        trailing.append(0)
        #expect(ASFWDriverConnector.parseDeviceDiscoveryWire(trailing) == nil)
    }

    @Test func staleSelectionIsDroppedWhenSnapshotIdentityChanges() {
        let selected = DeviceInstanceID(41)
        #expect(DeviceDiscoverySelectionPolicy.reconcile(
            selected: selected,
            available: [selected]
        ) == selected)
        #expect(DeviceDiscoverySelectionPolicy.reconcile(
            selected: selected,
            available: [DeviceInstanceID(42)]
        ) == nil)
        #expect(DeviceDiscoverySelectionPolicy.reconcile(
            selected: nil,
            available: [selected]
        ) == nil)
    }
}
