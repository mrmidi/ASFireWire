import Foundation

struct ASFWMCPCore<Driver: ASFWDriverControlling> {
    let configuration: ASFWMCPRuntimeConfiguration
    let driver: Driver

    func listTools() async -> [ASFWMCPToolDefinition] {
        guard configuration.mode != .disabled else { return [] }

        let nodes = await driver.listNodes()
        let protocolHints = Set(nodes.flatMap(\.protocolHints))
        let allTools = Self.toolCatalog
        return allTools.filter { tool in
            guard tool.requiredProtocolHints.isEmpty ||
                  tool.requiredProtocolHints.contains(where: { protocolHints.contains($0) }) else {
                return false
            }

            switch tool.visibility {
            case .always:
                return true
            case .readOnly:
                return configuration.mode == .mock ||
                       configuration.mode == .readOnlyDeveloper ||
                       configuration.mode == .developerWriteEnabled
            case .developerWrite:
                return configuration.mode == .mock || configuration.canListDeveloperWriteTools
            case .rawDeveloper:
                return configuration.mode == .mock || configuration.canListRawDeveloperTools
            }
        }
    }

    func listResources() async -> [ASFWMCPResourceDefinition] {
        guard configuration.mode != .disabled else { return [] }
        return Self.resourceCatalog
    }

    func readResource(uri: String) async -> ASFWMCPResourceEnvelope {
        guard configuration.mode != .disabled else {
            return disabledEnvelope(uri: uri)
        }

        switch uri {
        case "asfw://telemetry/snapshot":
            return await telemetrySnapshotEnvelope()
        case "asfw://nodes":
            return await nodesEnvelope()
        case "asfw://transactions/recent":
            return await transactionsEnvelope()
        case "asfw://controller/state":
            return await controllerStateEnvelope()
        default:
            return capabilityUnavailableEnvelope(uri: uri)
        }
    }

    private func telemetrySnapshotEnvelope() async -> ASFWMCPResourceEnvelope {
        let snapshot = await driver.fetchTelemetrySnapshot(configuration: configuration)
        return envelope(
            schema: "asfw.telemetry.snapshot.v1",
            uri: "asfw://telemetry/snapshot",
            snapshot: snapshot,
            data: .object([
                "controller": .object([
                    "state": .string(snapshot.controller.state),
                    "linkActive": .bool(snapshot.controller.linkActive),
                    "localNodeId": snapshot.controller.localNodeId.map { .int(Int($0)) } ?? .null,
                    "rootNodeId": snapshot.controller.rootNodeId.map { .int(Int($0)) } ?? .null,
                    "irmNodeId": snapshot.controller.irmNodeId.map { .int(Int($0)) } ?? .null,
                    "isIRM": .bool(snapshot.controller.isIRM),
                    "isCycleMaster": .bool(snapshot.controller.isCycleMaster)
                ]),
                "bus": .object([
                    "nodeCount": .int(Int(snapshot.bus.nodeCount)),
                    "busResetCount": .uint64(snapshot.bus.busResetCount),
                    "gapCount": .int(Int(snapshot.bus.gapCount)),
                    "topologyValid": .bool(snapshot.bus.topologyValid)
                ]),
                "async": .object([
                    "recentEventCount": .int(Int(snapshot.async.recentEventCount)),
                    "droppedEventCount": .int(Int(snapshot.async.droppedEventCount)),
                    "timeouts": .int(Int(snapshot.async.timeouts)),
                    "lastCompletionNs": snapshot.async.lastCompletionNs.map { .uint64($0) } ?? .null
                ]),
                "protocols": .object([
                    "avcUnits": .int(Int(snapshot.protocols.avcUnits)),
                    "sbp2Units": .int(Int(snapshot.protocols.sbp2Units)),
                    "diceTcatNodes": .int(Int(snapshot.protocols.diceTcatNodes)),
                    "cmpCapableNodes": .int(Int(snapshot.protocols.cmpCapableNodes))
                ]),
                "policy": .object([
                    "runtimeMode": .string(snapshot.policy.runtimeMode.rawValue),
                    "writesListed": .bool(snapshot.policy.writesListed),
                    "writeGate": .string(snapshot.policy.writeGate)
                ])
            ]),
            links: [
                "asfw://controller/state",
                "asfw://nodes",
                "asfw://transactions/recent"
            ]
        )
    }

