//
//  DiagnosticsStore.swift
//  ASFW
//
//  Created by ASFireWire Project on 29.05.2026.
//

import Foundation
import Combine

@MainActor
@Observable
final class DiagnosticsStore {
    var isRefreshing = false
    var error: String?
    var reportText: String = "No diagnostics report loaded yet. Click Refresh to query the driver."
    var lastSnapshot: ASFWDiagnosticsSnapshot?
    var isClearingTrace = false
    
    @ObservationIgnored private let client: ASFWDiagnosticsClient
    @ObservationIgnored private let connectorObservable: ASFWDriverConnector.Observable
    @ObservationIgnored private var statusCancellable: AnyCancellable?
    
    init(connectorObservable: ASFWDriverConnector.Observable) {
        self.connectorObservable = connectorObservable
        self.client = ASFWDiagnosticsClient(connector: connectorObservable.connector)
        
        // Refresh diagnostics when driver connects
        statusCancellable = connectorObservable.$latestStatus
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in
                self?.refresh()
            }
    }
    
    func refresh() {
        guard connectorObservable.isConnected else {
            self.error = "Not connected to ASFW driver. Check connection status."
            self.reportText = "ASFW driver is not connected. Connect the driver using the controls in the toolbar, then try refreshing diagnostics."
            return
        }
        
        guard !isRefreshing else { return }
        isRefreshing = true
        error = nil
        
        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak self, weak connector] in
            guard let self, let connector else { return }
            
            do {
                print("[DiagStore] 🔍 Querying driver diagnostics selectors...")
                let snapshot = try await self.client.fetchSnapshot()
                print("[DiagStore] ✅ Successfully retrieved diagnostic snapshot.")

                // Loaded-driver build, so the report shows which dext is actually
                // running (build-freshness check). Best-effort; nil just omits the row.
                let version = await connector.getDriverVersion()
                let text = DiagnosticsTextFormatter.format(snapshot: snapshot, version: version)
                
                Task { @MainActor in
                    self.lastSnapshot = snapshot
                    self.reportText = text
                    self.isRefreshing = false
                }
            } catch {
                print("[DiagStore] ❌ Failed to fetch diagnostics: \(error)")
                let errorDescription = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription

                Task { @MainActor in
                    self.error = errorDescription
                    self.reportText = "ERROR: Failed to fetch diagnostics.\n\nDetails: \(errorDescription)"
                    self.isRefreshing = false
                }
            }
        }
    }
    
    func clearTrace() {
        guard connectorObservable.isConnected else { return }
        guard !isClearingTrace else { return }
        isClearingTrace = true
        
        Task.detached(priority: .userInitiated) { [weak self, weak client] in
            guard let self, let client else { return }
            
            do {
                print("[DiagStore] 🧹 Clearing async transactions trace...")
                try await client.clearAsyncTrace()
                print("[DiagStore] ✅ Cleared async transactions trace.")
                
                Task { @MainActor in
                    self.isClearingTrace = false
                    // Re-fetch snapshot immediately to show the cleared trace state
                    self.refresh()
                }
            } catch {
                print("[DiagStore] ❌ Failed to clear async trace: \(error)")
                let errorDescription = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
                
                Task { @MainActor in
                    self.error = "Failed to clear trace: \(errorDescription)"
                    self.isClearingTrace = false
                }
            }
        }
    }
}
