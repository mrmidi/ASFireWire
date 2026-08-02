import Testing
@testable import ASFW

struct MCPAvcFcpToolsTests {
    private let duetGUID: UInt64 = 0x0011223344556677
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

    private func toolNames(_ cfg: ASFWMCPRuntimeConfiguration, nodes: [ASFWMCPNodeSummary]) async -> Set<String> {
        let core = ASFWMCPCore(configuration: cfg, driver: MockASFWDriverControl(nodes: nodes))
        return await Set(core.listTools().map(\.name))
    }

    private func fcpAddress() -> ASFWMCPAddress {
        ASFWMCPAddress(nodeId: 0, generation: 17, addressHigh: 0xFFFF, addressLow: 0xF0000B00)
    }

    private func decide(_ cfg: ASFWMCPRuntimeConfiguration, _ req: ASFWMCPPolicyRequest) -> ASFWMCPPolicyDecision {
        ASFWMCPWritePolicyEngine(configuration: cfg).evaluate(req)
    }

    @Test func avcToolsHiddenWithoutAvcNode() async {
        let names = await toolNames(config(.readOnlyDeveloper), nodes: [])
        #expect(names.contains("asfw_avc_list_units") == false)
        #expect(names.contains("asfw_fcp_send_command") == false)
    }

    @Test func avcReadsListForAvcNodeAndDevCommandIsHidden() async {
        let names = await toolNames(config(.readOnlyDeveloper), nodes: MockASFWDriverControl.defaultNodes)
        #expect(names.isSuperset(of: [
            "asfw_avc_list_units", "asfw_avc_get_subunit_capabilities",
            "asfw_avc_get_subunit_descriptor", "asfw_fcp_send_command", "asfw_fcp_get_recent_responses"
        ]))
        #expect(names.contains("asfw_fcp_send_command_dev") == false)
    }

    @Test func devFcpCommandListsWhenGateOpen() async {
        let names = await toolNames(gateOpen, nodes: MockASFWDriverControl.defaultNodes)
        #expect(names.contains("asfw_fcp_send_command_dev"))
    }

    @Test func commandIntentMutationClassification() {
        #expect(ASFWMCPAvcCommandIntent.inquiry.isMutating == false)
        #expect(ASFWMCPAvcCommandIntent.status.isMutating == false)
        #expect(ASFWMCPAvcCommandIntent.control.isMutating)
        #expect(ASFWMCPAvcCommandIntent.notify.isMutating)
        #expect(ASFWMCPAvcCommandIntent.vendorDependent.isMutating)
    }

    @Test func payloadValidationCatchesEmptyAndOversize() {
        #expect(ASFWMCPFcpCommandRequest(targetGUID: duetGUID, address: fcpAddress(), intent: .status, payload: [0x01, 0x02]).validationError == nil)
        #expect(ASFWMCPFcpCommandRequest(targetGUID: duetGUID, address: fcpAddress(), intent: .status, payload: []).validationError == .malformedRequest)
        let oversize = [UInt8](repeating: 0, count: 513)
        #expect(ASFWMCPFcpCommandRequest(targetGUID: duetGUID, address: fcpAddress(), intent: .control, payload: oversize).validationError == .payloadTooLarge)
    }

    @Test func inquiryCommandsAreNotPolicyGated() {
        let request = ASFWMCPFcpCommandRequest(targetGUID: duetGUID, address: fcpAddress(), intent: .inquiry, payload: [0x01])
        #expect(request.policyRequest(currentGeneration: 17) == nil)
    }

    @Test func readOnlyIntentMustMatchAvcCType() {
        // The MIT-licensed ALSA ta1394 general protocol tests independently
        // exercise STATUS UnitInfo framing.  These are fresh policy tests that
        // ensure the declared MCP intent cannot disguise a CONTROL frame.
        let status = ASFWMCPFcpCommandRequest(
            targetGUID: duetGUID,
            address: fcpAddress(),
            intent: .status,
            payload: [0x01, 0xFF, 0x30, 0x00]
        )
        let inquiry = ASFWMCPFcpCommandRequest(
            targetGUID: duetGUID,
            address: fcpAddress(),
            intent: .inquiry,
            payload: [0x02, 0xFF, 0x30, 0x00]
        )
        let disguisedControl = ASFWMCPFcpCommandRequest(
            targetGUID: duetGUID,
            address: fcpAddress(),
            intent: .status,
            payload: [0x00, 0xFF, 0x30, 0x00]
        )

        #expect(status.hasMatchingReadOnlyCType)
        #expect(inquiry.hasMatchingReadOnlyCType)
        #expect(disguisedControl.hasMatchingReadOnlyCType == false)
    }

    @Test func controlCommandsArePolicyGated() throws {
        let request = ASFWMCPFcpCommandRequest(targetGUID: duetGUID, address: fcpAddress(), intent: .control, payload: [0x00, 0x11, 0x22, 0x33])
        let gated = try #require(request.policyRequest(currentGeneration: 17))
        #expect(decide(config(.readOnlyDeveloper), gated).decision == .requiresDeveloperMode)
        #expect(decide(gateOpen, gated).decision == .allowed)
    }

    @Test func controlCommandRefusedWhenProtocolUnsupported() {
        let request = ASFWMCPFcpCommandRequest(targetGUID: duetGUID, address: fcpAddress(), intent: .control, payload: [0x00])
        let gated = request.policyRequest(currentGeneration: 17, protocolSupported: false)!
        #expect(decide(gateOpen, gated).decision == .unsupportedProtocol)
    }
}
