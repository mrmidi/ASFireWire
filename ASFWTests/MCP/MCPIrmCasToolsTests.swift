import Testing
@testable import ASFW

struct MCPIrmCasToolsTests {
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

    private func toolNames(_ cfg: ASFWMCPRuntimeConfiguration) async -> Set<String> {
        let core = ASFWMCPCore(configuration: cfg, driver: MockASFWDriverControl(nodes: []))
        return await Set(core.listTools().map(\.name))
    }

    private func decide(_ cfg: ASFWMCPRuntimeConfiguration, _ req: ASFWMCPPolicyRequest) -> ASFWMCPPolicyDecision {
        ASFWMCPWritePolicyEngine(configuration: cfg).evaluate(req)
    }

    @Test func irmCatalogExposesReadsAndMutations() {
        let names = Set(ASFWMCPToolCatalog.irmCasTools.map(\.name))
        #expect(names.isSuperset(of: [
            "asfw_irm_get_state", "asfw_irm_get_bandwidth", "asfw_irm_get_channels", "asfw_irm_list_allocations",
            "asfw_cas_quadlet", "asfw_irm_allocate_channel", "asfw_irm_free_channel",
            "asfw_irm_allocate_bandwidth", "asfw_irm_free_bandwidth"
        ]))
    }

    @Test func irmReadsListWithoutDevicesAndMutationsAreHidden() async {
        let names = await toolNames(config(.readOnlyDeveloper))
        // IRM is bus-global: reads list even with no nodes.
        #expect(names.isSuperset(of: ["asfw_irm_get_state", "asfw_irm_get_channels", "asfw_irm_get_bandwidth"]))
        #expect(names.contains("asfw_cas_quadlet") == false)
        #expect(names.contains("asfw_irm_allocate_channel") == false)
    }

    @Test func irmMutationsListWhenGateOpen() async {
        let names = await toolNames(gateOpen)
        #expect(names.isSuperset(of: ["asfw_cas_quadlet", "asfw_irm_allocate_channel", "asfw_irm_free_bandwidth"]))
    }

    @Test func channelRequestValidatesRange() {
        #expect(ASFWMCPIrmChannelRequest(channel: 63, generation: 17, allocate: true).validationError == nil)
        #expect(ASFWMCPIrmChannelRequest(channel: 64, generation: 17, allocate: true).validationError == .malformedRequest)
    }

    @Test func bandwidthRequestValidatesCeiling() {
        #expect(ASFWMCPIrmBandwidthRequest(allocationUnits: 0x1333, generation: 17, allocate: true).validationError == nil)
        #expect(ASFWMCPIrmBandwidthRequest(allocationUnits: 0x2000, generation: 17, allocate: true).validationError == .malformedRequest)
    }

    @Test func channelAllocationIsPolicyGated() {
        let req = ASFWMCPIrmChannelRequest(channel: 10, generation: 17, allocate: true)
        #expect(decide(config(.readOnlyDeveloper), req.policyRequest(currentGeneration: 17)).decision == .requiresDeveloperMode)
        #expect(decide(gateOpen, req.policyRequest(currentGeneration: 17)).decision == .allowed)
        #expect(decide(gateOpen, req.policyRequest(currentGeneration: 18)).decision == .staleGeneration)
    }

    @Test func casQuadletReusesTransactionPolicyBridge() {
        let cas = ASFWMCPCompareSwapRequest(
            address: ASFWMCPAddress(nodeId: 2, generation: 17, addressHigh: 0xFFFF, addressLow: 0xF0000220),
            expected: 0,
            swap: 1
        )
        let req = cas.policyRequest(currentGeneration: 17)
        #expect(req.operationType == .compareSwap)
        #expect(decide(gateOpen, req).decision == .allowed)
        #expect(decide(config(.readOnlyDeveloper), req).decision == .requiresDeveloperMode)
    }
}