    private func controllerStateEnvelope() async -> ASFWMCPResourceEnvelope {
        let snapshot = await driver.fetchTelemetrySnapshot(configuration: configuration)
        return envelope(
            schema: "asfw.telemetry.controller_state.v1",
            uri: "asfw://controller/state",
            snapshot: snapshot,
            data: .object([
                "controllerState": .string(snapshot.controller.state),
                "linkActive": .bool(snapshot.controller.linkActive),
                "generation": .int(Int(snapshot.generation)),
                "nodeCount": .int(Int(snapshot.bus.nodeCount))
            ]),
            links: ["asfw://telemetry/snapshot"]
        )
    }

    private func nodesEnvelope() async -> ASFWMCPResourceEnvelope {
        let snapshot = await driver.fetchTelemetrySnapshot(configuration: configuration)
        let nodes = await driver.listNodes()
        return envelope(
            schema: "asfw.telemetry.nodes.v1",
            uri: "asfw://nodes",
            snapshot: snapshot,
            data: .object([
                "generation": .int(Int(snapshot.generation)),
                "nodes": .array(nodes.map { node in
                    .object([
                        "nodeId": .int(Int(node.nodeId)),
                        "address16": .string(node.address16),
                        "guid": node.guid.map { .string($0) } ?? .null,
                        "vendorId": node.vendorId.map { .string($0) } ?? .null,
                        "modelId": node.modelId.map { .string($0) } ?? .null,
                        "vendorName": node.vendorName.map { .string($0) } ?? .null,
                        "modelName": node.modelName.map { .string($0) } ?? .null,
                        "configRomCached": .bool(node.configRomCached),
                        "protocolHints": .array(node.protocolHints.map { .string($0) })
                    ])
                })
            ]),
            links: ["asfw://bus/topology", "asfw://devices"]
        )
    }

    private func transactionsEnvelope() async -> ASFWMCPResourceEnvelope {
        let snapshot = await driver.fetchTelemetrySnapshot(configuration: configuration)
        let events = await driver.listRecentTransactions(limit: 32)
        return envelope(
            schema: "asfw.telemetry.transactions_recent.v1",
            uri: "asfw://transactions/recent",
            snapshot: snapshot,
            data: .object([
                "eventCount": .int(events.count),
                "limit": .int(32),
                "events": .array(events.map { event in
                    .object([
                        "timestampNs": .uint64(event.timestampNs),
                        "generation": .int(Int(event.generation)),
                        "direction": .string(event.direction),
                        "context": .string(event.context),
                        "tLabel": .int(Int(event.tLabel)),
                        "tCode": .string(event.tCode),
                        "sourceId": .string(event.sourceId),
                        "destinationId": .string(event.destinationId),
                        "address": .string(event.address),
                        "payloadBytes": .int(Int(event.payloadBytes)),
                        "ackCode": .string(event.ackCode),
                        "rCode": .string(event.rCode),
                        "speed": .string(event.speed),
                        "matchedTransaction": .bool(event.matchedTransaction),
                        "dropReason": event.dropReason.map { .string($0) } ?? .null
                    ])
                })
            ]),
            links: ["asfw://telemetry/snapshot"]
        )
    }

    private func envelope(
        schema: String,
        uri: String,
        snapshot: ASFWMCPTelemetrySnapshot,
        data: ASFWMCPValue,
        links: [String] = []
    ) -> ASFWMCPResourceEnvelope {
        ASFWMCPResourceEnvelope(
            schema: schema,
            uri: uri,
            snapshotId: snapshot.snapshotId,
            capturedAt: snapshot.capturedAt,
            monotonicNs: snapshot.monotonicNs,
            generation: snapshot.generation,
            driverConnected: snapshot.driverConnected,
            stale: false,
            truncated: false,
            data: data,
            links: links,
            errors: []
        )
    }

    private func disabledEnvelope(uri: String) -> ASFWMCPResourceEnvelope {
        ASFWMCPResourceEnvelope(
            schema: "asfw.telemetry.error.v1",
            uri: uri,
            snapshotId: "disabled",
            capturedAt: nil,
            monotonicNs: nil,
            generation: nil,
            driverConnected: false,
            stale: false,
            truncated: false,
            data: .object([:]),
            links: [],
            errors: [
                ASFWMCPResourceError(code: .mcpDisabled, reason: "MCP is disabled.")
            ]
        )
    }

    private func capabilityUnavailableEnvelope(uri: String) -> ASFWMCPResourceEnvelope {
        ASFWMCPResourceEnvelope(
            schema: "asfw.telemetry.error.v1",
            uri: uri,
            snapshotId: "unavailable",
            capturedAt: nil,
            monotonicNs: nil,
            generation: nil,
            driverConnected: true,
            stale: false,
            truncated: false,
            data: .object([:]),
            links: [],
            errors: [
                ASFWMCPResourceError(code: .capabilityUnavailable, reason: "No mock resource is registered for \(uri).")
            ]
        )
    }
}

