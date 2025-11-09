//
//  TopologyViewModel.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import Foundation
import Combine

class TopologyViewModel: ObservableObject {
    @Published var selfIDCapture: SelfIDCapture?
    @Published var topology: TopologySnapshot?
    @Published var isLoading = false
    @Published var error: String?
    
    private var connector: ASFWDriverConnector
    private var statusCancellable: AnyCancellable?
    
    init(connector: ASFWDriverConnector) {
        self.connector = connector
        statusCancellable = connector.statusPublisher
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in
                self?.refresh()
            }
    }
    
    func startAutoRefresh(interval: TimeInterval = 1.0) {
        statusCancellable = connector.statusPublisher
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in
                self?.refresh()
            }
        refresh()
    }
    
    func stopAutoRefresh() {
        statusCancellable = nil
    }
    
    func refresh() {
        guard !isLoading else { 
            print("[TopologyVM] 🔄 Refresh already in progress, skipping")
            return 
        }
        print("[TopologyVM] 🔍 Starting refresh...")
        isLoading = true
        error = nil
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            
            // Fetch Self-ID data
            print("[TopologyVM] 📡 Calling getSelfIDCapture()...")
            let selfID = self.connector.getSelfIDCapture()
            print("[TopologyVM] 📡 getSelfIDCapture() returned: \(selfID != nil ? "✅ DATA (gen=\(selfID!.generation), \(selfID!.rawQuadlets.count) quads)" : "❌ NIL")")
            
            // Fetch topology
            print("[TopologyVM] 🌐 Calling getTopologySnapshot()...")
            let topo = self.connector.getTopologySnapshot()
            print("[TopologyVM] 🌐 getTopologySnapshot() returned: \(topo != nil ? "✅ DATA (gen=\(topo!.generation), \(topo!.nodes.count) nodes)" : "❌ NIL")")
            
            DispatchQueue.main.async {
                self.selfIDCapture = selfID
                self.topology = topo
                self.isLoading = false
                
                if selfID == nil && topo == nil {
                    print("[TopologyVM] ⚠️  No data from either call - setting error message")
                    self.error = "No topology data available. Wait for bus reset."
                } else {
                    print("[TopologyVM] ✅ Refresh complete - data updated!")
                }
            }
        }
    }
    
    deinit {
        statusCancellable = nil
    }
}
