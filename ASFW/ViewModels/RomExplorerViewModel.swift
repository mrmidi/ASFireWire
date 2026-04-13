//
//  RomExplorerViewModel.swift
//  ASFW
//
//  ViewModel for Config ROM exploration
//  Supports both file loading and live driver ROM reading
//

import Foundation
import Combine
import SwiftUI

@MainActor
final class RomExplorerViewModel: ObservableObject {
    enum SourceType {
        case file
        case driver
    }

    enum LiveReadState: Equatable {
        case idle
        case loadingCache
        case triggeringRead
        case polling(Int)
    }

    @Published var rom: RomTree?
    @Published var error: String?
    @Published var selection: DirectoryEntry?
    @Published var showBusInfo: Bool = false
    @Published var showInterpreted: Bool = true
    @Published var isLoading: Bool = false
    @Published var sourceType: SourceType = .file
    @Published var statusMessage: String?
    @Published var liveReadState: LiveReadState = .idle

    // Node selection for driver ROM reading
    @Published var selectedNode: TopologyNode?
    @Published var availableNodes: [TopologyNode] = []

    // Reference to connector for driver ROM reading
    private var connector: ASFWDriverConnector?
    private var topologyViewModel: TopologyViewModel?

    // Summarized info for UI (vendor/model names, modalias, units)
    var summary: RomSummary? {
        guard let rom else { return nil }
        return Summarizer.summarize(tree: rom)
    }

    var topologyGeneration: UInt16? {
        guard let gen = topologyViewModel?.topology?.generation else { return nil }
        return UInt16(gen)
    }

    var canReadSelectedNode: Bool {
        selectedNode != nil && connector != nil
    }

    init(connector: ASFWDriverConnector? = nil, topologyViewModel: TopologyViewModel? = nil) {
        self.connector = connector
        self.topologyViewModel = topologyViewModel
        if let topology = topologyViewModel?.topology {
            self.availableNodes = topology.nodes
        }
    }

    func setConnector(_ connector: ASFWDriverConnector, topologyViewModel: TopologyViewModel) {
        self.connector = connector
        self.topologyViewModel = topologyViewModel
        refreshAvailableNodes()
    }

    func refreshTopology() {
        topologyViewModel?.refresh()
        refreshAvailableNodes()
    }

    func refreshAvailableNodes() {
        if let topology = topologyViewModel?.topology {
            availableNodes = topology.nodes
            if let selected = selectedNode {
                selectedNode = topology.nodes.first(where: { $0.nodeId == selected.nodeId })
            }
        } else {
            availableNodes = []
            selectedNode = nil
        }
    }

    func selectNode(_ node: TopologyNode?) {
        selectedNode = node
        if node == nil {
            rom = nil
            error = nil
            statusMessage = nil
            selection = nil
            showBusInfo = false
        }
    }

    // MARK: - File Loading