extension ASFWMCPCore {
    static var toolCatalog: [ASFWMCPToolDefinition] {
        [
            ASFWMCPToolDefinition(name: "asfw_get_capabilities", group: "core", visibility: .always, readOnly: true, idempotent: true, summary: "Summarize MCP runtime mode and available dynamic groups."),
            ASFWMCPToolDefinition(name: "asfw_get_policy", group: "core", visibility: .always, readOnly: true, idempotent: true, summary: "Report current MCP policy and write-gate status."),
            ASFWMCPToolDefinition(name: "asfw_list_nodes", group: "core", visibility: .always, readOnly: true, idempotent: true, summary: "List current bus nodes and protocol hints."),
            ASFWMCPToolDefinition(name: "asfw_get_node_summary", group: "core", visibility: .always, readOnly: true, idempotent: true, summary: "Return one compact node summary."),
            ASFWMCPToolDefinition(name: "asfw_explain_capability", group: "core", visibility: .always, readOnly: true, idempotent: true, summary: "Explain why a capability is available, hidden, or policy-gated."),

            ASFWMCPToolDefinition(name: "asfw_get_controller_state", group: "bus_topology", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return controller state and bus health."),
            ASFWMCPToolDefinition(name: "asfw_get_topology", group: "bus_topology", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return current topology snapshot."),
            ASFWMCPToolDefinition(name: "asfw_get_config_rom", group: "config_rom", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return cached Config ROM summary."),
            ASFWMCPToolDefinition(name: "asfw_read_quadlet", group: "async_transactions", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Submit an async quadlet read."),
            ASFWMCPToolDefinition(name: "asfw_read_block", group: "async_transactions", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Submit an async block read."),
            ASFWMCPToolDefinition(name: "asfw_read_device_register", group: "register_access", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Read a device register/address-space value."),
            ASFWMCPToolDefinition(name: "asfw_dice_read_register", group: "dice_tcat", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Read a DICE/TCAT register.", requiredProtocolHints: ["dice_tcat"]),
            ASFWMCPToolDefinition(name: "asfw_irm_get_state", group: "irm_cas", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return IRM and bus manager state."),
            ASFWMCPToolDefinition(name: "asfw_avc_list_units", group: "avc_fcp", visibility: .readOnly, readOnly: true, idempotent: true, summary: "List AV/C units.", requiredProtocolHints: ["avc"]),
            ASFWMCPToolDefinition(name: "asfw_cmp_read_pcr", group: "cmp", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Read and decode a CMP plug control register.", requiredProtocolHints: ["cmp"]),
            ASFWMCPToolDefinition(name: "asfw_sbp2_list_units", group: "sbp2", visibility: .readOnly, readOnly: true, idempotent: true, summary: "List SBP-2 units.", requiredProtocolHints: ["sbp2"]),

            ASFWMCPToolDefinition(name: "asfw_write_quadlet", group: "async_transactions", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated async quadlet write."),
            ASFWMCPToolDefinition(name: "asfw_write_block", group: "async_transactions", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated async block write."),
            ASFWMCPToolDefinition(name: "asfw_compare_swap", group: "async_transactions", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated compare-swap transaction."),
            ASFWMCPToolDefinition(name: "asfw_dice_write_register", group: "dice_tcat", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated DICE/TCAT register write.", requiredProtocolHints: ["dice_tcat"]),
            ASFWMCPToolDefinition(name: "asfw_cmp_write_pcr", group: "cmp", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated CMP PCR write.", requiredProtocolHints: ["cmp"]),
            ASFWMCPToolDefinition(name: "asfw_write_ohci_register_dev", group: "register_access", visibility: .rawDeveloper, readOnly: false, idempotent: false, summary: "Raw developer-tier OHCI register write.")
        ]
    }

    static var resourceCatalog: [ASFWMCPResourceDefinition] {
        [
            ASFWMCPResourceDefinition(uri: "asfw://telemetry/snapshot", schema: "asfw.telemetry.snapshot.v1", summary: "Compact cross-system telemetry overview."),
            ASFWMCPResourceDefinition(uri: "asfw://controller/state", schema: "asfw.telemetry.controller_state.v1", summary: "Controller, link, and bus health."),
            ASFWMCPResourceDefinition(uri: "asfw://nodes", schema: "asfw.telemetry.nodes.v1", summary: "Current node summaries."),
            ASFWMCPResourceDefinition(uri: "asfw://transactions/recent", schema: "asfw.telemetry.transactions_recent.v1", summary: "Bounded recent async transaction events.")
        ]
    }
}
