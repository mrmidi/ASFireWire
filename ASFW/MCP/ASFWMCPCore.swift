import Foundation

struct ASFWMCPCore<Driver: ASFWDriverControlling> {
    let configuration: ASFWMCPRuntimeConfiguration
    let driver: Driver

    func listTools() async -> [ASFWMCPToolDefinition] {
        guard configuration.mode != .disabled else { return [] }

        let nodes = await driver.listNodes()
        let protocolHints = Set(nodes.flatMap(\.protocolHints))
        let allTools = ASFWMCPToolCatalog.all
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
        return ASFWMCPResourceCatalog.all
    }

    func readResource(uri: String) async -> ASFWMCPResourceEnvelope {
        guard configuration.mode != .disabled else {
            return disabledEnvelope(uri: uri)
        }

        switch uri {
        case "asfw://control-plane/health":
            return await controlPlaneHealthEnvelope()
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

    private func controlPlaneHealthEnvelope() async -> ASFWMCPResourceEnvelope {
        let snapshot = await driver.fetchTelemetrySnapshot(configuration: configuration)
        let assessment = controlPlaneHealthAssessment(snapshot: snapshot)
        return envelope(
            schema: "asfw.control_plane.health.v1",
            uri: "asfw://control-plane/health",
            snapshot: snapshot,
            data: .object([
                "status": .string(assessment.status),
                "reasons": .array(assessment.reasons.map { .string($0) }),
                "expectedGeneration": .int(Int(snapshot.generation)),
                "allowReadOnlyQueries": .bool(assessment.allowReadOnlyQueries),
                "allowTargetedReads": .bool(assessment.allowTargetedReads),
                "capabilities": .object([
                    "readOnlyToolsAvailable": .bool(configuration.mode != .disabled),
                    "developerWritesListed": .bool(snapshot.policy.writesListed),
                    "protocols": .object([
                        "avc": .bool(snapshot.protocols.avcUnits > 0),
                        "cmp": .bool(snapshot.protocols.cmpCapableNodes > 0),
                        "sbp2": .bool(snapshot.protocols.sbp2Units > 0),
                        "diceTcat": .bool(snapshot.protocols.diceTcatNodes > 0)
                    ])
                ]),
                "nextResources": .array([
                    .string("asfw://telemetry/snapshot"),
                    .string("asfw://nodes"),
                    .string("asfw://controller/state")
                ])
            ]),
            links: [
                "asfw://telemetry/snapshot",
                "asfw://nodes",
                "asfw://controller/state"
            ]
        )
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

    private func controlPlaneHealthAssessment(
        snapshot: ASFWMCPTelemetrySnapshot
    ) -> (status: String, reasons: [String], allowReadOnlyQueries: Bool, allowTargetedReads: Bool) {
        guard snapshot.driverConnected else {
            return ("unavailable", ["driverNotConnected"], false, false)
        }

        var reasons: [String] = []
        if snapshot.controller.state != "Running" {
            reasons.append("controllerNotRunning")
        }
        if !snapshot.controller.linkActive {
            reasons.append("linkInactive")
        }
        if !snapshot.bus.topologyValid {
            reasons.append("topologyInvalid")
        }
        if snapshot.async.droppedEventCount > 0 {
            reasons.append("asyncEventsDropped")
        }
        if snapshot.async.timeouts > 0 {
            reasons.append("asyncTimeoutsObserved")
        }

        return reasons.isEmpty
            ? ("ready", [], true, true)
            : ("degraded", reasons, true, false)
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
