import Foundation

// FW-97: opt-in, hardware-backed MCP smoke execution.
//
// The runner intentionally contains no device-specific write recipe.  It is a
// read-first acceptance gate for a connected driver: a mutation listed in a
// plan is refused unless both developer gates are present, and remains skipped
// until a separately reviewed recipe is supplied for that exact hardware.

enum ASFWMCPHardwareSmokeDisposition: String, Equatable {
    case passed
    case skipped
    case unsupported
    case failed
    case refused
}

struct ASFWMCPHardwareSmokeOptions: Equatable {
    /// Must be set by the caller before any live-driver operation is issued.
    var hardwareAccessEnabled: Bool
    /// First of two gates required for a plan's mutating step.
    var developerModeEnabled: Bool
    /// Second gate required for a plan's mutating step.
    var mutatingSmokeEnabled: Bool

    static let disabled = ASFWMCPHardwareSmokeOptions(
        hardwareAccessEnabled: false,
        developerModeEnabled: false,
        mutatingSmokeEnabled: false
    )

    static let readOnly = ASFWMCPHardwareSmokeOptions(
        hardwareAccessEnabled: true,
        developerModeEnabled: false,
        mutatingSmokeEnabled: false
    )

    var permitsMutatingSteps: Bool {
        hardwareAccessEnabled && developerModeEnabled && mutatingSmokeEnabled
    }
}

struct ASFWMCPHardwareSmokeStepResult: Equatable {
    let name: String
    let disposition: ASFWMCPHardwareSmokeDisposition
    let reason: String
    let resourceURI: String?
    let toolName: String?
}

struct ASFWMCPHardwareSmokeReport: Equatable {
    let generation: UInt32?
    let detectedNodes: [ASFWMCPNodeSummary]
    let results: [ASFWMCPHardwareSmokeStepResult]

    var failures: [ASFWMCPHardwareSmokeStepResult] {
        results.filter { $0.disposition == .failed }
    }

    var conciseSummary: String {
        let counts = Dictionary(grouping: results, by: \.disposition).mapValues(\.count)
        let generationText = generation.map { String($0) } ?? "unknown"
        return "generation=\(generationText) "
            + "nodes=\(detectedNodes.count) "
            + "passed=\(counts[.passed, default: 0]) "
            + "skipped=\(counts[.skipped, default: 0]) "
            + "unsupported=\(counts[.unsupported, default: 0]) "
            + "failed=\(counts[.failed, default: 0]) "
            + "refused=\(counts[.refused, default: 0])"
    }
}

private enum ASFWMCPHardwareSmokeConstants {
    // Standard Config ROM base, cross-checked with
    // ASFWDriver/ConfigROM/ROMReader.hpp.
    static let configRomBaseHigh: UInt16 = 0xFFFF
    static let configRomBaseLow: UInt32 = 0xF000_0400
    static let configRomPrefixLength: UInt32 = 64
}

struct ASFWMCPHardwareSmokeRunner<Driver: ASFWDriverControlling> {
    let core: ASFWMCPCore<Driver>

    func run(
        plan: ASFWMCPHardwareSmokePlan = ASFWMCPHardwareSmokeHarness.defaultPlan(),
        options: ASFWMCPHardwareSmokeOptions = .disabled
    ) async -> ASFWMCPHardwareSmokeReport {
        guard options.hardwareAccessEnabled else {
            return ASFWMCPHardwareSmokeReport(
                generation: nil,
                detectedNodes: [],
                results: plan.steps.map { step in
                    result(
                        for: step,
                        disposition: .refused,
                        reason: "Hardware execution requires explicit hardwareAccessEnabled opt-in."
                    )
                }
            )
        }

        let snapshot = await core.driver.fetchTelemetrySnapshot(configuration: core.configuration)
        guard snapshot.driverConnected else {
            return ASFWMCPHardwareSmokeReport(
                generation: snapshot.generation,
                detectedNodes: [],
                results: plan.steps.map { step in
                    result(for: step, disposition: .failed, reason: "Driver is not connected.")
                }
            )
        }

        let nodes = await core.driver.listNodes()
        var results: [ASFWMCPHardwareSmokeStepResult] = []
        for step in plan.steps {
            if step.mutatesHardware {
                results.append(mutationResult(for: step, options: options))
            } else {
                results.append(await execute(step))
            }
        }

        results.append(contentsOf: await configRomReadResults(nodes: nodes, generation: snapshot.generation))
        results.append(await hostRegisterProbe())
        results.append(contentsOf: protocolCapabilityResults(nodes: nodes))

        return ASFWMCPHardwareSmokeReport(
            generation: snapshot.generation,
            detectedNodes: nodes,
            results: results
        )
    }

    private func execute(_ step: ASFWMCPHardwareSmokeStep) async -> ASFWMCPHardwareSmokeStepResult {
        if let resourceURI = step.resourceURI {
            let envelope = await core.readResource(uri: resourceURI)
            if envelope.errors.isEmpty == false {
                let firstError = envelope.errors[0]
                return result(
                    for: step,
                    disposition: firstError.code == .capabilityUnavailable ? .unsupported : .failed,
                    reason: firstError.reason
                )
            }
        }

        if let toolName = step.toolName {
            let toolResult = await core.callTool(name: toolName)
            if toolResult.ok == false {
                let firstError = toolResult.errors.first
                return result(
                    for: step,
                    disposition: firstError?.code == .capabilityUnavailable ? .unsupported : .failed,
                    reason: firstError?.reason ?? "Tool returned an unsuccessful result."
                )
            }
        }

        return result(for: step, disposition: .passed, reason: "Completed without mutation.")
    }

