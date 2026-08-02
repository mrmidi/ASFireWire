import Testing
@testable import ASFW

struct MCPBeBoBToolsTests {
    private var unrestrictedWrite: ASFWMCPRuntimeConfiguration {
        ASFWMCPRuntimeConfiguration(
            mode: .unrestrictedWrite,
            writePolicyAvailable: false,
            swiftTestGatePassed: false,
            rawDeveloperTierEnabled: false
        )
    }

    private func bytes32(_ value: UInt32) -> [UInt8] {
        [UInt8(truncatingIfNeeded: value), UInt8(truncatingIfNeeded: value >> 8),
         UInt8(truncatingIfNeeded: value >> 16), UInt8(truncatingIfNeeded: value >> 24)]
    }

    @Test func decodesLittleEndianBridgeCoBootRomRecord() {
        var bytes = Array("bridgeCo".utf8)
        bytes += bytes32(1)
        bytes += bytes32(0)
        // Captured from the PHASE 88 BootROM. The bootloader record's GUID
        // field is little-endian and is distinct from Config ROM byte order.
        bytes += [0x03, 0xac, 0x0a, 0x00, 0xf7, 0xd1, 0xb1, 0x00]
        bytes += bytes32(3)
        bytes += bytes32(1)
        bytes += Array("20051215".utf8) + Array("163713".utf8) + [0, 0]
        bytes += bytes32(3)
        bytes += bytes32(0x0112_0d1f)
        bytes += bytes32(0x2008_0000)
        bytes += bytes32(0x0018_0000)
        bytes += Array("20040719".utf8) + Array("134046".utf8) + [0, 0]
        // The live read returns the 104-byte record plus one padded quadlet.
        bytes += Array(repeating: 0, count: 28)

        let record = ASFWMCPBeBoBBootRomInformation.decode(bytes)
        #expect(record?.guid == 0x00B1_D1F7_000A_AC03)
        #expect(record?.hardwareModelId == 3)
        #expect(record?.softwareTimestamp == "20051215T163713Z")
        #expect(record?.bootloaderTimestamp == "20040719T134046Z")
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

    @Test func clockTopologyMcpToolFollowsMusicSubunitSyncDiscovery() async throws {
        let driver = MockASFWDriverControl(nodes: [MockASFWDriverControl.bebobNode])
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)

        let result = await core.callTool(name: "asfw_bebob_get_clock_topology", arguments: .object([
            "targetGuid": .uint64(0x000A_AC03_00B1_D1F7),
            "nodeId": .int(3),
            "generation": .int(17),
        ]))

        guard case .object(let data) = result.data,
              case .object(let clockSource)? = data["clockSource"] else {
            Issue.record("Expected structured BeBoB clock topology data.")
            return
        }
        #expect(result.ok)
        #expect(data["kind"] == .string("bebobClockTopology"))
        #expect(data["recognized"] == .bool(true))
        #expect(data["syncInputPlug"] == .int(0))
        #expect(clockSource["classification"] == .string("internal"))
        #expect(await core.listTools().contains { $0.name == "asfw_bebob_get_clock_topology" })
        #expect(await driver.unexpectedWriteAttemptCount() == 3)
    }

    @Test func phase88LifecycleToolsUseTheDriverOwnedDuplexChoreography() async {
        let driver = MockASFWDriverControl(nodes: [MockASFWDriverControl.bebobNode])
        let core = ASFWMCPCore(configuration: unrestrictedWrite, driver: driver)
        let arguments: ASFWMCPValue = .object([
            "targetGuid": .uint64(0x000A_AC03_00B1_D1F7),
            "nodeId": .int(3),
            "generation": .int(17),
        ])

        let start = await core.callTool(name: "asfw_phase88_start_48k", arguments: arguments)
        let stop = await core.callTool(name: "asfw_phase88_stop", arguments: arguments)

        #expect(start.ok)
        #expect(stop.ok)
        #expect(await core.listTools().contains { $0.name == "asfw_phase88_start_48k" })
        #expect(await driver.unexpectedWriteAttemptCount() == 2)
        guard case .object(let startData) = start.data,
              case .array(let choreography)? = startData["choreography"] else {
            Issue.record("Expected the published PHASE 88 start choreography.")
            return
        }
        #expect(choreography.last == .string("verify_remote_pcrs"))
    }

    @Test func phase88LifecycleAcceptsCanonicalHexGuid() async {
        let driver = MockASFWDriverControl(nodes: [MockASFWDriverControl.bebobNode])
        let core = ASFWMCPCore(configuration: unrestrictedWrite, driver: driver)
        let result = await core.callTool(name: "asfw_phase88_start_48k", arguments: .object([
            "targetGuid": .string("0x000AAC0300B1D1F7"),
            "nodeId": .int(3),
            "generation": .int(17),
        ]))

        #expect(result.ok)
        #expect(await driver.unexpectedWriteAttemptCount() == 1)
    }

    @Test func phase88LifecycleRejectsMalformedHexGuid() async {
        let driver = MockASFWDriverControl(nodes: [MockASFWDriverControl.bebobNode])
        let core = ASFWMCPCore(configuration: unrestrictedWrite, driver: driver)
        let result = await core.callTool(name: "asfw_phase88_start_48k", arguments: .object([
            "targetGuid": .string("0xnot-a-guid"),
            "nodeId": .int(3),
            "generation": .int(17),
        ]))

        #expect(!result.ok)
        #expect(result.errors.first?.code == .malformedRequest)
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }
}
