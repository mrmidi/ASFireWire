import Testing
@testable import ASFW

struct MCPTestGateTests {
    @Test func disabledModeFailsClosedForRealAccess() async {
        let driver = MockASFWDriverControl()
        let core = ASFWMCPCore(configuration: .disabled, driver: driver)

        let result = await ASFWMCPTestGate.evaluate(core: core)

        #expect(result.passed == false)
        #expect(ASFWMCPTestGate.allowsRealAgentHardwareAccess(result) == false)
        #expect(result.failedChecks.isEmpty == false)
    }

    @Test func readOnlyModePassesGateAndHidesWrites() async {
        let driver = MockASFWDriverControl()
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)

        let result = await ASFWMCPTestGate.evaluate(core: core)
        let toolNames = await Set(core.listTools().map(\.name))

        #expect(result.passed)
        #expect(ASFWMCPTestGate.allowsRealAgentHardwareAccess(result))
        #expect(toolNames.contains("asfw_write_quadlet") == false)
        #expect(toolNames.contains("asfw_cmp_write_pcr") == false)
        #expect(toolNames.contains("asfw_dice_write_register") == false)
    }

    @Test func developerWriteModeFailsClosedWhenPolicyOrTestGateIsMissing() async {
        let driver = MockASFWDriverControl()
        let configuration = ASFWMCPRuntimeConfiguration(
            mode: .developerWriteEnabled,
            writePolicyAvailable: true,
            swiftTestGatePassed: false,
            rawDeveloperTierEnabled: false
        )
        let core = ASFWMCPCore(configuration: configuration, driver: driver)

        let result = await ASFWMCPTestGate.evaluate(core: core)
        let toolNames = await Set(core.listTools().map(\.name))

        #expect(result.passed)
        #expect(toolNames.contains("asfw_write_quadlet") == false)
        #expect(toolNames.contains("asfw_write_ohci_register_dev") == false)
    }

    @Test func developerWriteModeListsPolicyGatedWritesAfterGatePasses() async {
        let driver = MockASFWDriverControl()
        let configuration = ASFWMCPRuntimeConfiguration(
            mode: .developerWriteEnabled,
            writePolicyAvailable: true,
            swiftTestGatePassed: true,
            rawDeveloperTierEnabled: false
        )
        let core = ASFWMCPCore(configuration: configuration, driver: driver)

        let result = await ASFWMCPTestGate.evaluate(core: core)
        let toolNames = await Set(core.listTools().map(\.name))

        #expect(result.passed)
        #expect(toolNames.contains("asfw_write_quadlet"))
        #expect(toolNames.contains("asfw_dice_write_register"))
        #expect(toolNames.contains("asfw_cmp_write_pcr"))
        #expect(toolNames.contains("asfw_write_ohci_register_dev") == false)
    }

    @Test func noDeviceFixtureDoesNotExposeProtocolSpecificTools() async {
        let driver = MockASFWDriverControl(nodes: [])
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)

        let result = await ASFWMCPTestGate.evaluate(core: core)
        let toolNames = await Set(core.listTools().map(\.name))

        #expect(result.passed)
        #expect(toolNames.contains("asfw_avc_list_units") == false)
        #expect(toolNames.contains("asfw_cmp_read_pcr") == false)
        #expect(toolNames.contains("asfw_dice_read_register") == false)
        #expect(toolNames.contains("asfw_sbp2_list_units") == false)
        #expect(toolNames.contains("asfw_irm_get_state"))
    }

    @Test func protocolFixturesExposeExpectedReadSurfaces() async {
        let driver = MockASFWDriverControl(
            nodes: MockASFWDriverControl.defaultNodes + [MockASFWDriverControl.sbp2Node]
        )
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)

        let result = await ASFWMCPTestGate.evaluate(core: core)
        let toolNames = await Set(core.listTools().map(\.name))

        #expect(result.passed)
        #expect(toolNames.contains("asfw_avc_list_units"))
        #expect(toolNames.contains("asfw_cmp_read_pcr"))
        #expect(toolNames.contains("asfw_dice_read_register"))
        #expect(toolNames.contains("asfw_sbp2_list_units"))
        #expect(toolNames.contains("asfw_irm_get_state"))
    }

    @Test func gateFailsWhenSmokePlanContainsMutation() async {
        let driver = MockASFWDriverControl()
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)
        let smokePlan = ASFWMCPHardwareSmokeHarness.defaultPlan(includeMutatingOperations: true)

        let result = await ASFWMCPTestGate.evaluate(core: core, smokePlan: smokePlan)

        #expect(result.passed == false)
        #expect(result.failedChecks.contains { $0.id == "smoke.default_non_mutating" })
    }
}
