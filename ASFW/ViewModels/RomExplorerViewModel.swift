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
@Observable
final class RomExplorerViewModel {
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

    var rom: RomTree?
    var error: String?
    var selection: DirectoryEntry?
    var showBusInfo: Bool = false
    var showInterpreted: Bool = true
    var isLoading: Bool = false
    var sourceType: SourceType = .file
    var statusMessage: String?
    var liveReadState: LiveReadState = .idle

    // Node selection for driver ROM reading
    var selectedNode: TopologyNode?
    var availableNodes: [TopologyNode] = []

    // Reference to connector for driver ROM reading
    @ObservationIgnored private var connectorObservable: ASFWDriverConnector.Observable
    @ObservationIgnored private(set) var topologyViewModel: TopologyViewModel

    // Summarized info for UI (vendor/model names, modalias, units)
    @ObservationIgnored var summary: RomSummary? {
        guard let rom else { return nil }
        return Summarizer.summarize(tree: rom)
    }

    @ObservationIgnored var topologyGeneration: UInt16? {
        guard let gen = topologyViewModel.topology?.generation else { return nil }
        return UInt16(gen)
    }

    @ObservationIgnored var canReadSelectedNode: Bool {
        selectedNode != nil
    }

    init(connectorObservable: ASFWDriverConnector.Observable) {
        self.connectorObservable = connectorObservable
        self.topologyViewModel = TopologyViewModel(connectorObservable: connectorObservable)
        if let topology = topologyViewModel.topology {
            self.availableNodes = topology.nodes
        }
    }

    func setConnector(_ connector: ASFWDriverConnector.Observable, topologyViewModel: TopologyViewModel) {
        self.connectorObservable = connector
        self.topologyViewModel = topologyViewModel
        refreshAvailableNodes()
    }

    func refreshTopology() {
        topologyViewModel.refresh()
        refreshAvailableNodes()
    }

    func refreshAvailableNodes() {
        if let topology = topologyViewModel.topology {
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

        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak self, weak connector] in
            guard let self, let connector else { return }
            guard let result = await connector.getConfigROM(nodeId: node.nodeId, generation: gen) else {
                Task { @MainActor in
                    self.isLoading = false
                    self.liveReadState = .idle
                    self.rom = nil
                    self.error = nil
                    self.statusMessage = "ROM not cached for node \(node.nodeId). Click Read ROM."
                }
                return
            }
            Task { @MainActor in
                let staleSuffix = result.isExactGenerationMatch
                    ? ""
                    : " (stale cache gen \(result.resolvedGeneration), requested gen \(gen))"
                self.parseAndPublishROM(data: result.data,
                                        sourceType: .driver,
                                        statusMessage: "ROM loaded from cache (\(result.data.count) bytes)\(staleSuffix)")
            }
        }
    }

    func triggerROMReadForSelectedNode() {
        guard let node = selectedNode else {
            error = "Select a node first"
            return
        }
        Task {
            await triggerROMRead(nodeId: node.nodeId)
        }
    }

    func triggerROMRead(nodeId: UInt8) async {
        isLoading = true
        error = nil
        statusMessage = "Initiating ROM read for node \(nodeId)..."
        liveReadState = .triggeringRead
        sourceType = .driver

        let status = await connectorObservable.connector.triggerROMRead(nodeId: nodeId)
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
            error = connectorObservable.lastError ?? "Failed to initiate ROM read"
        }
    }

    private func pollForROM(nodeId: UInt8, remainingRetries: Int) {
        guard remainingRetries > 0 else {
            isLoading = false
            liveReadState = .idle
            statusMessage = "Timed out waiting for ROM read completion"
            return
        }
        guard let gen = topologyGeneration else {
            isLoading = false
            liveReadState = .idle
            error = "Topology generation unavailable while polling ROM read"
            return
        }

        liveReadState = .polling(13 - remainingRetries)
        statusMessage = "Waiting for ROM read... (attempt \(13 - remainingRetries)/12)"
        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak self, weak connector] in
            try? await Task.sleep(until: .now.advanced(by: .milliseconds(400)))
            guard let self, let connector else { return }
            if let result = await connector.getConfigROM(nodeId: nodeId, generation: gen) {
               Task { @MainActor in
                    if result.isExactGenerationMatch {
                        self.parseAndPublishROM(data: result.data,
                                                sourceType: .driver,
                                                statusMessage: "ROM read complete (\(result.data.count) bytes)")
                    } else {
                        self.statusMessage = "Waiting for fresh ROM read... saw stale cache from gen \(result.resolvedGeneration)"
                        self.pollForROM(nodeId: nodeId, remainingRetries: remainingRetries - 1)
                    }
                }
            } else {
                Task { @MainActor in
                    self.pollForROM(nodeId: nodeId, remainingRetries: remainingRetries - 1)
                }
            }
        }
    }

    private func parseAndPublishROM(data: Data, sourceType: SourceType, statusMessage: String) {
        Task.detached(priority: .userInitiated) { [weak self] in
            do {
                let romTree = try RomParser.parse(data: data)
                Task { @MainActor in
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
                Task { @MainActor in
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

    @ObservationIgnored var entriesToShow: [DirectoryEntry]? {
        guard let rom else { return nil }
        if showInterpreted {
            return RomInterpreter.interpretRoot(rom.rootDirectory)
        } else {
            return rom.rootDirectory
        }
    }

    @ObservationIgnored var selectionDescription: String? {
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

    @ObservationIgnored var sourceDescription: String {
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
