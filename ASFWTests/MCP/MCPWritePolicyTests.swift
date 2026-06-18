import Testing
@testable import ASFW

struct MCPWritePolicyTests {
    private func config(
        _ mode: ASFWMCPRuntimeMode,
        writePolicyAvailable: Bool = false,
        swiftTestGatePassed: Bool = false,
        rawDeveloperTierEnabled: Bool = false
    ) -> ASFWMCPRuntimeConfiguration {
        ASFWMCPRuntimeConfiguration(
            mode: mode,
            writePolicyAvailable: writePolicyAvailable,
            swiftTestGatePassed: swiftTestGatePassed,
            rawDeveloperTierEnabled: rawDeveloperTierEnabled
        )
    }

    private var gateOpen: ASFWMCPRuntimeConfiguration {
        config(.developerWriteEnabled, writePolicyAvailable: true, swiftTestGatePassed: true)
    }

    private func write(
        space: ASFWMCPAddressSpace = .unitsSpace,
        requested: UInt32 = 17,
        current: UInt32 = 17,
        protocolHint: String? = nil,
        protocolSupported: Bool = true,
        dryRun: Bool = false,
        rawTier: Bool = false,
        op: ASFWMCPOperationType = .write
    ) -> ASFWMCPPolicyRequest {
        ASFWMCPPolicyRequest(
            operationType: op,
            addressSpace: space,
            requestedGeneration: requested,
            currentGeneration: current,
            protocolHint: protocolHint,
            protocolSupported: protocolSupported,
            dryRun: dryRun,
            requiresRawDeveloperTier: rawTier
        )
    }

    private func decide(_ cfg: ASFWMCPRuntimeConfiguration, _ req: ASFWMCPPolicyRequest) -> ASFWMCPPolicyDecision {
        ASFWMCPWritePolicyEngine(configuration: cfg).evaluate(req)
    }

    // MARK: Reads

    @Test func readsAreAllowedInEveryMode() {
        for mode in [ASFWMCPRuntimeMode.mock, .readOnlyDeveloper, .developerWriteEnabled] {
            let decision = decide(config(mode), write(op: .read))
            #expect(decision.decision == .allowed)
        }
    }

    // MARK: The allowed path

    @Test func writeAllowedWhenGateOpenAndRequestValid() {
        let decision = decide(gateOpen, write())
        #expect(decision.decision == .allowed)
        #expect(decision.reachesDriverWritePath)
    }

    @Test func compareSwapIsGatedLikeAWrite() {
        #expect(decide(gateOpen, write(op: .compareSwap)).decision == .allowed)
        #expect(decide(config(.readOnlyDeveloper), write(op: .compareSwap)).decision == .requiresDeveloperMode)
    }

    // MARK: Mode gating

    @Test func readOnlyModeRequiresDeveloperMode() {
        let decision = decide(config(.readOnlyDeveloper), write())
        #expect(decision.decision == .requiresDeveloperMode)
        #expect(decision.requiredMode == .developerWriteEnabled)
        #expect(decision.reachesDriverWritePath == false)
    }

    @Test func developerWriteWithClosedGateReportsTestGateMissing() {
        let cfg = config(.developerWriteEnabled, writePolicyAvailable: true, swiftTestGatePassed: false)
        let decision = decide(cfg, write())
        #expect(decision.decision == .requiresDeveloperMode)
        #expect(decision.errorCode == .testGateMissing)
        #expect(decision.reachesDriverWritePath == false)
    }

    // MARK: Dry run

    @Test func mockModeResolvesAuthorizedWriteToDryRun() {
        let decision = decide(config(.mock), write())
        #expect(decision.decision == .dryRunOnly)
        #expect(decision.reachesDriverWritePath == false)
    }

    @Test func explicitDryRunInDeveloperModeIsDryRunOnly() {
        let decision = decide(gateOpen, write(dryRun: true))
        #expect(decision.decision == .dryRunOnly)
        #expect(decision.reachesDriverWritePath == false)
    }

    // MARK: Generation

