import Testing
@testable import ASFW

struct MCPHardwareSmokeRunnerTests {
    @Test func refusesWithoutExplicitHardwareOptIn() async {
        let driver = MockASFWDriverControl()
        let runner = ASFWMCPHardwareSmokeRunner(
            core: ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)
        )

        let report = await runner.run()

        #expect(report.generation == nil)
        #expect(report.detectedNodes.isEmpty)
        #expect(report.results.allSatisfy { $0.disposition == .refused })
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func readOnlyRunReportsLiveGenerationNodesAndAdapterGaps() async throws {
        let driver = MockASFWDriverControl()
        let runner = ASFWMCPHardwareSmokeRunner(
            core: ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)
        )

        let report = await runner.run(options: .readOnly)

        #expect(report.generation == 17)
        #expect(report.detectedNodes.count == 2)
        #expect(report.results.contains { $0.name == "Read Config ROM header (node 0)" && $0.disposition == .passed })
        #expect(report.results.contains { $0.name == "Read Config ROM prefix (node 0)" && $0.disposition == .passed })
        #expect(report.results.contains { $0.name == "Snapshot selected OHCI registers" && $0.disposition == .unsupported })
        #expect(report.results.contains { $0.name == "Probe avc capability (node 0)" && $0.disposition == .passed })
        #expect(report.failures.isEmpty)
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
        #expect(report.conciseSummary.contains("generation=17"))
    }

    @Test func mutatingPlanStepRequiresBothDeveloperGates() async throws {
        let driver = MockASFWDriverControl()
        let runner = ASFWMCPHardwareSmokeRunner(
            core: ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)
        )
        let plan = ASFWMCPHardwareSmokeHarness.defaultPlan(includeMutatingOperations: true)
        let mutation = try #require(plan.steps.first { $0.mutatesHardware })

        let missingDeveloperMode = await runner.run(
            plan: plan,
            options: ASFWMCPHardwareSmokeOptions(
                hardwareAccessEnabled: true,
                developerModeEnabled: false,
                mutatingSmokeEnabled: true
            )
        )
        let missingMutationOptIn = await runner.run(
            plan: plan,
            options: ASFWMCPHardwareSmokeOptions(
                hardwareAccessEnabled: true,
                developerModeEnabled: true,
                mutatingSmokeEnabled: false
            )
        )

        #expect(missingDeveloperMode.results.contains { $0.name == mutation.name && $0.disposition == .refused })
        #expect(missingMutationOptIn.results.contains { $0.name == mutation.name && $0.disposition == .refused })
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }
}
