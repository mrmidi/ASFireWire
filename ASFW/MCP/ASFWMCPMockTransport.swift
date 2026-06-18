import Foundation

struct ASFWMCPMockTransport<Driver: ASFWDriverControlling> {
    let core: ASFWMCPCore<Driver>

    func listTools() async -> [ASFWMCPToolDefinition] {
        await core.listTools()
    }

    func listResources() async -> [ASFWMCPResourceDefinition] {
        await core.listResources()
    }

    func readResource(_ uri: String) async -> ASFWMCPResourceEnvelope {
        await core.readResource(uri: uri)
    }

    func callTool(_ name: String, arguments: ASFWMCPValue = .object([:])) async -> ASFWMCPToolCallResult {
        await core.callTool(name: name, arguments: arguments)
    }
}

enum ASFWMCPHardwareSmokeHarness {
    static func defaultPlan(includeMutatingOperations: Bool = false) -> ASFWMCPHardwareSmokePlan {
        var steps = [
            ASFWMCPHardwareSmokeStep(
                name: "Read telemetry snapshot",
                resourceURI: "asfw://telemetry/snapshot",
                toolName: nil,
                mutatesHardware: false,
                requiresExplicitEnablement: false
            ),
            ASFWMCPHardwareSmokeStep(
                name: "Read controller state",
                resourceURI: "asfw://controller/state",
                toolName: nil,
                mutatesHardware: false,
                requiresExplicitEnablement: false
            ),
            ASFWMCPHardwareSmokeStep(
                name: "List nodes",
                resourceURI: "asfw://nodes",
                toolName: "asfw_list_nodes",
                mutatesHardware: false,
                requiresExplicitEnablement: false
            ),
            ASFWMCPHardwareSmokeStep(
                name: "Read recent transactions",
                resourceURI: "asfw://transactions/recent",
                toolName: nil,
                mutatesHardware: false,
                requiresExplicitEnablement: false
            )
        ]

        if includeMutatingOperations {
            steps.append(
                ASFWMCPHardwareSmokeStep(
                    name: "Optional developer write verification",
                    resourceURI: nil,
                    toolName: "asfw_write_quadlet",
                    mutatesHardware: true,
                    requiresExplicitEnablement: true
                )
            )
        }

        return ASFWMCPHardwareSmokePlan(steps: steps)
    }
}
