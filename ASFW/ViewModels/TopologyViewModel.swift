//
//  TopologyViewModel.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import Foundation
import Combine

@MainActor
@Observable
final class TopologyViewModel {
    var selfIDCapture: SelfIDCapture?
    var topology: TopologySnapshot?
    var isLoading = false
    var error: String?
    
    @ObservationIgnored private var connectorObservable: ASFWDriverConnector.Observable
    @ObservationIgnored private var statusCancellable: AnyCancellable?
    
    init(connectorObservable: ASFWDriverConnector.Observable) {
        self.connectorObservable = connectorObservable
        statusCancellable = connectorObservable.$latestStatus
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in
                self?.refresh()
            }
    }
    
    func startAutoRefresh(interval: TimeInterval = 1.0) {
        statusCancellable = connectorObservable.$latestStatus
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

        Task { [weak self] in
            guard let self = self else { return }

            // Fetch Self-ID data
            print("[TopologyVM] 📡 Calling getSelfIDCapture()...")
            let selfID = await self.connectorObservable.connector.getSelfIDCapture()
            print("[TopologyVM] 📡 getSelfIDCapture() returned: \(selfID != nil ? "✅ DATA (gen=\(selfID!.generation), \(selfID!.rawQuadlets.count) quads)" : "❌ NIL")")

            // Fetch topology
            print("[TopologyVM] 🌐 Calling getTopologySnapshot()...")
            let topo = await self.connectorObservable.connector.getTopologySnapshot()
            print("[TopologyVM] 🌐 getTopologySnapshot() returned: \(topo != nil ? "✅ DATA (gen=\(topo!.generation), \(topo!.nodes.count) nodes)" : "❌ NIL")")

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
