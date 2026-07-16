import Foundation

/// Per-surface MCP resource definitions, aggregated into the full catalog.
///
/// Mirrors ASFWMCPToolCatalog: each surface owns its resource slice so surfaces
/// can be added in independent files. `all` lists every group.
enum ASFWMCPResourceCatalog {
    static var all: [ASFWMCPResourceDefinition] {
        controlPlaneResources
            + coreResources
            + busTopologyResources
            + telemetryResources
    }

    static let controlPlaneResources: [ASFWMCPResourceDefinition] = [
        ASFWMCPResourceDefinition(
            uri: "asfw://control-plane/health",
            schema: "asfw.control_plane.health.v1",
            summary: "Versioned agent readiness and capability summary."
        )
    ]

    static let coreResources: [ASFWMCPResourceDefinition] = [
        ASFWMCPResourceDefinition(uri: "asfw://nodes", schema: "asfw.telemetry.nodes.v1", summary: "Current node summaries.")
    ]

    static let busTopologyResources: [ASFWMCPResourceDefinition] = [
        ASFWMCPResourceDefinition(uri: "asfw://controller/state", schema: "asfw.telemetry.controller_state.v1", summary: "Controller, link, and bus health.")
    ]

    static let telemetryResources: [ASFWMCPResourceDefinition] = [
        ASFWMCPResourceDefinition(uri: "asfw://telemetry/snapshot", schema: "asfw.telemetry.snapshot.v1", summary: "Compact cross-system telemetry overview."),
        ASFWMCPResourceDefinition(uri: "asfw://transactions/recent", schema: "asfw.telemetry.transactions_recent.v1", summary: "Bounded recent async transaction events.")
    ]
}
