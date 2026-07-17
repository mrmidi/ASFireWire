import Testing
@testable import ASFW

struct MCPCmpToolsTests {
    private func config(
        _ mode: ASFWMCPRuntimeMode,
        writePolicyAvailable: Bool = false,
        swiftTestGatePassed: Bool = false
    ) -> ASFWMCPRuntimeConfiguration {
        ASFWMCPRuntimeConfiguration(
            mode: mode,
            writePolicyAvailable: writePolicyAvailable,
            swiftTestGatePassed: swiftTestGatePassed,
            rawDeveloperTierEnabled: false
        )
    }

    private var gateOpen: ASFWMCPRuntimeConfiguration {
        config(.developerWriteEnabled, writePolicyAvailable: true, swiftTestGatePassed: true)
    }

    private func pcrAddress() -> ASFWMCPAddress {
        ASFWMCPAddress(nodeId: 0, generation: 17, addressHigh: 0xFFFF, addressLow: 0xF0000904)
    }

    private func toolNames(_ cfg: ASFWMCPRuntimeConfiguration, nodes: [ASFWMCPNodeSummary]) async -> Set<String> {
        let core = ASFWMCPCore(configuration: cfg, driver: MockASFWDriverControl(nodes: nodes))
        return await Set(core.listTools().map(\.name))
    }

    private func decide(_ cfg: ASFWMCPRuntimeConfiguration, _ req: ASFWMCPPolicyRequest) -> ASFWMCPPolicyDecision {
        ASFWMCPWritePolicyEngine(configuration: cfg).evaluate(req)
    }

    @Test func cmpToolsHiddenWithoutCmpNode() async {
        let names = await toolNames(config(.readOnlyDeveloper), nodes: [MockASFWDriverControl.sbp2Node])
        #expect(names.contains("asfw_cmp_list_plugs") == false)
        #expect(names.contains("asfw_cmp_read_pcr") == false)
    }

    @Test func cmpReadsListForCmpNodeAndWritesHidden() async {
        let names = await toolNames(config(.readOnlyDeveloper), nodes: MockASFWDriverControl.defaultNodes)
        #expect(names.isSuperset(of: ["asfw_cmp_list_plugs", "asfw_cmp_read_pcr"]))
        #expect(names.contains("asfw_cmp_write_pcr") == false)
        #expect(names.contains("asfw_cmp_establish_connection") == false)
        #expect(names.contains("asfw_cmp_break_connection") == false)
    }

    @Test func cmpWritesListWhenGateOpen() async {
        let names = await toolNames(gateOpen, nodes: MockASFWDriverControl.defaultNodes)
        #expect(names.isSuperset(of: ["asfw_cmp_write_pcr", "asfw_cmp_establish_connection", "asfw_cmp_break_connection"]))
    }

    @Test func malformedPlugIdIsRejected() {
        #expect(ASFWMCPCmpPcrWriteRequest(address: pcrAddress(), plug: 30, expected: 0, swap: 1).validationError == nil)
        #expect(ASFWMCPCmpPcrWriteRequest(address: pcrAddress(), plug: 31, expected: 0, swap: 1).validationError == .malformedRequest)
        #expect(ASFWMCPCmpConnectionRequest(address: pcrAddress(), plug: 99, establish: true).validationError == .malformedRequest)
    }

    @Test func pcrWriteIsPolicyDeniedInReadOnly() {
        let request = ASFWMCPCmpPcrWriteRequest(address: pcrAddress(), plug: 0, expected: 0, swap: 0x4000_0000)
        let decision = decide(config(.readOnlyDeveloper), request.policyRequest(currentGeneration: 17))
        #expect(decision.decision == .requiresDeveloperMode)
        #expect(decision.reachesDriverWritePath == false)
    }

    @Test func pcrWriteAllowedWhenGateOpenAndUsesCompareSwap() {
        let request = ASFWMCPCmpPcrWriteRequest(address: pcrAddress(), plug: 0, expected: 0, swap: 0x4000_0000)
        let policyRequest = request.policyRequest(currentGeneration: 17)
        #expect(policyRequest.operationType == .compareSwap)
        #expect(decide(gateOpen, policyRequest).decision == .allowed)
    }

    @Test func connectionWriteRefusedWhenProtocolUnsupported() {
        let request = ASFWMCPCmpConnectionRequest(address: pcrAddress(), plug: 0, establish: true)
        let decision = decide(gateOpen, request.policyRequest(currentGeneration: 17, protocolSupported: false))
        #expect(decision.decision == .unsupportedProtocol)
    }

    @Test func livePcrReadDecodesOutputCmpFields() async {
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: MockASFWDriverControl())
        let result = await core.callTool(name: "asfw_cmp_read_pcr", arguments: .object([
            "nodeId": .int(0),
            "generation": .int(17),
            "direction": .string("output"),
            "plug": .int(0),
        ]))

        guard case .object(let data) = result.data,
              case .object(let pcr)? = data["pcr"] else {
            Issue.record("Expected decoded CMP PCR data.")
            return
        }
        #expect(result.ok)
        #expect(pcr["streamDirection"] == .string("deviceToHost"))
        #expect(pcr["online"] == .bool(true))
        #expect(pcr["pointToPointConnections"] == .int(1))
        #expect(pcr["channel"] == .int(5))
        #expect(pcr["dataRate"] == .string("S400"))
        #expect(pcr["overheadId"] == .int(3))
    }

    @Test func cmpPlugInventoryReadsMprsAndBothDirections() async {
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: MockASFWDriverControl())
        let result = await core.callTool(name: "asfw_cmp_list_plugs", arguments: .object([
            "nodeId": .int(0),
            "generation": .int(17),
        ]))

        guard case .object(let data) = result.data,
              case .array(let plugs)? = data["plugs"] else {
            Issue.record("Expected a CMP plug inventory.")
            return
        }
        #expect(result.ok)
        #expect(data["outputPlugCount"] == .int(2))
        #expect(data["inputPlugCount"] == .int(2))
        #expect(plugs.count == 4)
    }
}
