import Testing
@testable import ASFW

struct MCPBusResetToolsTests {
    private var gateOpen: ASFWMCPRuntimeConfiguration {
        ASFWMCPRuntimeConfiguration(
            mode: .developerWriteEnabled,
            writePolicyAvailable: true,
            swiftTestGatePassed: true,
            rawDeveloperTierEnabled: false
        )
    }

    private func arguments(
        generation: UInt32 = 17,
        shortReset: Bool = false,
        acknowledgeInterruption: Bool = true,
        dryRun: Bool = false
    ) -> ASFWMCPValue {
        .object([
            "generation": .int(Int(generation)),
            "shortReset": .bool(shortReset),
            "acknowledgeInterruption": .bool(acknowledgeInterruption),
            "dryRun": .bool(dryRun)
        ])
    }

    @Test func resetToolIsHiddenUntilDeveloperWriteGatePasses() async {
        let readOnly = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: MockASFWDriverControl())
        #expect(await readOnly.listTools().contains { $0.name == "asfw_bus_reset_dev" } == false)

        let enabled = ASFWMCPCore(configuration: gateOpen, driver: MockASFWDriverControl())
        #expect(await enabled.listTools().contains { $0.name == "asfw_bus_reset_dev" })
    }

    @Test func resetUsesDeveloperWriteModeWithoutInterruptionAcknowledgement() async {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: gateOpen, driver: driver))

        let result = await transport.callTool(
            "asfw_bus_reset_dev",
            arguments: arguments(acknowledgeInterruption: false)
        )

        #expect(result.ok)
        #expect(await driver.unexpectedWriteAttemptCount() == 1)
    }

    @Test func resetDryRunNeverReachesDriver() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: gateOpen, driver: driver))

        let result = await transport.callTool("asfw_bus_reset_dev", arguments: arguments(dryRun: true))

        guard case .object(let data) = result.data else {
            Issue.record("Expected receipt object.")
            return
        }
        #expect(result.ok == false)
        #expect(data["status"] == .string("dryRun"))
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func resetReportsGenerationChange() async throws {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: gateOpen, driver: driver))

        let result = await transport.callTool(
            "asfw_bus_reset_dev",
            arguments: arguments(shortReset: true)
        )

        guard case .object(let data) = result.data else {
            Issue.record("Expected receipt object.")
            return
        }
        #expect(result.ok)
        #expect(data["kind"] == .string("busReset"))
        #expect(data["acceptedGeneration"] == .int(17))
        #expect(data["observedGeneration"] == .int(18))
        #expect(data["shortReset"] == .bool(true))
        #expect(await driver.unexpectedWriteAttemptCount() == 1)
    }

    @Test func staleGenerationIsRefusedBeforeReset() async throws {
        let driver = MockASFWDriverControl(generation: 18)
        let transport = ASFWMCPMockTransport(core: ASFWMCPCore(configuration: gateOpen, driver: driver))

        let result = await transport.callTool("asfw_bus_reset_dev", arguments: arguments(generation: 17))

        guard case .object(let data) = result.data else {
            Issue.record("Expected receipt object.")
            return
        }
        #expect(result.ok == false)
        #expect(data["status"] == .string("denied"))
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }
}
