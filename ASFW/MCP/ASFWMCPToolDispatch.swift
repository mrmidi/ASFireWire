import Foundation

// FW-91: MCP callTool execution dispatch.
//
// This layer maps MCP tool names and JSON-like argument values onto the typed
// FW-78..85 request structs, evaluates FW-79 write policy before any mutating
// driver call, and returns compact structured results. Surfaces whose live
// driver semantics are not wired yet still have explicit dispatch arms that
// return capabilityUnavailable instead of disappearing or bypassing policy.

extension ASFWMCPCore {
    func callTool(name: String, arguments: ASFWMCPValue = .object([:])) async -> ASFWMCPToolCallResult {
        guard configuration.mode != .disabled else {
            return .failure(toolName: name, code: .mcpDisabled, reason: "MCP is disabled.")
        }

        guard ASFWMCPToolCatalog.all.contains(where: { $0.name == name }) else {
            return .failure(toolName: name, code: .capabilityUnavailable, reason: "Unknown MCP tool \(name).")
        }

        let decoder: ASFWMCPToolArgumentDecoder
        do {
            decoder = try ASFWMCPToolArgumentDecoder(arguments)
        } catch {
            return malformedToolResult(name, reason: "Tool arguments must be an object.")
        }

        switch name {
        case "asfw_get_capabilities":
            return await capabilitiesResult(toolName: name)
        case "asfw_get_policy":
            return await policyResult(toolName: name)
        case "asfw_list_nodes":
            return await listNodesResult(toolName: name)
        case "asfw_get_node_summary":
            return await nodeSummaryResult(toolName: name, decoder: decoder)
        case "asfw_explain_capability":
            return await explainCapabilityResult(toolName: name, decoder: decoder)
        case "asfw_get_controller_state", "asfw_get_topology", "asfw_get_config_rom":
            return notImplementedToolResult(name, reason: "Read-only \(name) dispatch is reserved for the live telemetry adapter.")
        case "asfw_bus_reset_dev":
            return await dispatchBusReset(name, decoder: decoder)
        case "asfw_read_quadlet":
            return await dispatchReadQuadlet(name, decoder: decoder)
        case "asfw_read_block":
            return await dispatchReadBlock(name, decoder: decoder)
        case "asfw_write_quadlet":
            return await dispatchWriteQuadlet(name, decoder: decoder)
        case "asfw_write_block":
            return await dispatchWriteBlock(name, decoder: decoder)
        case "asfw_compare_swap", "asfw_cas_quadlet":
            return await dispatchCompareSwap(name, decoder: decoder, protocolHint: nil)
        case "asfw_read_device_register", "asfw_dice_read_register":
            return await dispatchReadQuadlet(name, decoder: decoder)
        case "asfw_read_device_register_block", "asfw_dice_read_block", "asfw_tcat_read_application_block":
            return await dispatchReadBlock(name, decoder: decoder)
        case "asfw_write_device_register":
            return await dispatchWriteQuadlet(name, decoder: decoder)
        case "asfw_write_device_register_block":
            return await dispatchWriteBlock(name, decoder: decoder)
        case "asfw_write_ohci_register_dev":
            return await dispatchOhciWrite(name, decoder: decoder)
        case "asfw_read_ohci_register", "asfw_snapshot_ohci_registers":
            return notImplementedToolResult(name, reason: "OHCI read dispatch needs the live driver adapter from FW-94.")
        case "asfw_irm_get_state", "asfw_irm_get_bandwidth", "asfw_irm_get_channels":
            return await dispatchIrmSnapshot(name, decoder: decoder)
        case "asfw_irm_list_allocations":
            return notImplementedToolResult(name, reason: "IRM allocation ownership is not exposed by the live driver adapter yet.")
        case "asfw_irm_allocate_channel", "asfw_irm_free_channel":
            return await dispatchIrmChannel(name, decoder: decoder, allocate: name == "asfw_irm_allocate_channel")
        case "asfw_irm_allocate_bandwidth", "asfw_irm_free_bandwidth":
            return await dispatchIrmBandwidth(name, decoder: decoder, allocate: name == "asfw_irm_allocate_bandwidth")
        case "asfw_avc_list_units":
            return await avcUnitInventoryResult(toolName: name)
        case "asfw_avc_get_subunit_capabilities":
            return await avcSubunitCapabilitiesResult(toolName: name, decoder: decoder)
        case "asfw_avc_get_subunit_descriptor", "asfw_fcp_get_recent_responses":
            return notImplementedToolResult(name, reason: "Recent FCP command/response records are not exposed by the live adapter yet.")
        case "asfw_fcp_send_command":
            return await dispatchFcpReadCommand(name, decoder: decoder)
        case "asfw_fcp_send_command_dev":
            return await dispatchFcpDeveloperCommand(name, decoder: decoder)
        case "asfw_cmp_list_plugs", "asfw_cmp_read_pcr":
            return notImplementedToolResult(name, reason: "CMP inspection dispatch needs protocol adapter support from FW-94.")
        case "asfw_cmp_write_pcr":
            return await dispatchCmpWritePcr(name, decoder: decoder)
        case "asfw_cmp_establish_connection", "asfw_cmp_break_connection":
            return await dispatchCmpConnection(name, decoder: decoder, establish: name == "asfw_cmp_establish_connection")
        case "asfw_sbp2_list_units", "asfw_sbp2_inspect_unit", "asfw_sbp2_get_session_status":
            return notImplementedToolResult(name, reason: "SBP-2 inspection dispatch needs protocol adapter support from FW-94.")
        case "asfw_sbp2_login_dev":
            return await dispatchSbp2Login(name, decoder: decoder)
        case "asfw_sbp2_submit_orb_dev":
            return await dispatchSbp2Orb(name, decoder: decoder)
        case "asfw_dice_decode_status":
            return notImplementedToolResult(name, reason: "DICE decode dispatch needs a concrete decoder surface.")
        case "asfw_dice_write_register":
            return await dispatchWriteQuadlet(name, decoder: decoder, protocolHint: "dice_tcat")
        case "asfw_tcat_write_application_block":
            return await dispatchWriteBlock(name, decoder: decoder, protocolHint: "dice_tcat")
        default:
            return notImplementedToolResult(name, reason: "Catalog tool \(name) has no dispatch arm.")
        }
    }

