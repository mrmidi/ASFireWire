import Foundation

/// Per-surface MCP tool definitions, aggregated into the full catalog.
///
/// Each protocol surface owns its own slice so surfaces can be added/expanded in
/// independent files and commits without all editing one shared array. A surface
/// ticket extracts its slice into its own file as an `extension ASFWMCPToolCatalog`
/// (see ASFWMCPRegisterTools.swift for the reference pattern). `all` is the only
/// place that lists every group, and it is stable across the fixed taxonomy.
enum ASFWMCPToolCatalog {
    static var all: [ASFWMCPToolDefinition] {
        coreTools
            + busTopologyTools
            + busResetDeveloperTools
            + configRomTools
            + asyncTransactionTools
            + registerTools
            + irmCasTools
            + avcFcpTools
            + cmpTools
            + sbp2Tools
            + diceTcatTools
    }

    static let coreTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(name: "asfw_get_capabilities", group: "core", visibility: .always, readOnly: true, idempotent: true, summary: "Summarize MCP runtime mode and available dynamic groups."),
        ASFWMCPToolDefinition(name: "asfw_get_policy", group: "core", visibility: .always, readOnly: true, idempotent: true, summary: "Report current MCP policy and write-gate status."),
        ASFWMCPToolDefinition(name: "asfw_list_nodes", group: "core", visibility: .always, readOnly: true, idempotent: true, summary: "List current bus nodes and protocol hints."),
        ASFWMCPToolDefinition(name: "asfw_get_node_summary", group: "core", visibility: .always, readOnly: true, idempotent: true, summary: "Return one compact node summary."),
        ASFWMCPToolDefinition(name: "asfw_explain_capability", group: "core", visibility: .always, readOnly: true, idempotent: true, summary: "Explain why a capability is available, hidden, or policy-gated.")
    ]

    static let busTopologyTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(name: "asfw_get_controller_state", group: "bus_topology", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return controller state and bus health."),
        ASFWMCPToolDefinition(name: "asfw_get_topology", group: "bus_topology", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return current topology snapshot.")
    ]

    static let configRomTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(name: "asfw_get_config_rom", group: "config_rom", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return cached Config ROM summary.")
    ]

    static let asyncTransactionTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(name: "asfw_read_quadlet", group: "async_transactions", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Submit an async quadlet read."),
        ASFWMCPToolDefinition(name: "asfw_read_block", group: "async_transactions", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Submit an async block read."),
        ASFWMCPToolDefinition(name: "asfw_write_quadlet", group: "async_transactions", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated async quadlet write."),
        ASFWMCPToolDefinition(name: "asfw_write_block", group: "async_transactions", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated async block write."),
        ASFWMCPToolDefinition(name: "asfw_compare_swap", group: "async_transactions", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated compare-swap transaction.")
    ]

    // `registerTools` is owned by ASFWMCPRegisterTools.swift (FW-80) — the
    // reference for how a surface ticket extracts its slice into its own file.

    // `irmCasTools` is owned by ASFWMCPIrmCasTools.swift (FW-81).

    // `avcFcpTools` is owned by ASFWMCPAvcFcpTools.swift (FW-82).

    // `cmpTools` is owned by ASFWMCPCmpTools.swift (FW-83).

    // `sbp2Tools` is owned by ASFWMCPSbp2Tools.swift (FW-84).

    // `diceTcatTools` is owned by ASFWMCPDiceTcatTools.swift (FW-85).
}
