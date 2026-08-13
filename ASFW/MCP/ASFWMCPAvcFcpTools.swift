import Foundation

// FW-82: AV/C and FCP tools (MCP_TOOL_TAXONOMY.md §5.6).
//
// AV/C unit/subunit inspection and inquiry/status FCP commands are read-only.
// Control/notify/vendor-dependent FCP commands mutate device state and are
// policy-gated developer-write. All AV/C tools require the "avc" protocol hint,
// so they only appear for AV/C-capable nodes. AV/C frames are big-endian byte
// payloads written to the target's FCP command register (units space).

extension ASFWMCPToolCatalog {
    static let avcFcpTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(name: "asfw_avc_list_units", group: "avc_fcp", visibility: .readOnly, readOnly: true, idempotent: true, summary: "List AV/C units, subunits, and plugs.", requiredProtocolHints: ["avc"]),
        ASFWMCPToolDefinition(name: "asfw_avc_get_subunit_capabilities", group: "avc_fcp", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return decoded AV/C subunit capabilities.", requiredProtocolHints: ["avc"]),
        ASFWMCPToolDefinition(name: "asfw_avc_get_subunit_descriptor", group: "avc_fcp", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return bounded AV/C subunit descriptor bytes and parsed summary.", requiredProtocolHints: ["avc"]),
        ASFWMCPToolDefinition(name: "asfw_fcp_send_command", group: "avc_fcp", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Send an inquiry/status-only FCP/AV/C command.", requiredProtocolHints: ["avc"]),
        ASFWMCPToolDefinition(name: "asfw_fcp_get_recent_responses", group: "avc_fcp", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Inspect recent FCP command/response records.", requiredProtocolHints: ["avc"]),
        // Read-only despite issuing a transaction, on the same grounds as
        // asfw_fcp_send_command: it is an AV/C STATUS command and changes no
        // device state. It exists as its own tool because it is the ONLY command
        // that may be sent to M-Audio special firmware, and unlike the generic
        // FCP tools it takes no payload — the driver builds the frame.
        ASFWMCPToolDefinition(name: "asfw_avc_probe_signal_format", group: "avc_fcp", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Read a device's sample rate via unit PLUG SIGNAL FORMAT (STATUS). Safe on freeze-prone M-Audio special firmware.", requiredProtocolHints: ["avc"]),
        ASFWMCPToolDefinition(name: "asfw_apogee_duet_apply_format_dev", group: "avc_fcp", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Guardedly apply and verify an Apogee Duet OXFW AM824 format transition.", requiredProtocolHints: ["avc"]),
        ASFWMCPToolDefinition(name: "asfw_fcp_send_command_dev", group: "avc_fcp", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Developer-tier raw FCP command that may mutate device state.", requiredProtocolHints: ["avc"])
    ]
}

/// AV/C command intent (`ctype`). Only inquiry/status are non-mutating.
enum ASFWMCPAvcCommandIntent: String, Equatable, CaseIterable {
    case inquiry
    case status
    case control
    case notify
    case vendorDependent

    var isMutating: Bool {
        switch self {
        case .inquiry, .status:
            return false
        case .control, .notify, .vendorDependent:
            return true
        }
    }
}

/// Generation-bound AV/C discovery evidence exposed to MCP. This deliberately
/// mirrors only decoded driver state; it is not a request to probe hardware.
struct ASFWMCPAVCUnitSummary: Equatable {
    struct Subunit: Equatable {
        let type: UInt8
        let id: UInt8
        let sourcePlugCount: UInt8
        let destinationPlugCount: UInt8
    }

    let id: UnitInstanceID
    let observedGuid: UInt64
    let generation: UInt32
    let nodeId: UInt32
    let vendorId: UInt32
    let modelId: UInt32
    let isoInputPlugCount: UInt8
    let isoOutputPlugCount: UInt8
    let externalInputPlugCount: UInt8
    let externalOutputPlugCount: UInt8
    let subunits: [Subunit]
}

struct ASFWMCPAVCSubunitCapabilities: Equatable {
    struct Plug: Equatable {
        struct SignalBlock: Equatable {
            let formatCode: UInt8
            let channelCount: UInt8
        }

        struct SupportedFormat: Equatable {
            let sampleRateCode: UInt8
            let formatCode: UInt8
            let channelCount: UInt8
        }

        let id: UInt8
        let isInput: Bool
        let type: UInt8
        let name: String
        let signalBlocks: [SignalBlock]
        let supportedFormats: [SupportedFormat]
    }

    let hasAudio: Bool
    let hasMIDI: Bool
    let hasSMPTE: Bool
    let currentRateCode: UInt8
    let supportedRatesMask: UInt32
    let plugs: [Plug]
}

extension ASFWMCPAVCUnitSummary {
    var mcpValue: ASFWMCPValue {
        .object([
            "deviceInstanceId": .uint64(id.device.rawValue),
            "unitDirectoryOffset": .uint64(UInt64(id.unitDirectoryOffset)),
            "observedGuid": .string(String(format: "0x%016llX", observedGuid)),
            "generation": .int(Int(generation)),
            "nodeId": .int(Int(nodeId)),
            "vendorId": .string(String(format: "0x%06X", vendorId)),
            "modelId": .string(String(format: "0x%06X", modelId)),
            "plugs": .object([
                "isoInput": .int(Int(isoInputPlugCount)),
                "isoOutput": .int(Int(isoOutputPlugCount)),
                "externalInput": .int(Int(externalInputPlugCount)),
                "externalOutput": .int(Int(externalOutputPlugCount)),
            ]),
            "subunits": .array(subunits.map {
                .object([
                    "type": .int(Int($0.type)),
                    "id": .int(Int($0.id)),
                    "sourcePlugCount": .int(Int($0.sourcePlugCount)),
                    "destinationPlugCount": .int(Int($0.destinationPlugCount)),
                ])
            }),
        ])
    }
}

extension ASFWMCPAVCSubunitCapabilities {
    var mcpValue: ASFWMCPValue {
        .object([
            "hasAudio": .bool(hasAudio),
            "hasMIDI": .bool(hasMIDI),
            "hasSMPTE": .bool(hasSMPTE),
            "currentRateCode": .int(Int(currentRateCode)),
            "supportedRatesMask": .uint64(UInt64(supportedRatesMask)),
            "plugs": .array(plugs.map { plug in
                .object([
                    "id": .int(Int(plug.id)),
                    "isInput": .bool(plug.isInput),
                    "type": .int(Int(plug.type)),
                    "name": .string(plug.name),
                    "signalBlocks": .array(plug.signalBlocks.map {
                        .object([
                            "formatCode": .int(Int($0.formatCode)),
                            "channelCount": .int(Int($0.channelCount)),
                        ])
                    }),
                    "supportedFormats": .array(plug.supportedFormats.map {
                        .object([
                            "sampleRateCode": .int(Int($0.sampleRateCode)),
                            "formatCode": .int(Int($0.formatCode)),
                            "channelCount": .int(Int($0.channelCount)),
                        ])
                    }),
                ])
            }),
        ])
    }
}

/// A raw FCP/AV/C command directed at a node's FCP command register.
/// Request for the closed signal-format probe.
///
/// Note what is absent: no payload, no opcode, no ctype. The driver builds the
/// whole frame. That is the difference between this and
/// `ASFWMCPFcpCommandRequest`, and the reason it is a separate type rather than
/// a preset payload — a preset can be edited by the next caller.
struct ASFWMCPSignalFormatProbeRequest: Equatable {
    let targetUnitID: UnitInstanceID
    let plugDirection: SignalFormatPlugDirection
    let plugID: UInt8
}

struct ASFWMCPSignalFormatProbeReceipt: Equatable {
    let targetUnitID: UnitInstanceID
    let plugDirection: SignalFormatPlugDirection
    let plugID: UInt8
    /// nil when the probe could not be issued at all; the AV/C-level refusals
    /// (NOT IMPLEMENTED, REJECTED, IN TRANSITION) arrive as a result, not as nil.
    let result: SignalFormatProbeResult?
    let status: ASFWMCPTransactionStatus
    let correlationId: String

    var ok: Bool { status == .ok }

    var mcpValue: ASFWMCPValue {
        var fields: [String: ASFWMCPValue] = [
            "deviceInstanceId": .uint64(targetUnitID.device.rawValue),
            "unitDirectoryOffset": .uint64(UInt64(targetUnitID.unitDirectoryOffset)),
            "plugDirection": .string(plugDirection == .input ? "input" : "output"),
            "plugId": .int(Int(plugID)),
            "status": .string(status.rawValue),
            "correlationId": .string(correlationId)
        ]
        if let result {
            fields["outcome"] = .string(String(describing: result.outcome))
            fields["transportStatus"] = .string(String(describing: result.transportStatus))
            fields["sfc"] = .int(Int(result.sfc))
            fields["summary"] = .string(result.summary)
            if result.outcome == .decoded {
                fields["sampleRateHz"] = .uint64(UInt64(result.sampleRateHz))
            }
        }
        return .object(fields)
    }
}

struct ASFWMCPFcpCommandRequest: Equatable {
    /// Runtime unit identity selected by the caller. The live adapter validates
    /// its current route immediately before issuing the command.
    let targetUnitID: UnitInstanceID
    /// Target node's FCP command register address.
    let address: ASFWMCPAddress
    let intent: ASFWMCPAvcCommandIntent
    /// AV/C frame bytes in bus (big-endian) order.
    let payload: [UInt8]

    /// FCP frames are bounded to 512 bytes.
    static let maxPayload = 512

    var validationError: ASFWMCPErrorCode? {
        if payload.count > Self.maxPayload { return .payloadTooLarge }
        if payload.isEmpty { return .malformedRequest }
        return nil
    }

    /// Policy request for mutating intents, or nil for inquiry/status reads,
    /// which are not gated by write policy.
    func policyRequest(
        currentGeneration: UInt32,
        protocolSupported: Bool = true,
        dryRun: Bool = false
    ) -> ASFWMCPPolicyRequest? {
        guard intent.isMutating else { return nil }
        return .forTransaction(
            kind: .writeBlock,
            address: address,
            currentGeneration: currentGeneration,
            protocolHint: "avc",
            protocolSupported: protocolSupported,
            dryRun: dryRun
        )
    }

    /// Read-only MCP calls must prove that the AV/C ctype byte agrees with
    /// their declared intent. This prevents a caller from labelling a CONTROL
    /// frame as STATUS to bypass the developer-write gate.
    var hasMatchingReadOnlyCType: Bool {
        guard let ctype = payload.first else { return false }
        switch intent {
        case .status:
            return ctype == 0x01
        case .inquiry:
            return ctype == 0x02
        case .control, .notify, .vendorDependent:
            return false
        }
    }
}

/// Receipt for a developer FCP command. `observedNodeId` and
/// `observedGeneration` describe the preflight route validated by this MCP
/// slice. FW-100 will replace those observations with the transport's exact
/// write-completion attempt context before this is used as a response matcher
/// diagnostic.
/// One retained FCP command/response exchange.
///
/// Scope: this ring records only exchanges issued **through MCP**. FCP traffic the
/// driver originates itself (discovery, AV/C bring-up, CMP sequencing) does not appear
/// here, so the tool reports `scope: "mcpIssued"` and an absence here is not evidence
/// that no FCP occurred. Use the driver log ring's `FCP` category for driver-side
/// chronology.
struct ASFWMCPFcpRecord: Equatable {
    let correlationId: String
    let targetUnitID: UnitInstanceID
    let nodeId: UInt32?
    let generation: UInt32
    let intent: String
    let request: [UInt8]
    let response: [UInt8]?
    let status: String
    let durationUsec: UInt64?
    let capturedAtUptimeNs: UInt64
}

extension ASFWMCPFcpRecord {
    var mcpValue: ASFWMCPValue {
        .object([
            "correlationId": .string(correlationId),
            "deviceInstanceId": .uint64(targetUnitID.device.rawValue),
            "unitDirectoryOffset": .uint64(UInt64(targetUnitID.unitDirectoryOffset)),
            "nodeId": nodeId.map { .int(Int($0)) } ?? .null,
            "generation": .int(Int(generation)),
            "intent": .string(intent),
            "request": .string(request.map { String(format: "%02X", $0) }.joined()),
            "response": response.map { .string($0.map { String(format: "%02X", $0) }.joined()) } ?? .null,
            "status": .string(status),
            "durationUsec": durationUsec.map { .uint64($0) } ?? .null,
            "capturedAtUptimeNs": .uint64(capturedAtUptimeNs)
        ])
    }
}

struct ASFWMCPFcpCommandReceipt: Equatable {
    let targetUnitID: UnitInstanceID
    let expectedNodeId: UInt32
    let expectedGeneration: UInt32
    let observedNodeId: UInt32?
    let observedGeneration: UInt32
    let response: [UInt8]?
    let status: ASFWMCPTransactionStatus
    let correlationId: String
    let durationUsec: UInt64?
    let policy: ASFWMCPPolicyDecision?

    var ok: Bool { status == .ok }
}

extension ASFWMCPFcpCommandReceipt {
    var mcpValue: ASFWMCPValue {
        .object([
            "kind": .string("fcpCommand"),
            "ok": .bool(ok),
            "status": .string(status.rawValue),
            "deviceInstanceId": .uint64(targetUnitID.device.rawValue),
            "unitDirectoryOffset": .uint64(UInt64(targetUnitID.unitDirectoryOffset)),
            "expectedNodeId": .int(Int(expectedNodeId)),
            "expectedGeneration": .int(Int(expectedGeneration)),
            "observedNodeId": observedNodeId.map { .int(Int($0)) } ?? .null,
            "observedGeneration": .int(Int(observedGeneration)),
            "correlationId": .string(correlationId),
            "durationUsec": durationUsec.map { .uint64($0) } ?? .null,
            "response": response.map { .array($0.map { .int(Int($0)) }) } ?? .null,
            "policy": policy.map(\.mcpValue) ?? .null
        ])
    }
}