    @Test func staleGenerationIsRefused() {
        let decision = decide(gateOpen, write(requested: 16, current: 17))
        #expect(decision.decision == .staleGeneration)
        #expect(decision.errorCode == .staleGeneration)
        #expect(decision.reachesDriverWritePath == false)
    }

    @Test func mockModeStillClassifiesStaleGeneration() {
        // Classification applies even in mock mode, so refusals surface there too.
        let decision = decide(config(.mock), write(requested: 16, current: 17))
        #expect(decision.decision == .staleGeneration)
    }

    // MARK: Address space

    @Test func configRomWriteIsUnsupported() {
        let decision = decide(gateOpen, write(space: .configRom))
        #expect(decision.decision == .unsupportedAddressSpace)
        #expect(decision.reachesDriverWritePath == false)
    }

    @Test func unknownAddressSpaceIsUnsupported() {
        #expect(decide(gateOpen, write(space: .unknown)).decision == .unsupportedAddressSpace)
    }

    @Test func writableSpacesAreAccepted() {
        for space in [ASFWMCPAddressSpace.csrCore, .unitsSpace, .physicalMemory, .ohciController] {
            #expect(decide(gateOpen, write(space: space)).decision == .allowed)
        }
    }

    // MARK: Protocol

    @Test func unsupportedProtocolIsRefused() {
        let decision = decide(gateOpen, write(protocolHint: "cmp", protocolSupported: false))
        #expect(decision.decision == .unsupportedProtocol)
        #expect(decision.reachesDriverWritePath == false)
    }

    // MARK: Raw developer tier

    @Test func rawTierWriteIsDeniedWithoutTierEnabled() {
        let decision = decide(gateOpen, write(rawTier: true))
        #expect(decision.decision == .denied)
        #expect(decision.requiredCapability == "rawDeveloperTier")
        #expect(decision.reachesDriverWritePath == false)
    }

    @Test func rawTierWriteAllowedWhenTierEnabled() {
        let cfg = config(.developerWriteEnabled, writePolicyAvailable: true, swiftTestGatePassed: true, rawDeveloperTierEnabled: true)
        #expect(decide(cfg, write(rawTier: true)).decision == .allowed)
    }

    // MARK: Coverage + safety invariant

    @Test func everyDecisionCategoryIsReachable() {
        var seen = Set<ASFWMCPWriteDecision>()
        seen.insert(decide(gateOpen, write()).decision)                                   // allowed
        seen.insert(decide(gateOpen, write(rawTier: true)).decision)                      // denied
        seen.insert(decide(config(.mock), write()).decision)                              // dryRunOnly
        seen.insert(decide(config(.readOnlyDeveloper), write()).decision)                 // requiresDeveloperMode
        seen.insert(decide(gateOpen, write(space: .configRom)).decision)                  // unsupportedAddressSpace
        seen.insert(decide(gateOpen, write(requested: 16, current: 17)).decision)         // staleGeneration
        seen.insert(decide(gateOpen, write(protocolHint: "cmp", protocolSupported: false)).decision) // unsupportedProtocol
        #expect(seen == Set(ASFWMCPWriteDecision.allCases))
    }

    @Test func onlyAllowedDecisionsReachTheDriverWritePath() {
        let configs = [config(.disabled), config(.mock), config(.readOnlyDeveloper), gateOpen]
        let requests = [
            write(), write(rawTier: true), write(dryRun: true),
            write(space: .configRom), write(requested: 16, current: 17),
            write(protocolHint: "cmp", protocolSupported: false)
        ]
        for cfg in configs {
            for req in requests {
                let decision = decide(cfg, req)
                if decision.reachesDriverWritePath {
                    #expect(decision.decision == .allowed)
                    #expect(cfg.canListDeveloperWriteTools)
                }
            }
        }
    }

    @Test func coreEvaluatesWritePolicyWithItsConfiguration() async {
        let driver = MockASFWDriverControl()
        let core = ASFWMCPCore(configuration: config(.readOnlyDeveloper), driver: driver)
        #expect(core.evaluateWritePolicy(write()).decision == .requiresDeveloperMode)
    }
}
