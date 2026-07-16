import Testing
@testable import ASFW

struct MCPToolDispatchTests {
    private var gateOpen: ASFWMCPRuntimeConfiguration {
        ASFWMCPRuntimeConfiguration(
            mode: .developerWriteEnabled,
            writePolicyAvailable: true,
            swiftTestGatePassed: true,
            rawDeveloperTierEnabled: false
        )
    }

    private func addressArgs(
        nodeId: UInt32 = 1,
        generation: UInt32 = 17,
        addressHigh: UInt32 = 0xFFFF,
        addressLow: UInt32 = 0xF0000400
    ) -> [String: ASFWMCPValue] {
        [
            "nodeId": .int(Int(nodeId)),
            "generation": .int(Int(generation)),
            "addressHigh": .int(Int(addressHigh)),
            "addressLow": .uint64(UInt64(addressLow))
        ]
    }

    private func object(_ result: ASFWMCPToolCallResult) throws -> [String: ASFWMCPValue] {
        guard case .object(let object) = result.data else {
            Issue.record("Expected tool result data to be an object.")
            return [:]
        }
        return object
    }

    private func policyObject(_ result: ASFWMCPToolCallResult) throws -> [String: ASFWMCPValue] {
        let data = try object(result)
        guard case .object(let policy)? = data["policy"] else {
            Issue.record("Expected transaction result to include a policy object.")
            return [:]
        }
        return policy
    }

