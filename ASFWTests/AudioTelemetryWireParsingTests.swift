import Foundation
import Testing
@testable import ASFW

struct AudioTelemetryWireParsingTests {
    private func setLE<T: FixedWidthInteger>(_ value: T, at offset: Int, in data: inout Data) {
        var raw = value.littleEndian
        withUnsafeBytes(of: &raw) { bytes in
            data.replaceSubrange(offset..<(offset + bytes.count), with: bytes)
        }
    }

    private func fixture(version: UInt16 = 5) -> Data {
        let headerBytes = 16
        let endpointBytes = 688
        var wire = Data(repeating: 0, count: headerBytes + 8 * endpointBytes)
        setLE(version, at: 0, in: &wire)
        setLE(UInt16(headerBytes), at: 2, in: &wire)
        setLE(UInt32(wire.count), at: 4, in: &wire)
        setLE(UInt32(2), at: 8, in: &wire)
        setLE(UInt32(endpointBytes), at: 12, in: &wire)

        let first = headerBytes
        setLE(UInt16(5), at: first, in: &wire)
        setLE(UInt16(endpointBytes), at: first + 2, in: &wire)
        setLE(UInt64(9), at: first + 8, in: &wire)
        setLE(UInt64(72), at: first + 16, in: &wire)
        setLE(UInt64(0x0003_DB00_01DD_DD11), at: first + 24, in: &wire)
        setLE(UInt64(4), at: first + 32, in: &wire)
        setLE(UInt32(48_000), at: first + 212, in: &wire)
        setLE(UInt32(10), at: first + 216, in: &wire)
        setLE(UInt32(8), at: first + 220, in: &wire)

        let second = headerBytes + endpointBytes
        setLE(UInt16(5), at: second, in: &wire)
        setLE(UInt16(endpointBytes), at: second + 2, in: &wire)
        setLE(UInt64(3), at: second + 8, in: &wire)
        setLE(UInt64(91), at: second + 16, in: &wire)
        setLE(UInt64(0x0013_0E01_0000_0001), at: second + 24, in: &wire)
        setLE(UInt64(7), at: second + 32, in: &wire)
        setLE(UInt32(96_000), at: second + 212, in: &wire)
        return wire
    }

    @Test func decodesStrongIdentityAndSortsEndpoints() throws {
        let snapshot = try #require(AudioTelemetryWireDecoder.decode(fixture()))
        #expect(snapshot.endpoints.map(\.endpointId) == [AudioEndpointID(3), AudioEndpointID(9)])
        #expect(snapshot.endpoints[0].deviceInstanceId == DeviceInstanceID(91))
        #expect(snapshot.endpoints[0].observedGuid == 0x0013_0E01_0000_0001)
        #expect(snapshot.endpoints[0].sampleRateHz == 96_000)
        #expect(snapshot.endpoints[1].deviceInstanceId == DeviceInstanceID(72))
        #expect(snapshot.endpoints[1].outputChannels == 10)
        #expect(snapshot.endpoints[1].inputChannels == 8)
    }

    @Test func rejectsUnknownVersionTruncationAndInvalidStrongIdentity() {
        #expect(AudioTelemetryWireDecoder.decode(fixture(version: 4)) == nil)
        #expect(AudioTelemetryWireDecoder.decode(Data(fixture().dropLast())) == nil)
        var invalid = fixture()
        setLE(UInt64(0), at: 16 + 16, in: &invalid)
        #expect(AudioTelemetryWireDecoder.decode(invalid) == nil)
    }
}
