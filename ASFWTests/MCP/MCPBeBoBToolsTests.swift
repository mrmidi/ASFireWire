import Testing
@testable import ASFW

struct MCPBeBoBToolsTests {
    private func bytes32(_ value: UInt32) -> [UInt8] {
        [UInt8(truncatingIfNeeded: value), UInt8(truncatingIfNeeded: value >> 8),
         UInt8(truncatingIfNeeded: value >> 16), UInt8(truncatingIfNeeded: value >> 24)]
    }

    @Test func decodesLittleEndianBridgeCoBootRomRecord() {
        var bytes = Array("bridgeCo".utf8)
        bytes += bytes32(0x0001_0000)
        bytes += bytes32(0x0203_0004)
        bytes += [0xf7, 0xd1, 0xb1, 0x00, 0x03, 0xac, 0x0a, 0x00]
        bytes += bytes32(3)
        bytes += bytes32(0x0100_0001)
        bytes += Array("20260716".utf8) + Array("10153000".utf8)
        bytes += bytes32(0x1234_5678)
        bytes += bytes32(0x0102_0003)
        bytes += bytes32(0x1000_0000)
        bytes += bytes32(0x0080_0000)
        bytes += Array("20250101".utf8) + Array("00000000".utf8)
        bytes += Array(repeating: 0, count: 24)

        let record = ASFWMCPBeBoBBootRomInformation.decode(bytes)
        #expect(record?.guid == 0x000A_AC03_00B1_D1F7)
        #expect(record?.hardwareModelId == 3)
        #expect(record?.softwareTimestamp == "20260716T10153000Z")
        #expect(record?.debugger == nil)
    }

    @Test func refusesNonBridgeCoMagic() {
        #expect(ASFWMCPBeBoBBootRomInformation.decode(Array(repeating: 0, count: 80)) == nil)
    }
}
