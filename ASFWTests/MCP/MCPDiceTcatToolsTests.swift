import Testing
@testable import ASFW

struct MCPDiceTcatToolsTests {
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

    private func diceAddress(low: UInt32 = 0xF0001000) -> ASFWMCPAddress {
        ASFWMCPAddress(nodeId: 1, generation: 17, addressHigh: 0xFFFF, addressLow: low)
    }

    private func toolNames(_ cfg: ASFWMCPRuntimeConfiguration, nodes: [ASFWMCPNodeSummary]) async -> Set<String> {
        let core = ASFWMCPCore(configuration: cfg, driver: MockASFWDriverControl(nodes: nodes))
        return await Set(core.listTools().map(\.name))
    }

    private func decide(_ cfg: ASFWMCPRuntimeConfiguration, _ req: ASFWMCPPolicyRequest) -> ASFWMCPPolicyDecision {
        ASFWMCPWritePolicyEngine(configuration: cfg).evaluate(req)
    }

    @Test func diceToolsHiddenWithoutCapableNode() async {
        let names = await toolNames(config(.readOnlyDeveloper), nodes: [MockASFWDriverControl.sbp2Node])
        #expect(names.contains("asfw_dice_read_register") == false)
        #expect(names.contains("asfw_tcat_read_application_block") == false)
    }

    @Test func diceReadsListForCapableNodeAndWritesHidden() async {
        let names = await toolNames(config(.readOnlyDeveloper), nodes: MockASFWDriverControl.defaultNodes)
        #expect(names.isSuperset(of: [
            "asfw_dice_read_register", "asfw_dice_read_block",
            "asfw_dice_decode_status", "asfw_tcat_read_application_block"
        ]))
        #expect(names.contains("asfw_dice_write_register") == false)
        #expect(names.contains("asfw_tcat_write_application_block") == false)
    }

    @Test func diceWritesListWhenGateOpen() async {
        let names = await toolNames(gateOpen, nodes: MockASFWDriverControl.defaultNodes)
        #expect(names.isSuperset(of: ["asfw_dice_write_register", "asfw_tcat_write_application_block"]))
    }

    @Test func blockReadsReuseTransactionBounds() {
        #expect(ASFWMCPDiceBlockReadRequest(address: diceAddress(), length: 64).validationError == nil)
        #expect(ASFWMCPDiceBlockReadRequest(address: diceAddress(), length: 7).validationError == .malformedRequest)
        #expect(ASFWMCPTcatApplicationBlockReadRequest(address: diceAddress(), length: 4096).validationError == .payloadTooLarge)
    }

    @Test func registerWriteIsPolicyDeniedInReadOnlyAndAllowedWhenOpen() {
        let request = ASFWMCPDiceRegisterWriteRequest(address: diceAddress(), value: 0x1234_5678)
        #expect(decide(config(.readOnlyDeveloper), request.policyRequest(currentGeneration: 17)).decision == .requiresDeveloperMode)
        #expect(decide(gateOpen, request.policyRequest(currentGeneration: 17)).decision == .allowed)
    }

    @Test func writeRefusedWhenProtocolUnsupported() {
        let request = ASFWMCPDiceRegisterWriteRequest(address: diceAddress(), value: 0x1)
        #expect(decide(gateOpen, request.policyRequest(currentGeneration: 17, protocolSupported: false)).decision == .unsupportedProtocol)
    }

    @Test func writeVerificationReflectsReadback() {
        let request = ASFWMCPDiceRegisterWriteRequest(address: diceAddress(), value: 0xABCD, verifyReadback: true)
        #expect(request.verify(readback: 0xABCD).verified)
        #expect(request.verify(readback: 0x0000).verified == false)
    }
}
