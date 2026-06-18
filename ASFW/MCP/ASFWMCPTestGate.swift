import Foundation

enum ASFWMCPTestGate {
    static func evaluate<Driver: ASFWDriverControlling>(
        core: ASFWMCPCore<Driver>,
        smokePlan: ASFWMCPHardwareSmokePlan = ASFWMCPHardwareSmokeHarness.defaultPlan()
    ) async -> ASFWMCPTestGateResult {
        var checks: [ASFWMCPTestGateCheck] = []

        let tools = await core.listTools()
        let resources = await core.listResources()
        let toolNames = Set(tools.map(\.name))
        let resourceURIs = Set(resources.map(\.uri))

        checks.append(check(
            id: "tools.non_empty_when_enabled",
            passed: core.configuration.mode == .disabled || tools.isEmpty == false,
            reason: "Enabled MCP modes must expose the minimal always-visible tools."
        ))

        checks.append(check(
            id: "resources.non_empty_when_enabled",
            passed: core.configuration.mode == .disabled || resources.isEmpty == false,
            reason: "Enabled MCP modes must expose resource definitions."
        ))

        let writeToolsHiddenOrGated = tools.allSatisfy { tool in
            switch tool.visibility {
            case .developerWrite:
                return core.configuration.canListDeveloperWriteTools || core.configuration.mode == .mock
            case .rawDeveloper:
                return core.configuration.canListRawDeveloperTools || core.configuration.mode == .mock
            case .always, .readOnly:
                return true
            }
        }
        checks.append(check(
            id: "writes.hidden_without_gates",
            passed: writeToolsHiddenOrGated,
            reason: "Write-capable tools must stay hidden unless write policy and Swift test gates are open."
        ))

        checks.append(check(
            id: "required.resources_present",
            passed: resourceURIs.isSuperset(of: [
                "asfw://telemetry/snapshot",
                "asfw://controller/state",
                "asfw://nodes",
                "asfw://transactions/recent"
            ]),
            reason: "The first telemetry resources must be discoverable."
        ))

        checks.append(check(
            id: "required.tools_present",
            passed: core.configuration.mode == .disabled || toolNames.isSuperset(of: [
                "asfw_get_capabilities",
                "asfw_get_policy",
                "asfw_list_nodes",
                "asfw_get_node_summary",
                "asfw_explain_capability"
            ]),
            reason: "The minimal always-visible tool set must be discoverable."
        ))

        let telemetry = await core.readResource(uri: "asfw://telemetry/snapshot")
        checks.append(check(
            id: "telemetry.envelope_stable",
            passed: telemetry.schema == "asfw.telemetry.snapshot.v1" &&
                    telemetry.uri == "asfw://telemetry/snapshot" &&
                    telemetry.snapshotId.isEmpty == false &&
                    telemetry.errors.isEmpty,
            reason: "Telemetry snapshots must use stable resource envelopes."
        ))

        checks.append(check(
            id: "smoke.default_non_mutating",
            passed: smokePlan.containsMutatingOperations == false,
            reason: "Default hardware smoke plan must not mutate hardware."
        ))

        // FW-79 tightening (MCP_TEST_GATE.md §6): a write may reach the driver
        // write path only when developer-write policy and the test gate are open.
        // Probe with a representative, generation-current write so the decision is
        // governed by mode/gate state rather than incidental staleness.
        let writeProbe = core.writePolicyEngine.evaluate(
            ASFWMCPPolicyRequest(
                operationType: .write,
                addressSpace: .unitsSpace,
                requestedGeneration: 0,
                currentGeneration: 0
            )
        )
        checks.append(check(
            id: "policy.write_path_gated",
            passed: writeProbe.reachesDriverWritePath == false || core.configuration.canListDeveloperWriteTools,
            reason: "Writes may reach the driver write path only when developer-write policy and the Swift test gate are open."
        ))

        return ASFWMCPTestGateResult(checks: checks)
    }

    static func allowsRealAgentHardwareAccess(_ result: ASFWMCPTestGateResult) -> Bool {
        result.passed
    }

    private static func check(id: String, passed: Bool, reason: String) -> ASFWMCPTestGateCheck {
        ASFWMCPTestGateCheck(id: id, passed: passed, reason: reason)
    }
}
