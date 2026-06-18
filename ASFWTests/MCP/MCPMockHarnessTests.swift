import Testing
@testable import ASFW

struct MCPMockHarnessTests {
    @Test func mockModeListsAlwaysVisibleAndReadTools() async {
        let driver = MockASFWDriverControl()
        let core = ASFWMCPCore(configuration: .mock, driver: driver)
        let transport = ASFWMCPMockTransport(core: core)

        let names = await transport.listTools().map(\.name)

        #expect(names.contains("asfw_get_capabilities"))
        #expect(names.contains("asfw_list_nodes"))
        #expect(names.contains("asfw_read_quadlet"))
        #expect(names.contains("asfw_dice_read_register"))
    }

    @Test func readOnlyModeHidesDeveloperWriteTools() async {
        let driver = MockASFWDriverControl()
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)
        let transport = ASFWMCPMockTransport(core: core)

        let names = await transport.listTools().map(\.name)

        #expect(names.contains("asfw_read_quadlet"))
        #expect(names.contains("asfw_write_quadlet") == false)
        #expect(names.contains("asfw_dice_write_register") == false)
        #expect(names.contains("asfw_write_ohci_register_dev") == false)
    }

    @Test func developerWriteModeListsWritesOnlyAfterGatesPass() async {
        let driver = MockASFWDriverControl()
        let gatedConfig = ASFWMCPRuntimeConfiguration(
            mode: .developerWriteEnabled,
            writePolicyAvailable: false,
            swiftTestGatePassed: false,
            rawDeveloperTierEnabled: false
        )
        let openConfig = ASFWMCPRuntimeConfiguration(
            mode: .developerWriteEnabled,
            writePolicyAvailable: true,
            swiftTestGatePassed: true,
            rawDeveloperTierEnabled: false
        )

        let gatedNames = await ASFWMCPMockTransport(
            core: ASFWMCPCore(configuration: gatedConfig, driver: driver)
        ).listTools().map(\.name)
        let openNames = await ASFWMCPMockTransport(
            core: ASFWMCPCore(configuration: openConfig, driver: driver)
        ).listTools().map(\.name)

        #expect(gatedNames.contains("asfw_write_quadlet") == false)
        #expect(openNames.contains("asfw_write_quadlet"))
        #expect(openNames.contains("asfw_write_ohci_register_dev") == false)
    }

    @Test func telemetryResourceUsesStableEnvelope() async throws {
        let driver = MockASFWDriverControl()
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)
        let transport = ASFWMCPMockTransport(core: core)

        let envelope = await transport.readResource("asfw://telemetry/snapshot")

        #expect(envelope.schema == "asfw.telemetry.snapshot.v1")
        #expect(envelope.uri == "asfw://telemetry/snapshot")
        #expect(envelope.generation == 17)
        #expect(envelope.driverConnected)
        #expect(envelope.errors.isEmpty)

        guard case .object(let data) = envelope.data else {
            Issue.record("Telemetry data should be an object.")
            return
        }

        #expect(data["policy"] != nil)
        #expect(data["controller"] != nil)
        #expect(data["bus"] != nil)
        #expect(data["protocols"] != nil)
    }

    @Test func nodesResourceIsBackedByMockDriverWithoutHardware() async throws {
        let driver = MockASFWDriverControl()
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)
        let transport = ASFWMCPMockTransport(core: core)

        let envelope = await transport.readResource("asfw://nodes")

        guard case .object(let data) = envelope.data,
              case .array(let nodes)? = data["nodes"] else {
            Issue.record("Nodes resource should include a nodes array.")
            return
        }

        #expect(nodes.count == 2)
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func defaultHardwareSmokePlanContainsNoMutatingOperations() {
        let plan = ASFWMCPHardwareSmokeHarness.defaultPlan()

        #expect(plan.steps.isEmpty == false)
        #expect(plan.containsMutatingOperations == false)
        #expect(plan.steps.allSatisfy { $0.requiresExplicitEnablement == false })
    }

    @Test func mutatingHardwareSmokeStepRequiresExplicitEnablement() throws {
        let plan = ASFWMCPHardwareSmokeHarness.defaultPlan(includeMutatingOperations: true)
        let mutatingStep = try #require(plan.steps.first { $0.mutatesHardware })

        #expect(mutatingStep.toolName == "asfw_write_quadlet")
        #expect(mutatingStep.requiresExplicitEnablement)
    }
}
