import Foundation
import Testing
@testable import ASFW

struct AVCUnitWireParsingTests {
    private func appendLE<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var raw = value.littleEndian
        withUnsafeBytes(of: &raw) { data.append(contentsOf: $0) }
    }

    private func fixture(version: UInt16 = 2) -> Data {
        let headerBytes: UInt16 = 16
        let recordBytes: UInt16 = 68
        var wire = Data()

        appendLE(version, to: &wire)
        appendLE(headerBytes, to: &wire)
        appendLE(UInt32(headerBytes + recordBytes), to: &wire)
        appendLE(UInt32(1), to: &wire)
        appendLE(UInt32(0), to: &wire)

        appendLE(version, to: &wire)
        appendLE(recordBytes, to: &wire)
        appendLE(UInt64(73), to: &wire)
        appendLE(UInt32(0x28), to: &wire)
        appendLE(UInt32(19), to: &wire)
        appendLE(UInt64(0x0003_DB00_01DD_DD11), to: &wire)
        appendLE(UInt16(0xFFC2), to: &wire)
        wire.append(1) // initialized
        wire.append(1) // subunit count
        appendLE(UInt32(0x0003DB), to: &wire) // root vendor
        appendLE(UInt32(0x001000), to: &wire) // root model
        appendLE(UInt32(0x00A02D), to: &wire) // selected-unit vendor
        appendLE(UInt32(0x01DDDD), to: &wire) // selected-unit model
        appendLE(UInt32(0x00A02D), to: &wire) // specifier
        appendLE(UInt32(0x000101), to: &wire) // version
        wire.append(contentsOf: [2, 2, 0, 0])

        appendLE(version, to: &wire)
        appendLE(UInt16(8), to: &wire)
        wire.append(contentsOf: [0x0C, 0x00, 0x02, 0x02])
        return wire
    }

    @Test func parsesStrongUnitIdentityAndIndependentEvidence() throws {
        let units = try #require(ASFWDriverConnector.parseAVCUnitsWire(fixture()))
        let unit = try #require(units.first)

        #expect(units.count == 1)
        #expect(unit.id == UnitInstanceID(device: DeviceInstanceID(73), unitDirectoryOffset: 0x28))
        #expect(unit.observedGuid == 0x0003_DB00_01DD_DD11)
        #expect(unit.generation == 19)
        #expect(unit.nodeID == 0xFFC2)
        #expect(unit.rootVendorID == 0x0003DB)
        #expect(unit.rootModelID == 0x001000)
        #expect(unit.unitVendorID == 0x00A02D)
        #expect(unit.unitModelID == 0x01DDDD)
        #expect(unit.specifierID == 0x00A02D)
        #expect(unit.unitVersion == 0x000101)
        #expect(unit.subunits.count == 1)
    }

    @Test func rejectsUnknownVersionAndTruncation() {
        #expect(ASFWDriverConnector.parseAVCUnitsWire(fixture(version: 3)) == nil)
        #expect(ASFWDriverConnector.parseAVCUnitsWire(fixture().dropLast()) == nil)
    }
}
