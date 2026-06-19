import Testing
@testable import ASFW

struct MCPRegisterToolsTests {
    private func config(
        _ mode: ASFWMCPRuntimeMode,
        writePolicyAvailable: Bool = false,
        swiftTestGatePassed: Bool = false,
        rawDeveloperTierEnabled: Bool = false
    ) -> ASFWMCPRuntimeConfiguration {
        ASFWMCPRuntimeConfiguration(
            mode: mode,
            writePolicyAvailable: writePolicyAvailable,
            swiftTestGatePassed: swiftTestGatePassed,
            rawDeveloperTierEnabled: rawDeveloperTierEnabled
        )
    }

    private var gateOpen: ASFWMCPRuntimeConfiguration {
        config(.developerWriteEnabled, writePolicyAvailable: true, swiftTestGatePassed: true)
    }

    private func deviceAddress(low: UInt32 = 0xF0000800) -> ASFWMCPAddress {
        ASFWMCPAddress(nodeId: 2, generation: 17, addressHigh: 0xFFFF, addressLow: low)
    }

    private func toolNames(_ cfg: ASFWMCPRuntimeConfiguration) async -> Set<String> {
        let core = ASFWMCPCore(configuration: cfg, driver: MockASFWDriverControl())
        return await Set(core.listTools().map(\.name))
    }

    // MARK: Catalog

    @Test func registerCatalogExposesFullSurface() {
        let names = Set(ASFWMCPToolCatalog.registerTools.map(\.name))
        #expect(names == [
            "asfw_read_device_register",
            "asfw_read_device_register_block",
            "asfw_read_ohci_register",
            "asfw_snapshot_ohci_registers",
            "asfw_write_device_register",
            "asfw_write_device_register_block",
            "asfw_write_ohci_register_dev"
        ])
    }

    @Test func registerToolsAreReachableThroughTheAggregator() {
        let all = Set(ASFWMCPToolCatalog.all.map(\.name))
        #expect(all.isSuperset(of: ["asfw_read_device_register", "asfw_write_device_register"]))
    }

    @Test func ohciWriteIsRawDeveloperTier() throws {
        let ohciWrite = try #require(ASFWMCPToolCatalog.registerTools.first { $0.name == "asfw_write_ohci_register_dev" })
        #expect(ohciWrite.visibility == .rawDeveloper)
        let deviceWrite = try #require(ASFWMCPToolCatalog.registerTools.first { $0.name == "asfw_write_device_register" })
        #expect(deviceWrite.visibility == .developerWrite)
    }

    // MARK: Discovery

    @Test func readOnlyModeListsRegisterReadsAndHidesWrites() async {
        let names = await toolNames(config(.readOnlyDeveloper))
        #expect(names.isSuperset(of: [
            "asfw_read_device_register",
            "asfw_read_device_register_block",
            "asfw_read_ohci_register",
            "asfw_snapshot_ohci_registers"
        ]))
        #expect(names.contains("asfw_write_device_register") == false)
        #expect(names.contains("asfw_write_device_register_block") == false)
        #expect(names.contains("asfw_write_ohci_register_dev") == false)
    }

    @Test func developerWriteListsDeviceWritesButNotRawOhci() async {
        let names = await toolNames(gateOpen)
        #expect(names.contains("asfw_write_device_register"))
        #expect(names.contains("asfw_write_device_register_block"))
        #expect(names.contains("asfw_write_ohci_register_dev") == false)
    }

    @Test func rawDeveloperTierListsOhciWrite() async {
        let names = await toolNames(config(.developerWriteEnabled, writePolicyAvailable: true, swiftTestGatePassed: true, rawDeveloperTierEnabled: true))
        #expect(names.contains("asfw_write_ohci_register_dev"))
    }

    // MARK: Schema validation

    @Test func deviceBlockReadReusesTransactionBounds() {
        #expect(ASFWMCPDeviceRegisterBlockReadRequest(address: deviceAddress(), length: 4).validationError == nil)
        #expect(ASFWMCPDeviceRegisterBlockReadRequest(address: deviceAddress(), length: 6).validationError == .malformedRequest)
        #expect(ASFWMCPDeviceRegisterBlockReadRequest(address: deviceAddress(), length: 4096).validationError == .payloadTooLarge)
    }

    @Test func ohciSnapshotIsBoundedAndAligned() {
        #expect(ASFWMCPOhciRegisterSnapshotRequest(offsets: [0, 4, 8]).validationError == nil)
        #expect(ASFWMCPOhciRegisterSnapshotRequest(offsets: []).validationError == .malformedRequest)
        #expect(ASFWMCPOhciRegisterSnapshotRequest(offsets: [2]).validationError == .malformedRequest)
        let tooMany = (0..<65).map { UInt32($0 * 4) }
        #expect(ASFWMCPOhciRegisterSnapshotRequest(offsets: tooMany).validationError == .payloadTooLarge)
    }

    @Test func ohciReadRejectsUnalignedOffset() {
        #expect(ASFWMCPOhciRegisterReadRequest(offset: 8).validationError == nil)
        #expect(ASFWMCPOhciRegisterReadRequest(offset: 3).validationError == .malformedRequest)
    }

    // MARK: Policy gating

    @Test func deviceRegisterWriteIsPolicyGated() {
        let request = ASFWMCPDeviceRegisterWriteRequest(address: deviceAddress(), value: 0x1234)
        let readOnly = ASFWMCPWritePolicyEngine(configuration: config(.readOnlyDeveloper))
            .evaluate(request.policyRequest(currentGeneration: 17))
        #expect(readOnly.decision == .requiresDeveloperMode)
        #expect(readOnly.reachesDriverWritePath == false)

        let open = ASFWMCPWritePolicyEngine(configuration: gateOpen)
            .evaluate(request.policyRequest(currentGeneration: 17))
        #expect(open.decision == .allowed)
    }

    @Test func deviceRegisterWriteRefusesStaleGeneration() {
        let request = ASFWMCPDeviceRegisterWriteRequest(address: deviceAddress(), value: 0x1234)
        let decision = ASFWMCPWritePolicyEngine(configuration: gateOpen)
            .evaluate(request.policyRequest(currentGeneration: 18))
        #expect(decision.decision == .staleGeneration)
    }

    @Test func ohciRegisterWriteRequiresRawTier() {
        let request = ASFWMCPOhciRegisterWriteRequest(offset: 0x100, value: 0xABCD)
        let withoutTier = ASFWMCPWritePolicyEngine(configuration: gateOpen)
            .evaluate(request.policyRequest(currentGeneration: 17))
        #expect(withoutTier.decision == .denied)
        #expect(withoutTier.requiredCapability == "rawDeveloperTier")

        let withTier = ASFWMCPWritePolicyEngine(
            configuration: config(.developerWriteEnabled, writePolicyAvailable: true, swiftTestGatePassed: true, rawDeveloperTierEnabled: true)
        ).evaluate(request.policyRequest(currentGeneration: 17))
        #expect(withTier.decision == .allowed)
    }
}
