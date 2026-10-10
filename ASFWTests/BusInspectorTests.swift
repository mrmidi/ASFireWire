import Testing
@testable import ASFW

@MainActor
struct BusInspectorTests {
    private func node(_ id: UInt8) -> TopologyNode {
        TopologyNode(id: id, nodeId: id, portCount: 1, gapCount: 5, powerClass: 1,
                     maxSpeedMbps: 400, isIRMCandidate: false, linkActive: true,
                     initiatedReset: false, isRoot: id == 0, parentPort: nil,
                     portStates: [.notPresent], links: [])
    }

    private func topology(_ generation: UInt32, nodeID: UInt8 = 1) -> TopologySnapshot {
        TopologySnapshot(generation: generation, capturedAt: 0, nodeCount: 2,
                         rootNodeId: 0, irmNodeId: nil, localNodeId: 0, gapCount: 5,
                         busBase16: 0, nodes: [node(0), node(nodeID)], warnings: [])
    }

    private func device(_ generation: UInt32, nodeID: UInt8 = 1,
                        state: FWDeviceState = .ready, kind: UInt8 = 1) -> FWDeviceInfo {
        FWDeviceInfo(id: 42, guid: 42, vendorId: 1, modelId: 2,
                     vendorName: "Example", modelName: "Interface", nodeId: nodeID,
                     generation: generation, state: state, units: [], deviceKind: kind)
    }

    @Test func everydayFeaturesAndReportsStayOutsideAdvancedTools() {
        let sections: [ModernContentView.SidebarSection] = [
            .dvCapture, .busInspector, .avcUnits, .duet, .audioAnalyzer,
            .diagnostics, .diceReport, .avcReport, .logs, .loggingSettings
        ]
        #expect(sections.allSatisfy { !$0.requiresAdvancedTools })
        #expect(ModernContentView.SidebarSection.dvCapture.group == .video)
        #expect(ModernContentView.SidebarSection.avcCommands.requiresAdvancedTools)
        #expect(ModernContentView.SidebarSection.busReset.requiresAdvancedTools)
        #expect(!ModernContentView.SidebarSection.allCases.contains { $0.rawValue == "Saffire" })
        #if DEBUG
        #expect(AdvancedToolsSettings.defaultEnabled)
        #else
        #expect(!AdvancedToolsSettings.defaultEnabled)
        #endif
    }

    @Test func topologyIncludesLocalAndPendingNodesWithoutInventingDevices() {
        let items = BusInspectorNode.inventory(topology: topology(2), devices: [device(1)])
        #expect(items.count == 2)
        #expect(items.first?.isLocal == true)
        #expect(items.allSatisfy { $0.device == nil })
        #expect(items.contains { $0.isAVCDevice(registeredGUIDs: [42]) } == false)
    }

    @Test func identitySurvivesNodeRenumberingButUnknownNodesAreGenerationScoped() throws {
        let old = BusInspectorNode.inventory(topology: topology(1), devices: [device(1)])
        let new = BusInspectorNode.inventory(topology: topology(2, nodeID: 2),
                                             devices: [device(2, nodeID: 2)])
        let oldDevice = try #require(old.first { $0.device != nil })
        let newDevice = try #require(new.first { $0.device != nil })
        #expect(oldDevice.id == newDevice.id)
        #expect(oldDevice.node.nodeId != newDevice.node.nodeId)
        #expect(old.first?.id != new.first?.id)
    }

    @Test func avcPresenceRequiresCurrentLiveDiscovery() {
        let current = BusInspectorNode.inventory(topology: topology(2), devices: [device(2)])
        #expect(current.contains { $0.isAVCDevice(registeredGUIDs: []) })
        let removed = BusInspectorNode.inventory(topology: topology(2), devices: [device(2, state: .terminated)])
        #expect(removed.contains { $0.isAVCDevice(registeredGUIDs: [42]) } == false)
        let storage = BusInspectorNode.inventory(topology: topology(2), devices: [device(2, kind: 4)])
        #expect(storage.contains { $0.isAVCDevice(registeredGUIDs: []) } == false)
        #expect(BusInspectorNode.inventory(topology: nil, devices: [device(2)]).isEmpty)
    }

    @Test func dicePresenceUsesROMIdentityWithoutTreatingAVCAsDICE() {
        let avc = BusInspectorNode.inventory(topology: topology(2), devices: [device(2)])
        #expect(!avc.contains { $0.isDICEDevice })
        let unit = FWUnitInfo(specId: 0x00130E, swVersion: 1, state: .ready, romOffset: 7,
                              managementAgentOffset: nil, lun: nil, unitCharacteristics: nil,
                              fastStart: nil, vendorName: nil, productName: nil)
        let dice = FWDeviceInfo(id: 42, guid: 0x00130E0400400001,
                                vendorId: 0x00130E, modelId: 1, vendorName: "Example",
                                modelName: "DICE", nodeId: 1, generation: 2, state: .ready,
                                units: [unit], deviceKind: 3)
        let current = BusInspectorNode.inventory(topology: topology(2), devices: [dice])
        #expect(current.contains { $0.isDICEDevice })
        #expect(!current.contains { $0.isAVCDevice(registeredGUIDs: []) })
        #expect(!BusInspectorNode.inventory(topology: topology(3), devices: [dice])
            .contains { $0.isDICEDevice })
    }

    @Test func nodeSelectionClearsPreviousROMStateImmediately() {
        let model = RomExplorerViewModel()
        model.error = "Previous node error"
        model.statusMessage = "Previous node status"
        model.isLoading = true
        model.liveReadState = .polling(2)
        model.selectNode(node(2))
        #expect(model.selectedNode?.nodeId == 2)
        #expect(model.error == nil)
        #expect(model.statusMessage == nil)
        #expect(model.isLoading == false)
        #expect(model.liveReadState == .idle)
    }
}
