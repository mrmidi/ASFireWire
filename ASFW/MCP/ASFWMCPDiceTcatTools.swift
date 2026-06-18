import Foundation

// FW-85: DICE/TCAT low-level register tools (MCP_TOOL_TAXONOMY.md §5.9).
//
// Low-level protocol register/application-space access — NOT audio-control UX
// (no phantom power, routing, mixer, or device-control semantics here). DICE/TCAT
// registers are node addresses reached over FW-78 transactions; writes are
// policy-gated via forTransaction with the "dice_tcat" hint and support readback
// verification. All tools use the "dice_tcat" protocol hint.

extension ASFWMCPToolCatalog {
    static let diceTcatTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(name: "asfw_dice_read_register", group: "dice_tcat", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Read a DICE register and optionally decode it.", requiredProtocolHints: ["dice_tcat"]),
        ASFWMCPToolDefinition(name: "asfw_dice_read_block", group: "dice_tcat", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Read a bounded DICE register block.", requiredProtocolHints: ["dice_tcat"]),
        ASFWMCPToolDefinition(name: "asfw_dice_decode_status", group: "dice_tcat", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Decode supplied or cached DICE status/register data.", requiredProtocolHints: ["dice_tcat"]),
        ASFWMCPToolDefinition(name: "asfw_tcat_read_application_block", group: "dice_tcat", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Read a TCAT application-space block.", requiredProtocolHints: ["dice_tcat"]),
        ASFWMCPToolDefinition(name: "asfw_dice_write_register", group: "dice_tcat", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated DICE register write.", requiredProtocolHints: ["dice_tcat"]),
        ASFWMCPToolDefinition(name: "asfw_tcat_write_application_block", group: "dice_tcat", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated TCAT application block write.", requiredProtocolHints: ["dice_tcat"])
    ]
}

/// Result of a readback verification after a register write.
struct ASFWMCPRegisterWriteVerification: Equatable {
    let requested: UInt32
    let readback: UInt32

    var verified: Bool { requested == readback }
}

struct ASFWMCPDiceRegisterReadRequest: Equatable {
    let address: ASFWMCPAddress
    let decode: Bool

    init(address: ASFWMCPAddress, decode: Bool = false) {
        self.address = address
        self.decode = decode
    }
}

struct ASFWMCPDiceBlockReadRequest: Equatable {
    let address: ASFWMCPAddress
    let length: UInt32

    var validationError: ASFWMCPErrorCode? {
        ASFWMCPReadBlockRequest(address: address, length: length).validationError
    }
}

struct ASFWMCPDiceRegisterWriteRequest: Equatable {
    let address: ASFWMCPAddress
    let value: UInt32
    let verifyReadback: Bool

    init(address: ASFWMCPAddress, value: UInt32, verifyReadback: Bool = false) {
        self.address = address
        self.value = value
        self.verifyReadback = verifyReadback
    }

    func policyRequest(currentGeneration: UInt32, protocolSupported: Bool = true, dryRun: Bool = false) -> ASFWMCPPolicyRequest {
        .forTransaction(
            kind: .writeQuadlet,
            address: address,
            currentGeneration: currentGeneration,
            protocolHint: "dice_tcat",
            protocolSupported: protocolSupported,
            dryRun: dryRun
        )
    }

    /// Verify a readback value against the written value.
    func verify(readback: UInt32) -> ASFWMCPRegisterWriteVerification {
        ASFWMCPRegisterWriteVerification(requested: value, readback: readback)
    }
}

struct ASFWMCPTcatApplicationBlockReadRequest: Equatable {
    let address: ASFWMCPAddress
    let length: UInt32

    var validationError: ASFWMCPErrorCode? {
        ASFWMCPReadBlockRequest(address: address, length: length).validationError
    }
}

struct ASFWMCPTcatApplicationBlockWriteRequest: Equatable {
    let address: ASFWMCPAddress
    let payload: [UInt8]
    let verifyReadback: Bool

    init(address: ASFWMCPAddress, payload: [UInt8], verifyReadback: Bool = false) {
        self.address = address
        self.payload = payload
        self.verifyReadback = verifyReadback
    }

    var validationError: ASFWMCPErrorCode? {
        ASFWMCPWriteBlockRequest(address: address, payload: payload).validationError
    }

    func policyRequest(currentGeneration: UInt32, protocolSupported: Bool = true, dryRun: Bool = false) -> ASFWMCPPolicyRequest {
        .forTransaction(
            kind: .writeBlock,
            address: address,
            currentGeneration: currentGeneration,
            protocolHint: "dice_tcat",
            protocolSupported: protocolSupported,
            dryRun: dryRun
        )
    }
}