    private func dispatchReadQuadlet(_ name: String, decoder: ASFWMCPToolArgumentDecoder) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPReadQuadletRequest(address: try decoder.address())
            return transactionToolResult(name, await driver.executeReadQuadlet(request))
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchReadBlock(_ name: String, decoder: ASFWMCPToolArgumentDecoder) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPReadBlockRequest(address: try decoder.address(), length: try decoder.uint32("length"))
            if let error = request.validationError {
                return malformedTransactionResult(name, kind: request.kind, generation: request.address.generation, code: error)
            }
            return transactionToolResult(name, await driver.executeReadBlock(request))
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchWriteQuadlet(
        _ name: String,
        decoder: ASFWMCPToolArgumentDecoder,
        protocolHint: String? = nil
    ) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPWriteQuadletRequest(
                address: try decoder.address(),
                value: try decoder.uint32("value"),
                verifyReadback: try decoder.bool("verifyReadback", default: false)
            )
            let policyRequest = ASFWMCPPolicyRequest.forTransaction(
                kind: request.kind,
                address: request.address,
                currentGeneration: await currentGeneration(),
                protocolHint: protocolHint,
                protocolSupported: await protocolSupported(protocolHint),
                dryRun: try decoder.bool("dryRun", default: false)
            )
            return await dispatchMutatingTransaction(
                name,
                kind: request.kind,
                generation: request.address.generation,
                policyRequest: policyRequest
            ) {
                await driver.executeWriteQuadlet(request)
            }
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchWriteBlock(
        _ name: String,
        decoder: ASFWMCPToolArgumentDecoder,
        protocolHint: String? = nil
    ) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPWriteBlockRequest(
                address: try decoder.address(),
                payload: try decoder.bytes("payload"),
                verifyReadback: try decoder.bool("verifyReadback", default: false)
            )
            if let error = request.validationError {
                return malformedTransactionResult(name, kind: request.kind, generation: request.address.generation, code: error)
            }
            let policyRequest = ASFWMCPPolicyRequest.forTransaction(
                kind: request.kind,
                address: request.address,
                currentGeneration: await currentGeneration(),
                protocolHint: protocolHint,
                protocolSupported: await protocolSupported(protocolHint),
                dryRun: try decoder.bool("dryRun", default: false)
            )
            return await dispatchMutatingTransaction(
                name,
                kind: request.kind,
                generation: request.address.generation,
                policyRequest: policyRequest
            ) {
                await driver.executeWriteBlock(request)
            }
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchCompareSwap(
        _ name: String,
        decoder: ASFWMCPToolArgumentDecoder,
        protocolHint: String?
    ) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPCompareSwapRequest(
                address: try decoder.address(),
                expected: try decoder.uint32("expected"),
                swap: try decoder.uint32("swap")
            )
            let policyRequest = ASFWMCPPolicyRequest.forTransaction(
                kind: request.kind,
                address: request.address,
                currentGeneration: await currentGeneration(),
                protocolHint: protocolHint,
                protocolSupported: await protocolSupported(protocolHint),
                dryRun: try decoder.bool("dryRun", default: false)
            )
            return await dispatchMutatingTransaction(
                name,
                kind: request.kind,
                generation: request.address.generation,
                policyRequest: policyRequest
            ) {
                await driver.executeCompareSwap(request)
            }
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchOhciWrite(_ name: String, decoder: ASFWMCPToolArgumentDecoder) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPOhciRegisterWriteRequest(offset: try decoder.uint32("offset"), value: try decoder.uint32("value"))
            if request.offset % 4 != 0 {
                return malformedToolResult(name, reason: "offset must be quadlet-aligned.")
            }
            let generation = await currentGeneration()
            let policy = evaluateWritePolicy(request.policyRequest(currentGeneration: generation, dryRun: try decoder.bool("dryRun", default: false)))
            guard policy.reachesDriverWritePath else {
                return transactionToolResult(
                    name,
                    .policyRefusal(kind: .writeQuadlet, correlationId: correlationId(name), generation: generation, policy: policy)
                )
            }
            return notImplementedToolResult(name, reason: "OHCI register writes require the live driver adapter from FW-94.")
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchIrmChannel(_ name: String, decoder: ASFWMCPToolArgumentDecoder, allocate: Bool) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPIrmChannelRequest(channel: try decoder.uint32("channel"), generation: try decoder.uint32("generation"), allocate: allocate)
            if let error = request.validationError {
                return malformedTransactionResult(name, kind: .compareSwap, generation: request.generation, code: error)
            }
            return policyOnlyMutationResult(name, kind: .compareSwap, generation: request.generation, policyRequest: request.policyRequest(currentGeneration: await currentGeneration(), dryRun: try decoder.bool("dryRun", default: false)))
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchIrmBandwidth(_ name: String, decoder: ASFWMCPToolArgumentDecoder, allocate: Bool) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPIrmBandwidthRequest(allocationUnits: try decoder.uint32("allocationUnits"), generation: try decoder.uint32("generation"), allocate: allocate)
            if let error = request.validationError {
                return malformedTransactionResult(name, kind: .compareSwap, generation: request.generation, code: error)
            }
            return policyOnlyMutationResult(name, kind: .compareSwap, generation: request.generation, policyRequest: request.policyRequest(currentGeneration: await currentGeneration(), dryRun: try decoder.bool("dryRun", default: false)))
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchIrmSnapshot(_ name: String, decoder: ASFWMCPToolArgumentDecoder) async -> ASFWMCPToolCallResult {
        do {
            let snapshot = await driver.executeIRMSnapshot(
                ASFWMCPIrmSnapshotRequest(generation: try decoder.uint32("generation"))
            )
            return ASFWMCPToolCallResult(toolName: name, ok: snapshot.ok, data: snapshot.mcpValue, errors: [])
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchBusReset(_ name: String, decoder: ASFWMCPToolArgumentDecoder) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPBusResetRequest(
                generation: try decoder.uint32("generation"),
                shortReset: try decoder.bool("shortReset", default: false)
            )
            guard try decoder.bool("acknowledgeInterruption", default: false) else {
                return .failure(
                    toolName: name,
                    code: .policyDenied,
                    reason: "Bus reset interrupts active streams; set acknowledgeInterruption=true to proceed."
                )
            }

            let policy = evaluateWritePolicy(
                request.policyRequest(
                    currentGeneration: await currentGeneration(),
                    dryRun: try decoder.bool("dryRun", default: false)
                )
            )
            guard policy.reachesDriverWritePath else {
                let receipt = ASFWMCPBusResetReceipt(
                    requestedGeneration: request.generation,
                    acceptedGeneration: nil,
                    observedGeneration: await currentGeneration(),
                    shortReset: request.shortReset,
                    status: policy.isDryRun ? .dryRun : .denied,
                    correlationId: correlationId(name),
                    durationUsec: nil,
                    policy: policy
                )
                return ASFWMCPToolCallResult(toolName: name, ok: false, data: receipt.mcpValue, errors: [])
            }

            let liveReceipt = await driver.executeBusReset(request)
            let receipt = ASFWMCPBusResetReceipt(
                requestedGeneration: liveReceipt.requestedGeneration,
                acceptedGeneration: liveReceipt.acceptedGeneration,
                observedGeneration: liveReceipt.observedGeneration,
                shortReset: liveReceipt.shortReset,
                status: liveReceipt.status,
                correlationId: liveReceipt.correlationId,
                durationUsec: liveReceipt.durationUsec,
                policy: policy
            )
            return ASFWMCPToolCallResult(toolName: name, ok: receipt.ok, data: receipt.mcpValue, errors: [])
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchFcpDeveloperCommand(_ name: String, decoder: ASFWMCPToolArgumentDecoder) async -> ASFWMCPToolCallResult {
        do {
            let intent = try decoder.intent()
            let request = ASFWMCPFcpCommandRequest(
                targetGUID: try decoder.uint64("targetGuid"),
                address: try decoder.address(),
                intent: intent,
                payload: try decoder.bytes("payload")
            )
            if let error = request.validationError {
                return malformedTransactionResult(name, kind: .writeBlock, generation: request.address.generation, code: error)
            }
            guard let policyRequest = request.policyRequest(
                currentGeneration: await currentGeneration(),
                protocolSupported: await protocolSupported("avc"),
                dryRun: try decoder.bool("dryRun", default: false)
            ) else {
                return malformedToolResult(name, reason: "Developer FCP command requires a mutating intent.")
            }
            let policy = evaluateWritePolicy(policyRequest)
            guard policy.reachesDriverWritePath else {
                let receipt = ASFWMCPFcpCommandReceipt(
                    targetGUID: request.targetGUID,
                    expectedNodeId: request.address.nodeId,
                    expectedGeneration: request.address.generation,
                    observedNodeId: nil,
                    observedGeneration: await currentGeneration(),
                    response: nil,
                    status: policy.isDryRun ? .dryRun : .denied,
                    correlationId: correlationId(name),
                    durationUsec: nil,
                    policy: policy
                )
                return ASFWMCPToolCallResult(toolName: name, ok: false, data: receipt.mcpValue, errors: [])
            }
            var receipt = await driver.executeFCPCommand(request)
            receipt = ASFWMCPFcpCommandReceipt(
                targetGUID: receipt.targetGUID,
                expectedNodeId: receipt.expectedNodeId,
                expectedGeneration: receipt.expectedGeneration,
                observedNodeId: receipt.observedNodeId,
                observedGeneration: receipt.observedGeneration,
                response: receipt.response,
                status: receipt.status,
                correlationId: receipt.correlationId,
                durationUsec: receipt.durationUsec,
                policy: policy
            )
            return ASFWMCPToolCallResult(toolName: name, ok: receipt.ok, data: receipt.mcpValue, errors: [])
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchFcpReadCommand(_ name: String, decoder: ASFWMCPToolArgumentDecoder) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPFcpCommandRequest(
                targetGUID: try decoder.uint64("targetGuid"),
                address: try decoder.address(),
                intent: try decoder.intent(),
                payload: try decoder.bytes("payload")
            )
            if let error = request.validationError {
                return malformedTransactionResult(name, kind: .writeBlock, generation: request.address.generation, code: error)
            }
            guard request.hasMatchingReadOnlyCType else {
                return malformedToolResult(
                    name,
                    reason: "Read-only FCP accepts only STATUS (ctype 0x01) or SPECIFIC_INQUIRY (ctype 0x02) frames. Use asfw_fcp_send_command_dev for mutating commands."
                )
            }

            let receipt = await driver.executeFCPCommand(request)
            return ASFWMCPToolCallResult(toolName: name, ok: receipt.ok, data: receipt.mcpValue, errors: [])
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchCmpWritePcr(_ name: String, decoder: ASFWMCPToolArgumentDecoder) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPCmpPcrWriteRequest(
                address: try decoder.address(),
                plug: try decoder.uint32("plug"),
                expected: try decoder.uint32("expected"),
                swap: try decoder.uint32("swap")
            )
            if let error = request.validationError {
                return malformedTransactionResult(name, kind: .compareSwap, generation: request.address.generation, code: error)
            }
            return await dispatchMutatingTransaction(
                name,
                kind: .compareSwap,
                generation: request.address.generation,
                policyRequest: request.policyRequest(currentGeneration: await currentGeneration(), protocolSupported: await protocolSupported("cmp"), dryRun: try decoder.bool("dryRun", default: false))
            ) {
                await driver.executeCompareSwap(ASFWMCPCompareSwapRequest(address: request.address, expected: request.expected, swap: request.swap))
            }
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchCmpConnection(_ name: String, decoder: ASFWMCPToolArgumentDecoder, establish: Bool) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPCmpConnectionRequest(address: try decoder.address(), plug: try decoder.uint32("plug"), establish: establish)
            if let error = request.validationError {
                return malformedTransactionResult(name, kind: .compareSwap, generation: request.address.generation, code: error)
            }
            return policyOnlyMutationResult(
                name,
                kind: .compareSwap,
                generation: request.address.generation,
                policyRequest: request.policyRequest(currentGeneration: await currentGeneration(), protocolSupported: await protocolSupported("cmp"), dryRun: try decoder.bool("dryRun", default: false))
            )
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchSbp2Login(_ name: String, decoder: ASFWMCPToolArgumentDecoder) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPSbp2LoginRequest(address: try decoder.address())
            return policyOnlyMutationResult(
                name,
                kind: .writeBlock,
                generation: request.address.generation,
                policyRequest: request.policyRequest(currentGeneration: await currentGeneration(), protocolSupported: await protocolSupported("sbp2"), dryRun: try decoder.bool("dryRun", default: false))
            )
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchSbp2Orb(_ name: String, decoder: ASFWMCPToolArgumentDecoder) async -> ASFWMCPToolCallResult {
        do {
            let request = ASFWMCPSbp2OrbRequest(address: try decoder.address(), orb: try decoder.bytes("orb"))
            if let error = request.validationError {
                return malformedTransactionResult(name, kind: .writeBlock, generation: request.address.generation, code: error)
            }
            return policyOnlyMutationResult(
                name,
                kind: .writeBlock,
                generation: request.address.generation,
                policyRequest: request.policyRequest(currentGeneration: await currentGeneration(), protocolSupported: await protocolSupported("sbp2"), dryRun: try decoder.bool("dryRun", default: false))
            )
        } catch {
            return malformedToolResult(name, reason: error.localizedDescription)
        }
    }

    private func dispatchMutatingTransaction(
        _ name: String,
        kind: ASFWMCPTransactionKind,
        generation: UInt32,
        policyRequest: ASFWMCPPolicyRequest,
        execute: () async -> ASFWMCPTransactionResult
    ) async -> ASFWMCPToolCallResult {
        let policy = evaluateWritePolicy(policyRequest)
        guard policy.reachesDriverWritePath else {
            return transactionToolResult(
                name,
                .policyRefusal(kind: kind, correlationId: correlationId(name), generation: generation, policy: policy)
            )
        }

        return transactionToolResult(name, await execute())
    }

    private func policyOnlyMutationResult(
        _ name: String,
        kind: ASFWMCPTransactionKind,
        generation: UInt32,
        policyRequest: ASFWMCPPolicyRequest
    ) -> ASFWMCPToolCallResult {
        let policy = evaluateWritePolicy(policyRequest)
        guard policy.reachesDriverWritePath else {
            return transactionToolResult(
                name,
                .policyRefusal(kind: kind, correlationId: correlationId(name), generation: generation, policy: policy)
            )
        }
        return notImplementedToolResult(name, reason: "\(name) policy passed, but live protocol execution is deferred to FW-94.")
    }

    private func currentGeneration() async -> UInt32 {
        await driver.fetchTelemetrySnapshot(configuration: configuration).generation
    }

    private func protocolSupported(_ hint: String?) async -> Bool {
        guard let hint else { return true }
        return await driver.listNodes().contains { $0.protocolHints.contains(hint) }
    }

    private func transactionToolResult(_ name: String, _ result: ASFWMCPTransactionResult) -> ASFWMCPToolCallResult {
        ASFWMCPToolCallResult(toolName: name, ok: result.ok, data: result.mcpValue, errors: [])
    }

    private func malformedTransactionResult(
        _ name: String,
        kind: ASFWMCPTransactionKind,
        generation: UInt32,
        code: ASFWMCPErrorCode
    ) -> ASFWMCPToolCallResult {
        let result = ASFWMCPTransactionResult.malformed(kind: kind, correlationId: correlationId(name), generation: generation)
        return .failure(toolName: name, code: code, reason: "Request failed schema validation: \(code.rawValue).", data: result.mcpValue)
    }

    private func malformedToolResult(_ name: String, reason: String) -> ASFWMCPToolCallResult {
        .failure(toolName: name, code: .malformedRequest, reason: reason)
    }

    private func notImplementedToolResult(_ name: String, reason: String) -> ASFWMCPToolCallResult {
        .failure(
            toolName: name,
            code: .capabilityUnavailable,
            reason: reason,
            data: .object(["status": .string("notImplemented")])
        )
    }

    private func correlationId(_ name: String) -> String {
        "mcp-\(name)"
    }
}

private extension ASFWMCPCore {
    func avcUnitInventoryResult(toolName: String) async -> ASFWMCPToolCallResult {
        let units = await driver.listAVCUnits()
        return .success(
            toolName: toolName,
            data: .object([
                "kind": .string("avcUnitInventory"),
                "units": .array(units.map(\.mcpValue)),
            ])
        )
    }

    func avcSubunitCapabilitiesResult(
        toolName: String,
        decoder: ASFWMCPToolArgumentDecoder
    ) async -> ASFWMCPToolCallResult {
        do {
            let guid = try decoder.uint64("targetGuid")
            let type = try decoder.uint32("subunitType")
            let id = try decoder.uint32("subunitId")
            guard type <= UInt32(UInt8.max), id <= UInt32(UInt8.max) else {
                return malformedToolResult(toolName, reason: "subunitType and subunitId must fit in one byte")
            }

            let unit = await driver.listAVCUnits().first { $0.guid == guid }
            guard unit?.subunits.contains(where: { $0.type == UInt8(type) && $0.id == UInt8(id) }) == true else {
                return .failure(
                    toolName: toolName,
                    code: .capabilityUnavailable,
                    reason: "The requested AV/C subunit is not present in the current discovery snapshot."
                )
            }
            guard let capabilities = await driver.avcSubunitCapabilities(
                guid: guid, type: UInt8(type), id: UInt8(id)
            ) else {
                return .failure(
                    toolName: toolName,
                    code: .capabilityUnavailable,
                    reason: "The driver could not provide decoded capabilities for the requested AV/C subunit."
                )
            }

            return .success(
                toolName: toolName,
                data: .object([
                    "kind": .string("avcSubunitCapabilities"),
                    "targetGuid": .string(String(format: "0x%016llX", guid)),
                    "subunitType": .int(Int(type)),
                    "subunitId": .int(Int(id)),
                    "capabilities": capabilities.mcpValue,
                ])
            )
        } catch {
            return malformedToolResult(toolName, reason: error.localizedDescription)
        }
    }

    func capabilitiesResult(toolName: String) async -> ASFWMCPToolCallResult {
        let tools = await listTools()
        let groups = Set(tools.map(\.group)).sorted()
        return .success(
            toolName: toolName,
            data: .object([
                "runtimeMode": .string(configuration.mode.rawValue),
                "toolCount": .int(tools.count),
                "groups": .array(groups.map { .string($0) }),
                "developerWritesListed": .bool(configuration.canListDeveloperWriteTools),
                "rawDeveloperTierEnabled": .bool(configuration.rawDeveloperTierEnabled)
            ])
        )
    }

    func policyResult(toolName: String) async -> ASFWMCPToolCallResult {
        .success(
            toolName: toolName,
            data: .object([
                "runtimeMode": .string(configuration.mode.rawValue),
                "writePolicyAvailable": .bool(configuration.writePolicyAvailable),
                "swiftTestGatePassed": .bool(configuration.swiftTestGatePassed),
                "developerWritesListed": .bool(configuration.canListDeveloperWriteTools),
                "rawDeveloperTierEnabled": .bool(configuration.rawDeveloperTierEnabled)
            ])
        )
    }

    func listNodesResult(toolName: String) async -> ASFWMCPToolCallResult {
        let nodes = await driver.listNodes()
        return .success(
            toolName: toolName,
            data: .array(nodes.map(\.mcpValue))
        )
    }

    func nodeSummaryResult(toolName: String, decoder: ASFWMCPToolArgumentDecoder) async -> ASFWMCPToolCallResult {
        do {
            let nodeId = try decoder.uint32("nodeId")
            guard let node = await driver.listNodes().first(where: { $0.nodeId == nodeId }) else {
                return .failure(toolName: toolName, code: .capabilityUnavailable, reason: "No node \(nodeId) exists in the current snapshot.")
            }
            return .success(toolName: toolName, data: node.mcpValue)
        } catch {
            return .failure(toolName: toolName, code: .malformedRequest, reason: error.localizedDescription)
        }
    }

    func explainCapabilityResult(toolName: String, decoder: ASFWMCPToolArgumentDecoder) async -> ASFWMCPToolCallResult {
        do {
            let capability = try decoder.string("capability")
            let listedNames = Set(await listTools().map(\.name))
            let catalogTool = ASFWMCPToolCatalog.all.first { $0.name == capability || $0.group == capability }
            return .success(
                toolName: toolName,
                data: .object([
                    "capability": .string(capability),
                    "known": .bool(catalogTool != nil),
                    "listed": .bool(listedNames.contains(capability)),
                    "runtimeMode": .string(configuration.mode.rawValue)
                ])
            )
        } catch {
            return .failure(toolName: toolName, code: .malformedRequest, reason: error.localizedDescription)
        }
    }
}

private struct ASFWMCPToolArgumentDecoder {
    private let object: [String: ASFWMCPValue]

    init(_ arguments: ASFWMCPValue) throws {
        guard case .object(let object) = arguments else {
            throw ASFWMCPToolArgumentError.malformed("arguments must be an object")
        }
        self.object = object
    }

    func address() throws -> ASFWMCPAddress {
        ASFWMCPAddress(
            nodeId: try uint32("nodeId"),
            generation: try uint32("generation"),
            addressHigh: UInt16(try boundedUInt64("addressHigh", max: UInt64(UInt16.max))),
            addressLow: try uint32("addressLow")
        )
    }

    func uint32(_ key: String) throws -> UInt32 {
        UInt32(try boundedUInt64(key, max: UInt64(UInt32.max)))
    }

    func uint64(_ key: String) throws -> UInt64 {
        try boundedUInt64(key, max: UInt64.max)
    }

    func bool(_ key: String, default defaultValue: Bool) throws -> Bool {
        guard let value = object[key] else { return defaultValue }
        guard case .bool(let bool) = value else {
            throw ASFWMCPToolArgumentError.malformed("\(key) must be a boolean")
        }
        return bool
    }

    func string(_ key: String) throws -> String {
        guard let value = object[key] else {
            throw ASFWMCPToolArgumentError.malformed("missing required field \(key)")
        }
        guard case .string(let string) = value else {
            throw ASFWMCPToolArgumentError.malformed("\(key) must be a string")
        }
        return string
    }

    func intent() throws -> ASFWMCPAvcCommandIntent {
        let rawValue = try string("intent")
        guard let intent = ASFWMCPAvcCommandIntent(rawValue: rawValue) else {
            throw ASFWMCPToolArgumentError.malformed("intent must be one of \(ASFWMCPAvcCommandIntent.allCases.map(\.rawValue).joined(separator: ", "))")
        }
        return intent
    }

    func bytes(_ key: String) throws -> [UInt8] {
        guard let value = object[key] else {
            throw ASFWMCPToolArgumentError.malformed("missing required field \(key)")
        }
        guard case .array(let values) = value else {
            throw ASFWMCPToolArgumentError.malformed("\(key) must be an array of byte integers")
        }
        return try values.map { value in
            switch value {
            case .int(let int) where int >= 0 && int <= Int(UInt8.max):
                return UInt8(int)
            case .uint64(let uint) where uint <= UInt64(UInt8.max):
                return UInt8(uint)
            default:
                throw ASFWMCPToolArgumentError.malformed("\(key) must contain only byte integers")
            }
        }
    }

    private func boundedUInt64(_ key: String, max: UInt64) throws -> UInt64 {
        guard let value = object[key] else {
            throw ASFWMCPToolArgumentError.malformed("missing required field \(key)")
        }
        let raw: UInt64
        switch value {
        case .int(let int) where int >= 0:
            raw = UInt64(int)
        case .uint64(let uint):
            raw = uint
        default:
            throw ASFWMCPToolArgumentError.malformed("\(key) must be an unsigned integer")
        }
        guard raw <= max else {
            throw ASFWMCPToolArgumentError.malformed("\(key) exceeds \(max)")
        }
        return raw
    }
}

private enum ASFWMCPToolArgumentError: LocalizedError {
    case malformed(String)

    var errorDescription: String? {
        switch self {
        case .malformed(let reason):
            return reason
        }
    }
}

extension ASFWMCPTransactionResult {
    var mcpValue: ASFWMCPValue {
        let object: [String: ASFWMCPValue] = [
            "kind": .string(kind.rawValue),
            "ok": .bool(ok),
            "status": .string(status.rawValue),
            "rcode": rCode.map { .string($0) } ?? .null,
            "generation": .int(Int(generation)),
            "durationUsec": durationUsec.map { .uint64($0) } ?? .null,
            "correlationId": .string(correlationId),
            "payload": payload.map { .array($0.map { .int(Int($0)) }) } ?? .null,
            "decoded": decoded ?? .null,
            "policy": policy.map(\.mcpValue) ?? .null
        ]
        return .object(object)
    }
}

extension ASFWMCPPolicyDecision {
    var mcpValue: ASFWMCPValue {
        .object([
            "decision": .string(decision.rawValue),
            "reason": .string(reason),
            "errorCode": errorCode.map { .string($0.rawValue) } ?? .null,
            "requiredMode": requiredMode.map { .string($0.rawValue) } ?? .null,
            "requiredCapability": requiredCapability.map { .string($0) } ?? .null
        ])
    }
}

extension ASFWMCPNodeSummary {
    var mcpValue: ASFWMCPValue {
        .object([
            "nodeId": .int(Int(nodeId)),
            "address16": .string(address16),
            "guid": guid.map { .string($0) } ?? .null,
            "vendorId": vendorId.map { .string($0) } ?? .null,
            "modelId": modelId.map { .string($0) } ?? .null,
            "vendorName": vendorName.map { .string($0) } ?? .null,
            "modelName": modelName.map { .string($0) } ?? .null,
            "configRomCached": .bool(configRomCached),
            "protocolHints": .array(protocolHints.map { .string($0) })
        ])
    }
}