    @Test func readQuadletDispatchesThroughDriver() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver))

        let result = await transport.callTool("asfw_read_quadlet", arguments: .object(addressArgs()))

        let data = try object(result)
        #expect(result.ok)
        #expect(data["kind"] == .string("readQuadlet"))
        #expect(data["status"] == .string("ok"))
        #expect(data["payload"] == .array([.int(49), .int(51), .int(57), .int(52)]))
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func malformedReadBlockReturnsSchemaError() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver))
        var args = addressArgs()
        args["length"] = .int(6)

        let result = await transport.callTool("asfw_read_block", arguments: .object(args))

        let data = try object(result)
        #expect(result.ok == false)
        #expect(result.errors.first?.code == .malformedRequest)
        #expect(data["kind"] == .string("readBlock"))
        #expect(data["status"] == .string("malformed"))
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func readOnlyModeWriteIsPolicyRefusedBeforeDriverAccess() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver))
        var args = addressArgs(addressLow: 0xF0000800)
        args["value"] = .uint64(0x1234_5678)

        let result = await transport.callTool("asfw_write_quadlet", arguments: .object(args))

        let data = try object(result)
        let policy = try policyObject(result)
        #expect(result.ok == false)
        #expect(data["status"] == .string("denied"))
        #expect(policy["decision"] == .string("requiresDeveloperMode"))
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func developerWriteModeExecutesAllowedWriteThroughDriver() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: gateOpen, driver: driver))
        var args = addressArgs(addressLow: 0xF0000800)
        args["value"] = .uint64(0x1234_5678)
        args["verifyReadback"] = .bool(true)

        let result = await transport.callTool("asfw_write_quadlet", arguments: .object(args))

        let data = try object(result)
        #expect(result.ok)
        #expect(data["status"] == .string("ok"))
        #expect(data["payload"] == .array([.int(0x12), .int(0x34), .int(0x56), .int(0x78)]))
        #expect(await driver.unexpectedWriteAttemptCount() == 1)
    }

    @Test func developerFcpCommandReturnsRouteBoundReceipt() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: gateOpen, driver: driver))
        var args = addressArgs(nodeId: 0, addressLow: 0xF0000B00)
        args["targetGuid"] = .uint64(0x0011223344556677)
        args["intent"] = .string("control")
        args["payload"] = .array([.int(0x00), .int(0xFF), .int(0x19)])

        let result = await transport.callTool("asfw_fcp_send_command_dev", arguments: .object(args))

        let data = try object(result)
        #expect(result.ok)
        #expect(data["kind"] == .string("fcpCommand"))
        #expect(data["status"] == .string("ok"))
        #expect(data["targetGuid"] == .string("0x0011223344556677"))
        #expect(data["expectedNodeId"] == .int(0))
        #expect(data["expectedGeneration"] == .int(17))
        #expect(data["observedNodeId"] == .int(0))
        #expect(await driver.unexpectedWriteAttemptCount() == 1)
    }

    @Test func readOnlyFcpStatusCommandRoutesThroughDriver() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver))
        var args = addressArgs(nodeId: 0, addressLow: 0xF0000B00)
        args["targetGuid"] = .uint64(0x0011223344556677)
        args["intent"] = .string("status")
        // UNIT_INFO status, padded to one quadlet as ASFW's AVCUnit emits it.
        args["payload"] = .array([.int(0x01), .int(0xFF), .int(0x30), .int(0x00)])

        let result = await transport.callTool("asfw_fcp_send_command", arguments: .object(args))

        let data = try object(result)
        #expect(result.ok)
        #expect(data["status"] == .string("ok"))
        #expect(data["expectedNodeId"] == .int(0))
        #expect(data["expectedGeneration"] == .int(17))
        #expect(data["observedNodeId"] == .int(0))
        #expect(await driver.unexpectedWriteAttemptCount() == 1)
    }

    @Test func avcInventoryExposesDecodedDriverStateWithoutFcpTraffic() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver))

        let result = await transport.callTool("asfw_avc_list_units")
        let data = try object(result)

        #expect(result.ok)
        #expect(data["kind"] == .string("avcUnitInventory"))
        #expect(data["units"] == .array([.object([
            "guid": .string("0x0011223344556677"),
            "nodeId": .int(0),
            "vendorId": .string("0x0003DB"),
            "modelId": .string("0x01DDDD"),
            "plugs": .object([
                "isoInput": .int(1), "isoOutput": .int(1),
                "externalInput": .int(1), "externalOutput": .int(1),
            ]),
            "subunits": .array([.object([
                "type": .int(12), "id": .int(0),
                "sourcePlugCount": .int(1), "destinationPlugCount": .int(1),
            ])]),
        ])]))
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func avcCapabilitiesRequireDiscoveredGuidAndSubunit() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver))
        let args: ASFWMCPValue = .object([
            "targetGuid": .uint64(0x0011223344556677),
            "subunitType": .int(0x0C),
            "subunitId": .int(0),
        ])

        let result = await transport.callTool("asfw_avc_get_subunit_capabilities", arguments: args)
        let data = try object(result)

        #expect(result.ok)
        #expect(data["kind"] == .string("avcSubunitCapabilities"))
        #expect(data["targetGuid"] == .string("0x0011223344556677"))
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func avcCapabilitiesRefuseUnavailableDiscoveryTargetWithoutFcpTraffic() async {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver))
        let args: ASFWMCPValue = .object([
            "targetGuid": .uint64(0xDEAD_BEEF_0000_0001),
            "subunitType": .int(0x0C),
            "subunitId": .int(0),
        ])

        let result = await transport.callTool("asfw_avc_get_subunit_capabilities", arguments: args)

        #expect(result.ok == false)
        #expect(result.errors.first?.code == .capabilityUnavailable)
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func readOnlyFcpRejectsControlFrameClaimedAsStatus() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver))
        var args = addressArgs(nodeId: 0, addressLow: 0xF0000B00)
        args["targetGuid"] = .uint64(0x0011223344556677)
        args["intent"] = .string("status")
        args["payload"] = .array([.int(0x00), .int(0xFF), .int(0x30), .int(0x00)])

        let result = await transport.callTool("asfw_fcp_send_command", arguments: .object(args))

        #expect(result.ok == false)
        #expect(result.errors.first?.code == .malformedRequest)
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func developerFcpCommandRefusesStaleGenerationBeforeDriverAccess() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: gateOpen, driver: driver))
        var args = addressArgs(nodeId: 0, generation: 16, addressLow: 0xF0000B00)
        args["targetGuid"] = .uint64(0x0011223344556677)
        args["intent"] = .string("control")
        args["payload"] = .array([.int(0x00), .int(0xFF), .int(0x19)])

        let result = await transport.callTool("asfw_fcp_send_command_dev", arguments: .object(args))

        let data = try object(result)
        #expect(result.ok == false)
        #expect(data["status"] == .string("denied"))
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func mockModeDryRunDoesNotReachDriverWritePath() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: .mock, driver: driver))
        var args = addressArgs(addressLow: 0xF0000800)
        args["value"] = .int(1)

        let result = await transport.callTool("asfw_write_quadlet", arguments: .object(args))

        let data = try object(result)
        let policy = try policyObject(result)
        #expect(result.ok == false)
        #expect(data["status"] == .string("dryRun"))
        #expect(policy["decision"] == .string("dryRunOnly"))
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func unsupportedProtocolWriteIsRefusedBeforeDriverAccess() async throws {
        let driver = MockASFWDriverControl(nodes: [MockASFWDriverControl.sbp2Node])
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: gateOpen, driver: driver))
        var args = addressArgs(addressLow: 0xF0000800)
        args["value"] = .int(1)

        let result = await transport.callTool("asfw_dice_write_register", arguments: .object(args))

        let policy = try policyObject(result)
        #expect(result.ok == false)
        #expect(policy["decision"] == .string("unsupportedProtocol"))
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func everyCatalogToolHasADispatchOutcome() async {
        let driver = MockASFWDriverControl(nodes: MockASFWDriverControl.defaultNodes + [MockASFWDriverControl.sbp2Node])
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: gateOpen, driver: driver))

        for tool in ASFWMCPToolCatalog.all {
            let result = await transport.callTool(tool.name, arguments: .object([:]))
            #expect(result.toolName == tool.name)
            #expect(result.errors.first?.reason.contains("has no dispatch arm") != true)
        }
    }
}