    private func configRomReadResults(
        nodes: [ASFWMCPNodeSummary],
        generation: UInt32
    ) async -> [ASFWMCPHardwareSmokeStepResult] {
        guard nodes.isEmpty == false else {
            return [ASFWMCPHardwareSmokeStepResult(
                name: "Read Config ROM prefix",
                disposition: .skipped,
                reason: "No discovered remote node is available.",
                resourceURI: nil,
                toolName: "asfw_read_block"
            )]
        }

        var results: [ASFWMCPHardwareSmokeStepResult] = []
        for node in nodes {
            guard node.configRomCached else {
                results.append(ASFWMCPHardwareSmokeStepResult(
                    name: "Read Config ROM prefix (node \(node.nodeId))",
                    disposition: .skipped,
                    reason: "Config ROM is not cached for this node.",
                    resourceURI: nil,
                    toolName: "asfw_read_block"
                ))
                continue
            }

            let arguments: ASFWMCPValue = .object([
                "nodeId": .int(Int(node.nodeId)),
                "generation": .int(Int(generation)),
                "addressHigh": .int(Int(ASFWMCPHardwareSmokeConstants.configRomBaseHigh)),
                "addressLow": .int(Int(ASFWMCPHardwareSmokeConstants.configRomBaseLow))
            ])
            let quadlet = await core.callTool(name: "asfw_read_quadlet", arguments: arguments)
            results.append(toolResult(
                name: "Read Config ROM header (node \(node.nodeId))",
                toolName: "asfw_read_quadlet",
                result: quadlet
            ))

            let prefixArguments: ASFWMCPValue = .object([
                "nodeId": .int(Int(node.nodeId)),
                "generation": .int(Int(generation)),
                "addressHigh": .int(Int(ASFWMCPHardwareSmokeConstants.configRomBaseHigh)),
                "addressLow": .int(Int(ASFWMCPHardwareSmokeConstants.configRomBaseLow)),
                "length": .int(Int(ASFWMCPHardwareSmokeConstants.configRomPrefixLength))
            ])
            let prefix = await core.callTool(name: "asfw_read_block", arguments: prefixArguments)
            results.append(toolResult(
                name: "Read Config ROM prefix (node \(node.nodeId))",
                toolName: "asfw_read_block",
                result: prefix
            ))
        }
        return results
    }

    private func hostRegisterProbe() async -> ASFWMCPHardwareSmokeStepResult {
        // The catalog already advertises this read-only surface.  Calling it
        // makes adapter gaps explicit in the smoke report rather than silently
        // treating OHCI register inspection as covered by telemetry.
        let probe = await core.callTool(name: "asfw_snapshot_ohci_registers")
        return toolResult(
            name: "Snapshot selected OHCI registers",
            toolName: "asfw_snapshot_ohci_registers",
            result: probe
        )
    }

    private func protocolCapabilityResults(
        nodes: [ASFWMCPNodeSummary]
    ) -> [ASFWMCPHardwareSmokeStepResult] {
        let listedTools = Set(ASFWMCPToolCatalog.all.map(\.name))
        let probes: [(hint: String, tool: String)] = [
            ("avc", "asfw_avc_list_units"),
            ("cmp", "asfw_cmp_list_plugs"),
            ("sbp2", "asfw_sbp2_list_units"),
            ("dice_tcat", "asfw_dice_read_register")
        ]

        return nodes.flatMap { node in
            probes.compactMap { probe in
                guard node.protocolHints.contains(probe.hint) else { return nil }
                return ASFWMCPHardwareSmokeStepResult(
                    name: "Probe \(probe.hint) capability (node \(node.nodeId))",
                    disposition: listedTools.contains(probe.tool) ? .passed : .unsupported,
                    reason: listedTools.contains(probe.tool)
                        ? "Discovery advertised \(probe.hint); \(probe.tool) is registered."
                        : "Discovery advertised \(probe.hint), but no matching MCP tool is registered.",
                    resourceURI: nil,
                    toolName: probe.tool
                )
            }
        }
    }

    private func mutationResult(
        for step: ASFWMCPHardwareSmokeStep,
        options: ASFWMCPHardwareSmokeOptions
    ) -> ASFWMCPHardwareSmokeStepResult {
        guard options.permitsMutatingSteps else {
            return result(
                for: step,
                disposition: .refused,
                reason: "Mutating smoke steps require developerModeEnabled and mutatingSmokeEnabled."
            )
        }
        return result(
            for: step,
            disposition: .skipped,
            reason: "No device-specific mutation recipe is embedded in the smoke runner."
        )
    }

    private func toolResult(
        name: String,
        toolName: String,
        result: ASFWMCPToolCallResult
    ) -> ASFWMCPHardwareSmokeStepResult {
        guard result.ok else {
            let firstError = result.errors.first
            return ASFWMCPHardwareSmokeStepResult(
                name: name,
                disposition: firstError?.code == .capabilityUnavailable ? .unsupported : .failed,
                reason: firstError?.reason ?? "Tool returned an unsuccessful result.",
                resourceURI: nil,
                toolName: toolName
            )
        }
        return ASFWMCPHardwareSmokeStepResult(
            name: name,
            disposition: .passed,
            reason: "Read completed without mutation.",
            resourceURI: nil,
            toolName: toolName
        )
    }

    private func result(
        for step: ASFWMCPHardwareSmokeStep,
        disposition: ASFWMCPHardwareSmokeDisposition,
        reason: String
    ) -> ASFWMCPHardwareSmokeStepResult {
        ASFWMCPHardwareSmokeStepResult(
            name: step.name,
            disposition: disposition,
            reason: reason,
            resourceURI: step.resourceURI,
            toolName: step.toolName
        )
    }
}
