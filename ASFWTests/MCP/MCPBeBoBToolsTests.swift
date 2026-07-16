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

    @Test func decodesLinuxStyleGenericUnitPlugInfoResponse() {
        let information = ASFWMCPBeBoBUnitPlugInformation.decode(
            [0x0c, 0xff, 0x02, 0x00, 0x01, 0x01, 0x02, 0x03]
        )
        #expect(information == ASFWMCPBeBoBUnitPlugInformation(
            isochronousInputCount: 1,
            isochronousOutputCount: 1,
            externalInputCount: 2,
            externalOutputCount: 3
        ))
        #expect(ASFWMCPBeBoBUnitPlugInformation.decode([0x0a, 0xff, 0x02, 0x00, 0, 0, 0, 0]) == nil)
    }

    @Test func unitPlugInfoMcpToolUsesTheFixedStatusCommand() async throws {
        let driver = MockASFWDriverControl(nodes: [MockASFWDriverControl.bebobNode])
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)

        let result = await core.callTool(name: "asfw_bebob_get_unit_plug_info", arguments: .object([
            "targetGuid": .uint64(0x000A_AC03_00B1_D1F7),
            "nodeId": .int(3),
            "generation": .int(17),
        ]))

        guard case .object(let data) = result.data,
              case .object(let information)? = data["information"] else {
            Issue.record("Expected structured BeBoB PLUG_INFO data.")
            return
        }
        #expect(result.ok)
        #expect(data["kind"] == .string("bebobUnitPlugInfo"))
        #expect(data["recognized"] == .bool(true))
        #expect(information["isochronousInput"] == .int(1))
        #expect(information["isochronousOutput"] == .int(1))
        #expect(information["externalInput"] == .int(0))
        #expect(information["externalOutput"] == .int(0))
        #expect(await driver.unexpectedWriteAttemptCount() == 1)
    }
}
