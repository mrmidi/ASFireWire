import Foundation

// FW-80: register access tools with policy-aware writes.
//
// Reference pattern for the FW-81..85 fan-out: a protocol surface owns its tool
// catalog slice (here as an `extension ASFWMCPToolCatalog`), its request schemas,
// and its bridge to the FW-79 write policy — all in one file, so surfaces don't
// collide. Tool/visibility taxonomy: MCP_TOOL_TAXONOMY.md §5.4.
//
// Device registers are node addresses reached over async transactions (FW-78);
// their writes are policy-gated like any async write. OHCI/controller registers
// are host-side, classified as `.ohciController`, read-only for diagnostics, and
// writable only through the raw developer tier.

extension ASFWMCPToolCatalog {
    static let registerTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(name: "asfw_read_device_register", group: "register_access", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Read a device register/address-space value."),
        ASFWMCPToolDefinition(name: "asfw_read_device_register_block", group: "register_access", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Read a bounded block from a device register/address space."),
        ASFWMCPToolDefinition(name: "asfw_read_ohci_register", group: "register_access", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Read a host OHCI/controller register for diagnostics."),
        ASFWMCPToolDefinition(name: "asfw_snapshot_ohci_registers", group: "register_access", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return a bounded snapshot of selected OHCI registers."),
        ASFWMCPToolDefinition(name: "asfw_write_device_register", group: "register_access", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated device register write."),
        ASFWMCPToolDefinition(name: "asfw_write_device_register_block", group: "register_access", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated device register block write."),
        ASFWMCPToolDefinition(name: "asfw_write_ohci_register_dev", group: "register_access", visibility: .rawDeveloper, readOnly: false, idempotent: false, summary: "Raw developer-tier OHCI register write.")
    ]
}

// MARK: - Device register requests (node address space)

struct ASFWMCPDeviceRegisterReadRequest: Equatable {
    let address: ASFWMCPAddress
    /// Request decoded register fields alongside the raw value when ASFW can.
    let decode: Bool

    init(address: ASFWMCPAddress, decode: Bool = false) {
        self.address = address
        self.decode = decode
    }
}

struct ASFWMCPDeviceRegisterBlockReadRequest: Equatable {
    let address: ASFWMCPAddress
    let length: UInt32
    let decode: Bool

    init(address: ASFWMCPAddress, length: UInt32, decode: Bool = false) {
        self.address = address
        self.length = length
        self.decode = decode
    }

    /// Reuses the FW-78 block-read schema bounds.
    var validationError: ASFWMCPErrorCode? {
        ASFWMCPReadBlockRequest(address: address, length: length).validationError
    }
}

struct ASFWMCPDeviceRegisterWriteRequest: Equatable {
    let address: ASFWMCPAddress
    let value: UInt32
    let verifyReadback: Bool

    init(address: ASFWMCPAddress, value: UInt32, verifyReadback: Bool = false) {
        self.address = address
        self.value = value
        self.verifyReadback = verifyReadback
    }

    func policyRequest(currentGeneration: UInt32, dryRun: Bool = false) -> ASFWMCPPolicyRequest {
        .forTransaction(kind: .writeQuadlet, address: address, currentGeneration: currentGeneration, dryRun: dryRun)
    }
}

struct ASFWMCPDeviceRegisterBlockWriteRequest: Equatable {
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

    func policyRequest(currentGeneration: UInt32, dryRun: Bool = false) -> ASFWMCPPolicyRequest {
        .forTransaction(kind: .writeBlock, address: address, currentGeneration: currentGeneration, dryRun: dryRun)
    }
}

// MARK: - OHCI/controller registers (host-side)

struct ASFWMCPOhciRegisterReadRequest: Equatable {
    /// Controller register offset (quadlet-aligned).
    let offset: UInt32
    let decode: Bool

    init(offset: UInt32, decode: Bool = false) {
        self.offset = offset
        self.decode = decode
    }

    var validationError: ASFWMCPErrorCode? {
        offset % 4 == 0 ? nil : .malformedRequest
    }
}

struct ASFWMCPOhciRegisterSnapshotRequest: Equatable {
    let offsets: [UInt32]

    /// Snapshot is bounded so an agent cannot dump the whole register file.
    static let maxOffsets = 64

    var validationError: ASFWMCPErrorCode? {
        if offsets.isEmpty { return .malformedRequest }
        if offsets.count > Self.maxOffsets { return .payloadTooLarge }
        if offsets.contains(where: { $0 % 4 != 0 }) { return .malformedRequest }
        return nil
    }
}

struct ASFWMCPOhciRegisterWriteRequest: Equatable {
    let offset: UInt32
    let value: UInt32

    /// OHCI writes are host-side raw developer-tier escape hatches: there is no
    /// bus generation to pin, so the request is generation-neutral and always
    /// flagged as requiring the raw developer tier.
    func policyRequest(currentGeneration: UInt32, dryRun: Bool = false) -> ASFWMCPPolicyRequest {
        ASFWMCPPolicyRequest(
            operationType: .write,
            addressSpace: .ohciController,
            requestedGeneration: currentGeneration,
            currentGeneration: currentGeneration,
            dryRun: dryRun,
            requiresRawDeveloperTier: true
        )
    }
}