    func open(url: URL) {
        isLoading = true
        error = nil
        statusMessage = "Parsing ROM file..."
        sourceType = .file
        liveReadState = .idle

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                let romTree = try RomParser.parse(fileURL: url)
                DispatchQueue.main.async {
                    self?.rom = romTree
                    self?.error = nil
                    self?.selection = nil
                    self?.showBusInfo = true
                    self?.isLoading = false
                    self?.statusMessage = "Loaded ROM file (\(romTree.rawROM.count) bytes)"
                }
            } catch {
                DispatchQueue.main.async {
                    self?.rom = nil
                    self?.error = String(describing: error)
                    self?.statusMessage = nil
                    self?.isLoading = false
                }
            }
        }
    }

    // MARK: - Driver ROM Reading

    func loadROMFromSelectedNodeCache() {
        guard let node = selectedNode else {
            error = "Select a node first"
            return
        }
        loadROMFromNode(node)
    }

    func loadROMFromNode(_ node: TopologyNode) {
        guard let connector = connector else {
            error = "Driver not connected"
            return
        }
        guard let gen = topologyGeneration else {
            error = "Topology generation unknown. Refresh topology and try again."
            return
        }

        isLoading = true
        error = nil
        statusMessage = "Checking ROM cache for node \(node.nodeId)..."
        sourceType = .driver
        selectedNode = node
        liveReadState = .loadingCache

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            guard let data = connector.getConfigROM(nodeId: node.nodeId, generation: gen) else {
                DispatchQueue.main.async {
                    self.isLoading = false
                    self.liveReadState = .idle
                    self.rom = nil
                    self.error = nil
                    self.statusMessage = "ROM not cached for node \(node.nodeId). Click Read ROM."
                }
                return
            }
            DispatchQueue.main.async {
                self.parseAndPublishROM(data: data,
                                        sourceType: .driver,
                                        statusMessage: "ROM loaded from cache (\(data.count) bytes)")
            }
        }
    }

    func triggerROMReadForSelectedNode() {
        guard let node = selectedNode else {
            error = "Select a node first"
            return
        }
        triggerROMRead(nodeId: node.nodeId)
    }

    func triggerROMRead(nodeId: UInt8) {
        guard let connector = connector else {
            error = "Driver not connected"
            return
        }

        isLoading = true
        error = nil
        statusMessage = "Initiating ROM read for node \(nodeId)..."
        liveReadState = .triggeringRead
        sourceType = .driver

        let status = connector.triggerROMRead(nodeId: nodeId)
        switch status {
        case .initiated:
            statusMessage = "ROM read initiated. Waiting for driver to cache the ROM..."
            pollForROM(nodeId: nodeId, remainingRetries: 12)
        case .alreadyInProgress:
            isLoading = false
            liveReadState = .idle
            statusMessage = "ROM read already in progress. Try loading cache again in a moment."
        case .failed:
            isLoading = false
            liveReadState = .idle
            error = connector.lastError ?? "Failed to initiate ROM read"
        }
    }

    private func pollForROM(nodeId: UInt8, remainingRetries: Int) {
        guard remainingRetries > 0 else {
            isLoading = false
            liveReadState = .idle
            statusMessage = "Timed out waiting for ROM read completion"
            return
        }
        guard let connector, let gen = topologyGeneration else {
            isLoading = false
            liveReadState = .idle
            error = "Topology generation unavailable while polling ROM read"
            return
        }

        liveReadState = .polling(13 - remainingRetries)
        statusMessage = "Waiting for ROM read... (attempt \(13 - remainingRetries)/12)"

        DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: .now() + 0.4) { [weak self] in
            guard let self else { return }
            if let data = connector.getConfigROM(nodeId: nodeId, generation: gen) {
                DispatchQueue.main.async {
                    self.parseAndPublishROM(data: data,
                                            sourceType: .driver,
                                            statusMessage: "ROM read complete (\(data.count) bytes)")
                }
            } else {
                DispatchQueue.main.async {
                    self.pollForROM(nodeId: nodeId, remainingRetries: remainingRetries - 1)
                }
            }
        }
    }

    private func parseAndPublishROM(data: Data, sourceType: SourceType, statusMessage: String) {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                let romTree = try RomParser.parse(data: data)
                DispatchQueue.main.async {
                    self?.rom = romTree
                    self?.sourceType = sourceType
                    self?.error = nil
                    self?.selection = nil
                    self?.showBusInfo = true
                    self?.isLoading = false
                    self?.liveReadState = .idle
                    self?.statusMessage = statusMessage
                }
            } catch {
                DispatchQueue.main.async {
                    self?.rom = nil
                    self?.error = "Failed to parse Config ROM: \(error.localizedDescription)"
                    self?.isLoading = false
                    self?.liveReadState = .idle
                }
            }
        }
    }

    // MARK: - Selection Management

    func selectBusInfo() {
        showBusInfo = true
        selection = nil
    }

    func select(entry: DirectoryEntry) {
        selection = entry
        showBusInfo = false
    }

    var entriesToShow: [DirectoryEntry]? {
        guard let rom else { return nil }
        if showInterpreted {
            return RomInterpreter.interpretRoot(rom.rootDirectory)
        } else {
            return rom.rootDirectory
        }
    }

    var selectionDescription: String? {
        guard let sel = selection else { return nil }
        var out: [String] = []
        out.append("Key: \(sel.keyName) (0x\(String(sel.keyId, radix: 16))) type: \(sel.type)")
        if let q = sel.entryQuadletIndex { out.append("Entry q: \(q)") }
        if let raw = sel.rawEntryWord { out.append(String(format: "Entry raw: 0x%08x", raw)) }
        if let rel = sel.relativeOffset24 { out.append("Relative offset: \(rel) quadlets") }
        if let target = sel.targetQuadletIndex { out.append("Target q: \(target)") }
        switch sel.value {
        case .immediate(let v): out.append(String(format: "Immediate: 0x%08x", v))
        case .csrOffset(let v): out.append(String(format: "CSR: 0x%012llx", v))
        case .leafPlaceholder(let off): out.append(String(format: "Leaf offset (bytes): 0x%08x", off))
        case .leafDescriptorText(let s, _): out.append("Descriptor text: \"\(s)\"")
        case .leafEUI64(let v): out.append(String(format: "EUI-64: 0x%016llx", v))
        case .leafData(let d): out.append("Leaf data bytes: \(d.count)")
        case .directory(let d): out.append("Directory entries: \(d.count)")
        }
        return out.joined(separator: "\n")
    }

    var sourceDescription: String {
        switch sourceType {
        case .file:
            return "File"
        case .driver:
            if let node = selectedNode {
                return "Node \(node.nodeId)"
            }
            return "Driver"
        }
    }
}
