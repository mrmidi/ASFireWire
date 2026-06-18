import Testing
@testable import ASFW

struct MCPTransactionSchemaTests {
    private func address(
        high: UInt16 = 0xFFFF,
        low: UInt32 = 0xF0000800,
        generation: UInt32 = 17
    ) -> ASFWMCPAddress {
        ASFWMCPAddress(nodeId: 2, generation: generation, addressHigh: high, addressLow: low)
    }

    @Test func addressComposes48BitOffset() {
        let addr = ASFWMCPAddress(nodeId: 2, generation: 17, addressHigh: 0xFFFF, addressLow: 0xF0000400)
        #expect(addr.offset48 == 0xFFFFF0000400)
    }

    @Test func mutationClassificationMatchesPrimitive() {
        #expect(ASFWMCPTransactionKind.readQuadlet.isMutating == false)
        #expect(ASFWMCPTransactionKind.readBlock.isMutating == false)
        #expect(ASFWMCPTransactionKind.writeQuadlet.isMutating)
        #expect(ASFWMCPTransactionKind.writeBlock.isMutating)
        #expect(ASFWMCPTransactionKind.compareSwap.isMutating)
    }

    @Test func readBlockValidationAcceptsAlignedLength() {
        #expect(ASFWMCPReadBlockRequest(address: address(), length: 4).validationError == nil)
        #expect(ASFWMCPReadBlockRequest(address: address(), length: 2048).validationError == nil)
    }

    @Test func readBlockValidationRejectsZeroAndUnaligned() {
        #expect(ASFWMCPReadBlockRequest(address: address(), length: 0).validationError == .malformedRequest)
        #expect(ASFWMCPReadBlockRequest(address: address(), length: 6).validationError == .malformedRequest)
    }

    @Test func readBlockValidationRejectsOversizeLength() {
        #expect(ASFWMCPReadBlockRequest(address: address(), length: 4096).validationError == .payloadTooLarge)
    }

    @Test func writeBlockValidationFollowsSameRules() {
        #expect(ASFWMCPWriteBlockRequest(address: address(), payload: [0, 0, 0, 0]).validationError == nil)
        #expect(ASFWMCPWriteBlockRequest(address: address(), payload: []).validationError == .malformedRequest)
        #expect(ASFWMCPWriteBlockRequest(address: address(), payload: [1, 2, 3]).validationError == .malformedRequest)
        let oversize = [UInt8](repeating: 0, count: 4096)
        #expect(ASFWMCPWriteBlockRequest(address: address(), payload: oversize).validationError == .payloadTooLarge)
    }

    @Test func writeRequestsDefaultToNoReadback() {
        #expect(ASFWMCPWriteQuadletRequest(address: address(), value: 0x1234).verifyReadback == false)
        #expect(ASFWMCPWriteBlockRequest(address: address(), payload: [0, 0, 0, 0]).verifyReadback == false)
    }

    @Test func policyRefusalResultReflectsDeniedDecision() {
        let denied = ASFWMCPPolicyDecision(decision: .denied, reason: "nope", errorCode: .policyDenied)
        let result = ASFWMCPTransactionResult.policyRefusal(
            kind: .writeQuadlet,
            correlationId: "c1",
            generation: 17,
            policy: denied
        )
        #expect(result.ok == false)
        #expect(result.status == .denied)
        #expect(result.payload == nil)
        #expect(result.policy?.decision == .denied)
    }

    @Test func policyRefusalResultReflectsDryRunDecision() {
        let dryRun = ASFWMCPPolicyDecision(decision: .dryRunOnly, reason: "dry", errorCode: .dryRunOnly)
        let result = ASFWMCPTransactionResult.policyRefusal(
            kind: .writeBlock,
            correlationId: "c2",
            generation: 17,
            policy: dryRun
        )
        #expect(result.ok == false)
        #expect(result.status == .dryRun)
    }

    @Test func malformedResultCarriesNoPolicy() {
        let result = ASFWMCPTransactionResult.malformed(kind: .readBlock, correlationId: "c3", generation: 17)
        #expect(result.ok == false)
        #expect(result.status == .malformed)
        #expect(result.policy == nil)
    }

    // MARK: Bridge to the policy surface

    @Test func addressClassificationMapsKnownBlocks() {
        func addr(_ low: UInt32) -> ASFWMCPAddress {
            ASFWMCPAddress(nodeId: 2, generation: 17, addressHigh: 0xFFFF, addressLow: low)
        }
        #expect(ASFWMCPAddressSpace.classify(addr(0xF0000000)) == .csrCore)
        #expect(ASFWMCPAddressSpace.classify(addr(0xF0000400)) == .configRom)
        #expect(ASFWMCPAddressSpace.classify(addr(0xF0000800)) == .unitsSpace)
        #expect(ASFWMCPAddressSpace.classify(ASFWMCPAddress(nodeId: 2, generation: 17, addressHigh: 0, addressLow: 0x1000)) == .physicalMemory)
    }

    @Test func transactionKindMapsToOperationType() {
        #expect(ASFWMCPTransactionKind.readBlock.operationType == .read)
        #expect(ASFWMCPTransactionKind.writeQuadlet.operationType == .write)
        #expect(ASFWMCPTransactionKind.compareSwap.operationType == .compareSwap)
    }

    @Test func transactionRequestBuilderClassifiesAndPinsGeneration() {
        let addr = ASFWMCPAddress(nodeId: 2, generation: 16, addressHigh: 0xFFFF, addressLow: 0xF0000800)
        let req = ASFWMCPPolicyRequest.forTransaction(kind: .writeQuadlet, address: addr, currentGeneration: 17)
        #expect(req.addressSpace == .unitsSpace)
        #expect(req.operationType == .write)
        #expect(req.requestedGeneration == 16)
        #expect(req.currentGeneration == 17)
    }
}
