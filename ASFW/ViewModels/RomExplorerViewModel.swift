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
    @Published var rom: RomTree?
    @Published var error: String?
    @Published var selection: DirectoryEntry?
    @Published var showBusInfo: Bool = false
    @Published var showInterpreted: Bool = true
    @Published var isLoading: Bool = false
    @Published var sourceType: SourceType = .file

    // Node selection for driver ROM reading
    @Published var selectedNode: TopologyNode?
    @Published var availableNodes: [TopologyNode] = []

    // Reference to connector for driver ROM reading
    private var connector: ASFWDriverConnector?
    private var topologyViewModel: TopologyViewModel?

    enum SourceType {
        case file
        case driver
    }

    // Summarized info for UI (vendor/model names, modalias, units)
    var summary: RomSummary? {
        guard let rom else { return nil }
        return Summarizer.summarize(tree: rom)
    }

    init(connector: ASFWDriverConnector? = nil, topologyViewModel: TopologyViewModel? = nil) {
        self.connector = connector
        self.topologyViewModel = topologyViewModel

        // Load nodes if topology is available
        if let topology = topologyViewModel?.topology {
            self.availableNodes = topology.nodes
        }
    }

    func setConnector(_ connector: ASFWDriverConnector, topologyViewModel: TopologyViewModel) {
        self.connector = connector
        self.topologyViewModel = topologyViewModel
        refreshAvailableNodes()
    }

    func refreshAvailableNodes() {
        if let topology = topologyViewModel?.topology {
            availableNodes = topology.nodes
        }
    }

    // MARK: - File Loading

    func open(url: URL) {
        isLoading = true
        error = nil
        sourceType = .file

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                let romTree = try RomParser.parse(fileURL: url)
                DispatchQueue.main.async {
                    self?.rom = romTree
                    self?.error = nil
                    self?.selection = nil
                    self?.showBusInfo = true
                    self?.isLoading = false
                }
            } catch {
                DispatchQueue.main.async {
                    self?.rom = nil
                    self?.error = String(describing: error)
                    self?.isLoading = false
                }
            }
        }
    }

    // MARK: - Driver ROM Reading

    func loadROMFromNode(_ node: TopologyNode) {
        guard let connector = connector else {
            error = "Driver not connected"
            return
        }

        isLoading = true
        error = nil
        sourceType = .driver
        selectedNode = node

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }

            // Retrieve cached ROM from driver using current bus generation
            let generation: UInt16? = {
                if let gen = self.topologyViewModel?.topology?.generation {
                    return UInt16(gen)
                }
                return nil
            }()

            guard let gen = generation else {
                DispatchQueue.main.async {
                    self.isLoading = false
                    self.error = "Topology generation unknown. Refresh topology and try again."
                }
                return
            }

            guard let data = connector.getConfigROM(nodeId: node.nodeId, generation: gen) else {
                DispatchQueue.main.async {
                    self.isLoading = false
                    self.error = "No Config ROM cached for node \(node.nodeId) (gen=\(gen)). Click 'Read ROM' in ROM Explorer or wait for next bus reset."
                }
                return
            }

            // Parse the ROM data
            do {
                let romTree = try RomParser.parse(data: data)
                DispatchQueue.main.async {
                    self.rom = romTree
                    self.error = nil
                    self.selection = nil
                    self.showBusInfo = true
                    self.isLoading = false
                }
            } catch {
                DispatchQueue.main.async {
                    self.rom = nil
                    self.error = "Failed to parse Config ROM: \(error.localizedDescription)"
                    self.isLoading = false
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
        switch sel.value {
        case .immediate(let v): out.append(String(format: "Immediate: 0x%08x", v))
        case .csrOffset(let v): out.append(String(format: "CSR: 0x%012llx", v))
        case .leafPlaceholder(let off): out.append(String(format: "Leaf offset (relative): 0x%08x", off))
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
